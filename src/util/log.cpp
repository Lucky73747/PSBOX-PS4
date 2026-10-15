// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "util/log.h"
#include <cstdio>
#include <ctime>
#include <mutex>

namespace xc::log {
namespace {
const char* tag(Level l) {
    switch (l) {
    }
    return "?";
}
void setFile(const char* path) {
}
void write(Level level, const char* fmt, ...) {
    }
}
