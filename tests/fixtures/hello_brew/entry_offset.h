#pragma once

// AEEMod_Load's offset within hello_brew.bin -- verified against the
// actual build, NOT assumed to be 0 (the compiler is free to place
// functions in any order). See README.md in this directory to
// regenerate/reverify after changing hello_brew.c.
//
// Shared between tests/brew_lifecycle_test.cpp and the standalone
// frontend's bundled M0 demo so the two never drift out of sync.
constexpr unsigned int kHelloBrewAeeModLoadOffset = 0x104;
