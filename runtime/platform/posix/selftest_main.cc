// SPDX-License-Identifier: GPL-3.0-or-later
// Host entry of the CPU self-test: pkp_selftest <exefs dir> [log file]
#include <cstdio>

#include "pkp/rt.h"

int main(int argc, char** argv) {
    const char* exefs = argc > 1 ? argv[1] : "Pokemon Pokopia [0400A3A01EE4C000]/updates/327680/exefs";
    if (argc > 2) pkp::log_set_file(argv[2]);
    static pkp::SelfTestReport r;
    pkp::log("Pokopia Native on PS5: host CPU self-test of the recompiled sdk module (%s)", exefs);
    pkp::run_selftest(&r, exefs);
    static pkp::SelfTestReport probe;
    pkp::run_layout_probe(&probe);
    return r.failed ? 1 : 0;
}
