#pragma once

// AEEMod_Load's offset within hello_gl.bin -- verified against the
// actual build, NOT assumed to be 0 (the compiler is free to place
// functions in any order). See README.md in this directory to
// regenerate/reverify after changing hello_gl.c.
constexpr unsigned int kHelloGlAeeModLoadOffset = 0x388;
