// SPDX-License-Identifier: GPL-3.0-or-later
// Monotonic clock bridge used by shared GSSV code in the isolated PS4 target.
#include "platform/platform.h"

#include <orbis/libkernel.h>
#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdlib.h>
#include <unistd.h>

extern "C" int xc_ps4_random_bytes(void* buffer, size_t length);

namespace xc::platform {

uint64_t nowMs() {
    return sceKernelGetProcessTime() / 1000u;
}

uint64_t nowUs() {
    return sceKernelGetProcessTime();
}

void sleepMs(uint32_t ms) {
    sceKernelUsleep(static_cast<uint64_t>(ms) * 1000u);
}

bool randomBytes(void* out, size_t len) {
    if (!out && len != 0) return false;
    return xc_ps4_random_bytes(out, len) == 0;
}

namespace {
struct ThreadTask { std::function<void()> fn; };

int runThreadTask(void* opaque) {
    std::unique_ptr<ThreadTask> task(static_cast<ThreadTask*>(opaque));
    task->fn();
    return 0;
}
}  // namespace

bool startThread(Thread& thread, std::function<void()> fn, size_t stackBytes) {
    if (thread.handle) return false;
    auto* task = new ThreadTask{std::move(fn)};
    const SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        delete task;
        return false;
    }
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           reinterpret_cast<void*>(static_cast<SDL_ThreadFunction>(runThreadTask)));
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, task);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "xcloud-image-cache");
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, static_cast<Sint64>(stackBytes));
    SDL_Thread* created = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!created) {
        delete task;
        return false;
    }
    thread.handle = created;
    return true;
}

void Thread::join() {
    if (!handle) return;
    SDL_WaitThread(static_cast<SDL_Thread*>(handle), nullptr);
    handle = nullptr;
}

}  // namespace xc::platform
