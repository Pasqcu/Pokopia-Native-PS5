// SPDX-License-Identifier: GPL-3.0-or-later
// avrt.h subset: MMCSS thread characteristics (audio thread boost) are a no-op on the port.
#pragma once
#include "windows.h"
inline HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR, DWORD* index) { if (index) *index = 0; return (HANDLE)(intptr_t)1; }
inline BOOL AvRevertMmThreadCharacteristics(HANDLE) { return TRUE; }
inline BOOL AvSetMmThreadPriority(HANDLE, int) { return TRUE; }
#define AVRT_PRIORITY_HIGH 1
#define AVRT_PRIORITY_CRITICAL 2
