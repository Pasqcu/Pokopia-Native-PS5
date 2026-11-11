// SPDX-License-Identifier: GPL-3.0-or-later
// The PS5 graphics title's boot screen: where the boot is while the game starts. The system's splash screen goes away at
// once (it stays over every picture otherwise, the game's too). A status screen shows the boot's step, an estimated
// percentage, the time, what the process is doing (CPU, memory, the graphics back end's counters, nxrt's log) and the last
// lines of nxrt's log, errors in red. The CPU draws it on VideoOut until the graphics start (RADV takes VideoOut then); the
// renderer draws it on the game's swapchain from there until the game's first frame (pkp::boot::status_screen_start).
// /app0/env.txt: PKP_BOOT_SCREEN=0 leaves the status screen out (the splash screen still goes away).
#pragma once

namespace pkp::boot_screen {

enum class Step { Runtime, Graphics, Modules, Game };

void start();                  // main, after the environment is set and before nxrt::init (installs a log sink)
void step(Step s);
void hand_over();              // before start_graphics: VideoOut is given back for RADV
void on_renderer();            // after start_graphics: the status goes on the game's swapchain
void fail(const char* what);   // the boot stopped: the screen keeps the error

}  // namespace pkp::boot_screen
