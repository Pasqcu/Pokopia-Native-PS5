// SPDX-License-Identifier: GPL-3.0-or-later
// tlhelp32.h subset: no process snapshots on the port (thread enumeration for gauges reports none).
#pragma once
#include "windows.h"
#define TH32CS_SNAPTHREAD 0x4u
typedef struct tagTHREADENTRY32 { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32;
inline HANDLE CreateToolhelp32Snapshot(DWORD, DWORD) { return INVALID_HANDLE_VALUE; }
inline BOOL Thread32First(HANDLE, THREADENTRY32*) { return FALSE; }
inline BOOL Thread32Next(HANDLE, THREADENTRY32*) { return FALSE; }
