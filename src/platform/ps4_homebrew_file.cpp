// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal read-only asset access for the standalone PS4 homebrew bootstrap.
#include "platform/platform.h"

#include <cstdio>
#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace xc::platform {

std::string dataDir() { return "/data/XCHM00001"; }
std::string assetDir() { return "/app0/assets"; }
std::string caBundlePath() { return "/app0/assets/cacert.pem"; }

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) out.append(buf, n);
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    const std::string temporary = path + ".tmp";
    FILE* f = std::fopen(temporary.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace xc::platform
