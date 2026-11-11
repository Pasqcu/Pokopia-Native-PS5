// SPDX-License-Identifier: GPL-3.0-or-later
// psapi.h subset for the nxrt port: process memory counters (memory guard, gauges).
#pragma once
#include "windows.h"

typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
        QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;

BOOL GetProcessMemoryInfo(HANDLE p, PPROCESS_MEMORY_COUNTERS pmc, DWORD cb);
