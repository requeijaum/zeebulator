#pragma once

#include <istream>
#include <ostream>

#include "core/cpu/arm_interpreter.h"

namespace zeebulator {

// Real, versioned Zeebulator save-state format (TASKS_TOOLING.md Phase B).
//
// Stage 1 only: captures guest CPU registers/CPSR and the full guest
// memory image (ArmInterpreter::Serialize) -- NOT host-side HLE class
// state (Mixer's active voices, IShellHle's pending timers, MediaHle's
// registered notify callbacks, ModRuntime's heap bookkeeping, ...),
// since none of those classes have a serialize hook yet (stage 2's
// job). Reloading a stage-1 save resumes guest code/data correctly,
// but expect audio/timer glitches immediately after a load until the
// guest naturally re-arms whatever host-side state it depends on --
// this is a real, known, documented gap, not an oversight.
//
// The magic/version header exists so a stage-2 format extension can
// still recognize (and refuse, rather than corrupt-read) an
// older stage-1-only file, or vice versa.
bool SaveState(const ArmInterpreter& cpu, std::ostream& out);
bool LoadState(ArmInterpreter& cpu, std::istream& in);

}  // namespace zeebulator
