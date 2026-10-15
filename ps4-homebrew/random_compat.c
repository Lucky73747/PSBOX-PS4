// SPDX-License-Identifier: GPL-3.0-or-later
// OpenOrbis lacks arc4random_buf; use the PS4 system CSPRNG for transport entropy.
#include <orbis/Sysmodule.h>
#include <orbis/_types/sysmodule.h>

#include <pthread.h>
#include <stddef.h>

extern int sceRandomGetRandomNumber(void *buffer, size_t length);

static pthread_once_t random_module_once = PTHREAD_ONCE_INIT;
static int random_module_status = -1;

static void load_random_module(void) {
    random_module_status = sceSysmoduleLoadModule(ORBIS_SYSMODULE_RANDOM);
}

int xc_ps4_random_bytes(void *buffer, size_t length) {
    if (length == 0) return 0;
    if (!buffer) return -1;
    if (pthread_once(&random_module_once, load_random_module) != 0 || random_module_status < 0)
        return -1;
    return sceRandomGetRandomNumber(buffer, length);
}

// usrsctp's FreeBSD userspace branch expects the BSD API name.
void arc4random_buf(void *buffer, size_t length) {
    if (xc_ps4_random_bytes(buffer, length) != 0) __builtin_trap();
}
