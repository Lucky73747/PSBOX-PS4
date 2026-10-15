// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal net::perform bridge for the PS4 bootstrap. It uses the initialized
// SceHttp/SceSsl contexts and keeps certificate verification enabled.
#include "net/http.h"

#include "network.h"

#include <orbis/Http.h>
#include <orbis/libkernel.h>
#include <orbis/_types/http.h>

#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

namespace xc::net {
namespace {
std::mutex g_httpGateMutex;
std::condition_variable g_httpGateChanged;
size_t g_httpApiWaiters = 0;
bool g_httpActive = false;

class HttpRequestLease {
public:
        std::unique_lock<std::mutex> lock(g_httpGateMutex);
        if (!imageRequest) {
            ++g_httpApiWaiters;
            --g_httpApiWaiters;
        } else {
        }
        g_httpActive = true;
    }

    ~HttpRequestLease() {
        {
            std::lock_guard<std::mutex> lock(g_httpGateMutex);
            g_httpActive = false;
        }
        g_httpGateChanged.notify_all();
    }

    HttpRequestLease(const HttpRequestLease&) = delete;
    HttpRequestLease& operator=(const HttpRequestLease&) = delete;
};

void logHttp(const char* stage, const char* detail = "") {
    char line[384];
    std::snprintf(line, sizeof(line), "[%s] https stage=%s %s\n", kBuild, stage, detail);
}

int32_t verifyCertificate(int32_t, uint32_t verifyError, void* const[], int32_t, void*) {
    // SceHttp's callback return is an allow/deny result. Never override a
    // certificate validation error for account authentication.
    return verifyError == 0 ? 1 : 0;
}

int methodId(const std::string& method) {
    if (method == "GET") return ORBIS_METHOD_GET;
    if (method == "POST") return ORBIS_METHOD_POST;
    if (method == "HEAD") return ORBIS_METHOD_HEAD;
    if (method == "PUT") return ORBIS_METHOD_PUT;
    if (method == "DELETE") return ORBIS_METHOD_DELETE;
    return -1;
}

std::string httpError(const char* step, int32_t rc) {
    char text[128];
    std::snprintf(text, sizeof(text), "%s failed (0x%08x)", step,
                  static_cast<unsigned>(rc));
    return text;
}

} // namespace

std::string urlEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [key, value] : fields) {
        if (!out.empty()) out.push_back('&');
        out += urlEncode(key);
        out.push_back('=');
        out += urlEncode(value);
    }
    return out;
}

    // SceHttp uses one shared PS4 context. Keep each request lifecycle atomic,
    // and let login/catalog calls jump ahead of image downloads so artwork
    // cannot hold up library pagination.
    const bool imageRequest = request.url.find("store-images.s-microsoft.com") != std::string::npos;
    Response response;
    const bool traceStages = !imageRequest;
    auto trace = [&](const char* stage, const char* detail = "") {
        if (traceStages) logHttp(stage, detail);
    };
    // The OAuth endpoints themselves contain no credentials; bodies and all
    // device/user codes remain deliberately absent from diagnostics.
    trace("begin", request.url.c_str());
    const int32_t context = xc::ps4::httpContextId();
    if (context < 0) {
        response.error = "SceHttp context is not initialized";
        return response;
    }
    const int method = methodId(request.method);
    if (method < 0) {
        response.error = "unsupported HTTP method";
        return response;
    }

    const int32_t templ = sceHttpCreateTemplate(
    if (templ < 0) {
        response.error = httpError("sceHttpCreateTemplate", templ);
        return response;
    }
    trace("template-created");
    int32_t rc = sceHttpsSetSslCallback(templ, verifyCertificate, nullptr);
    if (rc < 0) {
        response.error = httpError("sceHttpsSetSslCallback", rc);
        sceHttpDeleteTemplate(templ);
        return response;
    }
    trace("strict-tls-callback-set");
    const uint32_t timeoutUs = static_cast<uint32_t>(request.timeoutMs > 0 ? request.timeoutMs : 20000) * 1000u;
    if ((rc = sceHttpSetResolveTimeOut(templ, timeoutUs)) < 0 ||
        (rc = sceHttpSetConnectTimeOut(templ, timeoutUs)) < 0 ||
        (rc = sceHttpSetSendTimeOut(templ, timeoutUs)) < 0) {
        response.error = httpError("SceHttp timeout setup", rc);
        sceHttpDeleteTemplate(templ);
        return response;
    }
    trace("timeouts-set");

    trace("connection-create-begin");
    const int32_t connection = sceHttpCreateConnectionWithURL(templ, request.url.c_str(), 0);
    if (connection < 0) {
        response.error = httpError("sceHttpCreateConnectionWithURL", connection);
        sceHttpDeleteTemplate(templ);
        return response;
    }
    trace("connection-created");
    const int32_t req = sceHttpCreateRequestWithURL(
        connection, method, request.url.c_str(), request.body.size());
    if (req < 0) {
        response.error = httpError("sceHttpCreateRequestWithURL", req);
        sceHttpDeleteConnection(connection);
        sceHttpDeleteTemplate(templ);
        return response;
    }
    trace("request-created");

    for (const auto& [name, value] : request.headers) {
        rc = sceHttpAddRequestHeader(req, name.c_str(), value.c_str(), 0);
        if (rc < 0) {
            response.error = httpError("sceHttpAddRequestHeader", rc);
            goto cleanup;
        }
    }
    trace("send-begin");
    if ((rc = sceHttpSendRequest(req, request.body.empty() ? nullptr : request.body.data(),
                                 request.body.size())) < 0) {
        response.error = httpError("sceHttpSendRequest", rc);
        goto cleanup;
    }
    trace("send-complete");
    if ((rc = sceHttpGetStatusCode(req, &response.status)) < 0) {
        response.error = httpError("sceHttpGetStatusCode", rc);
        response.status = 0;
        goto cleanup;
    }
    {
        char status[48];
        std::snprintf(status, sizeof(status), "http_status=%d", response.status);
        trace("response-status", status);
    }

    {
        char body[4096];
        trace("read-begin");
        for (;;) {
            const int32_t bytes = sceHttpReadData(req, body, sizeof(body));
            if (bytes < 0) {
                response.error = httpError("sceHttpReadData", bytes);
                response.status = 0;
                break;
            }
            if (bytes == 0) break;
            if (response.body.size() + static_cast<size_t>(bytes) > (1024u * 1024u)) {
                response.error = "HTTP response exceeded 1 MiB limit";
                response.status = 0;
                break;
            }
            response.body.append(body, static_cast<size_t>(bytes));
        }
        char readInfo[48];
        std::snprintf(readInfo, sizeof(readInfo), "body_bytes=%zu", response.body.size());
        trace("read-complete", readInfo);
    }

cleanup:
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(connection);
    sceHttpDeleteTemplate(templ);
    return response;
}

} // namespace xc::net
