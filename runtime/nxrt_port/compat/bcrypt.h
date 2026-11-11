// SPDX-License-Identifier: GPL-3.0-or-later
// bcrypt.h subset: the system random number generator (csrng service).
#pragma once
#include <stdlib.h>
#include "windows.h"
typedef LONG NTSTATUS;
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002u
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
inline NTSTATUS BCryptGenRandom(void*, unsigned char* buf, ULONG n, ULONG) {
    arc4random_buf(buf, n);
    return STATUS_SUCCESS;
}
