// SPDX-License-Identifier: GPL-3.0-or-later
#include "network.h"

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <orbis/_types/http.h>
#include <orbis/_types/sysmodule.h>

#include <cstdio>
#include <cstring>

namespace xc::ps4 {
namespace {
constexpr int kNetPoolBytes = 1024 * 1024;
constexpr size_t kHttpPoolBytes = 256 * 1024;
// 0x809517d5 is SCE_SSL_ERROR_OUT_OF_MEMORY. The PS4 SSL pool needs at least
// 256 KiB for up to three connections; leave headroom for certificate data
// and repeated catalog/image TLS handshakes even though SceHttp is serialized.
constexpr size_t kSslPoolBytes = 512 * 1024;
constexpr uint32_t kNetworkTimeoutUs = 8 * 1000 * 1000;
constexpr char kConnectivityUrl[] = "http://www.msftconnecttest.com/connecttest.txt";
constexpr char kConnectivityMarker[] = "Microsoft Connect Test";
int32_t g_netPool = -1;
int32_t g_sslContext = -1;
int32_t g_httpContext = -1;

int32_t loadModule(OrbisSysModuleInternal id) {
    return static_cast<int32_t>(sceSysmoduleLoadModuleInternal(id));
}

void fail(NetworkStatus& status, const char* step, int32_t error) {
    std::snprintf(status.failedStep, sizeof(status.failedStep), "%s", step);
    status.errorCode = error;
}

} // namespace

int32_t httpContextId() { return g_httpContext; }

NetworkStatus initializeNetworkAndCheckInternet() {
    NetworkStatus status{};
    int32_t rc = loadModule(ORBIS_SYSMODULE_INTERNAL_NET);
    if (rc < 0) { fail(status, "load-net-module", rc); return status; }

    rc = loadModule(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    if (rc < 0) { fail(status, "load-netctl-module", rc); return status; }

    rc = loadModule(ORBIS_SYSMODULE_INTERNAL_SSL);
    if (rc < 0) { fail(status, "load-ssl-module", rc); return status; }

    rc = loadModule(ORBIS_SYSMODULE_INTERNAL_HTTP);
    if (rc < 0) { fail(status, "load-http-module", rc); return status; }

    rc = sceNetInit();
    if (rc < 0) { fail(status, "sceNetInit", rc); return status; }

    g_netPool = sceNetPoolCreate("xcloud-net", kNetPoolBytes, 0);
    if (g_netPool < 0) { fail(status, "sceNetPoolCreate", g_netPool); return status; }

    rc = sceNetCtlInit();
    if (rc < 0) { fail(status, "sceNetCtlInit", rc); return status; }

    OrbisNetCtlInfo info{};
    rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc < 0) {
        fail(status, "sceNetCtlGetInfo(ip)", rc);
    } else {
        std::snprintf(status.ipAddress, sizeof(status.ipAddress), "%s", info.ip_address);
        status.hasIpAddress = status.ipAddress[0] != '\0' && std::strcmp(status.ipAddress, "0.0.0.0") != 0;
    }
    status.stackReady = true;

    g_sslContext = sceSslInit(kSslPoolBytes);
    if (g_sslContext < 0) { fail(status, "sceSslInit", g_sslContext); return status; }

    g_httpContext = sceHttpInit(g_netPool, g_sslContext, kHttpPoolBytes);
    if (g_httpContext < 0) { fail(status, "sceHttpInit", g_httpContext); return status; }

    const int32_t requestTemplate = sceHttpCreateTemplate(
    if (requestTemplate < 0) { fail(status, "sceHttpCreateTemplate", requestTemplate); return status; }

    rc = sceHttpSetResolveTimeOut(requestTemplate, kNetworkTimeoutUs);
    if (rc < 0) { fail(status, "sceHttpSetResolveTimeOut", rc); sceHttpDeleteTemplate(requestTemplate); return status; }
    rc = sceHttpSetConnectTimeOut(requestTemplate, kNetworkTimeoutUs);
    if (rc < 0) { fail(status, "sceHttpSetConnectTimeOut", rc); sceHttpDeleteTemplate(requestTemplate); return status; }

    const int32_t connection = sceHttpCreateConnectionWithURL(requestTemplate, kConnectivityUrl, 0);
    if (connection < 0) {
        fail(status, "sceHttpCreateConnectionWithURL", connection);
        sceHttpDeleteTemplate(requestTemplate);
        return status;
    }

    const int32_t request = sceHttpCreateRequestWithURL(
        connection, ORBIS_METHOD_GET, kConnectivityUrl, 0);
    if (request < 0) {
        fail(status, "sceHttpCreateRequestWithURL", request);
        sceHttpDeleteConnection(connection);
        sceHttpDeleteTemplate(requestTemplate);
        return status;
    }

    rc = sceHttpSendRequest(request, nullptr, 0);
    if (rc < 0) {
        fail(status, "sceHttpSendRequest", rc);
    } else {
        rc = sceHttpGetStatusCode(request, &status.httpStatus);
        if (rc < 0) {
            fail(status, "sceHttpGetStatusCode", rc);
        } else if (status.httpStatus == 200) {
            char body[128]{};
            const int32_t bytes = sceHttpReadData(request, body, sizeof(body) - 1);
            if (bytes < 0) {
                fail(status, "sceHttpReadData", bytes);
            } else {
                body[bytes] = '\0';
                status.internetReachable = std::strstr(body, kConnectivityMarker) != nullptr;
                if (!status.internetReachable)
                    fail(status, "connectivity-response-mismatch", status.httpStatus);
            }
        } else {
            fail(status, "connectivity-http-status", status.httpStatus);
        }
    }

    sceHttpDeleteRequest(request);
    sceHttpDeleteConnection(connection);
    sceHttpDeleteTemplate(requestTemplate);
    return status;
}

} // namespace xc::ps4
