// SPDX-License-Identifier: GPL-3.0-or-later
// mmsystem.h subset: the timer resolution request is a no-op on the port (POSIX sleeps are not quantised to 15.6 ms).
#pragma once
#include "windows.h"
typedef UINT MMRESULT;
#define TIMERR_NOERROR 0u
inline MMRESULT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
inline MMRESULT timeEndPeriod(UINT) { return TIMERR_NOERROR; }
