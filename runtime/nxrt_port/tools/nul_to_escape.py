# SPDX-License-Identifier: GPL-3.0-or-later
# Copies a source file with each NUL byte written as the C escape \0 (upstream gfx/platform.cpp has a raw NUL inside a
# character literal; CMake's strings end at a NUL, so a transform could not see past it). Same meaning for the compiler.
import sys
data = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(data.replace(b"\x00", b"\\0"))
