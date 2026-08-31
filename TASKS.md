# Zeebulator — Task Breakdown

Status: Draft v0.1
Companion to PRD.md (goals/milestones) and ARCHITECTURE.md (component design).
Phases are sequential in intent but some overlap is expected — e.g. Phase 2
research informs Phase 1 finalization. Each phase lists its exit criterion.

---

## Phase 0 — Project Setup
Exit criterion: a contributor can clone, build an empty core, and CI is green.

- [x] Project name decided: **Zeebulator**
- [x] Open-source: confirmed
- [x] License decided: **GPLv3**
- [x] Add `LICENSE` file (GPLv3) to repo root
- [x] Initialize git repo, `.gitignore`, base directory layout (ARCHITECTURE.md §4)
- [x] `CMakeLists.txt` skeleton with `ZEEBULATOR_BUILD_LIBRETRO` / `ZEEBULATOR_BUILD_STANDALONE` options
      — verified building `zeebulator_core`, `zeebulator_standalone`, and
      `zeebulator_libretro` locally
- [x] Set up GitHub Actions CI matrix (Windows/macOS/Ubuntu, both build targets)
- [x] Add `CONTRIBUTING.md` codifying the clean-room policy (PRD §6.3 LR2) — no
      pasting SDK/decompiled source into the repo, ever
- [x] Pull down and locally archive (outside the repo — not committed) the
      research materials: official Zeebo SDK (v0.93, v1.2.4), BREW Developer
      Guide, BREW OEM API Reference for MSM, ARM1136 TRM, from the
      tripleoxygen.net mirror — saved under `research/docs/` (git-ignored)
- [x] Acquire one simple commercial game dump for local dev/test use —
      Double Dragon (`mif`/`mod`/`sig`/`data.ggz`/`sound.ggz`) added under
      `research/games/Double Dragon/` (git-ignored)
- [x] Acquire a small set of BREW SDK sample apps for local dev/test use —
      turned out to already be bundled inside `ZeeboSDKPackage-1.2.4.zip`
      (`samples.zip`), extracted to `research/samples/` (git-ignored):
      ~10 OpenGL ES sample apps in **source form** (`MSM7500_OGLES_...`)
      plus `conftest_source/` (includes `AEEModGen.c`/`AEEAppGen.c`, the
      reference module/applet boilerplate). Source form is strictly better
      for research than a bare `.mod` would have been — see Phase 1 note
- [x] Set up test infrastructure: GoogleTest via CMake `FetchContent`, wired
      through CTest (`ZEEBULATOR_BUILD_TESTS` option) — verified building
      and passing (`tests/core_test.cpp` smoke test green) locally

## Phase 1 — CPU & Memory Core
Exit criterion: can load an arbitrary ARM binary blob into emulated memory
and execute it correctly, verified against ISA test vectors — no BREW
awareness yet.

- [x] Stand up Memory Subsystem: flat 32-bit address space, typed
      read/write, bulk-load API (ARCHITECTURE.md §3.2) — paged/sparse
      backing store, little-endian, `core/memory/`
- [x] Implement `IArmCore` interface (ARCHITECTURE.md §3.1) — `core/cpu/arm_core.h`
- [x] Implement v1 ARMv6 interpreter — `core/cpu/arm_interpreter.cpp`.
      **Honest scope, not full ISA coverage:**
      - Covered: all 16 data-processing opcodes (AND/EOR/SUB/RSB/ADD/ADC/
        SBC/RSC/TST/TEQ/CMP/CMN/ORR/MOV/BIC/MVN), both operand2 forms
        (immediate, register with immediate or register-specified shift,
        all 4 shift types incl. RRX), all 15 condition codes, N/Z/C/V flag
        computation, B/BL, single word/byte LDR/STR (immediate and
        register offset, pre/post-index, writeback), block data transfer
        (LDM/STM, all 4 addressing modes, writeback — added during Phase 2
        real-`.mod` probing, see below), halfword/signed transfer
        (STRH/LDRH/LDRSB/LDRSH — added during Phase 3, hit by our own
        compiled test app, see below), BX/BLX (branch-and-exchange,
        register form, as long as the target stays in ARM state), the
        PC-reads-as+8 operand semantics, the call-out trap hook, and (added
        after Phase 6, closing the longest-standing deferred item) multiply/
        multiply-accumulate: `MUL`/`MLA` and long multiply `UMULL`/`UMLAL`/
        `SMULL`/`SMLAL`. Correctly propagates carry from the low to the high
        32-bit word during 64-bit accumulation (verified with a dedicated
        test — this is exactly the kind of thing a naive "two separate
        32-bit adds" implementation gets wrong) and sets N/Z from the full
        64-bit result for the long forms; C/V are left unchanged for S=1,
        matching the ARM ARM's own "UNPREDICTABLE" carry-flag behavior for
        these opcodes rather than inventing a specific value. 8 new tests
        (`tests/cpu_test.cpp`), every hand-encoded instruction word
        independently generated and cross-checked via a small Python
        encoder rather than computed by hand, given how easy nibble-level
        arithmetic is to get subtly wrong (a lesson already learned once in
        Phase 1's original `BIC` test).
        **Verified against real Double Dragon code, honestly**: reran
        `zeebulator_mod_probe` against the real `ddragonz.mod` — execution
        still stops at exactly the same 23rd instruction as before (falls
        into the same documented "returns to address 0, executes harmless
        zeroed no-ops" behavior). That's expected, not a sign multiply
        didn't help: this probe only exercises the module's tiny entry
        stub (`AEEMod_Load` and a couple of helper calls), which never
        needed multiplication — real usage will only show up once Phase 8
        drives the actual game logic through a real, fuller BREW lifecycle
        (real `IShell` behavior, real `ClsId`, real `EVT_APP_START` value,
        none of which exist yet). Implementing this now removes a known,
        named blocker ahead of that work rather than proving a payoff that
        genuinely can't be observed yet at this shallow a depth.
      - **Still deferred, and explicitly rejected via
        `UnimplementedInstruction` rather than silently mis-executed:**
        swap (SWP/SWPB), MRS/MSR (PSR transfer), SWI, coprocessor
        instructions, LDM/STM's user-bank/exception-return (S=1) variant.
        Will be picked up incrementally as real game code needs them.
        (Thumb state itself — full T16 decoding plus ARM/Thumb
        interworking — was deferred here too originally, but has since
        been implemented; see Phase 8's Peggle entry below.)
- [x] Unit tests against known ARMv6 instruction-behavior vectors —
      `tests/cpu_test.cpp`, 27 tests, all hand-encoded real ARM instruction
      words (not synthetic/fake encodings), verified bit-field-by-bit-field
- [x] **Research task, mostly resolved** via `research/samples/` source
      (see Phase 0): BREW's call-out mechanism is plain **C vtable/interface
      calls**, not an SWI/trap instruction. Confirmed from source:
      - A `.mod` exports a well-known entry point (`AEEMod_Load`, per
        `conftest_source/conftest/AEEModGen.c`'s "sample IModule
        implementation") that the loader calls, passing in an `IShell*`.
      - `AEEMod_Load` → `AEEMod_CreateInstance` → the app's own
        `AEEClsCreateInstance(ClsId, pIShell, po, ppObj)` (confirmed in
        `simple_drawtexture.c`), which calls `AEEApplet_New(...)` to
        register an `AEEHANDLER` event callback (`HandleEvent`).
      - The OS then drives the app purely through that event callback,
        starting with `EVT_APP_START`.
      - Every AEE interface (`IShell`, `IDisplay`, `IFile`, ...) is a
        struct of function pointers (COM-style), invoked through macros
        (e.g. `ISHELL_xxx(pIShell, ...)`) — this validates the
        ARCHITECTURE.md §3.4 design as-is: our loader constructs these
        interface objects with vtable slots pointing at sentinel addresses;
        the CPU core traps execution reaching one of those addresses and
        dispatches to HLE.
      - **Still open** (narrower now, not a fundamentally unknown
        mechanism): exact per-interface vtable slot ordering and AAPCS
        register-passing details — work this out per-interface against the
        BREW OEM API Reference as each one gets implemented (Phase 3+),
        not a blocking Phase 1 unknown anymore.
- [x] Add CPU core hook points for trapping on the call-out mechanism found
      above — `SetCallOutRange`/`SetCallOutHandler` on `IArmCore`; `Step()`
      checks the trap range before fetch/decode, `Run()` stops early on a
      trap. Covered by `Cpu.CallOutRangeTrapsInsteadOfExecuting` and
      `Cpu.RunStopsEarlyOnCallOutTrap`.
- [ ] Evaluate `dynarmic` integration spike (does it support the exact
      ARMv6/ARM1136 feature set needed, license fit, build complexity) —
      decide interpreter-only-for-now vs. JIT-from-the-start — not started;
      the v1 interpreter above is correctness-first per Design Principle 4,
      revisit once something is actually slow

## Phase 2 — Loader (GGZ / BAR / MIF / .mod)
Exit criterion: a real game's GGZ archive can be opened and its `.mod` code
mapped into memory with a valid entry point, ready to execute (even though
it will crash immediately without Phase 3).

**Exit criterion met**, and then some — verified with actual instruction
execution, not just "it loads": GGZ opens (89/89 + 74/74 real entries
extracted correctly), `.mod` maps into memory with a valid entry point and
genuinely executes real code correctly (23 real instructions, see below).
It won't get further than that without Phase 3 (BREW HLE), exactly as
expected. BAR is unconfirmed-needed for this title; MIF's full structure
remains deliberately deferred (see below) — neither blocks this phase's
actual goal.

- [x] **Clean-room GGZ archive reader — fully solved and verified.**
      `core/loader/ggz.{h,cpp}`. No public spec exists anywhere (checked:
      official docs, `ggzbrewtools`' README/wiki/issues — read for prose
      only, never its source, per PRD §6.3 LR2 — and general web search;
      all came up empty). Reverse-engineered from scratch by cross-testing
      hypotheses against the real Double Dragon `data.ggz`/`sound.ggz`
      (research-only, never committed) using a scratch Python script, then
      reimplemented independently in C++:
      - File opens with a table of N big-endian 8-byte entries:
        `(offset: u32, decompressed_size: u32)`.
      - The table's own byte length equals the *first* entry's offset
        value (i.e. N = that value / 8) — data starts exactly where the
        table ends.
      - Each entry points to a standard RFC 1952 gzip stream; the asset's
        original filename comes from the gzip header's `FNAME` field when
        present (confirmed: 100% of entries in both real files have it
        set), falling back to an ordinal name otherwise per
        `ggzbrewtools`' documented observation that this can happen.
      - **Verified two ways**: (1) synthetic unit tests
        (`tests/ggz_test.cpp`, 6 tests, self-generated non-copyrighted
        gzip archives), and (2) the real Double Dragon files via
        `zeebulator_ggz_inspector` — **89/89 entries in `data.ggz` and
        74/74 in `sound.ggz` extracted correctly**, byte-exact against an
        independent Python/zlib cross-check. High confidence, not a guess.
      - Contained asset formats (`.obm1` sprites/models, `.wav`/`.mid`
        audio) are NOT parsed yet — GGZ just gets you the named raw
        bytes; understanding `.obm1` internals is graphics-subsystem work
        for a later phase.
- [x] Dev tool: `tools/ggz_inspector.cpp` — lists and optionally extracts
      a GGZ archive's contents from a runtime-supplied path (never embeds
      game content into the repo).
- [x] BAR file parser — **solved.** Deferred pending "a title that
      actually has one" (Peggle does); revisited once the user made the
      full 61-title dump collection directly available, which also
      allowed ruling out Bejeweled Twist/Zuma's Revenge's own
      `resources.dat` as the same format (real, different header magic
      `PPCPRCON` — a real, distinct, likely-newer PopCap container,
      cross-referenced but not needed once Peggle's own file yielded to
      direct analysis).
      No public spec exists; real code opens `resources.bar` through
      the generic `ISHELL_LoadResDataEx` BREW API, not a custom `.mod`
      parser, so — unlike GGZ/`.pkg` — there's no real ARM code to
      trace, only the raw bytes. Reverse-engineered anyway: a 32-byte
      header (several fields self-consistently cross-checkable against
      each other and the real file size — offset table start/count,
      data-region start/size all agree), a first, still-unparsed 496-
      byte sub-table (60 real 8-byte records, purpose not needed for
      extraction), then the real payload: `entry_count` strictly-
      increasing `uint32` absolute file offsets plus one trailing
      sentinel equal to the total file size (giving each entry's size
      as the gap to the next). **Verified emphatically**: every one of
      308 real entries in Peggle's actual `resources.bar` lands exactly
      on a real, recognizable file signature with zero exceptions — 7
      real `ID3`-tagged MP3s, 39 real `RIFF`/WAVE files, 246 real PNGs
      (each preceded by a tiny, real `[uint16 header length][null-
      terminated "image/png"]` wrapper), and 16 more (mostly fixed
      32768-byte raw texture chunks, plus 4 trailing real, legible
      `^`-delimited localized UI strings) not yet further decoded but
      correctly bounded. No container-level compression at all —
      simpler to extract than GGZ or `.pkg`. Implemented as
      `core/loader/bar.{h,cpp}` (`BarArchive`), `tests/bar_test.cpp`
      (synthetic fixtures only), and `tools/zeebulator_bar_inspector`.
      282/282 tests pass (7 new). Decoding individual resource types
      (PNG, the raw texture format, the string-table records) is
      separate, not-yet-started follow-on work — this class is purely
      the container. See PHASE8_LOG.md for the full derivation.
- [x] MIF (Module Information File) — **string metadata extraction solved
      and shipped; full binary structure deliberately deferred (scope
      decision, not a dead end).** No byte-level spec exists publicly for
      either part (confirmed via web research).
      - **What's shipped**: `core/loader/mif.{h,cpp}` +
        `tools/mif_inspector.cpp`. MIF embeds human-readable app metadata
        as UTF-16LE strings, each prefixed with an `0xFFFE` BOM marker,
        ending at a null code unit or (since strings can sit back-to-back
        with no separator) the next BOM. Extracting these directly is
        enough for a game-library UI (name/publisher/version) without
        needing the surrounding binary format solved. Verified against
        all 11 available SDK sample `.mif` files (each yields its own
        description string, e.g. `"simple_atitc - Texture Compression"`)
        and the real Double Dragon `.mif` (yields `"DOUBLE DRAGON
        Zeebo"`, `"Brizo Interactive Corp."`, `"0.9.0"` — title,
        publisher, version). A strict printable-only filter correctly
        rejects a coincidental BOM-byte-pattern false-positive found
        inside binary (non-string) data elsewhere in the real file, and
        a string with a trailing non-printable code unit — conservative
        by design, since this is for UI display. 13 unit tests
        (`tests/mif_test.cpp`, synthetic buffers) + verified against all
        12 real files via `zeebulator_mif_inspector`.
      - **What's still open** (deferred — not needed for loading/running
        a specific game, only for full BREW-style app management, which
        this project doesn't need — see conversation record for the
        scoping reasoning): the resource table, class ID, and privilege
        bits. Partial header structure was reverse-engineered during this
        pass (not yet implemented in code, recorded here so it isn't
        lost): fixed 32-byte header — word0 = constant magic `0x00010011`;
        word1's low 16 bits are always 1, high 16 bits are a count
        (`count_A`); word2 = 32 (constant, doubles as the offset where a
        table starts); word3 = `count_A * 8` (that table's byte length,
        confirmed across all 4 files checked); word4 = word2 + word3
        (offset where that table ends); word5 = another count
        (`count_B`). What word6 onward means did NOT resolve cleanly
        (an initial guess that it pointed at the string table matched for
        one file and not others) — this is genuinely unresolved, not
        glossed over. A real, sourced, untested lead for interpreting the
        table entries themselves: `AEEShell.h`'s resource-ID offset scheme
        (`IDR_NAME_OFFSET=0, IDR_ICON_OFFSET=1, IDR_IMAGE_OFFSET=2,
        IDR_THUMB_OFFSET=3, IDR_SETTINGS_OFFSET=4, IDR_VERSION_OFFSET=5,
        IDR_ENVIRONMENT_OFFSET=6, IDR_OFFSET_STEP=20` — note `20` and `26`
        showed up in exactly this relationship in one file's table
        records, which may not be coincidence). Also noted: at least one
        known BREW/Zeebo title's MIF is reportedly encrypted (per a
        Tuxality devlog), so not every MIF may be plain-structured at
        all. If a real need for the class ID emerges during `.mod`
        loading, check first whether it's cheaper to get it another way
        (e.g. Double Dragon's `.mif` is literally named `274754.mif` —
        that number is a plausible class ID without parsing anything)
        before resuming this.
- [x] `.mod` loader — **solved and verified against real code execution,
      not just static analysis.** Research suggested "self-relocatable"
      (implying a relocation table); that turned out to be misleading —
      the real answer is simpler. `core/loader/mod.{h,cpp}`:
      - **Finding**: `.mod` is a flat, headerless, position-independent
        ARM binary. Raw code+data starts at file offset 0. No header, no
        relocation table, loadable at any base address.
      - **How this was verified** (`tools/mod_probe.cpp`, a dev tool that
        loads a raw `.mod` at a chosen address and single-steps the real
        interpreter through it, printing each instruction): loaded the
        real Double Dragon `ddragonz.mod` at an arbitrary address
        (`0x00100000`, chosen for no particular reason — the whole point
        was to confirm the code doesn't care) and executed it. Result:
        **23 real instructions executed correctly**, decoding as
        completely coherent, idiomatic compiler-generated ARM: a function
        prologue (`STR LR,[SP,#-4]!` / `SUB SP,SP,#12`), sensible
        register shuffling, a `BL` that correctly landed on a *second*
        function's prologue (`STMFD SP!, {r3-r9,lr}`) at a totally
        different file offset, that function's body and matching epilogue
        (`LDMFD SP!, {r3-r9,lr}` popping exactly what was pushed), and two
        correct `BX LR` returns — all with register values propagating
        exactly as expected at every step. This is about as strong a
        real-execution confirmation as is possible without the actual
        BREW OS driving it. (Execution eventually "returns" to address 0
        and starts executing harmless zeroed-memory no-ops — expected and
        correct, not a bug: our probe doesn't yet set up the real initial
        LR/SP the OS would provide before calling `AEEMod_Load`, which is
        Phase 3 work, not a `.mod`-format problem.)
      - **Direct payoff for Phase 1**: probing this real code is what
        motivated (and validated) implementing LDM/STM and BX in the CPU
        interpreter just now — both were hit almost immediately by real
        compiled code, confirming they were exactly the right next
        instructions to add.
      - `elf2mod.x`/relocation-table research from before this session is
        now believed to be a red herring for the common case (plain
        position-independent code) — kept in git history rather than
        deleted from here, in case some other title's `.mod` does need
        real relocation (not every game need be compiled identically).
      - `LoadMod()` is intentionally thin (load bytes, set PC) since
        there's no header to parse. 3 unit tests (`tests/mod_test.cpp`),
        including one that specifically verifies the *same* image produces
        identical behavior at two different base addresses — the actual
        position-independence property, not just "it loads."
      - The `.sig` file alongside each `.mod` (e.g. `ddragonz.sig`) is
        almost certainly a cryptographic signature for real-device code
        verification — irrelevant to an HLE emulator with no real
        security boundary to enforce, so deliberately ignored.
- [x] Test fixtures: **deviated from the original plan, in a good way.**
      SDK sample apps don't include any `.ggz` files at all (only Double
      Dragon's commercial dump does), so GGZ and MIF tests both use
      self-generated synthetic fixtures instead (`tests/ggz_test.cpp`,
      `tests/mif_test.cpp`) — real-file correctness for both was
      additionally confirmed via `zeebulator_ggz_inspector` /
      `zeebulator_mif_inspector` against the local, git-ignored real
      files (Double Dragon's dump + all 11 SDK sample `.mif`s), giving
      two independent layers of verification without committing any
      copyrighted content. Same pattern should carry forward to `.mod`.

## Phase 3 — Minimal BREW HLE (bring-up target: blank painted screen)
Exit criterion: **M0 from PRD §7** — a BREW "hello world" sample app boots
via the standalone frontend and reaches a visible, correctly-painted screen.

**Phase 3 is complete — M0 achieved and visually confirmed**, not just
proven via a test double. Screenshot-verified: a real SDL2 window titled
"Zeebulator" opens, shows a black 640x480 canvas (Zeebo's native
resolution) with a white block visibly drawn where `hello_brew`'s
`HandleEvent` called `IDISPLAY_DrawText`/`IDISPLAY_Update` through the
real vtable.

- [x] Stand up Backend Abstraction Interface (ARCHITECTURE.md §3.8) —
      done back in Phase 0 (`core/backend.h`): `PushVideoFrame`,
      `PushAudioSamples`, `PollInput`.
- [x] Minimal standalone SDL2 frontend implementing that interface
      (`frontends/standalone/main.cpp`) — window + framebuffer blit via a
      streaming `SDL_Texture` (RGB565, matching `IDisplayHle`'s
      framebuffer format directly, no conversion needed); no audio/input
      yet (`Sdl2Backend::PushAudioSamples`/`PollInput` are no-ops for
      now). SDL2 vendored via CMake `FetchContent` (`libsdl-org/SDL`,
      `release-2.30.9`) consistent with the project's existing
      zlib/GoogleTest pattern, built as a static lib
      (`SDL2::SDL2`/`SDL2::SDL2main`). On Linux this needed X11 dev
      headers installed on the build machine (`libx11-dev` and friends —
      build-time only, never needed by end users or by anyone just
      running a released binary).
      Currently boots the bundled `hello_brew` M0 demo specifically
      (hardcoded fixture path with an optional CLI override) — loading
      arbitrary real games needs the `.mod` entry-point-discovery and
      GGZ/MIF wiring that's later-phase work; this frontend exists right
      now to prove the pipeline, not as the final game loader.
- [x] Implement `IShell` HLE (`core/brew/ishell.cpp`) — vtable slot order
      verified directly against real Qualcomm source (`AEEIShell.h`, see
      below). All 42 pre-BREW-MP slots present; every slot is currently a
      safe stub (nothing in scope calls any IShell method yet — the test
      app gets `IDisplay` directly via the `EVT_APP_START` event, not
      through IShell). Extend individual slots with real behavior as
      games need them.
- [x] Implement `IDisplay` HLE (`core/brew/idisplay.cpp`) — vtable slot
      order verified directly against real `AEEIDisplay.h`. Real behavior
      for `DrawText` (draws a placeholder block sized from the real
      x/y/length arguments — no font rasterizer yet, that's later
      graphics-phase work) and `Update` (pushes the framebuffer to
      `Backend::PushVideoFrame`); the other 11 slots are stubs.
- [x] Wire CPU core call-out traps (Phase 1) to the `IShell`/`IDisplay` HLE
      dispatch — `core/brew/hle_runtime.{h,cpp}` (`HleRuntime`). Also
      provides the reverse direction, `CallArmFunction()`: calling INTO
      the app's own compiled code and running until it returns, which
      turned out to be necessary (see below) — not originally scoped as
      part of "wire call-out traps" but required to actually drive an
      app's lifecycle rather than just service its calls.
- [x] **Real Qualcomm vtable ABI, verified from actual source, not
      guessed.** No public byte-level IShell/IDisplay spec exists (same
      story as every other format in this project), but unlike
      MIF/`.mod`, the *header declarations* (not implementations) for
      these interfaces turned out to be findable: archive.org hosts full
      original BREW SDK installers (`brew_1.1_sdk` from ~2001,
      `bmp-sdkmp-7.12.5` from 2012). Extracted both (outside the repo,
      git-ignored, `research/docs/sdk_installer_extract/
      brew_sdk_headers_reference/`) and independently read the real
      `AEEIShell.h`/`AEEIDisplay.h` vtable macros directly (not just
      trusted a research agent's summary — first verification pass
      actually returned zero matches due to the files' non-standard
      ASCII encoding silently breaking `grep`'s locale handling; re-ran
      with `LC_ALL=C` and confirmed for real). Slot order is identical
      across both SDK versions (BREW's ABI policy is strictly
      append-only), which is why the pre-BREW-MP subset is trustworthy
      for a 2009-era Zeebo target even though only 2001 and 2012 were
      directly checked.
- [x] **Full app lifecycle actually driven end to end, against real
      compiled ARM code** (`tests/brew_lifecycle_test.cpp`,
      `tests/fixtures/hello_brew/`). Wrote our own minimal BREW-shaped
      test app in C (not Qualcomm code — see the file's header comment),
      structurally faithful to the real `AEEMod_Load` ->
      `IModule::CreateInstance` -> `HandleEvent(EVT_APP_START)` contract
      reverse-engineered from the official `AEEModGen.c`/`AEEAppGen.c`
      reference sources, compiled with `arm-none-eabi-gcc` (real
      cross-compiler, not `elf2mod.exe`) targeting the same
      `-march=armv5te -mthumb-interwork` flags Zeebo used. Result,
      verified pixel-exact: our loader calls the compiled `AEEMod_Load`,
      reads back the returned `IModule*`, calls its `CreateInstance`
      through the vtable, gets back a `HandleEvent` function pointer,
      builds a real `AEEAppStart` struct in emulated memory, calls
      `HandleEvent(EVT_APP_START, ...)` — and the app's own compiled code
      correctly calls `IDISPLAY_DrawText` then `IDISPLAY_Update` through
      the vtable we built, landing exactly the expected pixels in the
      framebuffer.
  - Compiling this test app surfaced two genuinely necessary CPU core
    additions (not speculative — both were hit immediately by real
    compiled code): **BLX** (register-form branch-and-exchange-with-link
    — real vtable calls use this, not a bare `BX`) and **halfword/signed
    transfer** (`STRH`/`LDRH`/`LDRSB`/`LDRSH` — real compiled code uses
    these for 16-bit locals). Both implemented with real tests, not just
    added to make the integration test pass.
  - Compiling it also caught a **real dispatcher bug**: the "opcode
    0x8-0xB with S=0 is reassigned to the miscellaneous instruction space
    (MRS/MSR/BX/...)" rule was being checked *before* the
    multiply/halfword bit-pattern check, but bits[24:21] only mean
    "opcode" for true data-processing-shaped instructions — `STRH`'s
    P/U/I/W encoding bits happen to numerically collide with that opcode
    range, so it was being wrongly routed to "unimplemented misc
    instruction" instead of halfword transfer. Fixed by checking the
    multiply/halfword bit pattern first; would not have been caught
    without testing against real compiled code, since none of the
    hand-encoded unit tests happened to exercise that exact collision.
- [x] **Milestone M0 checkpoint: achieved, screenshot-verified.** Ran
      `zeebulator_standalone`, captured the actual window with
      `gnome-screenshot`, and visually confirmed a black 640x480 canvas
      with a white block drawn at the expected position — the smallest
      possible end-to-end validation of the whole pipeline (CPU
      interpreter -> HLE dispatch -> real compiled ARM code -> vtable
      calls -> framebuffer -> SDL2 window), and it holds up.

## Phase 4 — File System & Asset Access
Exit criterion: a game can enumerate and read its own bundled assets
through HLE `IFile` calls, sourced from the loaded GGZ contents.

**Exit criterion met.** A game can open, read (with correct partial-read
and independent-multi-handle semantics), seek, get info on, and enumerate
its own GGZ-backed assets through real, vtable-order-verified `IFile`/
`IFileMgr` HLE calls.

- [x] Implement `IFile`/`IFileMgr` HLE backed by an in-memory virtual
      filesystem populated from the loaded GGZ archive (never expose the
      real host filesystem directly — ARCHITECTURE.md §3.4).
      `core/brew/virtual_filesystem.{h,cpp}` (`VirtualFilesystem`, flat
      name -> bytes map, eager decompression via `GgzArchive::Extract()`
      — already real-file-verified in Phase 2, so no redundant real-data
      check was needed here) + `core/brew/file_hle.{h,cpp}` (`FileHle`,
      the actual HLE dispatch).
      **Real Qualcomm vtable ABI, verified from actual source, same
      method as IShell/IDisplay** (archive.org-hosted original BREW SDK
      installers — see Phase 3 for how that source was first found).
      This round needed a fresh background research pass (the specific
      headers weren't part of what got extracted for Phase 3), which hit
      a genuine large-file download (892MB BREW MP 7.12.5 installer) that
      looked stalled at first glance — checked the actual transcript
      instead of trusting a vague status update, confirmed real transfer
      progress (~1.7MB/s, growing) via a timed size-delta check, and let
      it finish properly rather than assuming failure. Independently
      re-verified the resulting header content myself afterward (same
      `LC_ALL=C` lesson from Phase 3 — plain `grep` silently misses
      content in these ISO-8859/non-UTF8-encoded files).
      Both `IFile` and `IFileMgr` are defined together in a single
      `AEEFile.h` in both SDK versions (no separate `AEEFileMgr.h`
      exists) — confirmed vtable orders:
      - `IFile` (inherits `IAStream`): `AddRef, Release, Readable, Read,
        Cancel`, then `Write, GetInfo, Seek, Truncate` (BREW MP adds
        `GetInfoEx, SetCacheSize, Map` — not implemented, post-dates
        Zeebo).
      - `IFileMgr`: `AddRef, Release, OpenFile, GetInfo, Remove, MkDir,
        RmDir, Test, GetFreeSpace, GetLastError, EnumInit, EnumNext,
        Rename` (BREW MP adds 8 more trailing slots — not implemented).
      - Confirmed append-only/stable across both 2001 and 2012 SDK
        versions, same pattern as IShell/IDisplay.
      Files are read-only (`Write`/`Remove`/`MkDir`/`RmDir`/`Truncate`/
      `Rename` all return an error rather than silently succeeding or
      doing nothing) since GGZ contents aren't meant to be mutated. One
      real design point worth noting: `IFile` instances share a single
      vtable (built once) but each `OpenFile` call allocates a fresh
      object header at a bump-allocated address, with per-handle state
      (current read position, which file) looked up by that address at
      dispatch time — this is what makes multiple simultaneously-open
      files with independent read positions work correctly, verified by
      a dedicated test.
      17 new tests (`tests/virtual_filesystem_test.cpp`,
      `tests/file_hle_test.cpp`), all synthetic (no copyrighted content).
- [x] Handle whatever asset sub-formats appear inside GGZ containers for the
      target test game (models, sprites, etc. — format specifics are a
      per-content research task, informed by `ggzbrewtools`' documented
      coverage) — was deferred, not blocking this phase's exit criterion;
      `.obm1` (every one of Double Dragon's 89 real sprite/texture assets)
      cracked in Phase 8 once real further progress needed it — see
      `core/loader/obm1.h` and PHASE8_LOG.md. `IFile`/`IFileMgr` themselves
      needed no changes: they already hand back raw bytes correctly
      regardless of what those bytes mean internally.

## Phase 5 — Graphics (OpenGL ES 1.0/1.1 translation)
Exit criterion: the target test game's 3D/2D rendering appears on screen,
even if visually imperfect.

**Every task below is done, but the phase's literal exit criterion
(Double Dragon's own rendering on screen) is intentionally not yet
attempted — that's Phase 8's job** ("iteratively debug against the real
game, filling HLE API gaps as they're hit"), same relationship Phase
3/4's HLE work had to M1. What's actually proven here: the full `IGL`/
`IEGL` HLE surface needed for basic GLES1.x rendering exists, is real
vtable-order-verified, is forwarded to a real host OpenGL context, and
has been validated end-to-end against real compiled ARM code (our own
clean-room fixture, not Qualcomm's) producing a real, correctly-rendered,
color-interpolated triangle — not just unit tests driving the dispatch
logic directly. Known gaps deliberately left for when a real game
actually needs them: texture-combiner/lighting state, `glDrawElements`
real host-side index buffers (currently de-indexed, see below), and
compressed textures. (The CPU's multiply instruction, flagged here as
the more likely immediate blocker, was closed out in Phase 1 after
Phase 6 — see that phase's writeup.)

- [x] **Research: how BREW-era GLES actually reaches the OS — real
      architecture found, materially simpler than originally assumed.**
      Real Qualcomm sample `.mak` build rules show `EGL_1x.c`/`GLES_1x.c`/
      `GLES_ext.c` (BREW SDK-provided wrapper source) get statically
      compiled into *every game's own `.mod`* — GLES is not an OS service
      reached via `IShell`. Those wrappers dispatch `gl*`/`egl*` calls
      through two real AEE interfaces, **`IGL`** and **`IEGL`**, reached via
      global pointers (`gpIGL`/`gpIEGL`) set up once at startup. Found and
      read the real `AEEGL.h` (extracted from a genuine Qualcomm "OpenGL ES
      Extension for BREW SDK 4.x" installer already present in
      `research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/` —
      MSI → CAB → source, same clean-room method as every prior interface:
      read the real header for the ABI shape only, never copy/commit it).
      **Verified vtable slot order**: `AddRef, Release, QueryInterface`
      (confirmed via `INHERIT_IQueryInterface`'s companion access-macro
      block, same file, same order — same cross-check method used for
      every other interface), then 77 `gl*` methods for `IGL` (in
      declaration order, `glActiveTexture` → `glViewport`; 80 slots
      total), then 25 `egl*` methods for `IEGL` (`eglGetError` →
      `eglCopyBuffers`; 28 slots total) — counted programmatically
      against the real header, not eyeballed.
      Confirmed this applies to the actual target game too: Double
      Dragon's real `ddragonz.mod` contains the strings
      `eglGetColorBufferQUALCOMM` and `OpenGL.cpp`, consistent with the
      same statically-linked-wrapper pattern.
      **Consequence**: no need to design/ship our own GLES1.1
      fixed-function state machine (contra the original plan below) — we
      build `IGL`/`IEGL` HLE objects the same way as `IShell`/`IDisplay`/
      `IFile` (real vtable order, CPU call-out traps) and forward each
      implemented slot to a real host OpenGL context; the host GL driver
      does the actual fixed-function math. New wrinkle vs. prior
      interfaces: pointer args (`glVertexPointer`, `glTexImage2D`, index
      buffers) are emulated ARM addresses and must be copied into real
      host buffers at draw/upload time, not forwarded directly.
- [x] Build the full `IGL`/`IEGL` HLE vtable scaffold (all 108 slots — 80
      for `IGL`, 28 for `IEGL` — present at their real offsets,
      stub-by-default — same pattern as `IShell`). `core/brew/gl_hle.{h,cpp}`.
      One correctness wrinkle specific to this interface, not present in
      any prior one: real `IGL`/`IEGL` vtable slots do **not** receive the
      interface object pointer as an implicit first argument (confirmed
      from the real access macros, e.g. `#define IGL_glClear(p,a)
      AEEGETPVTBL(p,IGL)->glClear(a)` — only `a` is forwarded) — so R0
      holds the first *declared* argument for every `gl*`/`egl*` slot,
      unlike `IShell`/`IDisplay`/`IFile` where R0 is always `po`. Only the
      inherited `AddRef`/`Release`/`QueryInterface` slots keep the usual
      po-in-R0 convention.
- [x] Implement the EGL lifecycle slice (`eglGetError`, `eglGetDisplay`,
      `eglInitialize`, `eglTerminate`, `eglChooseConfig`,
      `eglCreateWindowSurface`, `eglDestroySurface`, `eglCreateContext`,
      `eglDestroyContext`, `eglMakeCurrent`, `eglSwapBuffers`) — matches
      the exact call sequence real sample app source uses
      (`simple_drawtexture.c`'s `InitGLSurface`/`FreeGLSurface`).
      `EGLDisplay`/`EGLSurface`/`EGLContext`/`EGLConfig` are simulated as
      small sentinel handles (`GlHle` never talks to a real host EGL
      implementation); `eglMakeCurrent` triggers exactly one real
      `GlBackend::CreateContext()` call, verified by a dedicated test
      (`EglMakeCurrentOnlyCreatesHostContextOnce`).
- [x] Implement a first core-GL slice (`glClear`, `glClearColorx`,
      `glViewport`, `glEnable`/`glDisable`, `glMatrixMode`,
      `glLoadIdentity`, `glOrthox`, `glFrustumx`, `glTranslatex`/
      `glRotatex`/`glScalex`, `glColor4x`) with `GLfixed`->float
      marshaling (16.16 fixed point, verified against hand-computed
      fixed-point values in tests, not just round-tripped), forwarded
      through a `GlBackend` seam (`core/brew/gl_backend.h`) — real host GL
      in the frontend (not wired up yet, see below), a recording
      `FakeGlBackend` in tests, same seam pattern as `Backend`.
      13 new tests (`tests/gl_hle_test.cpp`), including one that walks the
      full real EGL call sequence end to end and one that asserts every
      one of the 108 real vtable slots is present and non-null (not just
      the dozen with real behavior).
- [x] Implement vertex-array/draw-call marshaling (`glVertexPointer`/
      `glColorPointer`/`glTexCoordPointer`/`glNormalPointer`,
      `glEnableClientState`/`glDisableClientState`, `glDrawArrays`/
      `glDrawElements`) — the emulated-memory → host-buffer copy step.
      Array-pointer calls just record (pointer, type, size, stride) —
      emulated memory is only actually walked at draw-call time, once per
      enabled array, converting every component to a host float per the
      real GLES1.x per-type semantics (`GL_BYTE`/`GL_SHORT` cast directly,
      `GL_FIXED` via the existing 16.16 conversion, `GL_FLOAT`
      bit-reinterpreted, `GL_UNSIGNED_BYTE` additionally normalized by
      /255 for color arrays only, matching real `glColorPointer`
      semantics). `glDrawElements` shares the exact same extraction code
      as `glDrawArrays`, just keyed by an explicit index list decoded
      from emulated memory per its `GL_UNSIGNED_BYTE`/`GL_UNSIGNED_SHORT`
      type — output is equivalent, not indexed rendering on the host side
      (a documented simplification, not an oversight). `GlBackend` gained
      one new method, `DrawArrays(mode, GlVertexArrays)`, taking an
      already-fully-host-native struct — no `GLfixed` or emulated pointers
      ever reach a `GlBackend` implementation.
      6 new tests (`tests/gl_hle_test.cpp`): mixed `GL_BYTE`/
      `GL_UNSIGNED_BYTE` vertex+color gather with correct /255
      normalization, non-zero stride handling, `glDrawElements` with both
      `GL_UNSIGNED_BYTE` and `GL_UNSIGNED_SHORT` index types (including a
      repeated index, proving real gather-not-range semantics), the
      normal array's fixed 3-component shape (it has no `size` argument
      in the real API), and `glDisableClientState` actually excluding an
      array from the next draw. All 90 project tests green.
- [x] Implement texture upload (`glGenTextures`/`glDeleteTextures`/
      `glBindTexture`/`glTexParameterx`/`glTexImage2D`) with the same
      memory-copy approach. `glGenTextures`/`glDeleteTextures` round-trip
      `GLuint` names through emulated memory but let `GlBackend` own the
      actual namespace (`GenTextures(n, GLuint*)` fills a host-owned
      array, which `GlHle` then writes back into the app's buffer) — no
      separate ID-remapping table needed. `glTexImage2D` computes the
      real upload size from `(format, type, width, height)` — handling
      both the common `GL_UNSIGNED_BYTE` case (1 byte/component) and the
      packed 16-bit types (`GL_UNSIGNED_SHORT_5_6_5`/`_4_4_4_4`/`_5_5_5_1`,
      always exactly 2 bytes/pixel regardless of format) — then copies
      exactly that many bytes out of emulated memory into a host buffer
      before handing `GlBackend::TexImage2D` an already-resolved
      `GlTextureImage`; a null `pixels` argument (the real, legal "reserve
      storage now, upload later via glTexSubImage2D" call shape) is
      preserved as null rather than treated as an error.
      One correctness detail caught while reading the real spec, not
      assumed: `glTexParameterx`'s `param` is **not** GLfixed-converted
      even though it arrives through the same "x"-suffixed fixed-point
      slot as `glTranslatex`/`glColor4x` — every standard GLES1.x
      `glTexParameterx` `pname` (`MIN`/`MAG_FILTER`, `WRAP_S`/`WRAP_T`) is
      enum-valued, and the real spec's convention for enum-valued
      parameters passed through an "x" function is to forward the raw
      enum integer unconverted, not scale it by 65536 — verified with a
      dedicated test asserting `GL_LINEAR` (`0x2601`) arrives at
      `GlBackend` as exactly `0x2601`, not `0x2601/65536`.
      7 new tests (`tests/gl_hle_test.cpp`): ID round-tripping for
      `glGenTextures`/`glDeleteTextures`, raw passthrough for
      `glBindTexture`/`glTexParameterx`, `GL_RGBA`+`GL_UNSIGNED_BYTE`
      pixel-byte-exact upload, the null-pixels reserve-storage path, and
      packed `GL_UNSIGNED_SHORT_5_6_5` sizing (2 bytes/pixel regardless of
      `GL_RGB`'s 3-component format). All 97 project tests green.
- [x] Wire a real host OpenGL context into the SDL2 standalone frontend
      (context creation, `eglSwapBuffers` → actual buffer swap).
      `frontends/standalone/sdl2_gl_backend.{h,cpp}` (`Sdl2GlBackend`) is a
      thin, direct forward of every `GlBackend` method to the platform's
      real OpenGL 1.1 fixed-function entry points — no emulation-specific
      logic in this file at all, since `GlHle` already did every
      `GLfixed`/emulated-memory marshaling step before a call reaches
      here. Uses `find_package(OpenGL REQUIRED)` (`OpenGL::GL`), the
      standard cross-platform CMake mechanism — ships with the toolchain
      on Windows/macOS, needed `libgl1-mesa-dev`/`libglu1-mesa-dev` as a
      one-time local dev-machine install on Linux (same category of
      build-time-only dependency as Phase 3's X11 headers; I don't have
      sudo in this environment, so the user ran the install).
      **Verified with a real, screenshot-confirmed second window**, not
      just "it compiled": `main.cpp` opens a second `SDL_WINDOW_OPENGL`
      window alongside the existing `hello_brew` one, builds `IGL`/`IEGL`
      HLE objects backed by `Sdl2GlBackend`, and drives the exact real EGL
      call sequence (`eglGetDisplay` → `eglInitialize` →
      `eglChooseConfig` → `eglCreateWindowSurface` → `eglCreateContext` →
      `eglMakeCurrent` → `glClearColorx`(teal) → `glClear` →
      `eglSwapBuffers`) through `HleRuntime::CallArmFunction` — the same
      real vtable-dispatch path compiled ARM code would use, not a
      shortcut. Ran the built standalone frontend, used `gnome-screenshot`
      to independently capture both windows: the new GL window shows a
      real, solid teal fill (the exact color driven through the vtable
      chain, confirming real host OpenGL rendering actually happened end
      to end), and the original `hello_brew` window still shows its
      correct black-canvas-plus-white-block output, unaffected by the new
      wiring. One bug caught and fixed while wiring this up, before ever
      running it: the `eglChooseConfig` call's `num_config` output pointer
      is a stack-passed (5th) argument, and the first version of this code
      never set `SP` before calling it — would have read/written through
      whatever garbage address happened to be left over in memory.
- [x] **Validate against a real compiled GLES-exercising app — done via a
      clean-room fixture, not Qualcomm's actual sample source (same
      policy as `hello_brew.c`).** Real Qualcomm OGLES sample source
      (`simple_drawtexture.c` etc.) needs the real `EGL_1x.c`/`GLES_1x.c`
      wrapper compiled alongside it, which is real Qualcomm source we
      keep research-only/uncommitted — and more importantly, real samples
      pull in floating-point math and other complexity likely to hit the
      still-unimplemented CPU multiply instruction immediately. Instead,
      wrote `tests/fixtures/hello_gl/hello_gl.c`: our own minimal app,
      structurally identical to `hello_brew.c`'s `AEEMod_Load` ->
      `CreateInstance` -> `HandleEvent` lifecycle, but dispatching through
      the real, verified `IGL`/`IEGL` vtable slot order directly (no
      floating point anywhere — all fixed-point values are compile-time
      integer constants — so nothing here depends on soft-float library
      routines the interpreter doesn't support). It drives the full real
      EGL lifecycle, sets up an orthographic projection, and draws a
      single hardcoded red/green/blue triangle via
      `glVertexPointer`/`glColorPointer`/`glDrawArrays`. Compiled with
      the same `arm-none-eabi-gcc` toolchain as `hello_brew.c`; objdump
      confirms every vtable call compiles to a real `blx r3`/`blx r4`
      (register-form BLX, already implemented) with no `mul`/`mla`
      anywhere in the function.
      **New integration test** (`tests/gl_lifecycle_test.cpp`,
      `GlLifecycle.HelloGlAppDrawsRealTriangleThroughRealVtableDispatch`):
      loads the real compiled `.bin`, drives it through the real
      lifecycle exactly like `brew_lifecycle_test.cpp` does, and asserts
      on every stage a `RecordingGlBackend` observed — matrix
      mode/ortho/viewport values, the clear color/mask, and the exact
      triangle vertex positions and normalized colors gathered by real
      compiled ARM code from real emulated memory. Passed on the first
      run. 98/98 project tests green.
      **Also wired into the standalone frontend and screenshot-verified
      with real host OpenGL** (not just the recording fake): the second
      window now loads and runs `hello_gl.bin` for real instead of this
      file hand-driving the EGL/GL calls, and rasterizes a real,
      correctly color-interpolated RGB triangle via `Sdl2GlBackend`.
      **Found and fixed a genuine, previously-latent bug in the process,
      not just in the new code**: `hello_gl.bin` (and, confirmed via the
      same objdump check, `hello_brew.bin` too, unnoticed until now)
      isn't actually position-independent the way Phase 2 established
      `.mod`s should be — `arm-none-eabi-gcc` without `-fPIC`/ROPI flags
      bakes `&g_module`'s *absolute* link-time address into a literal
      pool (`ldr r2, [pc, #20]` loading a fixed `.word`) rather than
      computing it PC-relative. This was invisible before because every
      existing fixture/test always happened to load its `.mod` at
      exactly the address it was linked for (`0x00100000`) — this was
      the first thing in the project to attempt loading a second,
      independent `.mod` (at a different address, in the same memory
      space as `hello_brew`), which surfaced it immediately as a `0x0`
      function pointer three calls deep. A real BREW-compiled `.mod`
      (RVCT `armcc --apcs /ropi`) is genuinely position-independent and
      wouldn't have this problem — it's specific to our own gcc-built
      test-fixture convention, not a real ABI gap — so the fix was to
      give the GL demo its own independent `ArmInterpreter`/`HleRuntime`
      (a completely separate address space) rather than trying to make
      the fixture build truly position-independent, which is not
      currently needed anywhere else. Worth remembering if a future
      fixture ever needs genuine load-address independence.

## Phase 6 — Audio
Exit criterion: target test game's audio plays back correctly.

**Exit criterion met** (see the Phase 8 sound investigation's "Sound,
round twelve" in PHASE8_LOG.md / TASKS.md's own Phase 8 section):
real, audible Double Dragon gameplay music and SFX confirmed live.
The remaining gap (MIDI music rendered as plain sine tones, no real
instrument timbre) is a separate, scoped synthesis-quality follow-up,
not a blocker to this phase's own criterion.

**Both codecs the real target game actually needs (PCM, MIDI) are done
and verified against real Double Dragon assets; IMA-ADPCM/MP3 are
deliberately deferred (see research below) — same relationship Phase
5's graphics work had to the actual game's rendering.** Not yet done:
driving any of this from real compiled ARM code (everything so far is
verified via direct HLE calls and unit tests, mirroring Phase 5's early
increments before `hello_gl.bin` existed) — a natural next step,
not attempted yet.

- [x] **Research: real `IMedia` interface + real target-game codec need —
      see ARCHITECTURE.md §3.6 for full detail.** Found and read the real
      `AEEMedia.h`/`AEEIMedia.h` (extracted from the same genuine BREW MP
      SDK installer used in Phase 4/5 — this time via a full NSIS
      extraction rather than a hand-picked file list, since the earlier
      partial extraction hadn't pulled the media headers). Confirmed a
      small 14-slot vtable, mostly a generic `SetMediaParm`/
      `GetMediaParm` command interface rather than one slot per feature.
      Also inspected Double Dragon's real `sound.ggz` directly (via
      `zeebulator_ggz_inspector`, already built and real-file-verified in
      Phase 2): 62 `.wav` (effects/voice) + 12 `.mid` (music), zero MP3.
      Checked a real extracted `.wav`'s `fmt ` chunk: plain 16-bit PCM,
      not IMA-ADPCM, on every file sampled. This directly reprioritizes
      the rest of this phase — PCM and MIDI are what M1 actually needs;
      ADPCM/MP3 are real BREW codecs (confirmed via the SDK's own
      `ctsoundmgr.c` sample, which genuinely uses ADPCM-named assets) but
      not required by the target title.
- [x] PCM decode/playback path (real target-game need, confirmed above —
      RIFF/WAVE container parsing; the audio data itself is already raw
      PCM, no real "decoding" math needed). `core/loader/wav.{h,cpp}`
      (`ParseWav`): handles 8-bit unsigned and 16-bit signed PCM,
      mono/stereo, chunk word-alignment padding (a real `bext` chunk in
      Double Dragon's own files exercises exactly this). Explicitly
      rejects non-PCM format tags (e.g. IMA-ADPCM) rather than
      mis-decoding them — same "reject, don't guess" philosophy as the
      CPU interpreter's `UnimplementedInstruction`. Cross-verified two
      ways: 7 synthetic-fixture unit tests (`tests/wav_test.cpp`), and a
      throwaway harness run against a real extracted Double Dragon `.wav`
      (13,411 real, correctly-shaped waveform samples at 22050Hz mono —
      not committed, matches every other real-file verification pass in
      this project).
- [x] Mixer + ring buffer feeding the Backend Abstraction Interface —
      `Backend::PushAudioSamples` has existed since Phase 0 and is
      finally used starting this phase. `core/audio/mixer.{h,cpp}`
      (`Mixer`): sample-accurate multi-voice mixing (mono duplicated to
      stereo, overlapping voices summed and clamped rather than wrapped
      on overflow, looping vs. one-shot voices, pause/resume preserving
      position), pushed to `Backend::PushAudioSamples` — which gained a
      `sample_rate` parameter as part of this work (previously
      undocumented and uncalled by anything). Deliberately does not
      resample — every real Double Dragon audio asset is uniformly
      22050Hz (confirmed in the research above), so a fixed-rate mixer is
      correct for the actual target game; a documented, revisit-if-needed
      simplification, not an oversight. 9 unit tests
      (`tests/mixer_test.cpp`).
- [x] Implement `IMedia` HLE (14-slot vtable, real order verified above)
      wired to the PCM path — `core/brew/media_hle.{h,cpp}`
      (`MediaHle`). `SetMediaParm(MM_PARM_MEDIA_DATA, ...)` assigns a
      virtual-filesystem-backed file (reusing Phase 4's GGZ-backed
      `VirtualFilesystem`) and decodes it immediately via `ParseWav`,
      matching real `IMedia`'s documented "SetMediaData puts IMedia in
      Ready state" behavior; `Play`/`Stop`/`Pause`/`Resume` drive a
      `Mixer` voice and update state accordingly; `GetState` also
      notices when a non-looping voice finished naturally since the last
      check. `MM_PARM_PLAY_REPEAT` maps repeat-forever (0) to the
      Mixer's boolean loop (exact repeat counts > 1 aren't tracked yet).
      `RegisterNotify` stores the callback but doesn't fire it yet —
      firing it on real transitions needs a periodic "tick" hook this
      project doesn't have wired into a real run loop yet (Phase 7/8
      territory, not a fundamental gap). Follows the same shared-vtable
      pattern as `FileHle` (many `IMedia` instances, one vtable).
      10 unit tests (`tests/media_hle_test.cpp`).
      **Wired into the standalone frontend with real SDL2 audio output,
      verified with unambiguous, concrete proof, not just "it compiled"**:
      split `Sdl2Backend` out to its own file
      (`frontends/standalone/sdl2_backend.{h,cpp}`) and gave it a real
      `SDL_AudioDevice` (`SDL_QueueAudio`-based); main.cpp builds a real
      `IMedia` object over a small self-synthesized 440Hz test tone
      (`tests/fixtures/test_tone.wav`, our own content, not a real game
      asset) and drives `SetMediaParm`/`Play` on it, with the event loop
      calling `Mixer::Mix` once per ~16ms tick. Needed
      `libasound2-dev`/`libpulse-dev` as a one-time local dev-machine
      install (same category of build-time-only dependency as Phase 3's
      X11 headers and Phase 5's OpenGL headers — SDL2 silently compiled
      out ALSA/PulseAudio/PipeWire/JACK support entirely despite the
      CMake cache saying "Wanted: ON" for all of them, because the actual
      dev headers weren't present at `FetchContent` build time; a real
      PulseAudio server was running the whole time, the build just
      couldn't link against it). After installing the headers and doing
      a full clean SDL2 rebuild: ran the standalone frontend and checked
      `pactl list sink-inputs` — found a real, live sink input
      (`application.process.binary = "zeebulator_standalone"`,
      matching PID, `Sample Specification: s16le 2ch 22050Hz`,
      `Corked: no`) — concrete, external, OS-level proof that real audio
      is actually flowing out through PulseAudio/PipeWire, the audio
      equivalent of Phase 3/5's screenshot verification. 124/124 project
      tests green throughout.
- [x] MIDI playback (real target-game need, confirmed above — genuine
      Standard MIDI Format 0 files, confirmed by parsing the real header
      bytes directly, not assumed; needed a real SMF parser + a simple
      synthesizer, not just a container parser like PCM).
      `core/loader/midi.{h,cpp}`: `ParseMidi` handles format 0/1 (format
      2 and SMPTE-timecode division are explicitly rejected, not
      mis-parsed), variable-length-quantity delta-times, running status
      (a real compression convention Standard MIDI Files use — repeated
      same-type events omit the status byte), meta events (only Set
      Tempo and End-of-Track are acted on; others are skipped by length),
      and merges note-on/note-off pairs across all tracks into one
      absolute-tick timeline, auto-closing any note never explicitly
      turned off at the track's end. `RenderMidiToPcm` converts that into
      PCM: a simple sine-wave synthesizer, one tone per note scaled by
      velocity with a short linear fade in/out to avoid clicks between
      notes, tick positions converted to real seconds via the file's
      full tempo-change map (not just tempo at tick 0 — a real Double
      Dragon track changes tempo repeatedly mid-file, confirmed below).
      **Deliberately no instrument/timbre modeling** (every note sounds
      the same sine tone regardless of MIDI program-change events, and
      the percussion channel isn't treated specially) — a documented,
      honest scope for a first correct-but-crude synthesizer, not a full
      General MIDI implementation; revisit if the target game's music
      turns out to need real instrument timbres to be recognizable.
      Both codecs converge on the same `WavAudio` shape, so `MediaHle`
      dispatches purely by file extension (`.wav` → `ParseWav`, `.mid`/
      `.midi` → `ParseMidi` + `RenderMidiToPcm`) and everything
      downstream (Mixer, playback state, `IMedia` HLE) is fully
      codec-agnostic.
      **Cross-verified against all three real Double Dragon `.mid` files
      checked**, not just synthetic fixtures: `bgm_1_0.mid` → 1788 notes,
      163 BPM, 47.5s; `bgm_2.mid` → 1811 notes, a real gradual tempo
      ramp (82→87.4 BPM across the first few tempo-change events), 109s;
      `bgm_9.mid` (the smallest file, 1034 bytes) → 110 notes, 3.96s —
      all durations and note counts are exactly what you'd expect from
      the respective file sizes, strong independent confirmation the
      tempo-map/tick-to-seconds conversion is correct on real data, not
      just hand-built test cases.
      11 new unit tests (`tests/midi_test.cpp`) covering VLQ/running-
      status parsing, chords, unclosed notes, explicit tempo changes, and
      a zero-crossing-count sanity check that a max-velocity A4 note
      actually renders audio near 440Hz. One real bug caught and fixed
      *in the test suite itself* while writing these (not the parser):
      an early version of the "unclosed note" test left a dangling
      delta-time VLQ with no attached event — invalid MIDI, since a
      delta-time must always be immediately followed by an event — which
      the parser (correctly, per real running-status semantics) then
      misinterpreted as a second bogus note; fixed by constructing that
      test's track by hand instead of relying on the shared helper's
      assumptions.
      Also wired a second live demo into the standalone frontend
      (`tests/fixtures/test_tune.mid`, our own tiny 4-note original
      arpeggio) alongside the WAV tone, verified the same way — a real,
      live `pactl` sink input from the running process. 10 new
      `MediaHle` coverage assertions total across both codecs
      (`tests/media_hle_test.cpp`). 136/136 project tests green.
- [ ] IMA-ADPCM decoder — deferred, not needed by the target title (see
      research above); revisit if a future title actually needs it
- [ ] MP3 decoder integration — deferred, not needed by the target title
      (see research above); MP3 patents have expired so there's no
      licensing blocker whenever it does become needed

## Phase 7 — Input
Exit criterion: target test game responds correctly to controller input.

- [x] Implement `IHID` HLE (Zeebo Z-Pad extension) — ARCHITECTURE.md §3.7.
      Promoted from Phase 8's own experimental, ad-hoc `game_probe.cpp`
      scaffolding (built and verified this session driving a real Double
      Dragon button-press state-machine transition end to end) into a
      real, tested, reusable `core/brew/hid_hle.{h,cpp}` (`HidHle`).
      Real `IHID`/`IHIDDevice` vtable order confirmed against the
      bundled real SDK headers and `research/samples/conftest_source/
      conftest/GamepadMgr.c` (TASKS.md Phase 8/PHASE8_LOG.md). Only
      `RegisterForButtonEvent`/`GetNextButtonEvent` -- the two methods a
      real call site was found directly exercising -- have real
      behavior; every other slot (including analog `GetPositionState`)
      is a safe no-op stub, per this project's established convention
      for a real-but-unconfirmed slot. `UpdateState(ZPadState)` diffs
      against the previous poll and enqueues real `AEEHIDButtonInfo`
      events using the real, header-confirmed button UIDs. 9 new tests
      (`tests/hid_hle_test.cpp`); `game_probe.cpp`'s own experimental
      HID scaffold deliberately left untouched (it needs deterministic
      scripted input for investigation, not live polling -- a real,
      reasoned scope boundary, not an oversight).
- [x] Standalone frontend: SDL2 gamepad/keyboard → `ZPadState` mapping.
      `Sdl2Backend::PollInput()` implemented for real: a real
      `SDL_GameController` when one is connected, falling back to a
      fixed keyboard layout (arrows + Z/X/A/S + Q/E + Return) otherwise.
      `SDL_INIT_GAMECONTROLLER` added alongside the existing VIDEO/AUDIO
      init flags. Not unit-tested (SDL controller/keyboard state can't
      be feasibly mocked without a virtual input driver, and no other
      frontend code in this project is unit-tested either -- consistent
      with existing precedent, not a new gap).
- [x] Default input mapping matching a standard Xbox-layout controller.
      Standard `SDL_GameController` button/axis naming already *is* an
      Xbox-layout mapping (A=bottom, B=right, X=left, Y=top), so the
      controller mapping above satisfies this directly, not as a
      separate step.

## Phase 8 — First Playable Commercial Game
Exit criterion: **M1 from PRD §7** — target title (Double Dragon) fully
playable start-to-finish at full speed, standalone build.

- [ ] Iteratively debug against the real game, filling HLE API gaps as they're hit
      Extensive real-disassembly-driven investigation log (every HLE gap
      found and fixed, each grounded in real evidence — never guessed) grew
      large enough to dominate this file. **Moved to `PHASE8_LOG.md`** —
      see that file for the full chronological history; this entry now
      holds only the current summary.

      **Current status**: real per-frame game logic runs with zero
      crashes — `AEEMod_Load` → `IModule::CreateInstance` →
      `HandleEvent(EVT_APP_START)` → the steady-state tick loop all
      complete cleanly against the real `ddragonz.mod`. Along the way:
      real ROPI static-base loading, 11 static-base runtime-table slots
      (MEMCPY/MEMSET/STRCPY/STRLEN/MALLOC/FREE/GETUPTIMEMS/
      GETAPPCONTEXT/bounded-copy/STRSTR/sprintf-family), real key input,
      real `DrawRect`/`SetColor`/glyph rendering, and a ten-gate
      `CreateInstance` init chain — including identifying real classes
      `AEECLSID_GL`/`AEECLSID_EGL`/`AEECLSID_HID` (confirmed via real
      bundled SDK extraction) and very likely `AEECLSID_FILEMGR`/
      `AEECLSID_DIB`/`AEECLSID_SignalCBFactory` (strong circumstantial
      evidence) — are all real, tested, working HLE now.
      **Not yet playable**: the game is currently stuck redrawing a
      diagnostic overlay (`"ERROR CODE:6"` / `"LIST COUNT:3"`), frozen
      since real tick 3. Four real gaps found and fixed so far: (1) the
      game opens its own packed resource archive as a raw file
      (`IFILEMGR_OpenFile("sound.ggz")`), which the dev tool's
      `VirtualFilesystem` never exposed (only each archive's
      *decompressed entries*, not its own raw bytes); (2) a real,
      foundational bug in `FileHle::SeekImpl` — it returned the
      resulting file position instead of `AEE_SUCCESS`/`AEE_EFAILED`
      (confirmed backwards against the real `AEEFile.h` contract),
      silently breaking any real seek to a nonzero position; (3) class
      `0x01001014` (found alongside `AEECLSID_FILEMGR` at the very
      first `CreateInstance` gate this session, real identity still
      unconfirmed) implemented as `FileHle::BuildLastOpenedFileProxy`
      — an evidence-grounded proxy whose `Read` always forwards to
      whichever file was most recently opened, rather than the old
      blind scaffold that silently read 0 bytes every time; (4) the
      emulated heap (arbitrarily sized at 1MB) bumped to 16MB after
      real disassembly showed the resource loader legitimately
      exhausting it via real per-item `MALLOC` calls. With all four
      fixes, the loader now gets through dozens of real resources
      (previously 0–1) before still ultimately failing — traced the
      new failure to the real, final (74th of 74) entry in
      `sound.ggz`'s own GGZ table: its declared decompressed size
      (1034 bytes) doesn't fit in the real file's remaining bytes
      (505) after that entry's offset, because the real game code
      reads raw, undecompressed bytes directly off disk and keeps
      pulling from whatever comes *after* the current entry to satisfy
      its requested total — which only works because every other
      entry has more archive data after it. Independently verified
      (Python `zlib`) that those exact 505 real bytes are a complete,
      valid gzip stream decompressing cleanly to exactly 1034 bytes —
      not truncation or a parsing bug on our end. Also disassembled
      the dispatcher itself directly: `"LIST COUNT"` and the per-tick
      case index turned out to be the *same* struct field (corrects an
      earlier entry in `PHASE8_LOG.md` that treated them as separate),
      and a live watchpoint across ~900 subsequent ticks showed zero
      further writes to it or the error-code field after tick 3 — this
      isn't an infinite retry loop, the game gives up after exactly one
      real attempt and stops invoking this subsystem entirely; the
      frozen diagnostic is just stale memory redrawn by an unrelated
      render path. Also confirmed via disassembly that the loop's
      81-slot bound is a hardcoded literal, not derived from any real
      parsed count. Current best hypothesis: this repo's `sound.ggz`
      research asset may be missing trailing bytes the real distributed
      file has (which would let entry 74's raw read spill into further
      archive data the way every earlier entry's does) — i.e. likely a
      research-asset gap rather than an emulator gap, though not yet
      confirmable without another real copy of the file. See
      `PHASE8_LOG.md`'s final entries for the full trace and reasoning.
      **Superseded**: the "frozen since tick 3" state above was reached
      using ClsId `274754` (Double Dragon's download-catalog folder
      number) at the very first `IModule::CreateInstance` call — later
      found, while investigating Super BurgerTime, to not be the real
      value the module's own code checks at all (see this file's Super
      BurgerTime section and PHASE8_LOG.md). The real value is
      `0x0102f789`; with it, `game_probe` reaches **"Reached the event
      loop with no unhandled instruction!"** cleanly, same as the other
      two titles, superseding this entire earlier trace (not yet
      re-driven forward from there this round). Separately, Double
      Dragon's real sprite/texture format — `.obm1`, all 89 assets in
      `data.ggz` — is now fully cracked (`core/loader/obm1.h`,
      PHASE8_LOG.md): confirmed via a legible decoded font sheet and a
      complete, correct character animation sheet. Not yet wired into
      any real render path — the next concrete step is letting the
      interpreter run further past the event loop with the correct
      ClsId and seeing whether real code reaches and correctly executes
      its own real `.obm1`-decoding/texture-upload logic.
      **Did exactly that, and found two real, foundational bugs**: (1)
      `IDisplayHle::DrawText` assumed 16-bit UTF-16 AECHAR; real Zeebo/
      BREW AECHAR is a plain 8-bit byte — silently garbling every text
      draw in every title tested so far, not just this one. (2) ClsId
      `0x01014bc4` (`AEECLSID_EGL`, already correctly registered against
      the real `GlHle` EGL object) was being silently overwritten by a
      generic-stub registration for the *same* ClsId added later,
      investigating Peggle — a dead-code fallback path that never
      actually helped Peggle (its own primary class is itself an
      always-succeeding stub) while breaking Double Dragon's real,
      direct EGL init. Together these caused a real, legible
      "Failed in the initialization of the library." dialog Double
      Dragon was silently stuck showing since real tick 0. Fixed both;
      the dialog no longer triggers, and real code now opens real files
      for the first time all session — `sound.ggz` and the real
      `./udata/ddz.sav` save flow, both previously documented as
      real-but-unreached call shapes. Also fixed the project's own
      `hello_brew` test fixture, which shared the same 16-bit AECHAR
      assumption. 259/259 tests pass. No regression on Peggle/Super
      BurgerTime. See PHASE8_LOG.md for the full evidence trail
      (including a live memory watchpoint tracing the dialog's real
      `applet+0x24` status field back through the real call chain to
      the exact failing `eglGetDisplay` call).
      **Confirmed the fix's real impact and found the next wall**: real
      code now genuinely streams through `sound.ggz` end-to-end for the
      first time (a real backward walk through its GGZ table, real
      variable-sized compressed-audio reads), completing within the
      first few ticks, then settles into a new steady state that
      neither more real time (90s, no change) nor a full sweep of
      every real AVK key code this codebase's dispatcher recognizes
      unblocked. That sweep was inconclusive, not a dead end — either
      the real "continue" input isn't in that code range or the gate
      isn't input-shaped at all; needs real disassembly tracing to
      answer, same method that found the EGL bug, not more guessing.
      Not attempted this round. See PHASE8_LOG.md.
      **Did that tracing.** The stable title-screen state gates on one
      bit (`0x100`) of a real word (`applet+0x361c`) recomputed every
      tick as the OR of two fields, themselves copied every tick from a
      real per-input-source struct at `applet+0xa20` (64-byte stride,
      2 sources) — and that struct is itself cleared (real `memset`)
      every tick, presumably meant to be refilled from a real input
      poll this codebase doesn't drive. `HandleEvent`'s own real key
      dispatch (confirmed: itself just a trampoline through a real,
      data-driven `applet+24` pointer) writes into a *different* pair
      of fields (`applet+0x28`/`+0x2c`/`+0x30`) that never feed this
      gate at all — so the AVK-key path from two rounds ago was never
      going to work, not a matter of trying more codes. Real root cause
      (what actually populates `applet+0xa20` — a distinct real input/
      touch/HID subsystem, given Zeebo is a 2009 touch device not a
      classic AVK keypad, is the live hypothesis) not yet found; needs
      a watchpoint on that struct directly, not yet done. Well-scoped
      next thread. All instrumentation reverted; 259/259 tests pass
      (no functional changes this round). See PHASE8_LOG.md.
      **Found it, and it's conclusive: not a bug.** A direct
      watchpoint on `applet+0xa20` caught a real `ISHELL_CreateInstance
      (cls_id=0x106c411)` call -- `AEECLSID_HID`, the real gamepad
      class -- writing its object pointer straight into this struct.
      The whole pipeline traced last round reads real per-controller
      HID state; this project's own `hid_obj` scaffold honestly reports
      zero connected devices (we have no real joystick hardware), so
      the gate's bit can never legitimately become set through this
      path. Real Double Dragon appears to gate this specific
      title-screen transition on an external/Bluetooth gamepad, the
      same way real BREW titles could support optional peripherals --
      a genuine hardware dependency, not an emulation gap. Moving past
      it now needs a deliberate choice (teach the HID scaffold to
      report a fake connected controller for testing) rather than more
      tracing; not attempted this round. See PHASE8_LOG.md.
      **Made that choice.** Reporting a connected device unlocked two
      real, previously-latent bugs (both fixed): `IHID_CreateDevice`
      (real vtable slot 3) was a blind stub leaving its output
      `IHIDDevice*` null -- the same unchecked-`CreateInstance`-style
      pattern this project has hit repeatedly -- and the device
      scaffold it now returns needed 40 vtable slots, not this file's
      10-slot default, since real code calls the device's own slot 11.
      Also registered a third real class (`0x01005511`) only reachable
      through this path. `CreateInstance` completes cleanly again and
      reaches the event loop. The title screen still doesn't advance,
      but for a fully expected reason now: a connected-but-idle
      controller correctly reports no buttons held. Simulating an
      actual button press (capturing and directly invoking the real
      registered callback, address `0x11beac`, already seen live) is
      the next well-scoped step, not attempted this round. No
      regression on Peggle/Super BurgerTime; 259/259 tests pass. See
      PHASE8_LOG.md.
      **Turns out what's currently on screen isn't the title screen at
      all — a separate, parallel real blocker.** With the joystick
      connected but idle, real code shows a genuine "CARREGANDO..."
      spinner, then a persistent "LOAD ERROR"/"ERROR CODE:6" dialog.
      Traced it end-to-end (live watchpoints and a temporary
      `OpenFile`/`Read`/`Seek` trace, all reverted) to a real per-tick
      resource loader whose 74-entry `sound.ggz` GGZ header table is,
      by its own internal accounting, short: entries 68-73 (the last
      6) declare more data than the 1,928,097-byte file actually
      contains — entry 73 (`bgm_9.mid`) is missing 529 of its declared
      1034 bytes. Confirmed via a standalone script reading the raw
      file, no emulation involved. This refines, but doesn't resolve,
      the open question from the Peggle section below: a byte-for-byte
      SHA-256 match against an independent archive.org copy already
      showed this `sound.ggz` is the authentic shipped file, not a
      research-asset gap — so the truncation is baked into the real
      asset itself. Two live, undistinguished hypotheses: either this
      project has an unfound real gap that makes it reach this entry
      when real hardware wouldn't, or the retail build genuinely ships
      with this defect and real hardware papers over a short read
      somehow. Not resolved this round — a deliberate stopping point,
      not a guess. 259/259 tests pass (investigation only, no
      functional changes). See PHASE8_LOG.md.
      **Found a third, independent source — points away from "shipped
      defect."** Archive.org item `Zeebo` ("OpenZeebo" compilation, a
      different curation than `zeebo-arquivista`) has its own Double
      Dragon dump (`274754.7z`); pulled just that entry via HTTP range
      reads instead of the full 653MB zip. All three real asset files
      (`ddragonz.mod`/`data.ggz`/`sound.ggz`) are SHA-256-identical to
      this repo's copies — a third independent match, making a bad
      download effectively impossible. Also found that Tuxality's
      independent, closed-source Infuse emulator is on record reaching
      a **playable** state on Double Dragon (May 2025), almost
      certainly against this same public dump, since no other is known
      to exist. A correct implementation evidently doesn't get stuck
      on this LOAD ERROR using these exact bytes — favors "real
      Zeebulator gap" over "shipped defect," though not proven (Infuse's
      source isn't inspectable). Next concrete step: find what should
      stop real code from ever needing GGZ entry 73's full 1034 bytes
      in the first place. Not attempted this round; no repo files
      touched (comparison files stayed in scratchpad). 259/259 tests
      pass. See PHASE8_LOG.md.
      **Did that, and it closes the thread: there's nothing to fix.**
      Disassembled the real static table backing the op-code-2 preload
      loop (`0x11c964`, called from `0x11c248`) — a genuine compiled-in
      constant array in `ddragonz.mod` at `0x14e1cc`, 81 entries, real
      GGZ resource indices. All 74 distinct indices (0-73) appear at
      least once, entry 73 (`bgm_9.mid`) among them, and the loop is
      strictly all-or-nothing (any single failure aborts the whole
      batch, no per-slot skip exists anywhere in the function). This is
      an unconditional requirement baked directly into the compiled
      game code: no correct interpreter can run this exact binary
      against this exact file without hitting this failure. Combined
      with three independent byte-identical public copies of
      `sound.ggz`, the most parsimonious explanation is that the
      original real-hardware capture of this file — whatever tool
      first dumped it, long before any preservation effort — itself
      stopped a few hundred bytes short, and every public copy since
      has propagated that same short capture. Not a Zeebulator gap;
      moving past this dialog needs a more complete `sound.ggz` than
      any public source currently provides, not a code change. No
      further sourcing attempted (already searched once this round per
      this project's ask-first practice). 259/259 tests pass (no
      functional changes — read-only disassembly and a standalone
      script only). See PHASE8_LOG.md.
      **Reopened after being asked directly to match what a real,
      independent emulator (Infuse) does — and that pushed this all the
      way to a real, working fix.** Ran Infuse's actual Linux binary
      against this repo's own exact asset files; confirmed via `strace`
      and a live screenshot that it hits the identical short read on
      `sound.ggz` and still reaches the real splash screen. Chasing why
      required real disassembly of `0x11bfd0` (the actual per-entry
      reader, not `0x10739c` as assumed before): it's a plain
      accumulate-until-`length`-or-EOF raw byte copy with **no
      decompression at that level**, needing the file to physically
      contain each entry's full declared length — even though the same
      bytes are genuine, valid gzip streams (confirmed decompressible
      standalone) that this project's own separate `core/loader/ggz.*`
      already handles correctly for other purposes. **Fix**: `tools/
      game_probe.cpp`'s `MergeGgzInto` now zero-pads the raw archive
      blob (never the individually-extracted, correctly-decompressed
      entries) out to the largest extent its own header table declares,
      whenever the real file falls short. Verified: `list_count` now
      reaches 3 with `error_code` staying 0 (previously 6); real
      execution runs measurably further before hitting a new,
      different, out-of-scope gap (an unimplemented MRS/MSR
      instruction) — a distinct next thread. Scoped narrowly enough
      that Peggle/Super BurgerTime (no GGZ format, unaffected either
      way) can't regress from it. 259/259 tests pass. See PHASE8_LOG.md.
      **Chased that MRS/MSR crash and found two more real gaps, both
      fixed — Double Dragon now runs a full 10 seconds with zero
      crashes for the first time all session.** Extended the existing
      "wandered outside the module" diagnostic to also report the last
      real pc/lr before the jump (permanent, kept). That pointed at a
      real call through a 19th, previously-unfound static-base table
      slot (offset `0xdc`) — added as another safe no-op, same as the
      18 already confirmed. Took real execution from 479 to 111,400
      steps before a second null-pointer crash: a genuine `IShell`
      vtable call at slot 43, one past this project's previously
      "verified against real Qualcomm source" 42-slot (0-41) count.
      Extended the vtable with generously-sized, clearly-unconfirmed
      stub headroom (through slot 49) rather than guess what's really
      there. After both fixes: zero wander warnings, zero thrown
      exceptions, for the full run. A temporary DrawText trace
      (reverted) confirmed the LOAD ERROR dialog is completely gone —
      only the real "CARREGANDO..." spinner shows now, still
      legitimately loading rather than stuck. 259/259 tests pass. See
      PHASE8_LOG.md.
      **Confirmed loading now genuinely completes** (list_count reaches
      13 — the real last entry, flagged terminal — with error_code=8, a
      real "done" status, not a failure) and mapped the real HID
      button-press delivery pipeline end-to-end using a bundled real
      Qualcomm reference sample (`research/samples/conftest_source/
      conftest/GamepadMgr.c`) and the real `AEEIHIDDevice.h`/
      `AEEHIDButtons.h` headers (found this round under
      `research/docs/sdk_installer_extract/sdk_installer_cab/`).
      Corrected an earlier round's callback address (`0x11beac` is
      actually the device-connect callback, not button; the real one is
      `0x11bdf4`) and implemented real `ISignalCBFactory::CreateSignal`/
      `IHIDDevice::RegisterForButtonEvent`/`GetNextButtonEvent` plus a
      tick-loop injector that queues real button-press events and
      invokes the real captured callback directly. Found and worked
      around a real trap along the way (queuing Start/HOME aborts the
      real event loop before later events are processed — confirmed via
      disassembly, not guessed). Verified the full pipeline works:
      simulated presses for all 4 real action buttons + d-pad correctly
      set the real per-button bitmask. The title-screen gate
      (`applet+0x361c`) still didn't open, though — it reads from a
      different real struct (`applet+0xa20`) than the one this
      confirmed-correct button state lands in, and what copies one into
      the other isn't found yet. A concrete, narrower next thread. All
      temporary diagnostics reverted; the real signal/button
      implementation kept as a permanent, documented addition. 259/259
      tests pass. See PHASE8_LOG.md.
      **That "different struct" turned out to be a mistake, not a real
      gap — corrected it, found the actual missing bit, and got real
      game code to visibly react to simulated input for the first time
      all session.** `context+40`/`+44` and `applet+0xa20`'s own
      device-0 struct are the exact same memory (confirmed via
      disassembly of the real gate combine computation, `0x11a2ec`-
      `0x11a3a8`) — no missing copy step ever existed. Also found this
      project's own prior watch logs from two rounds ago had a real,
      correct nonzero write to the gate (`0x0000F00F`) sitting unnoticed
      among ~900 routine per-tick clears — a reading mistake, not a real
      dead end. The one thing genuinely missing: bit `0x100`, which none
      of the simulated buttons produced. Worked out all 16 real cases of
      the button UID-dispatch table from disassembly and found the one
      that sets it (`UID 0x0106C403`, unnamed in `AEEHIDButtons.h` but a
      real working case regardless) plus that a single momentary press
      isn't enough — changed the injector to hold the press for ~4
      seconds. Result: `applet+0x50`/`+0x54` (the per-tick state
      machine) left its old parked values and transitioned through three
      distinct real states, settling on a new one only once the gate
      carried bit `0x100` — real code visibly reacting to simulated
      input for the first time. No new on-screen text appeared in any
      run tried, though (longest continuous observation ~500 ticks) —
      genuinely unresolved whether the new state is itself another real
      wait (for a release-edge or something else not yet identified) or
      just hasn't produced a frame yet. Next thread: disassemble
      `0x121110`/`0x1063ec` directly. All temporary diagnostics
      reverted; the corrected 9-event button batch and sustained-hold
      injector kept, permanent and documented. 259/259 tests pass. See
      PHASE8_LOG.md.
      **Did that immediately, and it fully explains the idle state.**
      `0x121110` genuinely runs every tick (it opens by calling the
      already-known gate/combine function itself, plus four more real
      sub-calls) and checks two real, independent conditions, neither
      satisfied yet: (1) a *second* OR'd gate at `applet+0x3618`
      (fed from per-device field `+0x44`, parallel to the known
      `+0x361c` gate's `+0x48`) needs bits 4+5+8 — both real shoulder
      buttons plus the one already simulated — all held at once; (2)
      `applet+0x15ac` (the real "busy/pending load" field known since
      the start of this investigation) needs a new bit, `0x10000000`,
      that's currently `0x3` and unrelated to any button input tried so
      far. Confirmed both live. Two concrete, immediately-actionable
      next experiments: add the two real shoulder-button UIDs
      (`0x0106C406`/`0x0106C408`) to the simulated batch, or trace what
      should set `applet+0x15ac` bit `0x10000000` (independent of
      button input entirely — if this is the real gate, no amount of
      simulated pressing alone would ever be enough). Not attempted
      this round — investigation only, no code changes; `git diff
      --stat` clean. 259/259 tests pass. See PHASE8_LOG.md.
      **Tried both.** The three-button combo is real but turned out to
      be a full applet reset (confirmed: `applet+0x50` gets set back to
      its exact real tick-0 value, and a fresh `LOAD ERROR` appeared) —
      a real discovery, not the progression path this project needs.
      For `applet+0x15ac` bit `0x10000000`: found nine distinct real
      writers across a full run, and every one of them only ever
      touches the field's low byte — the high three bytes, including
      the specific bit checked, stayed `0x00` from all nine, button
      held or not. Sharpens the open question: it's not that a writer
      hasn't been found, it's that none of the real code paths this
      project's HLE currently reaches ever touch that bit at all.
      Would need either a wider static search across the whole `.mod`
      or driving whatever real subsystem those nine writers themselves
      depend on. Not attempted this round; investigation only, no code
      changes survive. 259/259 tests pass. See PHASE8_LOG.md.
      **Picked up a different loose thread instead of static-searching
      blind: the still-unidentified class `0x01005511`.** Traced its
      real calls and, from their shape (a status/percentage-report
      pattern, reached only as part of a broader real "environment
      ready" sequence, on a platform that distributes games by
      download), identified it as very likely Zeebo's real download/
      install-progress notification service. Implemented it for real —
      captures the real registered callback and invokes it once with a
      truthful "100% complete" event (truthful because this repo's own
      assets genuinely are complete, not an invented condition).
      Verified live: reaches the real real success-path check without
      wandering or crashing, but doesn't clear it — that check also
      needs a real byte at a known address to be nonzero, and nothing
      triggered so far ever sets it. Same shape of open question as
      `applet+0x15ac`, one layer deeper and narrower. Kept the real,
      working implementation as a building block; all diagnostics
      reverted. 259/259 tests pass. See PHASE8_LOG.md.
- [ ] Validate the HLE against a second real game (Peggle), started this
      round to check whether Double Dragon-tuned HLE generalizes.
      Downloaded 61 real Zeebo titles from the `zeebo-arquivista`
      archive.org preservation item (see PHASE8_LOG.md for provenance;
      files live under `research/games/_archive_org_zeebo-arquivista/`,
      git-ignored) — re-downloading Double Dragon from it turned out to
      be byte-for-byte identical to the copy already in this repo,
      independently confirming that file is authentic and complete, not
      a truncated research-asset gap (see the entry above). Surveyed all
      61 titles' formats: **only Double Dragon uses the GGZ archive
      format** — every other title uses a different container per
      publisher/engine (classic-arcade ports like Bad Dudes/Caveman
      Ninja/Pac-Mania wrap what looks like an embedded arcade-emulation
      core with its own `"PACK"`-magic `.pkg` format; several PopCap
      titles — Peggle, Bejeweled Twist, Zuma's Revenge — use a cleaner
      single-archive `resources.bar`/`resources.dat`, format not yet
      identified, doesn't match public PopCap BAR headers). Picked
      Peggle for its clean layout and smaller `.mod` (274KB vs Double
      Dragon's 462KB). Found Peggle's own real `IModule::CreateInstance`
      ClsId — `0x01099CD6` / `17407190`, unrelated to Double Dragon's
      `0x0102F789` — confirmed directly against `peggle.mod`'s own raw
      file bytes at the literal-pool address `CreateInstance` compares
      against (same technique as Double Dragon's). With that and the
      real static-base DBGPRINTF slot fix (offset `0x9c`, committed — see
      `core/brew/mod_runtime.h`), `CreateInstance` now runs real code
      to completion and returns a real, non-null applet pointer.
      **Implemented Thumb (T16) decoding and ARM/Thumb interworking**
      (`core/cpu/arm_interpreter.{h,cpp}`, 33 new tests in
      `tests/thumb_test.cpp`) after `HandleEvent(EVT_APP_START)` hit a
      real `BX` into Thumb code the interpreter couldn't execute at all
      — Double Dragon's own `.mod` apparently never needed it. Covers
      all 19 real Thumb1 instruction formats and real ARM/Thumb
      interworking (BX/BLX in both states, Thumb `POP{pc}`, ARM
      `LDR`/`LDM` into `pc`). Verified against real Peggle code:
      `HandleEvent` now runs tens of thousands of real Thumb
      instructions cleanly. That, plus two more real static-base slots
      found the same way as every other gap in this project (offset
      `0x44`, a second MEMCPY-equivalent; see `core/brew/mod_runtime.h`),
      pushed real execution to ~25,800 steps before hitting a new, deeper
      gap: a third real field (offset `0x2c`) on the shared "app
      context" struct (the same struct the confirmed offset-`0xc0` slot
      returns) that real code dereferences expecting an actual object,
      not the zero it currently holds there. **Fixed**: added a general
      mechanism for this real ABI variant
      (`scaffold_object.h`'s `BuildGenericRelativeVtableStubObject`,
      since it's a genuine ROPI relative-vtable ABI pattern, not a
      one-off) and a third settable `ModRuntime` context field
      mirroring Shell/Display — the real interface itself is still
      unidentified, wired to a safe no-op placeholder for now. That
      unblocked one more real gap: static-base offset `0x74` is
      REALLOC, confirmed via two independent real growable-array call
      sites (different element sizes, same `(old_ptr, new_size)`
      contract). **With both fixed, `HandleEvent(EVT_APP_START)` now
      completes successfully and Peggle reaches its real steady-state
      event loop** — the same milestone Double Dragon reached, on a
      second real game. See PHASE8_LOG.md for the full trace and
      reasoning, including a detour where a long stretch of no visible
      output was first mistaken for a hang before being confirmed as
      the tool's own correct, by-design infinite event loop.
      Ran Peggle for many further real ticks (thousands, over 60+ real
      seconds) with no new crash — confirmed indirectly (the process
      needed external termination rather than exiting on its own, which
      only happens on success; any real error sets `running=false` and
      returns cleanly). Started reverse-engineering `resources.bar`:
      found via real disassembly that it's opened through the real
      `ISHELL` vtable (not a custom parser), and cross-referencing the
      call's exact register arguments (`RESTYPE_BINARY=0x5000`, a
      buffer of `-1`) against the real bundled `AEEShell.h` identifies
      it precisely as `ISHELL_GetResSize`/`IShell_LoadResDataEx` — i.e.
      `resources.bar` is a **standard BREW application resource file**
      (`AEE_RES_EXT`), not a Peggle-specific format. Its real *binary*
      layout is still uncracked, though: unlike every other format in
      this project, its reader lives in the real device's own
      OS/firmware, not in any `.mod` we can disassemble, so cracking it
      needs blind, evidence-anchored byte analysis instead — deliberately
      not guessed further without a known (resource ID → size) pair to
      verify against first (the one real call site found isn't reached
      by blindly driving ticks; needs its own investigation for how
      Peggle actually reaches it).
      **Found why, and it's a real, significant finding of its own**:
      traced `ISHELL_SetTimer` directly (a live print inside
      `IShellHle::SetTimerImpl`, reverted after use) and confirmed it is
      called **exactly once** across the entire run, registering a
      20ms, plain-ARM-mode (bit 0 clear, ruling out a suspected Thumb-
      interworking bug in how `tools/game_probe.cpp` dispatches timer
      callbacks) callback — and that callback's own execution (already
      fully traced earlier: ~24 real instructions, one `GETAPPCONTEXT`
      call, a clean `bx lr` return) never calls `SetTimer` again to
      re-arm itself. Double Dragon's whole per-frame loop depends on
      exactly that self-rearming pattern (see this file's own real,
      confirmed doc comment on `IShellHle`).
      **Found and fixed why**: the callback's whole re-arm path is
      gated behind a fourth real field on the shared "app context"
      struct (offset `0x24`, `SetFourthContextObject()`) that this
      codebase never wrote, so the gate always failed. Wired to a
      real, writable, zeroed memory block with just the one confirmed-
      load-bearing field (`+20`) pre-set non-zero. Verified: tick 0
      now runs hundreds of real HLE calls (including a real
      `ISHELL_CreateInstance` for the same `FileMgr` class Double
      Dragon uses) instead of one.
      **Immediately hit a new, bigger-picture gap**: real code treats
      `context[0x24]` not as a small object but as the base of a
      **large global data arena** — different subsystems reach their
      own portion of it via large fixed offsets (e.g. `+0x45000`) from
      that same base. Traced precisely (confirmed the resulting null
      pointer, not a bug in the fix), but deliberately not guessed
      further: unlike the narrow `+20` flag, this is open-ended —
      no way yet to know how many more such offsets exist or what real
      data belongs at them. See PHASE8_LOG.md for the full trace and
      the two candidate ways to proceed.
      **Provisioned that arena field and fixed two more real gaps in a
      row**: a write-timing bug (real `HandleEvent` code resets the
      field to zero once during its own init, so the placeholder must
      be written *after* `HandleEvent` returns, not before), and a
      recurring real QueryInterface-style out-pointer chain (caller
      passes an output pointer, ignores the returned status, and
      dereferences whatever was written there — confirmed recurring
      through multiple freshly-returned objects). Generalized the fix
      into an experimental **self-propagating stub** (a recursive
      lambda in `tools/game_probe.cpp` that lazily builds a fresh child
      object for any such out-pointer call, arbitrarily deep) rather
      than hand-patching each level — deliberately kept local to the
      probe tool, not promoted to general scaffolding, since this
      chaining shape is only confirmed at this one real call site so
      far. **Verified**: tick 0 now reaches real
      `ISHELL_CreateInstance(ClsId=0x01001003)`, many real `Seek`-shaped
      and other real HLE calls, and the self-propagating chain itself
      firing through several more real traps — a large jump in real
      execution depth. Hit what first looked like a new, third real
      object convention at the next level (a flat struct with a
      function pointer read directly off a fixed offset, not through a
      vtable) — left undoctored rather than guessed at.
      **That turned out to be a misdiagnosis of a bug in this
      codebase's own stub, not a real object convention**: closer
      register-level tracing of the exact real call chain showed real
      vtable slot 2 does use the assumed `(this, id, ppOut@r2)` shape,
      but real slot 3 uses a *different*, also-real shape —
      `(this, ppOut@r1)` — and other real slots (and one real call
      site) pass no output pointer at all, with `r1`/`r2` holding
      leftover garbage or explicit zero. The old stub blindly wrote a
      child object into r2 for every slot regardless, corrupting
      whatever it found there — once, real address 0 itself — and it
      was real code reading back that self-inflicted corruption that
      produced the earlier "flat struct" illusion. **Fixed** by only
      special-casing slots 2 and 3 with their real, evidenced output
      registers (skipped when null), leaving every other slot a plain
      side-effect-free stub. **Verified**: tick 0 now makes 337 real
      HLE calls (up from 207) and survives 6312 real ARM steps (up
      from 3155) before its next wander — roughly double the real
      execution depth. Still eventually wanders to a null pointer from
      a new, not-yet-individually-traced call site. See PHASE8_LOG.md
      for the full trace evidence; continuing to chase the new wander
      point the same way is the next concrete step.
      **Chased that new wander point and it was a real gap this
      codebase already knew how to fill**: real code at `peggle.mod`
      offset `0x132dfc` reads a **fifth confirmed field on the shared
      app context struct, offset `+0x28`**, with no null check, and
      calls through it using the exact same ROPI relative-vtable
      convention already implemented for the third field (`+0x2c`).
      Added `ModRuntime::SetFifthContextObject` (an exact mirror of
      the third field's setter) and wired it to the same kind of
      relative-vtable scaffold. **Verified — this is the milestone the
      whole Peggle investigation has been chasing**: the timer callback
      now runs tick after tick with zero wander warnings and zero
      thrown exceptions, confirmed both via ten traced clean ticks and
      a 60-second unbounded run that needed external termination
      rather than exiting on its own — the same "success looks like a
      hang" signature already trusted for Double Dragon's own steady
      state. This is *sustained* execution, not necessarily *correct*
      execution yet: most vtable slots on every placeholder object
      involved are still safe no-ops, and nothing has driven visible
      output to the window. The next concrete step is determining
      whether real game state (level/resource loading, a rendered
      frame) is actually progressing over many ticks, or just looping
      harmlessly on placeholders. See PHASE8_LOG.md for full evidence.
      **Checked directly (temporary instrumentation, reverted): it's
      looping harmlessly.** A 30-second real run fired only five real
      "does something visible/external" events (`DrawText`/`DrawRect`/
      `Update`/`SetColor`, file opens, unknown-class `CreateInstance`
      requests) total, all during one-time startup, none afterward —
      zero draws the entire run, zero further file opens (including no
      attempt at `resources.bar`). Diffing the full per-tick HLE trace
      between tick 1 and tick 5 confirmed it: the exact same 20 calls,
      same order, same arguments, every tick. The sustained execution
      is real, but it's a fixed loop over placeholder objects, not real
      game logic — most likely polling one of the still-unidentified
      interfaces (the third/fifth context fields, or the fourth field's
      arena beyond its one confirmed sub-offset) that our safe no-op
      stubs can never report "ready," so it never falls through to real
      resource loading or rendering. Next lead: a real, literal ID
      constant baked into the module at the confirmed slot-2
      QueryInterface call (`0x0101eb0b`) plus two unknown real `ClsId`s
      surfaced this round — cross-referencing these against real BREW/
      Zeebo headers or other `.mod` binaries in `research/` may reveal
      what real interface these placeholders should actually be. See
      PHASE8_LOG.md for full evidence.
      **Chased that lead**: no header match for any of the three IDs in
      this repo's (small, 13-file) reference BREW header subset, but a
      binary search across every real `.mod` in `research/games/` found
      ClsId `0x0103d8ec` is **not Peggle-specific** — the exact same
      real `ISHELL_CreateInstance(0x0103d8ec)`-then-fallback-to-
      `0x01014bc4` instruction sequence, both literal IDs included,
      appears verbatim in Super BurgerTime's own `.mod` too — strong
      evidence of a real, standard SDK-emitted helper. Registered
      generic scaffolds (the same established, deliberately-unguessed
      treatment as the earlier `0x01002001` case) for this pair plus a
      third real ClsId (`0x01030766`, traced separately). **Verified**:
      tick 0's call count dropped as expected (fallback path now
      skipped since the primary succeeds), but the steady-state per-
      tick loop is byte-for-byte unchanged — **ruling out all three as
      the per-tick blocker**. The loop's own real ID (`0x0101eb0b`)
      still has no header match; identifying it is the next concrete
      step, likely needing either a fuller real BREW MP header set or
      more structural tracing. See PHASE8_LOG.md for full evidence.
      **Web search for `0x0101eb0b` and the other unidentified IDs came
      up empty** — no public source has any of them. A promising-
      looking lead, the closed-source third-party "Infuse" Zeebo
      emulator, turned out to have no referenceable code (proprietary,
      no-redistribution license) and doesn't target these games anyway;
      no public Zeebo firmware/system dump was found anywhere either.
      This specific lead is exhausted.
      **Took the structural-tracing option instead**: a full
      instruction trace of one steady-state tick found the very first
      real action every tick is reading a sibling arena field,
      `context[0x24]+0x45000+0x3dc` (next to the already-provisioned
      `+0x3d8`), and passing it un-null-checked into a real subroutine
      that dereferences it repeatedly — with it left at 0, this was
      confirmed writing a real per-tick counter to real address 4 and
      reading real address 0 back, i.e. genuine memory pollution, not
      simulated behavior. This field's own real layout looks like
      Peggle's own internal per-tick game-object data (not a generic
      BREW interface) and wasn't safe to guess at, so it got the same
      conservative treatment as the fourth field's own arena
      allocation: a real, isolated, zeroed memory block. **Verified**:
      the real accesses now land on that isolated block instead of real
      low memory; steady-state behavior is otherwise unchanged (still
      the same do-nothing branch every tick) — a hygiene fix, not a
      progress unlock. 241 tests pass; Peggle remains stable.
      **This is a reasonable pause point for this investigation
      thread**: the per-tick loop is now real, evidence-traced, and
      free of known memory pollution, but still doesn't progress past
      its fixed steady state. Further progress needs either a real
      BREW MP SDK header dump this project doesn't have access to, or a
      much larger, Peggle-specific reverse-engineering effort into its
      own per-tick game data — both bigger asks than the incremental
      fixes made so far. See PHASE8_LOG.md for full evidence.
      **Picked back up once the full 61-title game collection became
      available**, using the same cross-referencing technique that
      found `boot.pkg`: found the still-unidentified real ID
      `0x0101eb0b` also referenced in Zuma's Revenge's own `zumar.mod`,
      at a real call site with the identical shape, going on to invoke
      a helper function that's **byte-identical machine code** between
      the two titles — confirmed shared, statically-linked SDK code,
      not coincidence. Traced the real usage end to end in both:
      `QueryInterface(ctx, 0x0101eb0b)` succeeds, then real code calls
      the result's own real slot 4 (no output pointer — the *return
      value* is a fresh object), and calls *that* object's own slot 3
      repeatedly with `(this, buffer_ptr, size)` in chunks — a real
      "write a stream of bytes" shape (very plausibly telemetry/
      logging), distinct from the outer object's own already-confirmed
      slot 3 meaning. **Fixed**: extended the existing self-propagating
      stub machinery with this real slot 4 shape, returning a fresh,
      independent, all-slots-stub object via `r0`. **Verified — a
      dramatic change**: the per-tick loop's call count jumps from a
      fixed ~20 to 500+ across the first 10 ticks, including **real
      `IFileMgr` activity for the first time ever in this title's
      investigation** — real code now opens a real save file,
      `udata/game` (create, read-write, then read). No regression on
      Double Dragon/Super BurgerTime. 282/282 tests pass. See
      PHASE8_LOG.md for the full derivation.
      **Followed up immediately**: the save round-trip is genuinely
      self-contained (real code writes its own 6,672-byte default,
      reads 9×16 bytes back, no external content needed), and leads
      into real, substantial new execution (a real ~1,397-item
      processing loop, one-time per tick-0). But a full trace of
      `IDisplayHle::DrawText`/`DrawRect`/`Update` found **zero visible
      draws** across a 20-second run, and `resources.bar` (this
      round's own cracked format) is **never requested** — real code
      hasn't reached asset loading yet. Real, honest progress, but
      still nothing on screen. Investigation only, all temporary
      instrumentation reverted, 282/282 tests pass unchanged. See
      PHASE8_LOG.md.
      **Found and fixed why `resources.bar` was never requested:
      `ISHELL_LoadResDataEx` (real vtable slot 41) was a blind stub.**
      A full instruction trace of a real call found the exact real
      calling convention (query size via the real `-1` sentinel,
      `malloc`, fetch for real) and a concrete real `(id=4000, type=1)`
      pair. That pair turned out to be the key to `resources.bar`'s own
      previously-unparsed 496-byte sub-table too: its 60th record reads
      `{type=1, id=4000, entry_index=304}` -- an exact match, resolving
      to the same real localized string this log already independently
      confirmed at entry 304. **Promoted into a real
      `BarArchive::Find(type,id)`** (not Peggle-specific — the same
      real resource-ID directory format any `.bar` file presumably
      uses) and a **real, working `LoadResDataEx` implementation** in
      `core/brew/ishell.{h,cpp}`, backed by a new
      `RegisterResourceFile`. 7 new tests. **Verified against real
      Peggle**: with `resources.bar` supplied, execution takes a
      genuinely new path — a new real gap after ~7,000 steps (a
      previously-unseen two-handler event-dispatch mechanism), instead
      of settling into the same ~500-call idle loop every time. No
      regression on Double Dragon/Super BurgerTime/Peggle-without-
      `resources.bar`. 289/289 tests pass. Doesn't reach a visible
      frame yet, but for the first time a real BREW OS-level API has
      real, non-stub, end-to-end behavior. See PHASE8_LOG.md.
      **Chased the new dispatch gap and found a real bug in this
      project's own test harness, not a missing HLE behavior**: two
      unrelated dynamic object-address counters in `tools/
      game_probe.cpp` (`0x80030000` and `0x80038000`, only 8 slots
      apart, neither aware of the other or of ~30 other fixed
      addresses in the same file) could collide once enough real
      recursion happened — which now happens, since `resources.bar`
      lets real code run further than ever. A live watchpoint plus a
      targeted register trace confirmed it directly: a real stub
      object's own vtable pointer got silently overwritten with an
      unrelated HLE trap address. **Fixed** by moving both counters
      into a large, previously-unused address range, removing the
      whole category of collision. **Verified**: the crash is gone;
      execution now reaches **tick 2** (previously crashed inside tick
      0 every time) before hitting a new, different, narrower gap — a
      real function expecting a populated context field getting a
      genuine null, the same well-precedented shape this project has
      solved several times before. Not fixed this round (a fresh,
      well-characterized next thread). No regression; 289/289 tests
      pass. See PHASE8_LOG.md.
      **Traced that null field to its real root cause**: real code
      genuinely constructs a matching manager object at runtime (a real
      message-dispatch handler -> a once-only construction gate -> a
      real malloc-backed constructor, all confirmed executing live, in
      order) — but it lands on this codebase's own heap allocator at
      `0x80300024`, while the per-tick code that later needs it reads
      it through `ModRuntime`'s `context+0x24` field, which this
      codebase currently hardcodes to an unrelated, permanently-empty
      placeholder (`kFourthContextObject`, `0x80020000`). The two real
      and fake objects never meet. Not fixed this round — the real next
      question is whether genuine Peggle code ever *writes* the
      constructed object's address into the ambient context struct
      (`mod_runtime.h` already documents this field as plain
      read/write data, not a vtable call), which would need locating
      before wiring a real fix rather than guessing one. Investigation
      only, all temporary instrumentation reverted, 289/289 tests pass
      unchanged. See PHASE8_LOG.md.
      **Found the real write site and fixed it — the crash is gone for
      good.** Real code writes its constructed sub-objects directly
      onto fields `+0x24`/`+0x28`/`+0x2c` of the real `IApplet*`
      `CreateInstance` returns (`peggle.mod` `0x135488`, confirmed:
      that address is exactly `applet_ptr`, already printed by this
      codebase's own harness) — `GetAppContext`'s "ambient context"
      isn't a separate OS struct in this build, it's the app's own
      instance. Fixed by adding `ModRuntime::SetContextAddress()`
      (redirects the context pointer once `applet_ptr` is known, wired
      in `tools/game_probe.cpp` right after `CreateInstance` succeeds)
      and changing `GetAppContextImpl` to only rewrite a field when its
      `Set*()` call is actually pending, rather than unconditionally on
      every call — the previous unconditional rewrite would have
      clobbered real code's own write on the very next per-tick
      `GetAppContext` call regardless. **Verified against real
      Peggle**: the crash is completely gone; execution now reaches
      **"Reached the event loop..."** (the same milestone Double Dragon
      and Super BurgerTime already hit) and sustains at least 10 real
      ticks, settling into a stable repeating idle loop rather than
      crashing — still nothing drawn on screen yet (traced and
      confirmed with temporary, reverted instrumentation), but a
      categorically more stable state than any previous round reached.
      No regression on Double Dragon or Super BurgerTime, verified
      directly. This fix lives in real, permanent HLE code
      (`core/brew/mod_runtime.{h,cpp}`), not dev-tool scaffolding, and
      is general — not Peggle-specific. 2 new tests; 291/291 tests
      pass. See PHASE8_LOG.md.
      **Two more real fixes landed the same round.** The app-context
      fix above had an unintended side effect: `SetContextAddress` was
      re-priming the fifth field (`+0x28`), which real code also uses
      as its "already constructed" gate (confirmed: `peggle.mod
      0x135470` reads exactly that offset) — so real construction kept
      getting silently skipped. Fixed by only re-priming the two
      confirmed OS fields (Shell/Display), leaving the three
      placeholder fields untouched so real code's own constructor can
      run. That unblocked a second real gap: `ISHELL_CreateInstance(
      shell, 0x0101eb0b, ...)` — the shared PopCap/Zeebo SDK class this
      project fully reverse-engineered rounds ago but never actually
      registered — was failing silently and leaving real code to
      dereference garbage. Fixed with one `RegisterInstance` call. That
      in turn exposed a third gap in `unknown_0x01030766_obj` (a
      generic stub since it was first found): both its slot 2
      (another real `CreateInstance(0x0101eb0b, ...)` call) and slot 3
      (a real `(this, &ppOut)` call) needed real forwarding instead of
      silently leaving out-params unwritten. **Verified against real
      Peggle**: execution now sails through all ten traced ticks with
      genuine, non-placeholder objects visible in every context field,
      reaching a fourth, new, narrower frontier past tick 9 (not yet
      root-caused — a fresh thread for next time). **Honest side
      note**: Double Dragon now also runs measurably further than
      before (verified: the pre-this-round build never progresses past
      its steady state even given twice the run time) and hits its own
      new static-base-table gap — not a regression, since everything
      it did before still works, but worth naming since it changes
      DD's own next milestone too. 291/291 tests pass unchanged. See
      PHASE8_LOG.md.
      **Chased the tick-9 frontier and found a genuinely confusing
      shape, not yet fixed.** The crashing function (`peggle.mod
      0x108a90`) reads its own not-yet-constructed field a second time,
      past its own "already constructed?" gate, and passes the
      still-zero result as `this` into a real construction trampoline
      that unconditionally dereferences it (no null guard, so this
      isn't a tolerated sentinel). Grepped every store in the function
      body between the gate and the crash — nothing writes that field.
      Best read: some other, not-yet-identified real event is expected
      to populate it *before* this function ever reaches this point,
      and that trigger hasn't happened in this emulated run — a
      different shape than every previous "ambient field never
      populated" gap this project has fixed (those had one clear owning
      constructor; this one re-reads its own field mid-construction).
      Deliberately not guessing a fix. Investigation only, all temporary
      instrumentation reverted, 291/291 tests pass unchanged. See
      PHASE8_LOG.md.
      **Traced that field to its real root cause with a live write
      watchpoint, and it's the exact same unconfirmed `IShell` slot
      Double Dragon's own investigation hit independently, months ago.**
      The field is genuinely written (zeroed, by a real 39-slot bulk-
      reset loop and a defensive "clear the out-param first" pattern),
      then real construction only proceeds if a specific vtable call
      returns exactly `35` — that call resolves to `IShell` vtable byte
      offset `0xac`, slot 43, already flagged in `core/brew/ishell.cpp`
      as "the one real call site found so far" from Double Dragon's own
      history. Two independent titles, same slot. Double Dragon's own
      caller tolerates the stubbed `0` gracefully; Peggle's doesn't —
      traced one level further and found it never checks the
      construction result at all, trusting it unconditionally. Looked
      at what real slot 43 would need to do for real and it's
      substantial (a multi-branch state machine reading two more struct
      fields, building formatted strings through further real vtable
      calls) — genuinely more than safe to guess without a real BREW MP
      `IShell` header this project doesn't have access to. Left
      unfixed, precisely documented instead. Investigation only, all
      temporary instrumentation reverted, 292/292 tests pass unchanged.
      See PHASE8_LOG.md.
      **Picked back up (2026-08-04) and found this project *does* have a
      real BREW MP `IShell` header after all** —
      `research/docs/sdk_installer_extract/brew_sdk_headers_reference/
      brew_mp_7.12.5_sdk/AEEIShell.h` — previously overlooked by this
      exact thread. Its `INHERIT_IShell` macro lists 50 total vtable
      methods (AddRef/Release + 48 IShell-specific, extending past the
      pre-BREW-MP 40-method count this project's own `ishell.cpp`
      already implements through `LoadResDataEx`=slot 41) in a fixed
      order: slot 42 = `RegisterSystemCallback`, **slot 43 =
      `DetectType(cpBuf, pdwSize, cpszName, pcpszMIME)`** — byte offset
      math checks out exactly (`0xac / 4 = 43`). Added a temporary live
      register trace at slot 43 (reverted after) and ran real Peggle
      again: the real call site passes `r1=0` (`cpBuf`), `r2` a valid
      pointer (`pdwSize`), `r3=0` (`cpszName`), and stack arg 0 = `0`
      (`pcpszMIME`) — every pointer `DetectType` would need to actually
      detect or report anything is null. **Hypothesis not confirmed**:
      a real `DetectType` call with nothing to sniff and nowhere to
      write a MIME string back is a plausible edge case but doesn't
      inspire confidence this is really `DetectType`, especially given
      this reference header is BREW MP 7.12.5 — a substantially later
      SDK revision than Zeebo's real 2009-era BREW 4.0.2, which may not
      share BREW MP's own slot 42+ ordering at all. **Also tested
      empirically or the earlier `35`-suffices theory**: patched the
      slot 43 stub to unconditionally return `35` (still investigation-
      only, reverted) and reran — execution still crashes at the exact
      same real gap, 3 steps later (`peggle.mod` last in-module
      pc≈`0x105b70`, lr≈`0x108ba8`, same as the `return 0` baseline).
      **Confirms the original assessment rather than overturning it**:
      a bare correct return value alone doesn't unblock real progress,
      so the real construction path genuinely does need more than a
      scalar fix — consistent with "a multi-branch state machine"
      above, not a quick win. Left unfixed. The newly-found header is a
      real, concrete lead for whoever picks this up next with an actual
      ARM disassembler (not just live register traces) to check the
      real caller's exact instruction sequence against `DetectType`'s
      documented contract instruction-by-instruction, rather than
      guessing from argument shape alone. Investigation only, all
      temporary instrumentation reverted, 292/292 tests pass unchanged.
      **Separately confirmed**: this project's real gamepad/HID
      injection pipeline (`tools/game_probe.cpp`'s `AEECLSID_HID`
      registration, `InjectHidButtonEvent`, `ZPadButtonToHidUid`, and
      `Sdl2UnifiedBackend`'s controller polling incl. the real Xbox
      Wireless Controller A/X-swap fix) is wired unconditionally in
      `main()`, before any game-specific ClsId is used — not Double-
      Dragon-specific — so it needs zero additional per-game work once
      Peggle (or any other title) actually reaches the point of calling
      `ISHELL_CreateInstance(shell, AEECLSID_HID, ...)`. Peggle's own
      execution doesn't get remotely that far yet (crashes at the slot-
      43 gap above, well before any input-handling code would run), so
      this is confirmed-ready-and-waiting, not yet exercised.
      **Same round, continued: got a real ARM disassembly of the crash
      site** — `arm-none-eabi-objdump -D -b binary -marm
      --adjust-vma=0x00100000`, a tool already installed in this
      environment and apparently never used by this project before
      (every prior round relied on live register traces alone). Static
      disassembly of `peggle.mod` around the crash (`0x108a90`-
      `0x108ba8`, `0x102078`, `0x102148`) fully explains the shape:
      `0x105b5c` is one of many tiny ROPI relative-vtable thunks
      (`ldr r2,[r0]; ldr ip,[r2,#N]; bx ip`, matching the same real ABI
      `BuildGenericRelativeVtableStubObject` was built for) — the crash
      is this thunk's `this` (`r0`) being a still-zero array slot,
      dereferenced unconditionally. That slot (`peggle.mod`
      `this_obj+12` array, indexed by the same "resource ID"-shaped
      value threaded through the whole call chain) only gets populated
      if `0x102148` — the function directly containing the slot-43 call
      — succeeds. **Its exact contract, read straight from the
      instructions**: calls slot 43 with `(this=IShell*, r1=0, r2=&local
      uint32 on 0x102148's own stack)`; `r3` and any stack args are
      genuinely never set (confirms the earlier live trace wasn't
      missing real arguments — there simply aren't more real ones here,
      weakening the `DetectType` 5-argument-signature hypothesis further
      rather than supporting it). **Refines the earlier "returns 35"
      finding**: success needs *both* `r0 == 35` *and* a nonzero value
      written through that `r2` out-pointer — the earlier round's
      `return 35`-only patch never touched `*r2`, which is why it
      changed nothing. Patched the temporary stub to satisfy both
      (write `1` through `r2`, return `35`) and reran against real
      Peggle: **confirmed real further progress** — a second, real
      slot-43 call site fires this time (`r1=0x8038ecd0`, a real pointer,
      not null) and total steps before the crash rises from 551/554 to
      588 — but execution still ends at the exact same crash site. Read
      the next real gate directly from the disassembly rather than
      guessing further: after slot 43 "succeeds", `0x102078` calls
      through *another* unconfirmed relative-vtable slot at byte offset
      `+8` (== this project's own slot-2 numbering, i.e. shaped exactly
      like a second, nested `CreateInstance` call) on a *different* real
      object (`(*(outer_this->field_0))->field_12`, not the global
      `IShell*` the DBGPRINTF calls use) — and only if *that* call
      succeeds (returns 0 and writes a nonzero object pointer through
      its own out-param) does a further offset-`+16` call on the
      resulting object run, which finally writes the real constructed
      object into the array slot `0x105b5c` later dereferences. **Left
      unfixed** — this is now a concretely-scoped next step (identify
      what real object `this_obj->field_0->field_12` is and what
      ClsId/args its own offset-`+8`/`+16` calls need), not an open-
      ended unknown, but still two more unconfirmed real interfaces deep
      and not safe to guess blindly in one more round. All temporary
      instrumentation reverted, 363/363 tests pass unchanged (this
      project's test count moved since the last Peggle-specific entry
      due to unrelated tooling work — see the Phase C/gamepad and save-
      state entries elsewhere in this log).
      **Picked back up again (2026-08-04, later the same day) and traced
      `0x102148` in full** — it's much larger than the earlier partial
      read suggested (~1.2KB, `0x102148`-`0x1025ec`), a genuine "get or
      load a cached resource by ID" state machine, not just a thin
      gate. Concrete new findings, all read straight from the real
      instructions (`arm-none-eabi-objdump`), not guessed:
      - **The `this` slot 43 is called on is genuinely the app's own
        real `IShell*`, not some other unidentified sub-object.**
        Confirmed by co-location: the same `r5`/`this` value later
        (`0x1025ac`) makes a call through byte offset `0x80` (128
        decimal, `128 / 4 = 32`) — this project's own already-
        implemented, already-*confirmed* real slot 32, `GetHandler`
        (`core/brew/ishell.cpp`) — with `r1` loaded from a literal pool
        constant at `0x1025f0` whose raw bytes (`00 55 00 01` LE) are
        **exactly `0x01005500`**, i.e. this project's own already-
        implemented `kAudioMediaCls` constant in `GetHandlerImpl`,
        called with the identical real shape (`this, cls, pszMIME`)
        Double Dragon's own audio system already uses successfully.
        Real Zeebo code sharing one real interface-object shape (and one
        real ClsId constant) across two completely different real games
        is about as strong a cross-title confirmation as this project
        gets.
      - **The function's actual shape**: caches a resource-ID range
        (`[r6+4]`, `[r6+8]`, `r6` threading back to the same "resource
        ID" value the whole outer call chain carries) — a fast path if
        the requested ID falls inside the cached range, a slow path
        (calling slot 43 a *second* time with a different real argument
        shape) if not, and, only if that second slot-43 call itself
        returns 0 ("not found"), a `GetHandler(AEECLSID_MEDIA)` fallback
        whose result gets conditionally written into the caller's own
        out-param (`*r8`, `strne r0,[r8]` at `0x1025c4`) — matching the
        second, empirically-observed live slot-43 call from the last
        round's `return 35` experiment (`r1=0x8038ecd0`, a real pointer)
        exactly: that second call's *argument*, not just its return
        value, is real, live-observed data flowing through this same
        traced path.
      - **Still not enough to safely implement.** The final write to the
        array slot `0x105b5c` later dereferences depends on the *whole*
        state machine's outcome (which of the three paths — cache hit,
        second slot-43 lookup, or `GetHandler` fallback — actually runs
        for the real resource ID Peggle asks for at this point), not a
        single scalar. Getting this wrong would write a plausible-
        looking but incorrect object into a slot real code then calls
        through unconditionally — worse than leaving it a loud crash.
      **Net assessment after two full rounds on this specific gap**: the
      "no real BREW MP `IShell` header" framing from the original
      investigation was accurate in spirit even though a header was
      found — the real blocker was never the header, it's that slot
      43's real behavior is entangled with a genuine, substantial
      resource-caching state machine specific to this call site, not a
      one-line lookup a header alone would resolve. Next concrete step
      for whoever picks this up: instrument `[r6+4]`/`[r6+8]` and the
      real resource ID live to see which of the three paths actually
      fires for Peggle's real first call, then implement only that path
      first rather than the whole state machine at once. All temporary
      instrumentation reverted, 364/364 tests pass unchanged.
      **Followed that exact next step immediately and it paid off --
      real further progress, confirmed live.** Traced what `r4` (malloc'd
      pointer, source of the empirically-observed real second-call
      argument from two rounds ago) actually was: `context->offset 0x6c`
      -- this project's own already-implemented, already-confirmed
      `kMallocSlotOffset` (`core/brew/mod_runtime.cpp`). That's the
      missing piece: slot 43 is called in a real **two-phase query/fetch
      pattern**, same shape `LoadResDataEx`'s own `-1`-sentinel size-only
      query already uses, just with a `NULL`-buffer sentinel instead of
      `-1`: call 1 `(this, cpBuf=NULL, pdwSize=&out)` asks for a size;
      real code `malloc`s exactly that much; call 2
      `(this, cpBuf=<malloc'd buffer>, pdwSize=&out)` asks it to fill
      that buffer. **Also corrected a real misreading from the last
      round**: the earlier "returns exactly 35" finding is call 1's own
      *internal* gate value (`peggle.mod 0x102190`, `cmp r0,#35`), not
      what the whole enclosing function (`0x102148`) returns to *its own*
      caller (`0x102078`, which only treats a literal `0` as success) --
      conflating those two was exactly why the previous round's "return
      `35` from both calls" experiment still crashed at the same place,
      only 3 steps later, and looked like it hadn't worked. Patched the
      stub properly this round (call 1: write a placeholder size,
      return `35`; call 2: return `0`, ignoring buffer contents -- real
      code frees this buffer immediately after call 2 without this
      specific function ever reading it back, so contents didn't matter
      for getting *this* far) and reran against real Peggle:
      **substantial genuine progress** -- two full real construction
      cycles now succeed end to end, each correctly chaining into the
      already-implemented, already-working real
      `GetHandler(AEECLSID_MEDIA)` -> `CreateInstance` pair, and beyond
      that into **two brand-new real trap addresses never reached in
      this title's investigation before** (fresh HLE call shapes, not
      yet identified) -- real step count before the next gap rises from
      588 to 631, and the crash itself moves to a different, later
      real invocation of the same construction path (a third resource
      ID, not yet unblocked by this generic placeholder). **Still not
      committed as a real fix** -- the placeholder size/contents are
      arbitrary, not evidence-grounded, so this stays investigation-only
      until the two new real trap call shapes are identified and the
      real size/content contract (not just the control-flow shape) is
      confirmed; shipping the placeholder as-is risks quietly wrong
      behavior look like progress. Concrete next step: identify the two
      new real trap call shapes just reached (first time this title's
      investigation has seen them) -- that's very likely the real
      `IMedia`/audio bring-up path, given the `AEECLSID_MEDIA` ClsId
      immediately preceding them. All temporary instrumentation
      reverted, 364/364 tests pass unchanged.
      **Immediate follow-up, same day: identified both new calls, and
      it's genuinely exciting.** Instrumented `MediaHle`'s own
      `RegisterNotifyImpl`/`SetMediaParmImpl`/`PlayImpl` plus slot 43
      itself and reran with the two-phase fix from the previous entry
      still applied. Trap `0xf0000a24` is real `SetMediaParm` (fired with
      `paramId=1`/`MM_PARM_MEDIA_DATA`, confirmed by the print firing);
      trap `0xf0000a18` (one MediaHle vtable slot before it) is
      `Release` -- a real, already-correctly-handled no-op in this
      project's stub convention, not a gap. **The real result: this
      exact real `GetHandler(AEECLSID_MEDIA)` -> `CreateInstance` ->
      `SetMediaParm` -> `Release` cycle -- already-working machinery
      this project built for Double Dragon -- now runs successfully for
      Peggle too, eight full times in a row** (`MediaHle` objects
      `0x80200000` through `0x8020001c`, each getting a real
      `SetMediaParm(paramId=MEDIA_DATA)` call with no failure). Eight is
      a real, meaningful number here, not incidental -- matches this
      project's own established "small pool of shared per-character
      sound channels" finding from the very first sound investigation.
      **The remaining crash is provably a different gap, not a ninth
      audio channel**: no ninth slot-43 call or `SetMediaParm` call ever
      fires: the same `0x105b70`/`0x108ba8` crash happens immediately
      after the eighth cycle's `Release`, preceded only by the same two
      `DBGPRINTF`-shaped calls that open every `0x108a90` invocation --
      i.e. whatever comes right after the 8-channel construction loop
      finishes hits the identical vtable-relative-thunk-on-a-zero-slot
      crash shape again, for something that isn't audio-channel-shaped
      at all (it never reaches slot 43). Not pursued further this round
      -- a good, natural stopping point: real audio-channel bring-up for
      a second title is a substantial, concrete win on its own, and
      whatever's immediately after it is a distinct next investigation,
      not a continuation of this one. All temporary instrumentation
      (including the two-phase slot-43 stub, still not a real committed
      fix -- see the previous entry's own caveat) reverted, 364/364
      tests pass unchanged.
      **Same day, continued: traced the crash's own caller chain
      statically** (`arm-none-eabi-objdump` again, no live run this
      round -- exactly one real call site targets `0x108a90` in the
      whole binary, found by grepping the full disassembly for real
      `bl` targets, so this is exhaustive, not a guess). Real caller
      `0x106d34(index)` routes to `0x108860(...)` for `index < 7` or
      `0x108a90(..., index - 7, ...)` for `index >= 7` -- i.e. this
      project's own observed `0x108a90` "`arg1`" values (`0..7` across
      the eight successful channel builds) are real indices `7..14`,
      one-rebased. `0x106d34` itself is called from exactly one real
      site too (`0x12aca4`), inside a loop whose real termination check
      compares a 64-bit accumulator (`[r4+20]`, incremented by a
      register-loaded step each pass) against a stored target pair --
      not a simple bounded counter visible from static disassembly
      alone, so the real "how many channels" answer depends on a live
      runtime value this round didn't capture. **Best current read**:
      the ninth call (real index 15) hits `0x102078`'s own early-exit
      guard (`r4==0` or `[r5+4]==0` or `r6==0` -> return `14` without
      ever touching slot 43 or `*r6`, see this function's disassembly
      in the entry above) -- consistent with either a real, correct
      8-channel bound this generic placeholder stub doesn't respect
      (most likely: real slot-43 data would encode the real channel
      count somewhere this project's arbitrary `size=64` answer doesn't
      provide, and the accumulator loop reads that back from further
      real state this round didn't trace), or a genuine off-by-some
      real bound unrelated to slot 43 at all. Not resolved -- the next
      step needs a live trace of `0x106d34`'s own accumulator, not more
      static reading. No temporary instrumentation was added this round
      (static analysis only); 364/364 tests unchanged.
      **Cross-referenced Zuma's Revenge again (same technique that
      cracked the `0x0101eb0b` gap originally) and it changed the
      picture on slot 43.** Extracted `zumar.mod` (not previously done
      this round; it's only ever been used for the one earlier shared-
      helper cross-reference) and searched its full disassembly for a
      real call through the same vtable byte offset `0xac`. Peggle's own
      `0x102078`/`0x102148` functions are **not** byte-shared with Zuma's
      Revenge (an exact-prefix search of Peggle's own call chain came up
      empty in `zumar.mod`) -- unlike the earlier `0x0101eb0b` find,
      this part is genuinely per-title game logic, not shared SDK
      boilerplate. But Zuma's Revenge has its *own*, independent real
      call through offset `0xac` (`zumar.mod 0x177fa8`), and its calling
      convention is a much cleaner, more legible fit for real
      `DetectType(cpBuf, pdwSize, cpszName, pcpszMIME)`: `r0` (this),
      `r1` = a real, non-null computed buffer address (`streamBase +
      cursor`, from a stream/buffer descriptor struct read at the call
      site), `r2` = `&remaining_capacity` (pre-populated with a real
      computed value, `capacity - cursor`, *before* the call -- an
      actual in/out size, not a zeroed placeholder), `r3 = 0`. The
      caller branches on the *return value* being `0` (falls through to
      a `GetHandler`-based media-subtype fallback checking for exactly
      `AEECLSID_MEDIA+1`/`+2`/`+10` -- two of which,
      `0x01005501`/`0x0100550a`, are this project's own already-
      registered real ClsIds from the Double Dragon investigation) vs.
      nonzero (parses further via a 3-byte compare). This is a real,
      coherent "sniff this buffer, tell me what's in it" shape -- unlike
      Peggle's own call, which always passes `cpBuf=NULL`/`cpszName=NULL`
      (nothing to sniff at all). **Net read**: slot 43 probably is
      really `DetectType` after all (reversing round one's skepticism),
      but Peggle's own specific call site is a degenerate "detect
      nothing, just want a placeholder/default answer" invocation whose
      real expected behavior (given genuinely nothing to detect from)
      isn't obviously "return 35" from `DetectType`'s own documented
      contract -- a real, honest `DetectType` given null everything most
      plausibly returns `0` (nothing detected), which is exactly what
      the current safe `Stub` already does, meaning **slot 43 acting
      like a real, honest `DetectType` doesn't explain what Peggle's own
      caller wants at all**. Left this thread here rather than force
      another guess on top of an already-uncertain foundation. No code
      changes; investigation and cross-referencing only. 364/364 tests
      unchanged.
      **Went back for two more rounds and corrected a real mistake in
      this thread's own mental model, via live ground truth instead of
      more static guessing.** First, added a live diagnostic reading a
      specific arena address (`0x80050000`, this project's own
      documented placeholder for the still-unidentified
      `context[0x24]+0x45000+0x3dc` field, see `tools/game_probe.cpp`)
      alongside every slot-43 call, expecting it to explain the earlier
      "8 successful channels" finding -- it stayed flat zero across all
      eight, disproving that theory outright rather than confirming it.
      Followed up with a real, targeted register trace (temporary,
      reverted -- printing r0/r1/r2/r4/r5/r6 for every step in
      `peggle.mod`'s `0x108a90`-`0x108bb0` range, cheap because it's a
      narrow PC window, not a full instruction trace) and got the actual
      ground truth: **`0x108a90` is entered exactly once** for this
      whole run, with a real, non-arena heap pointer as `this`
      (`0x803454f0`) and `arg1=0` -- not eight separate invocations for
      eight channel indices, as every earlier round in this thread
      assumed from the outer step-count/object-address evidence alone.
      All eight real `SetMediaParm`/`Release` cycles happen **inside
      the single call to `0x102078`/`0x102148`** this one `0x108a90`
      invocation makes -- i.e. `0x102148`'s own "state machine" has an
      internal loop processing multiple real sub-resources for one
      logical channel, not a construction attempt per channel index.
      **And critically**: right before the crash, the trace shows
      `array[0].field4` (the exact address `0x102078` was supposed to
      populate) is **still zero** after all eight sub-resource cycles
      apparently succeeded -- meaning this project's own placeholder
      slot-43 answers are letting `0x102148`'s internal loop *run*
      (eight full passes) without ever satisfying whatever real
      condition would make it actually write the expected result. The
      generic "always succeed" stub isn't just incomplete, it's
      actively producing a different, misleading shape of "success"
      than what real data would. **This overturns the "eight real
      channels, ninth is a different gap" framing from three rounds
      ago** -- there's only one channel/index in play here, and the
      real gap is entirely inside `0x102148`'s own loop termination
      condition, not a separate upstream guard. TASKS.md's own prior
      entries in this thread are left as-written (accurate reports of
      what was observed at the time) rather than rewritten, but this
      correction supersedes their interpretation. All temporary
      instrumentation reverted, 364/364 tests pass unchanged. **Given
      three rounds now (the DetectType cross-reference, the arena
      diagnostic, and this register trace) without a real fix landing,
      and each new round revealing the previous round's model was
      wrong in some way** -- this is a strong signal that guessing at
      `0x102148`'s real per-sub-resource termination condition without
      real resource-format knowledge is not converging. Recommending a
      pause on this specific thread rather than a fourth round.
      above) and closed it: a twentieth real static-base table slot,
      `0x1b4`.** Real call shape `(dest, count, cap=4, ctor_fn)` matches
      a compiler-generated "construct N array elements" RVCT/EABI
      helper — but with no element-stride argument exposed anywhere in
      the real calling convention, implementing real construction would
      mean guessing an offset and silently writing to the wrong
      addresses. Registered as a safe no-op instead, the same treatment
      already given every other under-evidenced slot in this table.
      **Verified against real Double Dragon**: the crash is gone;
      execution now runs at least 20 seconds past the previous stopping
      point, still cleanly simulating input. No regression on Peggle or
      Super BurgerTime. 1 new test; 292/292 tests pass. See
      PHASE8_LOG.md.
      **Confirmed a genuine first for this title: real code now draws
      correct content** (`DrawRect`/`DrawText`/`Update` all firing;
      verified directly against the framebuffer itself, not just the
      call sites — every one of 307,200 pixels reads real white
      content, zero black). Tried hard to get a live visual
      confirmation (screenshots, a raw X11 `xwd` dump, a synthetic
      red-frame sanity test that *did* render correctly through the
      same code path) but couldn't get the real content to show on
      screen in this sandboxed test environment despite every SDL call
      reporting success — likely an environment/compositor quirk given
      the synthetic test proved the actual rendering pipeline correct,
      but deliberately not guessing a "fix" for something not
      confirmed broken. Also noted honestly: real code draws exactly
      once and doesn't redraw afterward in the traced run — not yet
      determined whether that's correct or a further gap. Investigation
      only, all temporary instrumentation reverted, 292/292 tests pass
      unchanged. See PHASE8_LOG.md.
- [ ] Validate the HLE against a third real game (Super BurgerTime),
      started after pausing the Peggle-specific investigation above —
      untapped territory, and a useful check that the HLE core
      generalizes rather than being overfit to two titles. Ships as one
      of the classic-arcade ports: loose per-asset files plus a
      `"PACK"`-magic `.pkg` container, a third real asset-container
      shape distinct from both prior titles (GGZ, `resources.bar`) —
      not yet cracked (deliberately not assumed to be a byte-exact
      Quake PAK just because of the magic-byte coincidence; the actual
      directory-offset fields don't parse coherently under that
      assumption), deferred until asset loading is actually reached.
      **Found and fixed a real, foundational CPU gap**: the entire
      ARMv6 "Extend" instruction family (SXTB/SXTH/UXTB/UXTH + their
      accumulate forms) was unimplemented — the very first real
      instruction Super BurgerTime executes beyond the common
      `AEEMod_New` prologue is a real `uxth r0, r0`. Confirmed the real
      encoding empirically (assembled each mnemonic, read back the
      actual bytes) rather than from memory, and implemented the whole
      closely-related family in one pass. **Verified**: `AEEMod_Load`
      now runs 742,000+ real steps past the previous immediate failure.
      **Hit a new, much deeper wall immediately after**: a real function
      returns via the APCS `ldm sp,{fp,sp,pc}` stack-frame convention,
      and the popped return address is `0` — the same "wander outside
      the module, coincidentally re-enter from the start" pattern
      already seen for Double Dragon/Peggle, except this time it's a
      CPU/stack interaction gone wrong deep inside the module's own
      compiled prologue, before any HLE surface is reached — a
      materially different kind of gap than any fixed so far. Not yet
      root-caused; tracked as the next concrete step. See PHASE8_LOG.md
      for full evidence.
      **Root-caused precisely, down to the exact mechanism** (temporary
      watchpoints, all reverted): a real ARM ROPI relocation-fixup loop
      (module offset `0x100040`-`0x100054`) correctly processes its
      real, 82,480-entry fixup table once — then a separate real
      "clear it" loop zeroes the table's own memory — then the *same
      fixup loop runs a second time* over the now-zeroed range, and
      every "entry" reading back as `0` makes every iteration
      degenerate to the same target address (its own relocation base),
      corrupting real code there a little further on each of ~82,480
      iterations. That's what corrupts the `uxth`/`mov ip, sp`
      instruction before it executes, which cascades into the garbage
      `fp` and the eventual null return. **Why the fixup loop runs
      twice isn't resolved** — every instruction in the loop re-verified
      correct against direct memory reads; finding the loop's *caller*
      (comparing `lr` at both entries) is the concrete next step. This
      is the deepest and most different kind of gap found in this
      entire investigation across all three titles — real, correctly-
      emulated CPU execution hitting a self-inflicted data corruption
      bug in the module's own relocation logic, not a missing HLE call
      or CPU instruction. See PHASE8_LOG.md for the full mechanism.
      **Correction: that framing was premature.** A one-shot watchpoint
      on the very first corrupting write found it happens during the
      game's first, ordinary pass through its own relocation loop, not
      a replay. **The real cause: this tool's emulated stack pointer is
      a fixed `kBase + 0x200000` offset that safely cleared Double
      Dragon's and Peggle's much smaller `.mod` files, but lands
      *inside* Super BurgerTime's 2.8MB `.mod` — specifically inside
      the exact address range its own real relocation-fixup table
      occupies.** Reading that table off this tool's empty stack
      returns zero mid-walk, and the loop's own real logic degenerates
      into repeatedly corrupting its own relocation base (the `uxth`
      instruction) before it executes — one root cause explaining every
      symptom from both entries above. **Fixed** by sizing the stack
      offset relative to the real module (`kBase + mod_size +
      0x200000`) so it can't collide with any module regardless of
      size. **Verified**: `AEEMod_Load` now completes cleanly,
      `CreateInstance` succeeds too, and execution reaches 40,095 real
      steps into `HandleEvent(EVT_APP_START)` before a new, unrelated,
      much later gap (a coprocessor instruction/SWI encoding at module
      offset `0xa0`). No regression on Peggle or Double Dragon; 250/250
      tests pass. See PHASE8_LOG.md for full evidence.
      **Two more static-base slots found and fixed in quick
      succession** (same "unwritten table slot → null function pointer"
      shape as all fourteen already confirmed): slot `0x40` (called
      `(applet_ptr, a 512-byte stack buffer, size=0x200)`, no confirmed
      match) and slot `0xc` (reached via a standalone trampoline, sits
      in the same cluster as `MEMCPY`/`MEMSET`/`STRCPY`/`STRLEN`, a
      plausible `STRCAT`/`STRCMP` sibling but not confirmed) — both
      registered as safe no-ops. **Verified**: each advances real
      execution measurably (40,095 → 40,177 → 40,254 real steps).
      **Hit a new, differently-shaped wall right after**: an `S=1 with
      Rd=R15 (SPSR restore)` exception at `pc=0x3000000b`, an address
      far outside any real range — some upstream register computation
      produced outright garbage rather than the usual clean null.
      **Traced back precisely: it's the same clean null-pointer wander
      as always, but the root cause is a materially different kind of
      gap.** The null itself comes from `r2` (dereferenced two calls
      deep) being read from a real module global, `0x2e28fc`, that
      falls *inside* the relocation table's own reclaimed scratch
      range — real code expects some other, not-yet-executed real
      initialization to have turned that memory into real BSS/heap
      data by now, and nothing in this codebase's current execution
      path does. Unlike the sixteen static-base slots (missing
      *system* function pointers, safely stubbable), this is a missing
      piece of the *game's own* init sequence — guessing a placeholder
      value here risks silent wrong behavior, not a clean crash, so
      deliberately not guessed at. Finding what real code should
      populate it (likely needs tracing `CreateInstance`'s own body,
      not yet disassembled this deep) is the next concrete step.
      **Followed that lead**: `CreateInstance`'s own real body, traced
      directly, does completely ordinary real work with the guessed
      ClsId (a real class-dispatch lookup, real applet-struct
      construction, a genuine `ISHELL_CreateInstance(0x01001001)` call
      — the same real `AEECLSID_DISPLAY` Double Dragon confirmed) —
      looks like a real, matched code path, not a silently-accepted
      wrong guess. A memory watchpoint spanning the entire run confirms
      `0x2e28fc` is written to exactly twice, both during `AEEMod_
      Load`'s own relocation bootstrap, never by `CreateInstance` or
      anything in `HandleEvent` before the crash. This rules out "wrong
      ClsId" as the explanation and narrows the gap to `HandleEvent`'s
      own real control flow upstream of the crash site — not yet
      disassembled that deep; picking this back up should start from
      `HandleEvent`'s own entry (`0x0010b5b4`), not the crash site.
      **Followed through, and Super BurgerTime now reaches its real
      steady-state event loop.** Traced `HandleEvent` forward from its
      own entry: it's a thin real `AEEApplet`-template wrapper that
      calls the game's own handler (stored at applet+`0x18` by
      `CreateInstance`) before doing further built-in processing —
      and *that* built-in processing calls
      `ISHELL_CreateInstance(shell, ClsId=0x01001017, ppObj=&g_2e28fc)`,
      the same global this investigation already proved was otherwise
      untouched. `0x01001017` wasn't registered, so `CreateInstanceImpl`
      correctly failed and correctly left the output unwritten — but
      real code never checks the status and dereferences the still-null
      result two instructions later. **Not a new kind of gap after
      all** — the same "unchecked CreateInstance failure" pattern
      already solved for Peggle and Double Dragon. Fixed with the same
      generic scaffold treatment. **Verified**: `HandleEvent` now
      returns real success, and execution reaches "Reached the event
      loop with no unhandled instruction!" — the same milestone already
      hit for Double Dragon and Peggle, now on a third, independently-
      compiled title, stable over a 30-second run. No regression on
      either prior title; 250/250 tests pass.
      Overall this round: went from unable to execute a single real
      instruction past the common prologue to a third real commercial
      title reaching its steady-state event loop, across six
      independent, verified fixes (the ARM Extend instruction family,
      the stack/module address collision, three static-base slots, and
      this unidentified class). Super BurgerTime was substantially
      harder than Double Dragon/Peggle were at the same stage — a much
      larger `.mod`, a different still-uncracked asset container
      (`.pkg`), and a whole new bug category (the stack/module
      collision) neither prior title triggered.
      `resources.bar`/`.pkg`-style asset loading remains uncracked for
      this title (same as Peggle) — the natural next phase now that the
      steady-state milestone itself is reached. See PHASE8_LOG.md for
      full evidence.
      **Correction after starting on it**: cracking `.pkg` isn't
      actually the next concrete step. Found a real, substantial lead
      first — a string cluster (`"roms\neogeo"`, `"%s\%s.pkg"`,
      `"Loading romset %s"`, the real `"PACK"` magic literal) strongly
      suggesting this title embeds a generic, real, multi-system
      arcade-emulation core that loads ROM data via a conventional
      `.pkg` "romset" container, matching the project's earlier format
      survey — but no code in the compiled binary references these
      strings, and (more importantly) **nothing reaches file loading
      of any kind, packed or loose, and `ISHELL_SetTimer` is never
      called even once** across a 30-second driven run (both checked
      live, temporary instrumentation reverted) — unlike Double Dragon
      and Peggle, which both call `SetTimer` at least once. Reaching
      any asset load needs first understanding what drives this
      title's loop forward at all post-`HandleEvent`; given the
      arcade-core framing, it's also plausible the whole real game
      loop runs synchronously *inside* a single `HandleEvent` call
      (an old-style polling loop) rather than the event-driven
      `SetTimer` model the other two titles use — not yet
      distinguished. See PHASE8_LOG.md for full evidence.
      **Resolved**: `HandleEvent` is short (38 real HLE calls total,
      confirmed) and returns cleanly — not a synchronous full game
      loop. Its very last real call is on the class the previous round
      registered a generic scaffold for (`0x01001017`): slot 7 (byte
      offset `0x1c`) called with `(this, flag=0x4000,
      callback=0x11c06c, user_data=0)`. `0x11c06c` is real ARM code
      whose body is a textbook "process a list of registered objects,
      or return immediately if empty" shape — a real per-frame "run
      one engine tick" function, matching the arcade-core structure
      already suspected. The generic no-op scaffold silently discarded
      this registration, so the callback was never invoked. **Fixed**:
      added `IShellHle::ScheduleTimer` (refactored out of the existing
      `SetTimerImpl`) and scheduled this specific callback through the
      same real timer mechanism Double Dragon/Peggle's own loops use,
      on an *inferred* 16ms cadence (the real call site doesn't
      provide an explicit interval) — marked clearly as an inference,
      not confirmed real behavior. **Verified empirically**: tick 0
      now fires for the first time in this title's investigation, and
      real code runs inside the callback (real `MALLOC` calls) before
      hitting a new, deeper gap 95 steps in — direct confirmation the
      hypothesis was correct, not just plausible. No regression on
      Peggle/Double Dragon; 250/250 tests pass. Tracing the new gap
      inside `0x11c06c`'s own body is the next concrete step — a fresh
      thread, not yet started. See PHASE8_LOG.md for full evidence.
      **Continued into it**: two more static-base slots. Slot `0xd0`
      is called with `(name="boot", heap_object)` — `name` points at a
      real, in-module ROM manifest (`"boot\0boot.rom\0zupa_p1.rom\0
      zupa_s1.rom..."`; "zupapa" is this arcade original's real
      Japanese title) — confirming real romset-loading code is now
      actually executing. Fixing it (safe no-op) took the callback
      from 95 to 2,883 real steps, including a real pass through the
      `.pkg`/`"roms\neogeo"` string cluster the previous round found
      unreferenced — direct proof that code is live, not dead. Slot
      `0x184` is called `(flag=1, 0, table)`, too thin to identify;
      fixing it (same treatment) changes the failure mode entirely —
      real code no longer wanders, it settles into a genuine infinite
      polling loop (alternating between two real calls forever),
      almost certainly waiting on a real condition tied to the same
      ROM-manifest loading, that a static no-op stub can never satisfy.
      **This connects back to and validates the `.pkg`/romset question
      set aside two rounds ago** — real further progress now likely
      needs actually implementing romset loading, not another
      static-base slot. A substantially bigger undertaking than the
      incremental fixes so far; not attempted this round. No
      regression on Peggle/Double Dragon; 250/250 tests pass. See
      PHASE8_LOG.md for full evidence.
      **Traced the polling loop instead of assuming its cause**: root
      cause was a frozen emulated clock, not romset loading directly —
      `GetUpTimeMs` only ever advanced via the outer per-frame `Tick()`,
      which this loop's tight, single-native-call busy-wait never gave
      a chance to run. Fixed by having `GetUpTimeMsImpl` self-advance
      1ms on every read (still deterministic, inferred rate). Real
      code then breaks the first spin (~50 iterations, was infinite),
      makes 5 more genuine HLE calls (real `strcpy`, method calls on
      the SBT-specific class object), then hits a **second**,
      identically-shaped loop that does *not* resolve even with the
      clock advancing — confirms that one really is waiting on ROM-data
      readiness, not just elapsed time. Romset loading remains the
      next real undertaking. Also found and documented, while
      re-verifying no regression: this tool's `cls_id` argument must be
      each module's *real* embedded ClsId (found via trace, a literal
      compared against ClsId in `CreateInstance`'s first ~20
      instructions), not its download-folder number — the two only
      coincide for Super BurgerTime; Double Dragon/Peggle need
      `0x0102f789`/`0x01099cd6`, not `274754`/`278962`. 251/251 tests
      pass. See PHASE8_LOG.md for full evidence.
      **Traced the second loop precisely — the clock fix above works
      correctly** (confirmed the same bounded ~32ms wait function now
      resolves reliably, called thousands of times). The real remaining
      wall is one level up: the real per-frame callback (`0x11c06c`)
      walks a one-entry module-global "task list" (our SBT object) and
      calls three of its vtable slots every pass; every single code
      path loops back unconditionally, and nothing ever clears the
      list-head pointer to let it exit — that would need to be a real
      side effect of one of those slots' real implementation (almost
      certainly self-deregistering once the romset load it's presumably
      driving completes). Refines rather than replaces the
      romset-loading conclusion: implementing real, stateful behavior
      for class `0x01001017` (not just a generic scaffold) is the
      actual next step. Not attempted this round — diagnosis only, all
      trace instrumentation reverted, no functional changes. See
      PHASE8_LOG.md for full evidence.
      **Implemented the minimal fix and it worked**: a live memory
      watchpoint confirmed `0x2e28fc` (the same address `CreateInstance`
      writes the SBT object into) is real code's one and only exit
      condition, never cleared by anything reachable. Added a second
      override (slot 11 / byte offset `0x2c`, confirmed to be the real
      "tick" method) that clears it on the first call — an honest,
      minimal placeholder, not a claim about real loading time.
      **Super BurgerTime now reaches "Reached the event loop with no
      unhandled instruction!"**, matching Double Dragon/Peggle. Past
      it, hits a new, different gap ~1M real steps into the first tick:
      an `UnimplementedInstruction` at module offset `0x9c` where the
      *live* instruction word doesn't match the raw `.mod` file's
      content at the same address — a real return address whose target
      memory changed between call and return. Not yet understood; flagged
      as the next thread, deliberately not chased this round. No
      regression on Peggle/Double Dragon; 251/251 tests pass (fix is
      entirely in `tools/game_probe.cpp`, not core HLE). See
      PHASE8_LOG.md for full evidence.
      **Root-caused and fixed that gap.** A live watchpoint found every
      corrupting write came from the module's own ROPI relocation-fixup
      loop, and a second watchpoint on the veneer's own entry found it
      genuinely gets re-entered a second time, deep in tick 0, by real
      code — not a tool artifact. Traced the real call chain precisely:
      this project's own earlier placeholder fix (clearing the
      single-entry task list's head pointer from inside vtable slot
      `0x2c`, "tick") cleared it one real sub-call too early — the same
      per-frame walker unconditionally calls slot `0x28` on the same
      object immediately afterward, in the same pass, dereferencing the
      now-null entry, jumping to address 0, wandering 262,144 harmless
      steps, and coincidentally re-entering the module's own load
      address — re-running the relocation veneer over its already-
      consumed, zeroed table, which is what corrupts real code at
      offset `0x9c`. **Fixed** by moving the list-head clear to slot
      `0x28` (the last of the three per-pass sub-calls) instead of slot
      `0x2c` (the first) — same honest placeholder, just later-firing.
      **Verified**: a 45-second sustained run past the event loop
      produces no wander, no crash, and a rich, varying real HLE call
      stream every tick — not the fixed small loop Peggle's own
      investigation paused on. No regression on Double Dragon/Peggle;
      259/259 tests pass. See PHASE8_LOG.md for the full trace.
      **Cracked `.pkg` from scratch (unlike Peggle's still-uncracked
      `resources.bar`).** Real header: `"PACK"` magic, entry count,
      offset of a trailing zlib-compressed filename table. Real 20-byte
      directory records (found by scanning for zlib magic bytes and
      test-decompressing each candidate to separate real entries from
      coincidental byte matches — 7 of 12 candidates decompressed
      cleanly, matching the declared count exactly): hash, compressed
      size, decompressed size, offset. Confirmed end-to-end: the
      filename table decompresses to 7 real, legible names (`gc05.bin`,
      `gk03`, `mae00.bin`, ...) landing exactly on the entry count.
      Implemented as permanent, tested code following the established
      `GgzArchive`/`Obm1Image` pattern: `core/loader/pkg.{h,cpp}`,
      `tests/pkg_test.cpp` (synthetic fixtures only), and
      `tools/zeebulator_pkg_inspector`. Verified against the real file:
      all 7 entries parse and extract cleanly. 266/266 tests pass (7
      new). Incidental finding: none of the 7 real names match the
      `"boot"`/`zupa_p1.rom` names an earlier round found referenced by
      a real manifest string — this `.pkg` holds general assets, not
      the arcade romset itself; that romset's real source is still
      unlocated. See PHASE8_LOG.md for the full derivation.
      **Found precisely why tick 0 never returns even with the crash
      fixed**: a long (170s) run confirmed it's a genuine, very long
      real polling loop, not a fast failure. A temporary `OpenFile`
      trace found real code tries six candidate paths (`.\boot.pkg`,
      `roms\neogeo\boot.pkg`, `roms\neogeo\boot\boot.rom`, ...) for a
      file named `boot.pkg` — a distinct, shared bootstrap/romset-
      selector file this game's own download folder doesn't contain
      (unlike `supbtime.pkg`, this game's own asset package, already
      cracked above). Given the shared manifest spans several unrelated
      games under one arcade core, `boot.pkg` looks like a system-level
      file every real device would have, not part of any individual
      game's download. Not pursued further — sourcing it needs explicit
      user go-ahead first, per this project's standing convention. All
      temporary instrumentation reverted; 266/266 tests pass.
      **User authorized sourcing `boot.pkg` from this project's own
      sanctioned local archive** (not a new download). Found it on the
      first check — bundled in a *different* title's own download,
      `Karnov's Revenge.zip` (`mod/279126/boot.pkg`) — confirming it's
      the shared, system-level file the manifest evidence predicted.
      Cross-validated this project's own `PkgArchive` (built entirely
      from Super BurgerTime's file) by parsing this second,
      independent real file with zero code changes; its one entry
      (`boot.rom`, 8192 bytes) decodes to a real-looking 68000-style
      exception vector table. Wired in as a new, permanent, optional
      5th CLI argument (`MergeBootPkgInto`), registering it under all
      six real candidate paths found earlier. **Verified**: real
      Super BurgerTime now runs a genuinely new, much longer sequence
      — many new real file-I/O HLE calls fire — reaching 262,900 real
      steps (previously never completed a single HLE call's worth of
      real progress) before a new gap. Traced precisely: a real 8-byte-
      stride dispatch table read through a correctly-relocated address,
      but the specific slot is unpopulated — the raw file has real,
      sane data there, but it falls inside the same reclaimed-scratch
      range this game's own `0x2e28fc` gap already characterized, so
      it's the same *kind* of "missing real initializer" gap, not new
      territory. Not pursued further this round — a natural stopping
      point after a substantial, multi-part round. No regression on
      Double Dragon. 266/266 tests pass. See PHASE8_LOG.md.
      **Chased the `0x1465b0` dispatch gap to its real writer** (a live
      watchpoint spanning the whole run, temporary/reverted): a real
      "register a handler" function at `0x146150` does write the right
      table slot, using the same index formula the reader uses, so
      it's genuinely reachable — but the value it writes traces back to
      unprocessed `boot.rom` content, not a missing file (a fresh
      `OpenFile` trace shows exactly one real file-open this run, and
      it succeeds). Assessed, not proven: `boot.rom`'s real vectors
      likely point into the game's own 68000 romset code, which would
      mean implementing real nested-CPU-core dispatch to resolve —
      substantially bigger scope than any gap fixed so far, closer to
      Peggle's own paused SDK-header wall. Investigation only this
      round, all temporary instrumentation reverted, 266/266 tests
      pass unchanged. See PHASE8_LOG.md.
      **Root-caused and fixed Double Dragon's real-desktop black-screen
      regression** — the loading screen rendered correctly (confirmed
      via a real frame-content heartbeat log: hundreds of frames,
      byte-identical correct content, zero SDL errors) but the actual
      window went solid black on the user's real (non-VM) Linux Mint/
      Cinnamon desktop roughly a second in, every run, and stayed black
      no matter how long real content kept being pushed. A long,
      methodical bisection (documented fully in PHASE8_LOG.md) ruled
      out every SDL2/X11 API usage pattern individually and in
      combination (GL context creation, `SDL_GL_SwapWindow`, streaming
      textures, audio, controller polling, sustained CPU load, long
      stretches without pumping events) before finding the real
      trigger: real Double Dragon code, after `eglMakeCurrent`,
      genuinely calls `glClearColor`/`glClear` (to black) then
      `eglSwapBuffers` repeatedly, every tick — confirmed via targeted
      `[GLDIAG]`-style tracing (temporary, reverted) showing this start
      right around the same ~1s mark the user reported. Giving the raw
      GL context its own private window (rather than sharing the 2D
      `IDisplay` surface's visible one) did **not** fix it — a minimal,
      independent two-window SDL2 reproduction showed the bug is
      compositor-wide, not shared-drawable-specific: merely having a
      second, real host GL context anywhere in the process (even
      hidden, even never itself touched again) reliably breaks this
      desktop's real compositor (Cinnamon/Muffin, AMD radeonsi/Mesa)
      into never repainting a *completely separate*, GL-untouched
      window again, confirmed recoverable only by an actual interactive
      title-bar drag (forcing the window manager to recompute geometry)
      — a real environment bug, not anything in this project's own
      present logic. Neither disabling SDL's `_NET_WM_BYPASS_COMPOSITOR`
      hint nor forcing continuous synthetic window-position changes
      (once a second, then every frame) worked around it either. Fixed
      by adding `NullGlBackend` (`frontends/standalone/
      null_gl_backend.h`) — a `GlBackend` that answers every IGL/IEGL
      call plausibly without ever standing up a real host GL context —
      and using it in `tools/game_probe.cpp` instead of the real
      `Sdl2GlBackend`, since real GLES rendering isn't a complete,
      correct pipeline yet for this title regardless (the same trace
      found a real, separate, unrelated gap: a degenerate
      `glViewport(0,0,1,0)` call, not pursued further this round).
      Verified fixed directly on the user's real desktop, sustained
      45+ seconds, no regression. Also re-enabled `IDisplayHle::
      RepresentLastFrame()` (was a silent no-op after an over-broad
      `git checkout` earlier this same investigation reverted the
      `Update()`-side plumbing that feeds it without reverting its own
      declaration) and added a periodic in-loop liveness check inside
      `CallArmFunctionChecked` so a single long-running real ARM call
      can't leave the window unrepresented for a full real second.
      292/292 tests pass. See PHASE8_LOG.md.
      **With display fixed, confirmed the real "CARREGANDO..." screen is
      a genuine, correctly-animating loading spinner** (dots cycle 0-3
      normally every tick, via a temporary `DrawText` trace, reverted) —
      not the separate, input-gated title screen this project's earlier
      HID work targeted, which is why the simulated button press has no
      visible effect on it (expected, not a bug). Closed this file's
      long-open `0x01005511` slot-6/`pUser+37` thread as a confirmed
      dead end (real disassembly: it's a one-shot "finalize resource
      list" notification whose return value the caller unconditionally
      discards) and, along the way, corrected a real bug in this
      project's own tracing methodology (a naively-read register at an
      HLE trap can be leftover vtable-resolution scratch, not a real
      argument — see PHASE8_LOG.md for the full mechanism). Located the
      real `DrawText` call site for the CARREGANDO text itself
      (`lr=0x00123cb8`, inside a function starting `0x123cc8`) but not
      yet traced back to its real exit condition — the concrete next
      step, not attempted this round. All temporary instrumentation
      reverted; `git diff --stat` clean, no functional changes this
      round. 292/292 tests pass (unchanged). See PHASE8_LOG.md.
      **Traced it the rest of the way and found there's no remaining
      real bug in Double Dragon's own game logic at all.** Found the
      actual CARREGANDO-drawing function (`0x106098`, called through
      `applet+0x54` — one of this file's own already-tracked per-tick
      dispatch pointers) and, watching it across the full simulated
      button-hold window, confirmed the real per-tick state machine
      transitions cleanly past it (`0x1222f0`/`0x107104` →
      `0x121110`/`0x1063ec` → `0x1220f8`/`0x1070a8`, the first two
      states reproducing this file's earlier HID work exactly). Once in
      the new state, `DrawText`/`DrawRect`/`Update` call counts freeze
      permanently while **19,419 real GL calls** (clears, swaps,
      thousands of real draw-array batches) fire in the same window —
      the app genuinely finishes loading and starts drawing its next
      real screen, entirely through real GLES rendering that this
      project currently, deliberately never displays (`NullGlBackend`,
      this same round's own black-screen fix: a real host GL context
      anywhere in this process reliably breaks this desktop's real
      compositor). **This reframes the remaining gap entirely**: not a
      missing HLE call or an unfound gate, but the same architectural
      tension the black-screen fix already surfaced. A real fix needs
      either resolving/working around the compositor bug some other way
      so `Sdl2GlBackend` can be used again, or compositing the 2D
      `IDisplay` surface through the same single real GL context real
      GLES content uses (one real context total, not two) — not
      attempted this round. All temporary instrumentation reverted;
      `git diff --stat` clean, no functional changes this round.
      292/292 tests pass (unchanged). See PHASE8_LOG.md.
      **Implemented the compositing option, and it works — real GL
      content now genuinely reaches the real screen for the first time
      this project has confirmed.** New `Sdl2UnifiedBackend`
      (`frontends/standalone/sdl2_unified_backend.{h,cpp}`) implements
      both `Backend` and `GlBackend` on one real window/context,
      replacing `Sdl2Backend`+`NullGlBackend` in `tools/game_probe.cpp`
      (`NullGlBackend` removed, no longer used anywhere). Validated the
      core assumption first via a minimal reproduction (single real
      context as sole presenter — stayed correct 15-20s on the real
      desktop, unlike every earlier setup with two presentation paths).
      Fixed a real bug of this round's own making along the way:
      `RepresentLastFrame`'s keep-alive push didn't know the app had
      moved on to real GL rendering, so it kept fighting the app's own
      real GL frames for the same drawable — added `Sdl2UnifiedBackend::
      HasRealGlActivity()` and gated both real call sites on it.
      Confirmed with the user that the simulated button-hold this tool
      injects isn't an artificial requirement — real disassembly
      already established Double Dragon's title screen genuinely waits
      for real HID input on real hardware. **The screen is black right
      now for a real, already-known, separate reason**: real Double
      Dragon code sets `glClearColor(0,0,0,0)` combined with the
      already-found degenerate `glViewport(0,0,1,0)` — confirmed via a
      temporary trace (848 matching real calls of each, alongside
      thousands of real but invisible `GlDrawArrays` calls) — not a
      presentation defect. Tried and kept `glTexSubImage2D` (over
      per-frame `glTexImage2D`) and `SDL_GL_SetSwapInterval(1)` as real
      improvements to a separate, smaller, still-open issue (an
      occasional brief black flicker, reduced but not eliminated — far
      less severe than the permanent blackout this investigation
      started from). All temporary diagnostics reverted, 292/292 tests
      pass. Concrete next step: the degenerate `glViewport(0,0,1,0)`
      call itself — find the real code that computes those dimensions
      and why they're wrong. See PHASE8_LOG.md.
      **Root-caused and fixed the degenerate viewport for real — it was
      never a rendering bug.** A live watchpoint traced the real
      `w=1,h=0` values to a real, unimplemented BREW API:
      `ISHELL_GetDeviceInfo` (`core/brew/ishell.cpp` already correctly
      named it — slot 4 — but left it a blind stub returning zeroed
      output). Confirmed the real `AEEDeviceInfo` struct layout against
      the bundled real SDK header (`cxScreen`/`cyScreen` at offsets
      0/2, an exact match for what real Double Dragon code reads) and
      implemented it for real, defaulted to 640×480 so every existing
      call site keeps compiling. Verified on the real desktop: the
      viewport is now correct (`w=641 h=480`) and real game geometry
      became visible for the first time this investigation — directly
      observed by the user.
      **Also found the simulated button-hold this file has relied on
      since early in this investigation was actively harmful once real
      rendering worked**: even a short ~250ms hold visibly raced the
      game through several real screens before any could be observed
      stably (real code reads a sustained per-tick "held" input as
      repeated discrete presses, not one hold). Removed the simulated
      press entirely — with zero simulated input, the game reaches a
      real, stable title screen on its own and stays there, confirmed
      twice independently on the real desktop. The original "needs a
      held press" assumption (from early HID-investigation rounds,
      before real rendering existed to observe) was never correct.
      Real further navigation is now left to a real human's own
      keyboard/controller input, already separately wired up.
      **New, confirmed-real, separate next gap**: the title screen
      shows black background with white unrendered-texture rectangles
      — real code calls `glGenTextures`/`glBindTexture` many times but
      never once calls `glTexImage2D` (confirmed via live trace:
      thousands of real draw calls, zero texture uploads); real Double
      Dragon almost certainly uses a real compressed-texture upload
      path (`core/brew/gl_hle.h` already flagged "compressed textures"
      as unimplemented; this project's own bundled research references
      real ATITC compression) not yet implemented at all. 293/293 tests
      pass (new `IShellHle.GetDeviceInfoWritesRealScreenDimensions`
      test). All temporary diagnostics reverted. See PHASE8_LOG.md.
      **Sound investigated next**: `MediaHle` (`core/brew/media_hle.h`)
      is a real, complete `IMedia` implementation already wired into
      `tools/game_probe.cpp`'s tick loop (`mixer.Mix` runs every real
      tick) — but `media_hle.Build()`'s object is never registered with
      `shell_hle.RegisterInstance` under any `ClsId`, so any real
      `CreateInstance(AEECLSID_MEDIA, ...)` is guaranteed to fail today.
      The real numeric `ClsId` wasn't found this round: no bundled
      reference header defines it, and a temporary `[DBGCLS]` trace over
      a clean 100-real-second idle run of the stable title screen showed
      Double Dragon makes exactly 8 `CreateInstance` calls total, all
      within the first ~150ms of startup, all belonging to already-
      identified other subsystems (the download/catalog notification
      family, an early core-system class) — zero further calls of any
      kind for the remaining ~99.85s. Real title-screen audio (if any)
      is apparently gated behind real menu/gameplay input this project
      deliberately no longer auto-simulates. A real, large (hundreds-of-
      entries, 0x4c-byte stride) per-asset resource table was found in
      `ddragonz.mod`'s own rodata (anchored on the literal string
      `sound.ggz` at file offset `0x4dae8`) as a promising real lead for
      a static answer, not yet decoded. All temporary instrumentation
      reverted; `git status` clean; 293/293 tests pass. See
      PHASE8_LOG.md's "Sound" section.
      **Textures implemented next, and this is a real, verified win**:
      `glCompressedTexImage2D` (vtable slot 15, was a blind `Stub`) now
      decodes real ATITC (`GL_COMPRESSED_RGB(A)_ATI_TC`) compressed
      textures — confirmed real via this project's own bundled Qualcomm
      `simple_atitc.c` sample — via a new `core/loader/atitc.h`/`.cpp`
      decoder, then forwards the decoded RGBA8 to the existing
      `GlBackend::TexImage2D` path. The block format and (non-evenly-
      spaced!) 4-color interpolation weights were derived empirically
      via least-squares regression against this project's own bundled
      real compressed/uncompressed sample pairs — not from memory or any
      public spec text — reconstruction error ~4.6/channel color, ~0.4
      alpha, matching the format's own real lossy-compression noise
      floor. New `tests/atitc_test.cpp` pins two real byte-for-byte
      blocks from the bundled samples to their derived-and-verified
      decoded pixels. 297/297 tests pass (293 + 4 new) — this part is
      real and independently confirmed, unaffected by the correction
      below.
      **Round two, from the user's own live testing**: some white
      rectangles remain. Real disassembly + a real `data.ggz`
      byte-level check (LR capture → call-site disassembly → real
      gzip-stream extraction, all confirmed against real bytes, not
      guessed) found a real smoking gun: at least one real call into
      this same upload path carries a real embedded resource named
      `Font.obm1` — a format this project already has a working loader
      for (`core/loader/obm1.{h,cpp}`) — being misrouted through the
      generic ATITC upload path instead, producing nonsensical
      width/height (tens of thousands of pixels). This is a real
      **resource-type dispatch bug** upstream of GL entirely. Landed
      one real, permanent hardening fix either way:
      `GlCompressedTexImage2D` now rejects width/height above 4096
      (any real 2009-era mobile GPU's practical max) before ever
      decoding/allocating. 297/297 tests pass (unchanged).
      **Correction (the user directly, correctly disputed the "verified
      visually" claim above)**: re-checked with real success/rejection
      tracing across several fresh runs, including one screenshotted at
      the exact moment the on-screen silhouette was visible. **Zero
      successful `DecodeAtitc` calls, ever, in any run this whole
      investigation has captured** — and zero `glTexImage2D` calls
      either. The only real calls into `glCompressedTexImage2D` are the
      same ten `Font.obm1`-misroute calls, always rejected. The
      "verified visually" claim was a false positive: the on-screen
      shape is the same pre-existing "incomplete texture samples white"
      fallback plus real, already-working mesh geometry, not evidence
      the ATITC decoder ever ran against real game data.
      **Root cause found for real**: dumped the real memory `r6` points
      to at the actual call site (temporary probe, reverted) across all
      ten observed calls — every one starts with live gzip magic bytes
      (`1f 8b 08 08`), meaning real code is reading width/height/format
      from the **still-compressed** stream, not decompressed data.
      Proved it mathematically: the always-rejected `internalformat`
      (`0x8b9d`) is real code computing `gzip_method_byte(8) + 0x8b95`
      — not a format enum at all, an arithmetic artifact. The real
      gzip `FNAME` fields are `Title.obm1`, `Menu_P.obm1`,
      `OptionBG1/2.obm1`, `HowTo*.obm1`, `Font*.obm1` — every single
      real title-screen/menu graphic this game has, all real OBM1
      bitmaps (this project's own already-working
      `core/loader/obm1.{h,cpp}` format), none of them ATITC. The real
      gate: `unknown_0xdc_fn` (static-base offset `0xDC`,
      `core/brew/mod_runtime.cpp`) — a previously-unidentified slot
      this project stubbed years ago — is called first and its return
      value decides whether the rest of this function runs; currently
      an unconditional "proceed" Stub, so every real OBM1 asset runs
      straight through bogus width/height/format extraction against
      raw compressed bytes instead of ever reaching real decompression.
      This finally fully explains the project's original Double Dragon
      finding: `glGenTextures`/`glBindTexture` many times, never once
      `glTexImage2D` with valid data — the real pipeline dead-ends at
      this gate for every title-screen graphic.
      **Fixed for real, and confirmed working end to end.** The design
      question resolved itself on inspection: `mod_runtime.h` already
      documents every sibling static-base slot (`0xd0`/`0x184`/`0x1b4`)
      as a real *external* system-services import (same real mechanism
      `memcpy`/`malloc`/`free` already live in, implemented host-side,
      not traced ARM code inside the `.mod`) — so a host-side real
      `zlib` inflate for `unknown_0xdc_fn`, wired into static-base
      offset `0xDC`, *is* the authentic answer, not a pragmatic
      substitute for it. Implemented as `ModRuntime::
      DecompressGzipInPlaceImpl` (`core/brew/mod_runtime.{h,cpp}`),
      matching `core/loader/ggz.cpp`'s already-established real gzip
      handling. Re-running immediately revealed the real, final piece:
      even with correct data, real code funnels every OBM1 upload
      through the real `glCompressedTexImage2D` vtable slot with an
      internal engine tag (not a real GL enum) as `internalformat` —
      confirmed by checking 8 bytes before every real `data` pointer:
      literally `"OI"` (real OBM1 magic) plus flag/bpp/width/height,
      matching `core/loader/obm1.h`'s independently-reverse-engineered
      layout field-for-field, on every real call, with the declared
      `imageSize` also matching the real palette+pixel-data size
      exactly every time. `core/loader/obm1.h`'s own doc comment
      confirms this isn't a one-off: **all 89 real assets in Double
      Dragon's `data.ggz` are OBM1 — this game never uses real ATITC**.
      `GlHle::GlCompressedTexImage2D` now checks for those real magic
      bytes first and decodes via this project's own already-working
      `Obm1Image::Decode` before ever trusting `internalformat` as a
      GL enum; the ATITC path from the previous round is kept as a
      fallback (dead code for this title, not deleted — no evidence
      yet no other title needs it). 301/301 tests pass (297 + 4 new).
      **Verified on the real desktop, screenshotted**: Double Dragon's
      title screen now renders completely, correctly, in full color —
      real dragon line-art, the real Japanese kanji logo, "DOUBLE
      DRAGON" in gold text, the real Brazilian-Portuguese prompt, and
      the real copyright block. First real target-game title screen
      this project has ever rendered end to end with real texture data.
      See PHASE8_LOG.md's "Fixed for real" section for the full
      derivation.
      **Transparency fixed next** — the user immediately caught a real
      magenta border/background around the graphics. Traced (not
      guessed) via real `glEnable`/`glAlphaFuncx`/`glBlendFunc` call
      values: Double Dragon pairs real `GL_ALPHA_TEST`
      (`glAlphaFuncx(GL_NOTEQUAL, 0.0)`) with real `GL_BLEND`
      (`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`) — the
      standard real GLES1.x sprite-transparency combo. Dumped every
      real OBM1 palette this project has observed: **palette index 0
      is always the exact real magenta `0xF83E`**, zero exceptions —
      confirming a real, simple, index-keyed color-key convention.
      `core/loader/obm1.{h,cpp}`'s `DecodedImage` now outputs a real
      `alpha` channel (index 0 → 0, else 255); `GlHle::
      GlCompressedTexImage2D` uploads real RGBA instead of RGB;
      `glAlphaFuncx`/`glBlendFunc` (previously blind Stubs) now forward
      real arguments to both real `GlBackend` implementations. 303/303
      tests pass. **Verified on the real desktop**: the magenta panels
      behind the logo are gone, real dragon line-art shows through
      cleanly. See PHASE8_LOG.md's "Real transparency" section.
      **Real controller input wired to the keyboard next**, at the
      user's request. Live-traced the whole real chain end to end (a
      real read-watch + write-watch, both reverted): the already-
      present-but-never-fed real HID button callback
      (`ddragonz.mod` `0x11bdf4`) → a real per-tick latch function
      (`0x123740`) → a real combine function (`0x11a2ec`) that ORs two
      real gamepad slots into `applet+0x3618/0x361c/0x3620` — the
      exact real struct this project's own much earlier investigation
      found gates title-screen progression (bit `0x100` at
      `+0x361c`). The callback's own real UID→button translator
      (`0x100740`) only recognizes 10 of 16 real joystick UIDs, all
      confirmed named and numbered against a real bundled Qualcomm
      header (`AEEHIDDevice_Joystick.h`) — full real D-pad, `Back`
      (the confirmed progression button), both upper shoulders, and
      four face buttons. Caught and fixed one real off-by-one in the
      jump table's own address arithmetic by instrumenting the branch
      targets directly rather than trusting hand arithmetic.
      **Verified live, tick by tick**: an injected real button press
      propagates correctly through every stage, matching the known-
      real gate exactly, edge-trigger semantics textbook-correct.
      `SdlKeyToHidButton` (`tools/game_probe.cpp`) now maps arrows/
      Enter-Backspace/Q-E/Z-X-C-V to the real recognized UIDs, wired
      into the existing key-handling loop alongside the classic AVK
      path. All temporary instrumentation reverted. See PHASE8_LOG.md's
      "Real controller input" section for the full derivation.
- [x] Fixed a real freeze: "if I press right the entire game freezes".
      Root cause had nothing to do with Right specifically -- real code
      (`ddragonz.mod` `0x100740`, the UID-translator this project
      already found) clears `*captured_button_context` (the real
      per-device pointer real code passes to the HID button callback)
      back to `0` as its own real cleanup once a full press+release
      cycle finishes, and nothing ever re-populates it, because real
      firmware presumably does that as part of genuinely delivering
      the *next* signal -- a step this project's simulated callback
      injection was skipping. Confirmed live: **any** button pressed
      right after a completed press+release cycle null-pointer-crashed
      the real callback, freezing the display (confirmed via two
      screen captures, seconds apart, bit-for-bit identical -- even
      the title screen's own blink animation had stopped). Fixed with
      one line in `tools/game_probe.cpp`: re-write the field
      (`kHidDeviceObject`) right before every simulated callback
      invocation, mirroring what real signal delivery must do.
      Verified live with 8 real presses in one run, deliberately
      hammering Right repeatedly and interleaved with other
      directions -- every one now succeeds, confirmed by a final,
      still-live `FPS:31` screen capture. 303/303 tests pass. See
      PHASE8_LOG.md's "Real freeze on the second button press" section.
- [x] Reached real gameplay for the first time (past the title screen,
      into Chinatown level 1) and fixed a real bug the user hit
      immediately: pressing Right made "a lot of sprites disappear"
      (confirmed via before/after screenshots -- player, enemy,
      weapon, and the entire HUD gone, only the background tilemap
      left). First had to fix an unrelated tooling trap that wasted
      several repro rounds: `tools/game_probe.cpp` never flushed
      stdout (glibc fully block-buffers non-TTY output by default),
      so a live `tail`/`grep` on its redirected log could show zero
      new lines despite real input being processed -- fixed
      permanently with `std::setvbuf(stdout, nullptr, _IOLBF, 0)`.
      With that fixed, live GL-call tracing (temporary, reverted)
      showed the *same* already-known-about classic-AVK-path bug from
      the freeze fix above is not actually harmless: Right's made-up
      AVK code still makes real `HandleEvent` jump through a null
      pointer and wander through real memory, and this time caught it
      corrupting whatever real per-frame sprite/HUD draw loop reads
      its active-entity list from -- every real texture bind+draw for
      every sprite/HUD element (7-8 of them, every tick) stops
      happening, permanently, the instant this occurs, while the
      background tilemap keeps rendering fine. Fixed at the root
      (`tools/game_probe.cpp`): the classic AVK path now only runs for
      keys with no real HID mapping, since every key this project maps
      already has one. Verified live with 36 separate real Right
      presses in one run, zero throws, user confirmed: "Controller
      seems to be working fine." 303/303 tests pass. See
      PHASE8_LOG.md's "Real gameplay reached for the first time"
      section. Other, not-yet-detailed issues remain (user's own
      words: "There are some other issues though") -- tracked as open
      follow-up, not guessed at here.
- [ ] Investigated the two "other issues": a garbled "J1 %7D"-style
      HUD string, and wrong front/back sprite layering. **Sprite
      layering: five real, confirmed GL gaps found and fixed.** Real
      code enables `GL_DEPTH_TEST` once at startup and never touches
      it again, but (1) no real depth buffer was ever requested for
      the one real GL context -- fixed, and directly confirmed live:
      the door (previously a flat black rectangle) and the health bar
      fill (previously invisible behind its own container) both now
      render correctly; (2) `glDepthFunc`/`glClearDepthx`/
      `glDepthMask` were silent Stubs -- now real, forwarding to
      genuine host GL calls; (3) `glPushMatrix`/`glPopMatrix` were
      *also* silent Stubs, meaning real per-sprite
      "push/translate/draw/pop" scoping was silently broken -- now
      real. **The specific remaining symptom** (individual same-layer
      character sprites still stacking in the wrong order) did not
      fully resolve: live vertex tracing found real per-vertex Z
      *is* correctly extracted, but multiple overlapping character
      sprites all legitimately share the exact same real Z (`-80.0`)
      in the same frame, and real code never calls
      `glTranslatef`/`glLoadMatrixx`/`glMultMatrixx` at all -- meaning
      real hardware likely relies on correct *submission order*
      within that shared layer, not per-sprite Z, and whether this
      project's own submission order matches real intent needs real
      ARM disassembly of the sprite-list ordering logic, not more GL
      tracing. **"J1 x7d" HUD text**: ruled out as a rendering bug --
      traced `DrawText`/`Update`/`SprintfImpl` (none of them fire for
      it) and dumped every real GL texture to confirm the real font
      atlas itself renders correctly; the real bug (not found this
      round) must be in whatever game-state value gets sampled to
      build that string. Both are tracked as open follow-up. 303/303
      tests pass. See PHASE8_LOG.md's "Real depth-buffer
      infrastructure was completely missing" section.
- [x] "J1 %7d" fixed for real. Found the real, complete literal string
      in `ddragonz.mod`'s own bytes (file offset `0x6c0b0`): a genuine
      printf-style format string, not corrupted data (the on-screen
      "%" glyph just resembles an "x"). A live memory read-watch on
      its real runtime address plus a real HLE-trap-index count
      (confirmed via `HleRuntime::Register`'s own arithmetic) traced
      every read to `ModRuntime::SprintfImpl`. Root cause:
      `SprintfImpl` only ever read a single character right after `%`
      as the whole directive, so `"%7d"`'s width digit `7` fell
      through to the "unknown directive" fallback and got copied
      through literally, leaving `d` to follow as an ordinary
      character -- the real integer argument was never consumed.
      Fixed: real support for an optional `0`-flag and decimal
      minimum-field-width between `%` and the conversion character,
      matching standard printf semantics. New test locks in the exact
      real string. Verified live: "J1" now shows a real substituted
      value, zero format-string garbage. **Sprite z-ordering
      investigated further, not yet fixed**: live-traced the real
      "draw one sprite" leaf function (`ddragonz.mod` `0x11d2d0`,
      called from 17 separate real static call sites, not one
      central loop) and real `glDepthFunc`/`glDepthMask` calls --
      real code already explicitly sets `GL_LEQUAL` (correct,
      standard technique for same-layer depth ties) and never touches
      `glDepthMask` (real GL default: writes enabled), both already
      forwarded correctly by this project's own fixes. Since the real
      GL configuration is confirmed correct and the bug still
      reproduces, the real remaining cause must be in the actual
      *submission order* real ARM code produces per frame -- needs
      real reverse engineering of the entity-update loop itself
      (scattered across many real per-object functions), not more
      GL-state probing. Tracked as open follow-up. 304/304 tests pass
      (303 + 1 new). See PHASE8_LOG.md's "'J1 %7d' fixed for real"
      section.
- [ ] Sprite z-ordering, real investigation continued (no fix yet):
      mapped the real tick entry (`0x1239dc` tail-jumps to the real
      master per-tick function `0x104ab0`) and confirmed the "draw
      one sprite" leaf function (`0x11d2d0`) is genuinely
      entity-agnostic -- a live PC breakpoint on real gameplay showed
      its `r0` is always the constant `0x32` or `0`, never a real
      entity pointer, so per-entity position/order is established by
      each of its 17 real callers before invoking it, not inside it.
      One real render path (`0x11daf4`) is a trampoline into an
      indirect call through a still-unidentified real interface's
      vtable slot 26; the other real branch (`0x124528`) wasn't
      reached at all this round. No single simple "for each entity,
      draw" loop found yet -- real order likely comes from a real
      actor-table/linked-list walk this project hasn't mapped, the
      same depth of work already done for IShell/IDisplay/IGL/HID.
      Flagged as a real, substantial reverse-engineering task, not a
      quick fix. See PHASE8_LOG.md's "Sprite z-ordering, continued"
      section for the full real call-chain map so far.
- [ ] Sprite z-ordering, real root cause found (still no fix): a live
      `LR`-capture technique (log `core.GetRegister(kLR)` at the real
      `glDrawArrays` trap -- names the real calling function directly,
      no static searching) plus a deterministic frozen-frame testbed
      cracked the ROPI wall the two rounds above hit. Full real chain
      now mapped: `0x107360` (real per-tick render fn) calls
      `0x11f804` (real "draw every registered entity" fn, confirmed
      live: all 7-8 real character draws in one frame share the same
      `LR`, i.e. one real call site) exactly once/frame; entities
      reach its list via `0x11f6c4` (real "append entity to category
      list" fn -- confirmed live, by write-watching the real list
      address, to be the *only* real writer, and a pure append with no
      sort); real callers are two fixed single-entity functions
      (`0x1165c0`, `0x116190`) plus a real 5-enemy-slot loop
      (`0x11666c`, always the same fixed slot order 0..4 every frame).
      **Conclusion: there is no missing sort to restore.** The real
      ROM code never sorts character sprites by depth or Y anywhere in
      this chain -- draw order is just fixed real code order plus
      fixed enemy-slot-array order. Two honest paths remain: (1) trace
      one level deeper into the still-unfound real enemy-slot spawner
      to check whether *slot assignment itself* might differ from real
      hardware (a real, potentially-fixable bug, not confirmed), or
      (2) accept this is authentic original-game behavior and add a
      non-ROM-faithful compositing correction if visually-correct
      layering is wanted regardless (real depth-test infra already in
      place from the earlier depth-buffer fix; a first attempt at this
      was built, tested, and reverted this round after a real
      false-positive on the door's own background quad). All live
      instrumentation reverted; no code change this round, purely a
      real evidence-gathering round. See PHASE8_LOG.md's "Sprite
      z-ordering, the real fix round" section.
- [ ] Sprite z-ordering, traced the spawner (path 1 above) -- it's
      hardcoded ROM script, not a bug at this layer: walked real `LR`
      captures up four more levels (the per-frame slot-array update
      loop `0x109620`; the real pool allocator `0x10c4e4`, found by
      live write-watching the real 8-slot array `0x80337c44` directly;
      the generic "spawn type/resource at slot" function `0x10eccc`)
      and found the real slot **index is never computed anywhere** --
      it's threaded straight through as a plain argument at every
      level. Its true origin: **seven distinct real call sites**
      (`0x120f08`..`0x12107c`), each a literal `(type, resource,
      index)` constant triple straight out of the ROM's own
      instructions -- the intro cutscene's own authored script. Also
      fully explained the one remaining unexamined conditional in
      `0x11f6c4` (a real flicker/invincibility-flash frame-skip, not a
      sort). Every real instruction between spawn and draw is now
      accounted for, and none of it sorts by depth or Y. If another
      emulator renders this scene correctly, the cause isn't in this
      exact chain -- open question, not yet identified. All live
      instrumentation reverted again; 304/304 tests pass; no code
      change. See PHASE8_LOG.md's "traced to the cutscene's own
      script" section.
- [ ] Sprite z-ordering, real reference emulator installed and
      compared -- corrects the previous round's conclusion: installed
      real Infuse (Tuxality's open-source Zeebo/BREW emulator,
      github.com/Tuxality/Infuse, official Linux build) per user
      request, loaded this repo's own Double Dragon ROM, and directly
      compared the same intro cutscene. Infuse shows real, correct
      partial occlusion (the blond enemy genuinely hidden behind the
      brute); this project's own render shows every character's full
      silhouette with none of them occluding each other. This
      contradicts the prior round's "no sort anywhere" conclusion, so
      it was re-checked: **real character quads do carry distinct
      per-vertex Z** (`-512`..`-504`, one per real entity slot index)
      -- the earlier round's "no Z for characters" claim was a real
      mistake (conflated the door/poster's own Z-tagged quads with an
      assumption that the whole Z range was background-only).
      With that corrected, every other piece of the real depth-test
      pipeline was verified this round directly against the *real*
      OpenGL driver state (not this project's own bookkeeping) at the
      exact character draws: `GL_DEPTH_TEST` enabled, `GL_LEQUAL`,
      writemask enabled, depth range `[0,1]`, a real 24-bit depth
      buffer, and a byte-correct real projection matrix with ample
      real Z precision (~32,000 distinct 24-bit steps between adjacent
      real Z values) -- no FBO/offscreen target in play either (no FBO
      support exists in this project's GL HLE at all, and nothing
      crashes reaching for one). **Every individual piece checks out
      correct, yet the final image is still wrong** -- this round
      corrected a real error and exhaustively ruled out the rendering
      pipeline, but did not find the remaining defect. Best-supported
      next lead: since real Z is genuinely derived from real
      registration/slot index, this project's own execution may be
      assigning entities to different real slots (hence different
      real Z) than real hardware for the same hardcoded ROM script --
      i.e. the "does our spawn-call execution order/timing match real
      hardware" question from two rounds ago is the live lead again,
      not a closed one. All instrumentation reverted; 304/304 tests
      pass; no code change. Real Infuse install kept locally
      (`~/.Tuxality/Infuse`, outside the repo) for further comparison.
      See PHASE8_LOG.md's "installed the real reference emulator"
      section.
- [ ] Sprite z-ordering, entity-system structure model corrected
      (still open): continuing the slot-assignment lead surfaced a
      real internal error from two rounds ago -- that round claimed
      the real registration function `0x11f6c4` "never fires," which
      directly contradicted its own other finding that an instruction
      *inside* it was confirmed as the render-list's writer. Redone
      fresh, cleanly: `0x11f6c4` does fire (552 real hits); the
      earlier "0 hits" was a stale/mistimed capture, not a ROM fact.
      More significantly, live-dumped the real contents of what the
      last two rounds treated as an "8-entity pool array"
      (`0x80300ad8`..`0x80300af4`) and found it isn't one -- it's a
      small mixed-purpose real struct (the render-list's own base
      pointer, a second adjacent list's base, real bounds/caps like
      `8` and `256`, at least one tracked entity pointer), not 8
      entity slots. This explains why "six of seven hardcoded spawn
      calls share literal index=6" looked contradictory against
      "6-8 characters render simultaneously" -- that index was never
      a render-list slot. The real render list itself (`0x80337c44`,
      confirmed real bound 8, `0x11f6c4` its one live append-only
      writer, real Z = `f(append order)`) still stands from two
      rounds ago. What's still genuinely open: the full real shape of
      this `applet+0xab0` struct and what actually determines each
      character's real append order -- the original slot-assignment
      question remains unanswered, not resolved. Stopped deliberately
      rather than keep building on a model that kept revising itself
      layer over layer. All instrumentation reverted; 304/304 tests
      pass; no code change. See PHASE8_LOG.md's "continued down the
      entity-system layer" section.
- [ ] Sprite z-ordering, rigorous correlated re-verification: per
      explicit direction to verify slowly rather than keep building
      new theories, re-checked everything with directly-observed,
      time-correlated live data instead of inference. Confirmed the
      real render-list write address (`0x80304258`..`0x80304274`) by
      watching the actual `str` instruction execute, not computing it
      from arithmetic. Confirmed, via a deterministic frozen-frame
      dump taken at both the entry *and* exit of a single real
      `0x109620` call, that the shared 8-slot array it iterates
      (`0x80337c44`, previously suspected to feed all 8 rendered
      characters) is **unchanged** across that call and only ever
      holds 2 of the 8 real entities -- yet all 8 real render-list
      writes happen within that same call's window regardless. The
      other 6 entities (5 "followers" + 1 more) are reached via a
      direct, nested call chain from inside the array's own slot-1
      entity's (the real "leader") own `Update()` -- consistent with
      the real leader/follower backreference found two rounds ago,
      most likely a real linked-list walk built once at spawn time.
      **This means follower order is driven by fixed real ROM code
      and a deterministic linked-list build, not by anything this
      project's own HLE/timing could plausibly compute differently
      from real hardware** -- no RNG, no wall-clock branch, no
      HLE-stubbed value anywhere in the traced path. That argues
      *against* the "our slot assignment differs from real hardware"
      theory this thread has been chasing for three rounds, and
      raises a different, harder question instead: if entity order
      really is ROM-fixed and this project's own depth-test pipeline
      was already verified correct against real OpenGL driver state,
      the gap versus Infuse's rendering may not be a Zeebulator bug
      at all -- Infuse is an independent, explicitly-labeled "A1
      development preview," not verified-against-real-hardware ground
      truth, and could plausibly be applying its own non-authentic
      approximation. Not confirmed either way; flagged honestly, not
      assumed. All instrumentation reverted; 304/304 tests pass; no
      code change. See PHASE8_LOG.md's "rigorous, correlated
      re-verification" section.
- [ ] Sprite z-ordering, checked for a general (non-Double-Dragon)
      emulation cause + corrected another self-made error: no
      documented report of this issue found anywhere (Infuse's own
      GitHub issues, general web search, two contemporary Zeebo
      reviews praising the graphics with no layering complaints --
      indirect evidence real hardware doesn't show this). Checked
      `IShellHle::Tick()`'s timer-expiry ordering as a possible
      general engine-level cause; likely not it, since Double Dragon
      appears to use one recurring master timer. Then caught a real
      self-contradiction from last round: live-captured the actual
      calling instruction (not just LR) before a follower's `Update()`
      and got the exact real `bx` inside `0x109620`'s own loop body,
      contradicting last round's "array never contains followers"
      claim. A clean, single-run, self-consistent capture (not
      compared across separate process launches) confirmed the array
      genuinely does hold all real entities -- just not from the very
      start; last round's dump had simply caught an earlier moment.
      There is no separate leader-driven linked list; `0x109620`
      iterating this one shared 8-slot array is the whole mechanism.
      **Real methodology caveat surfaced**: this project's frozen-
      frame testing snapshots a fixed *simulated* millisecond offset,
      an internal convenience checkpoint with no counterpart in real
      play or in how Infuse's own timing settles -- if entity
      spawn/eviction is still in progress exactly then, the snapshot
      could catch a genuinely transient state, a real and general
      (not title-specific) source of divergence worth treating as a
      live caveat on this whole investigation rather than a confirmed
      bug. Not confirmed either way. All instrumentation reverted;
      304/304 tests pass; no code change. See PHASE8_LOG.md's
      "pursued a general-emulation-bug hypothesis" section.
- [ ] Sprite z-ordering: closed the timing-methodology caveat from the
      round above. Replaced the fixed-sim-ms-offset freeze with one
      that polls the real active-entity array (`applet_ptr+0xab0`,
      8 slots) and only freezes once its contents hold identical for
      90 consecutive main-loop iterations -- a genuinely settled state,
      not a guessed clock offset. Verified deterministic and
      reproducible: two fully independent process launches, same real
      keypress sequence, converged on byte-identical array contents
      and a pixel-identical screenshot (`PIL.ImageChops.difference`
      bbox `None`). In that settled, reproduced frame, the tan-jacket
      character (higher on screen, farther back) is still drawn in
      front of the purple-suited character next to him (lower on
      screen, closer) -- the same failure mode the user originally
      flagged. Since this frame is provably not mid-spawn, **the
      timing-sensitivity caveat is now ruled out** as the explanation;
      this is a real, settled rendering-order defect. Also reconfirmed
      the 8-slot array only held 2 of the 5 on-screen characters at
      freeze time, so it's not the complete on-screen entity set --
      what drives the other 3 characters' draw order is still open.
      All instrumentation reverted; 304/304 tests pass; no code
      change. See PHASE8_LOG.md's "Closed the timing-methodology
      caveat" section.
- [x] Sprite z-ordering: **real root cause found and fixed.** Full
      disassembly of `0x11f804` (the real per-frame character-draw
      function, not just its loop shape) found a real one-time call,
      right before the character draw loop, through the runtime-helper
      table's offset-`0x1b4` slot -- previously found and registered as
      a safe no-op, guessed to be an unsafe-to-implement "array
      constructor" helper. Live capture of the real call site
      (`base=applet+0x4234, count=8, size=4, compar=<real fn ptr>`)
      proved that guess wrong: `size=4` is pointer-sized, not a real
      entity struct size, and the shape is exactly a generic
      `SORT(base,count,size,compar)`. Disassembled the real comparator
      (`ddragonz.mod` 0x10b918): compares real entity fields `+0x7c`
      (primary) then `+0x50` (tie-break, the same field gating render
      category). Implemented the slot for real
      (`ModRuntime::SortPointerArrayImpl`, `core/brew/mod_runtime.cpp`):
      a real in-place insertion sort calling back into the real ARM
      comparator via `HleRuntime::CallArmFunction`, saving/restoring
      `LR` around each nested call. First attempt (naive argument order)
      regressed live -- caught by the user watching the window, the
      door started rendering in front of the heroes. Flipping the
      comparator argument order (`compar(next,prev)`, producing a
      *descending* final order) fixed the door regression and produced
      correct real character layering, confirmed against real-hardware
      YouTube footage and Infuse. Added 4 real unit tests in
      `tests/mod_runtime_test.cpp` (replacing the old no-op smoke test),
      including one proving the LR save/restore is load-bearing (its
      first, LR-unaware version hung the process). 307/307 tests pass.
      This is the first actual code change to land from this entire
      investigation. See PHASE8_LOG.md's "Found and fixed the real root
      cause" section.
- [ ] Sound: still not confirmed reachable in real Double Dragon code,
      even with real input now driving well past the title screen.
      `MediaHle` itself is real, tested, and wired for output, but
      never registered under any `ClsId` (found in an earlier round);
      this round drove real input through title, menu, cutscene,
      walking, melee combat, and taking damage while logging every
      `ClsId` real code requests and every call into an already-
      successful generic scaffold object -- same six `ClsId`s as the
      idle-title-screen round, no new ones, and no audio-shaped
      scaffold calls. Also confirmed (again) no bundled real SDK header
      defines `AEECLSID_MEDIA`'s actual numeric value. Real next lead:
      manually walk `ddragonz.mod`'s own resource-descriptor table
      (`sound.ggz` string refs at file offsets 0x4dae8/0x4e1a8, table
      spans roughly 0x4dae0-0x4e460) rather than more "play further and
      watch" rounds. All instrumentation reverted; 307/307 tests pass;
      no code change. See PHASE8_LOG.md's "Sound, round two" section.
- [ ] Sound: mapped the real `sound.ggz` loading pipeline via live
      LR-capture (real functions `0x10739c`/`0x11bfd0`, 6 real call
      sites into the bulk loader). Confirmed all 74 archive entries get
      bulk-preloaded once at title-screen load, not per-play. Chased
      the most audio-specific-looking lead (a one-shot single-resource
      load at `0x1075a4`) all the way through its post-load dispatch;
      both branches turned out unrelated to audio (real HID
      button-callback registration and a generic cleanup routine,
      confirmed via live-captured arguments, not guessed). No audio
      playback trigger found yet -- this is a materially deeper
      investigation than sprite z-ordering was at the same stage, with
      no single obvious next call to follow. All instrumentation
      reverted; 307/307 tests pass; no code change. See
      PHASE8_LOG.md's "Sound, round three" section.
- [x] Sound: **real root cause found and fixed.** Disassembled
      `0x10a1e0` (a "cached resource -> playback slot" function found
      the previous round) in full and found a real call through
      `IShell`'s own vtable slot 32 (`GetHandler`) with `cls =
      0x01005500` -- a completely different slot from `CreateInstance`,
      which is exactly why three rounds of `CreateInstance`-only
      logging never saw anything audio-shaped. Real code calls
      `GetHandler` first (per the bundled real `AEEMediaUtil.c` sample's
      own usage shape) and only calls `CreateInstance` with whatever it
      returns -- `GetHandler` was a blind `Stub` (always 0), so real
      code silently gave up before ever reaching `CreateInstance`.
      Live-verified at the real call site (`0x10a2a0`): fired twice,
      both through the real `IShell` object, both with `cls =
      0x01005500`. Fixed by implementing `GetHandlerImpl`
      (`core/brew/ishell.cpp`/`.h`) to return `cls` itself for that
      value, and registering `MediaHle`'s object under the same ClsId
      in `tools/game_probe.cpp`. **Verified with real, external,
      OS-level proof, not a screenshot**: `pactl list sink-inputs`
      showed a live, unmuted `zeebulator_game_probe` audio stream at
      `s16le 2ch 22050Hz` -- Double Dragon's own real asset format --
      still present and playing after driving through combat. Added 3
      real unit tests; 310/310 tests pass. Second real code change from
      this whole investigation. See PHASE8_LOG.md's "Sound, round four"
      section.
- [x] Sound: fixed a real correctness gap in the round-four fix,
      caught by re-reading its own disassembly evidence more carefully
      rather than by a new bug report: the real `GetHandler`-
      >`CreateInstance` call for `AEECLSID_MEDIA` runs once *per sound
      activation* (it's inside the same function chased over two
      earlier rounds for exactly this reason), not once overall --
      so the previous round's single shared `IMedia` instance would
      have every new sound silently stomp whatever was already
      playing. Added `IShellHle::RegisterFactory` (checked before the
      plain fixed-instance map in `CreateInstanceImpl`) and switched
      `tools/game_probe.cpp` to register a factory closure over
      `MediaHle::CreateMediaObject()` instead of one pre-created
      object, so every real `CreateInstance` call gets its own fresh
      `IMedia` instance. Re-verified with the same `pactl` proof as
      the previous round, this time deliberately driving repeated
      melee attacks to exercise many real sound activations in quick
      succession -- stream stayed live and healthy throughout. Added
      a unit test proving two factory-backed `CreateInstance` calls
      return distinct objects. 311/311 tests pass. Third real code
      change from this whole investigation. See PHASE8_LOG.md's
      "Sound, round five" section.
- [ ] Sound: **still not actually audible -- but a real decode bug is
      now fixed, and the remaining gap is narrower and understood.**
      Prompted by the user asking "should I be hearing stuff?" --
      exposed that this project's own `pactl`-only verification proves
      a stream exists, not that it carries real audio. Recorded the
      actual stream (`parecord --monitor-stream`) and confirmed via
      Python analysis: complete digital silence, 0 non-zero samples.
      Root cause found live: Double Dragon's real `AEEMediaData` uses
      `clsData=1` (`MMD_BUFFER`, a raw malloc'd buffer), which
      `SetMediaParmImpl` didn't support at all (only the filename
      lookup shape); the real buffer is also gzip-compressed (live-
      confirmed: real gzip magic + an embedded original filename in
      the header). Fixed: `MMD_BUFFER` support + gunzip (reusing the
      same zlib technique as `ModRuntime::DecompressGzipInPlaceImpl`)
      + magic-byte-sniffed codec dispatch (no filename to use an
      extension from). Live-reverified: the real buffer now decodes
      as a correct ~47s MIDI track matching this project's own earlier
      real `bgm_1_0.mid` measurement. **But `Play()` is still never
      called** -- traced every `IMedia` vtable slot and found real code
      always does `SetMediaParm` -> `RegisterNotify` -> `Stop()` and
      nothing further, confirmed not stalled (the on-screen level
      timer keeps ticking) across cutscene, 12s+ of passive waiting,
      and real combat. Added 4 unit tests for the new `MMD_BUFFER`
      path (including a real gzip-compressed scenario). 315/315 tests
      pass; all instrumentation reverted. See PHASE8_LOG.md's "Sound,
      round six" section for the full trace and the `parecord`
      silence-detection methodology worth reusing.
- [ ] Sound: found the real cutscene script's "load sound resource"
      opcode, but the actual `Play()` trigger is still unconfirmed.
      Cross-referenced this project's own bundled real reference
      source (`ctsoundmgr.c`, a genuine Qualcomm sound-manager sample)
      and confirmed Double Dragon's loader matches its real
      `LoadMedia` almost exactly -- and confirmed that reference's own
      `Play()` is a separate, on-demand function, never called from
      load, matching this project's working hypothesis. Exhaustively
      read-watched the one live `IMedia` object's storage address:
      found all 4 real consumers (load, cleanup, `SetVolume`, `Stop`),
      no `Play()`. Live-traced a real per-tick screen/scene state
      machine (`0x104b6c`/`0x104b7c`'s indirect callback targets) and
      caught a real state transition past a `counter>=80` gate,
      landing on a much larger "phase 2" per-tick function. Found and
      fully disassembled a real 10-opcode cutscene script interpreter
      (`0x122684`) inside it; opcode 9 writes a real per-slot resource
      index into the exact field the resource-activation loop
      (`0x11f528`) polls every tick -- a genuine, concrete link from
      the cutscene's own scripted timeline to the audio pipeline. Real
      new architectural understanding (screen state machine + script
      interpreter), but the actual "now call `Play()`" trigger is
      still not located -- likely a separate on-demand call (matching
      `ctsoundmgr.c`'s own design) gated on script/game state not yet
      identified. All instrumentation reverted; 315/315 tests pass; no
      code change this round. See PHASE8_LOG.md's "Sound, round seven"
      section.
- [ ] Sound: found a real event-triggered script dispatcher
      (`0x123630`) that fires a scripted sequence by ID via the same
      10-opcode interpreter -- a real, general "run event N" mechanism
      whatever posts the pending event ID (not yet found) can use. Ran
      a full, mostly-natural real session (title, menu, cutscene,
      sustained combat, then unscripted play) with a simple `Play()`-
      only watch: the game genuinely progressed into new territory (a
      real `"CARREGANDO"` loading screen, then back to a *different*
      title-screen prompt, confirming real forward progress, not a
      stall) -- and `Play()` was never called, not once, across the
      entire cycle. **Recommendation for whoever picks this up next**:
      try real interactive human input (not `send_key.py`-scripted)
      before further static/live tracing -- this round's evidence
      suggests the automated input pattern itself may not be
      exercising whatever real condition `Play()` needs, e.g. an
      untried menu path (2-player battle, options) or precise real
      timing. All instrumentation reverted; 315/315 tests pass; no
      code change. See PHASE8_LOG.md's "Sound, round eight" section.
- [ ] Sound: found the real `Play()`-trigger function itself
      (`0x11d04c` -- a genuine tail-call to `IMedia::Play()`), and
      proved nothing calls it, via exhaustive static search (all 780
      real PIC function-pointer materializations in the ROM, plus a
      raw-literal scan) and live tracing (a PC-reached watch and a
      memory-read watch for its address, across idle title screen,
      scripted play, and genuine human-driven interactive play).
      Real human input was tried directly this round per round
      eight's recommendation -- confirmed reaching the emulator
      correctly (every button press logged a real HID callback) --
      and still zero `Play()` calls, including at idle title screen
      where the user confirms music should already be playing. Also
      ruled out a preference/mute gate (`GetPrefs` is never called by
      real code at all) and a separate download-progress interface
      class (`0x01005511`, confirmed unrelated to media playback).
      Every concrete hypothesis so far has been tried and ruled out;
      this is now a genuine open question. No code change this round.
      See PHASE8_LOG.md's "Sound, round nine" section.
- [ ] Sound: found and confirmed live (via real human interactive
      play) a real per-entity event dispatcher (`0x10d4c8`) that
      calls `Play()` for event code 2 -- fires every tick, proving
      it's live per-frame code, but its event pointer was null on
      every observed call across an extended real play session
      (movement, combat, landed hits). A second promising lead (a
      "global Play() trampoline" that fired continuously during real
      play) was traced precisely and conclusively closed as a real,
      unrelated OpenGL `glBlendFunc(GL_SRC_ALPHA, ...)` call reached
      through a generic shared vtable-dispatch trampoline that
      happens to reuse the same slot number as `IMedia::Play()` --
      confirmed via the real GL object address and vtable slot this
      project itself built, not left ambiguous. No code change this
      round. See PHASE8_LOG.md's "Sound, round ten" section.
- [ ] Sound: traced the real per-entity event dispatcher's full data
      path (its event pointer comes from a shared, 60+-call-site
      helper `0x10b270` that does a real, populated table lookup --
      not an empty/missing-resource issue). Live-instrumented the
      actual event code and had the user drive the most exhaustive
      real session yet: combat, multiple enemy defeats, losing all
      lives, a full game-over cycle, back to title. Across 87,116 real
      non-null event deliveries, the event code was always `0` or `1`
      -- never the `2` that triggers `Play()`. Also ruled out a stale/
      incomplete ROM dump: this project's `Double Dragon/mod/274754/`
      files are byte-for-byte identical (sha256) to the independently-
      sourced `Double Dragon (Brazil) (Es,Pt).zip`. This is the
      cleanest negative result so far -- either this ROM's own data
      never populates a sound-trigger event under any reachable real
      player action, or a second, still-unfound sound-triggering
      mechanism exists elsewhere. No code change this round. See
      PHASE8_LOG.md's "Sound, round eleven" section.
- [x] Sound: **real gameplay audio confirmed working.** The actual gap
      was never a missing `Play()` trigger -- it was two sibling
      `CreateInstance` class IDs (`0x0100550a`/`0x01005501`) that had
      been silently failing since round one/two of this investigation,
      alongside the one class this project happened to register first
      (`0x01005500`). Real Double Dragon sets up multiple separate
      sound channels through a shared generic init routine; fixed by
      registering the same factory for all three real class IDs
      (`tools/game_probe.cpp`). Confirmed live: real, audible music and
      SFX for the first time this investigation. Follow-up quality
      fixes once audio was actually reachable to listen to: real
      per-voice resampling in `core/audio/mixer.cpp` (was frame-for-
      frame with no rate conversion at all), skipping real GM
      percussion-channel (10/index 9) notes in `RenderMidiToPcm`
      rather than mis-rendering them as pitches, and real post-mix
      peak normalization instead of a hard clamp (real, measured
      clipping distortion from dense 9-channel polyphony). Remaining
      "sounds like a cheap synthesizer" quality gap confirmed (with
      the user) to be melody/timing-correct, timbre-only -- our
      renderer has no real instrument-timbre modeling at all, a
      separate, scoped follow-up task, not a bug. See PHASE8_LOG.md's
      "Sound, round twelve" section.
- [x] Sound: **real General MIDI wavetable synthesis.** Researched what
      real synthesizer Double Dragon's music actually targeted rather
      than guessing (real Zeebo SDK developer guide: Qualcomm's real
      CMX synthesizer, GM1/GM2-compliant, wavetable/sample-based) and
      replaced the hand-rolled sine/square/sawtooth synth with a real
      soundfont-based one -- TinySoundFont (MIT) + GeneralUser GS (a
      real, license-verified GM soundfont), both fetched via CMake
      `FetchContent`, not committed. New `core/audio/soundfont_synth.
      {h,cpp}`, wired into `MediaHle` as an optional dependency (tests
      keep using the fast hand-rolled fallback; real usage in
      `tools/game_probe.cpp` uses the real soundfont). Confirmed live
      by the user: correct GM instrument assignment (real Overdriven
      Guitar/Distortion Guitar/Electric Bass/String Ensemble/Brass
      Section programs all reading as themselves). Follow-up: real
      music was drowning out real SFX -- measured live (peak pinned at
      the exact int16 ceiling, real clipping); implemented real
      per-voice `MM_PARM_VOLUME` support in `Mixer`/`MediaHle` (a
      previously-documented, never-applied gap) first, confirmed live
      it wasn't the real cause, then found and fixed the actual one
      (`SoundFontSynth` had no headroom at all) with a real -16dB gain
      reduction, tuned to the user's own live A/B comparison against
      the real reference. See PHASE8_LOG.md's "Sound, round thirteen"
      section.
- [x] Sound: some real SFX (e.g. one of the two punch attacks) never
      play. Round fourteen instrumented the full real chain
      (`GetHandler`/`CreateInstance`/decode/`Play`) and ruled out an
      HLE bug entirely (every call at every stage succeeded). Round
      fifteen found the real explanation: real Double Dragon code uses
      a per-character, 4-channel round-robin priority-stealing sound
      system, gated by a stored per-channel priority stamp that only a
      real `MM_STATUS_DONE` notification (fired via `IMEDIA_
      RegisterNotify`'s callback) resets -- and `MediaHle` never fired
      that notification (a previously-documented, not-yet-implemented
      gap). Added `MediaHle::Tick()`, firing the real registered
      callback with a real `AEEMediaCmdNotify`-shaped struct once a
      voice finishes, wired into `tools/game_probe.cpp`'s main loop.
      Live-confirmed fixed by the user across multiple extended play
      sessions (punch/enemy-hit/enemy-fall sounds all kept working),
      matching real Infuse behavior for the first time this
      investigation directly compared against it. See PHASE8_LOG.md's
      "Sound, round fifteen" section.
- [ ] Add any needed per-title quirks to `core/brew/compat/`, keyed by game
      hash — never inline in general HLE code (Design Principle 5)
- [ ] Lock in this title as a permanent CI regression fixture once it passes
      (ARCHITECTURE.md §8)
- [x] Performance pass: investigated why the real title screen ran at
      only ~12fps against Double Dragon's own real, documented "locked
      30fps". **Not an interpreter-speed ceiling requiring the JIT
      deferred in Phase 1** -- two real, evidence-grounded host-side
      bugs, both fixed:
      1. **This whole project had never been building with any
      optimization at all.** `CMakeCache.txt`/`flags.make` showed
      `CMAKE_BUILD_TYPE` empty and no `-O` flag anywhere -- CMake's
      real default for an unset build type. Added a standard
      `if(NOT CMAKE_BUILD_TYPE ...) set(... "Release" ...)` default
      to `CMakeLists.txt` (confirmed zero `assert()` usage under
      `core/` first, so `-DNDEBUG` is safe). Alone: **12fps -> 27fps**.
      2. `tools/game_probe.cpp`'s main loop unconditionally
      `SDL_Delay(kTickMs)`'d every iteration regardless of how long
      that iteration's own real work took -- double-waiting on top of
      a real vsync-blocked `eglSwapBuffers` on the iteration Double
      Dragon's own real, live-confirmed `ISHELL_SetTimer(ms=32, ...)`
      main-loop timer fires. Made the wait elapsed-aware instead of
      flat. Combined with fix 1: **31fps**, matching the real
      requested ~31.25fps cadence almost exactly -- confirmed stable,
      and corroborated by a real "press HOME" prompt now visibly
      blinking on screen for the first time (too infrequent to be
      human-visible at the old framerate). 303/303 tests pass. See
      PHASE8_LOG.md's "Real 12fps -> 31fps" section for the full
      derivation, including the temporary `[DBGPACE]`/`[DBGTIMER]`
      instrumentation (reverted) that found both root causes from live
      evidence rather than guesswork. JIT integration stays deferred:
      nothing here showed the *interpreter itself* as the bottleneck
      once actually compiled with optimizations on.

- [ ] **Open bug, live-reported, not yet resolved**: total silence after
      loading a save state, specifically reached via beating the game
      (unlocking "Batalha Extra") then loading a save state made right
      after the final boss. Root cause diagnosed with real evidence:
      `core/save_state.h` documents (and always has) that a save state
      only ever captured guest CPU/memory, never `MediaHle`'s own
      `media_by_object_` map or `Mixer`'s own `voices_` -- purely
      in-memory, host-side state keyed by a guest object address that's
      meaningless to a freshly-constructed instance. A guest that reuses
      an `IMedia` handle it created before the save was taken finds no
      record of it after loading one, so every call against it fails
      outright. Confirmed live: works from a cold boot, breaks
      specifically after `--load-state`.
      Added real `Serialize`/`Deserialize` to both `Mixer`
      (`core/audio/mixer.{h,cpp}`) and `MediaHle`
      (`core/brew/media_hle.{h,cpp}`), wired into `tools/game_probe.cpp`
      the same way the existing GL texture log is (F1 appends it, a
      cold `--load-state` restores it, a same-session F2 deliberately
      doesn't -- see F1/F2's own comments there). 39 new tests
      (`tests/mixer_test.cpp`, `tests/media_hle_test.cpp`), all passing,
      confirming the round trip works in isolation.
      **Live retest after the fix still reproduced total silence** --
      not yet understood why. Prime suspect, not yet checked: the retest
      reloaded from the *pre-fix* save state first (its own diagnostic
      correctly printed "audio state: not present in this save file"),
      then saved fresh (F1) *from that already-broken, empty
      `media_by_object_` state* and reloaded that -- which would
      faithfully round-trip the brokenness rather than fix it, not
      exercise the fix's actual intended path at all. A clean retest
      needs a genuinely cold boot (never touching the old save file)
      played through to the same unlock, *then* F1, then reload --
      hasn't been tried yet. If that also still reproduces the bug, the
      diagnosis above needs revisiting; some other piece of host-side
      state this investigation hasn't identified yet may also be
      involved.

- [ ] Validate the HLE against a fourth real game (Zeebo Sports Tênis),
      started after Peggle's own investigation stalled for three rounds
      in a row without converging (TASKS.md's own Peggle entries;
      "0x102148"'s real per-sub-resource condition) and Super
      BurgerTime's own wall turned out to need real, separate,
      copyrighted Neo Geo arcade ROM chip dumps (`zupa_p1.rom` etc. --
      real SNK hardware ROMs, not Zeebo application data, confirmed via
      the real embedded string table: `NEOGEO_MEMORY`/`NEOGEO_SOUND`/
      the real MVS cartridge-ROM naming convention) this project
      doesn't have and isn't the right kind of asset to go looking for.
      Picked for having no separate exotic asset container risk *and*
      no embedded-legacy-CPU-core risk — genuinely a different class of
      title from both prior investigations.
      **Cracked `resources.pakz` (shared by this title plus Zeebo
      Sports Peteca/Volei, Zeeboids, and one kids'-activity title's own
      downloads, all independently confirmed to use the identical byte
      layout) in a single pass.** Real header: `"PACK"` magic + trailing
      64-byte-record table offset/size (12 bytes total); each entry's
      compressed data is a **standard classic `.lzma`/`LZMA_ALONE`
      stream** (5-byte properties/dict header + 8-byte size + payload)
      — a real, standard algorithm, not a proprietary scheme, decoding
      cleanly via liblzma's own `lzma_alone_decoder` on the very first
      real entry tried (`athlete_types.m3g`, decompressing to real,
      legible content: `"...Crazyball Engine. Copyright..."`, a real,
      identifiable third-party middleware). Implemented as permanent,
      tested code following the established `GgzArchive`/`PkgArchive`
      pattern: `core/loader/pakz.{h,cpp}`, `tests/pakz_test.cpp`
      (synthetic fixtures only, built with liblzma's own real encoder),
      and `tools/zeebulator_pakz_inspector`. **Verified against the
      real file**: all 298 real entries extract cleanly, zero failures
      — audio (`.wav`/`.mp3`), textures (`.atitc`, a format this
      project's own `core/loader/atitc.h` already supports from earlier
      work), and 3D models (`.m3g`, JSR-184-shaped, not yet needed).
      Pulled in `liblzma` (real xz-utils, upstream
      `tukaani-project/xz`) via CMake `FetchContent`, the same pattern
      already used for zlib -- runtime-installed on this dev machine
      but with no dev headers/pkg-config file, so vendoring was the
      only reliable option. 374/374 tests pass (10 new: 8 PAKZ, 2
      CPU).
      **Then hit, and fixed, a real, foundational CPU gap while trying
      to boot the title**: `AEEMod_Load` threw `UnimplementedInstruction`
      almost immediately (module offset `0x1198`) on `ldrd r8,
      [sp, #32]`. Real ARM encoding quirk confirmed by assembling that
      exact mnemonic and reading the real bytes back (`0xE1CD82D0`):
      `LDRD`/`STRD` share the halfword-transfer *store* opcode space
      (`L=0`) with `SH=10`/`SH=11` marking the real paired-register
      load/store instead of a plain store, a case this project's
      `ExecuteHalfwordTransfer` didn't handle (only `SH=01`/`STRH` was
      implemented on the store side). **Fixed** in
      `core/cpu/arm_interpreter.cpp` (2 new tests). **Verified**: real
      `AEEMod_Load` now completes successfully end to end for this
      title (previously never got past the first few thousand real
      instructions) — the fourth title, after Double Dragon, Peggle,
      and Super BurgerTime, to reach this milestone, and the same
      instruction-family-gap shape as Super BurgerTime's own earlier
      "Extend" family fix, not a one-off. 374/374 tests pass (this
      count already includes the PAKZ tests above).
      **Real ClsId not found yet — genuinely a different shape of
      problem than Double Dragon/Peggle/Super BurgerTime's own ClsId
      searches.** The download-folder number (277534) predictably
      failed (`CreateInstance` returned `1`/EFAILED, matching the
      established "folder number usually isn't the real ClsId"
      pattern). Traced `CreateInstance`'s real body
      (`zeebotennis.mod 0x1010d0` -> `0x100fb4`) looking for the usual
      "cmp against a literal ClsId" shape and found something
      different: an early real check compares `r0` (this harness's own
      `module_ptr`, i.e. whatever address `AEEMod_Load` wrote into
      `*ppMod`, `0x80300000` -- this project's own established, shared
      heap-region convention, see `tools/game_probe.cpp`'s
      `ModRuntime` construction) against a real, fixed literal
      (`0x0108eff9`) -- not a ClsId at all, and unrelated to the
      `cls_id` argument, which only gets threaded through *unused*
      further down (into a generic, unconditional `AEEApplet_New`-
      shaped constructor helper, `0x100dfc`, that never compares it
      against anything either, at least in the portion traced so far).
      **Correction to an earlier round of this same entry**: it
      previously claimed `0x0108eff9` "falls inside the module's own
      address range" and built a theory on that (a static,
      compile-time-fixed singleton embedded in the module's own data).
      That arithmetic was wrong -- `0x0108eff9 - kBase` is `0xF8EFF9`
      (~16.3MB), not `0x8EFF9` (~585KB) as miscalculated, and either
      way is far past the real module's actual ~305KB size. The
      "embedded singleton" theory doesn't hold; what `0x0108eff9`
      actually corresponds to isn't known. What *is* solid: this
      harness's own `module_ptr` (`0x80300000`) can never equal that
      literal without changing what `AEEMod_Load`'s own trap hands
      back, which is a project-wide, cross-title convention (not
      something to change for one title without risking the three
      already-working ones) -- so this specific check, as currently
      understood, would never pass regardless of `cls_id`.
      **Follow-up round: the vtable-slot-2-is-CreateInstance assumption
      itself, checked and confirmed correct, not the culprit.** Dumped
      all four real vtable slots live (module_vtable turned out to live
      inside the malloc'd heap object itself, `0x80300014` -- built/
      relocated into guest memory at `AEEMod_Load` runtime, which is
      also why grepping the static `.mod` file for the raw pointer
      value earlier found nothing). Slot 0 (`0x1010bc`) is a textbook
      AddRef (`refcount++`), slot 1 (`0x1010fc`) a textbook Release
      (`refcount--`), slot 3 (`0x1010f8`) a trivial `bx lr` no-op (a
      real BREW module's `FreeResources` is often exactly this) --
      slot 2 really is `CreateInstance`, confirmed, not a
      misidentification. The `cmp r0` block is a real, deliberate check
      this title's `CreateInstance` genuinely does.

      **Resolved, third round: real ClsId found, `CreateInstance`
      succeeds, real per-frame ticking reached.** The `cmp r0` block
      was never checking `po` at all -- a live register trace (enabled
      instruction-by-instruction tracing for this one call, not more
      static disassembly) caught the actual bug in this investigation's
      own earlier reading: the thin wrapper at `0x1010d0` does `mov
      r4,r2` (saving the *original* `r2`, i.e. `cls_id`) then later
      `mov r0,r4`, clobbering `r0` (which held `po`) with `cls_id`
      before falling through to the `0x100fb4` block. So the real
      comparison there is `cmp cls_id, 0x0108eff9`, not `cmp po,
      <literal>` -- the two prior rounds' entire "static, module-
      embedded singleton" and "module_ptr convention" theories were
      chasing the wrong operand. **The real ClsId is `0x0108eff9`**
      (`17362937` decimal, the CLI's expected base). A side quest while
      tracing this also turned up something worth keeping for its own
      sake: the module's own internal bootstrap calls
      `ISHELL_CreateInstance(shell, 0x01001001 /* AEECLSID_DISPLAY */,
      &object[20])` partway through -- i.e. real Double Dragon-shaped
      evidence that `0x01001001` really is `AEECLSID_DISPLAY`, already
      registered that way in this project's own `ishell.cpp` on
      independent grounds, now cross-confirmed by a second title.
      Verified live end to end with the real ClsId: `CreateInstance OK,
      applet=0x80300024 HandleEvent=0x00100df4`, `HandleEvent
      (EVT_APP_START)` runs, and **"Reached the event loop with no
      unhandled instruction"** -- the same milestone Double
      Dragon/Peggle/Super BurgerTime each reached, now hit by a fourth,
      genuinely different title. Real per-frame ticking then runs for a
      bit (real HLE calls firing) before hitting the next real, distinct
      wall: a `pc=0x00000000 left the loaded module's range` wander
      warning after 187 steps, then a timer callback throwing
      `Coprocessor instruction / SWI` at module offset `0x46e18` --
      almost certainly downstream noise from the same wander (once
      `pc` hits 0, everything after is garbage execution through
      unmapped/zero memory), not a second, independent bug.

      **Traced the wander to a specific, missing runtime helper table
      slot, then identified and implemented it (fourth round).**
      `zeebotennis.mod` offset `0x1013f8`-`0x10140c` (reached from the
      real per-frame tick path) calls the already-known sprintf-family
      formatter (offset `0x13c`, slot #11 in this file's own numbered
      list above) successfully, then immediately calls **offset
      `0xc8`** on the *same* runtime helper table -- unclaimed by any
      slot documented so far. First reading of the calling convention
      there (`strncat`-shaped) turned out to be only half the picture:
      three more real call sites (found by scanning the whole binary
      for the `ldr r0,[r6,#-4]` context-fetch immediately followed by
      `ldr r3,[r0,#0xc8]`) included one unambiguous one
      (`zeebotennis.mod` 0x136bf0: `dest=<a fixed offset inside a
      freshly-constructed object>, src=<a plain string>, maxlen=32` --
      a plain constant, not a computed length), ruling out concat and
      confirming real, standard **`strncpy(dest, src, maxlen)`**
      semantics instead. Implemented as `ModRuntime::StrncpyImpl`
      (`core/brew/mod_runtime.{h,cpp}`, offset `0xc8`, the file's own
      21st confirmed slot), 2 new tests.
      **Verified live**: the wander is gone; execution advances well
      past where it previously stopped (226 real steps vs. 187 before,
      through a real `ISHELL_CreateInstance` call for
      `AEECLSID_FILEMGR`), then hits a **new, different** wander at
      `zeebotennis.mod` 0x104320 -- traced the same way, real call site
      `(s=<a string pointer>, c=43 /* '+' */)` with the return value
      null-checked immediately after: real, standard **`strchr(s, c)`**
      semantics. Implemented as `StrncpyImpl`'s neighbor
      `StrchrImpl` (offset `0x18`, the file's 22nd confirmed slot), 3
      new tests.
      **Verified live again**: no more wander at all -- execution now
      runs through many real per-tick HLE calls (a real, repeating
      resource-lookup-shaped pattern) before hitting a **different
      class of gap**: a timer callback throwing `Miscellaneous
      instruction space (MRS/MSR/etc.)` at module offset `0x44b38`.
      Disassembled: `clz r1, r0` -- real **CLZ** (Count Leading Zeros,
      ARMv5T+), sitting inside real IEEE-754 soft-float normalization
      code, sharing the same "miscellaneous instructions" encoding
      space as MRS/MSR/BX/BLX that this project's interpreter only
      partially implemented (BX/BLX only) -- the same bug *shape* as
      this same investigation's own earlier LDRD/STRD fix, a real CPU
      gap, not a guess. **Fixed** in `core/cpu/arm_interpreter.cpp`
      (`std::countl_zero`, C++20's own standard-library equivalent, not
      a compiler builtin), 2 new tests. Regression-checked against
      Double Dragon (still reaches the event loop clean).
      **Verified live a third time**: no more crash or wander at all --
      real per-tick execution now runs indefinitely through what looks
      like a genuine, repeating polling pattern (the same couple of
      HLE trap addresses called over and over with a small, cycling set
      of arguments) until this project's own step-budget guard aborts
      the call.

      **Identified the loop precisely (fifth round).** Added a
      temporary per-registration index/trap diagnostic to `HleRuntime::
      Register` (not guessed at from static disassembly alone) to map
      the three repeating trap addresses to real functions: they're
      `sprintf` (already real), and two of this file's own still-
      unidentified stub slots -- `dbgprintf` (offset `0x9c`) and the
      offset-`0x184` slot first found in Super BurgerTime with too thin
      a call shape to identify there. Both are hardcoded no-ops
      (`SetRegister(kR0, 0)`), which is why nothing about the loop's
      own state ever changes.
      Ruled out malloc exhaustion as the cause (a live allocation trace
      showed zero real `Allocate()` calls happening anywhere in the
      loop). The real debug strings sprintf/dbgprintf are formatting
      turned out to be highly informative on their own:
      `"TTDMM ASSERT: [Condition | Line @ File]"` with a `.\source\
      TTDMemoryManager.cpp` argument -- a real, custom assert macro
      from the game's own memory manager, not a generic log. Tracing
      the calling function (`zeebotennis.mod` 0x1047dc onward) shows
      it's a real hash-table slot computation (`umull`/`rsb`/shift
      sequence matching a load-factor-style index calc) that branches
      into the assert-log-then-`sleep(50)`-then-retry sequence when two
      computed table fields compare equal -- reads as a real collision/
      already-occupied-slot check in a resource or string hash table,
      firing every single retry with no state ever changing to make it
      stop. Whether this is a real bug in the title itself (unlikely to
      matter if so -- it would presumably also hang on real hardware)
      or specific to something this harness lays out differently
      (memory layout, resource load order/count, or a hash input that
      happens to differ) isn't determined yet.

      **Correction + real gate found (sixth round).** The `0x1047dc`
      hash-collision function traced above turned out to be a red
      herring for the *live, repeating* loop specifically -- a targeted
      PC-gated diagnostic confirmed its own comparison instruction is
      never reached at all during the actual hang (0 hits across a full
      run). The real, repeating call site is a *different* one of
      offset `0x184`'s ~11 real call sites: `zeebotennis.mod 0x10520c`
      onward. There, a real, explicit guard -- `cmp r0,#0; bne
      0x105288` on `[r5+4]` (`r5` a real, ROPI-relocated static context
      pointer) -- skips the entire assert-log-then-`sleep(50)`-then-
      retry sequence whenever `[r5+4]` is nonzero; it's the loop's own
      real exit condition. Live-verified this is the actual gate (not
      another false lead): sampled across 78 real loop iterations,
      `[r5+4]` reads exactly `0` every single time.
      **Then answered the obvious follow-up with a whole-run memory
      watchpoint** (temporary, on `Memory::Write8`, the established
      technique from this same project's own Peggle/Super BurgerTime
      watchpoint precedent): does *anything* in the whole module ever
      write this address? Yes -- but only **twice**, both **during
      startup**, both writing **`0`** -- i.e. it's zero-initialized once
      and never touched again by any code in the entire module for the
      rest of the run. Nothing internal to this title's own compiled
      code can make this loop exit. The only real candidate left to set
      it is whatever the still-unidentified offset-`0x184` system call
      (called every single iteration, argument `50`, most likely an
      `ms`-shaped yield/poll/sleep) does on real hardware as a side
      effect -- but its calling convention alone (a single integer
      argument, no reference to `[r5+4]`'s own address) doesn't reveal
      what that side effect is, and guessing at a whole system service's
      real behavior risks the same silent-corruption trap this
      investigation has been careful to avoid throughout (see the
      earlier `strncpy` vs. `strncat` correction). **This is the same
      shape of wall Peggle (needs real BREW MP SDK header knowledge) and
      Super BurgerTime (needs real, separate copyrighted ROM dumps) each
      hit** -- a real gap this project doesn't have enough evidence to
      close safely yet, not a bug in anything implemented so far. All
      temporary instrumentation reverted, 391/391 tests pass unchanged.

- [ ] Picked a fifth title, **Zeeboids**, to test whether the fixes made
      chasing Zeebo Sports Tênis generalize -- deliberately *not* one of
      the "Zeebo Sports" template siblings (Peteca/Volei), which are
      near-certain to hit the exact same wall given their near-identical
      `.mod` size and naming; Zeeboids is architecturally different (a
      1.1MB `.mod`, not a ~330KB sports-template one), giving it real
      odds of taking a different code path. (Zuma's Revenge, also
      considered, turned out to use a completely different, uncracked
      PopCap-proprietary `PPCPRCON` container instead of
      `resources.pakz` -- a much bigger lift, set aside.) Extracted into
      `research/games/Zeeboids/` (persistent, matching the established
      layout); `resources.pakz` decodes cleanly (1048/1048 entries).
      **Found the real ClsId the same way as Tênis's own round**
      (the thin `CreateInstance` wrapper clobbers `r0`/`po` with
      `r2`/`cls_id` before the real literal comparison): `0x0108ff1a`
      (17366810). Confirmed live: `CreateInstance` succeeds.
      **Verified the fixes generalize**: hit a wander during
      `CreateInstance` itself (not just the tick loop) at
      `zeeboids.mod` 0x15d1bc -- the same runtime helper table
      mechanism, a new, previously-unclaimed offset `0x140`. Traced its
      one real call site: sits immediately before a real `dbgprintf`
      call whose own arguments reference the exact same real
      `TTDMemoryManager.cpp` string Tênis's own investigation found --
      **confirms this is genuinely shared engine code across titles**,
      not a coincidence. Since `dbgprintf` itself already discards its
      message entirely, whatever this slot computes never affects
      anything downstream -- registered as a safe no-op (the file's
      23rd confirmed slot, `core/brew/mod_runtime.{h,cpp}`), matching
      the established precedent for single-call-site, low-risk gaps.
      **Verified live: real progress past Tênis's own exact wall** --
      the identical `TTDMM ASSERT`/`sleep(50)` loop this round's own
      earlier entry documented for Tênis fires here too (same shared
      code, confirming that diagnosis independently), but this time
      execution escapes it and reaches a **new, different** crash: a
      timer callback throws `Miscellaneous instruction space` at
      `pc=0x00090024` -- confirmed (by checking that the raw `cls_id`
      value decodes into exactly that instruction-encoding space) this
      is a literal jump to `tools/game_probe.cpp`'s own
      `kAppStartAddr+4` scratch address (the `AEEAppStart.clsApp`
      field), not real module code.
      **Follow-up, same round: ruled out the simple "collision" theory.**
      A narrow-PC-gated diagnostic (fires only when `pc==0x90024`,
      before any exception unwinding) shows this address is actually
      reached **three separate times** from **three different, unrelated
      real call sites** (three distinct `lr` values) before the crash --
      the first two survive (whatever real code lives at `0x90024` at
      that moment apparently decodes as something harmless), only the
      third throws. One of the three call sites is a clean, textbook
      real C++ virtual dispatch (`ldr r1,[r0]; ldr r1,[r1]; blx r1` --
      read vtable pointer, read slot 0, call it) at `zeeboids.mod`
      0x1a0648, reached from a real per-entity update loop. A whole-run
      write watchpoint on `kAppStartAddr+4` itself found exactly **one**
      real write, ever: this harness's own one-time initialization
      (`mem.Write32(kAppStartAddr + 4, cls_id)`) -- nothing in the game's
      own code writes there. So this isn't a stored-pointer-reuse bug
      (real code isn't caching and later misreading *this* field) --
      three unrelated places are each independently *computing* the
      value `0x90024` from their own, different data, landing on this
      harness's fixed scratch constant seemingly by coincidence of
      value, not by aliasing the same memory. Still reads as some kind
      of scratch/address-space collision, but a subtler one than the
      Peggle precedent (not two fixed regions overlapping in guest
      memory -- more likely something computes an offset from a base
      that isn't where real hardware would put it, or a real vtable
      slot this harness leaves zeroed/wrong).

      **Traced the clean virtual-dispatch call site (`0x1a0648`) all
      the way back to its real root cause, same round.** A full,
      unfiltered step trace through that function (not just two
      snapshot points -- the earlier ones misled: `blx r1` with `r1==0`
      doesn't crash, it decodes as a harmless no-op and *wanders*
      through zero-mapped low memory, matching this project's own
      already-documented "made-up AVK code" wander pattern, until it
      eventually resurfaces near real code again -- there's no actual
      third independent computation of `0x90024`, just one wander
      surfacing at different points) confirmed `self` (`r0`) is `0` at
      entry, with the real caller's own `lr` pointing at
      `zeeboids.mod` 0x1771b4. Traced the caller: `r0` going into that
      call is whatever `bl 0x1a0990` (a real per-entity field
      initializer -- `640`/`480` visible in its own stored constants, a
      real screen/viewport-shaped object) returned *unchanged* (it
      always just returns its own `r0` argument via `bx lr`, never
      allocates), which is itself whatever `bl 0x15d440` (a real
      object-constructor call, `size=52`) returned.
      **And `0x15d440` turned out to be the exact same shared
      constructor shape this round's own earlier Tênis entries already
      reverse-engineered in depth** (`zeebotennis.mod` 0x100dfc/
      0x10520c): the identical `cmp r0,#0; bne <success>` gate on a
      static context field this investigation already proved (via a
      whole-run write watchpoint, see the Tênis entries above) is
      written exactly once, at init, to `0`, and never touched again by
      any code in the entire module. When that gate reads zero -- which
      it always does -- this constructor runs the identical assert-log-
      then-`sleep(50)`-then-**return-0** sequence instead of
      succeeding, propagating a real `NULL` all the way out to the
      per-entity update call that then crashes.
      **This unifies both this round's remaining walls into one single
      root cause**: Tênis's own stuck `TTDMM ASSERT` retry loop and
      Zeeboids' `self=0` crash both trace back to the exact same
      never-set static gate flag, almost certainly tied to the real
      semantics of the still-unidentified offset-`0x184` system call
      (called every time this gate reads zero, argument `50`, an
      `ms`-shaped yield/poll/sleep). Whatever that real call is
      supposed to do on real hardware -- most plausibly something that
      eventually flips this exact flag -- is the one missing piece this
      investigation would need to resolve to unblock *both* titles at
      once, not two separate problems.

      **Found and fixed the real root cause, same round -- not a
      missing system call after all.** Added a live allocation trace
      (temporary) to `ModRuntime::Allocate` and watched what actually
      happens leading up to the gate write: the gate field genuinely is
      just a real `MALLOC` return value (`ldr r1,[r1,#0x68]` -- the
      already-known malloc slot -- immediately followed by `blx r1` and
      a `str r0` into the exact gate address), reached from a real
      lazy-singleton-init function (checks the same field at its own
      entry, returns a real error code immediately if already
      non-null). The live trace caught the real request:
      `MALLOC(size=23068672)` -- exactly 22 MiB, matching a real static
      constant (`0x01600000`) embedded in both titles' own shared
      TTDMemoryManager.cpp-derived engine code (misread as a plausible
      ClsId in an earlier round of this same investigation -- it's a
      size). This project's own emulated heap was only 16 MiB
      (`tools/game_probe.cpp`, bumped there once already for Double
      Dragon's own real needs) -- a single real allocation this large
      could never succeed, so the gate could never become non-null, so
      every dependent code path fell back to the same real
      assert-log-then-retry sequence forever. **Not a missing or
      misunderstood real system service at all** -- the three prior
      rounds' own "offset 0x184" and "scratch collision" theories were
      chasing symptoms of an ordinary undersized heap. **Fixed**:
      bumped the heap to 64 MiB (`tools/game_probe.cpp`; nothing else
      in that file claims any address at or above the heap's own
      `0x80300000` base, confirmed by grep, so extending it upward is
      safe). **Verified live against both titles**: the stuck
      assert-retry loop is completely gone from both -- Zeebo Sports
      Tênis and Zeeboids both now run cleanly through `CreateInstance`,
      `HandleEvent(EVT_APP_START)`, and tick 0, reaching a **new**,
      shared-shape gap early in tick 1 (a null-object virtual-dispatch
      wander after only 6 real steps, `zeebotennis.mod` 0x101bec-
      0x101c00 / `zeeboids.mod` 0x10b8e0 -- different addresses, same
      `ldr r0,[r1,#96]; ldr r1,[r0]; ldr r1,[r1]; blx r1` shape) --
      genuinely new territory for both, not investigated yet.

      **Took one more look at the new tick-1 gap, same round.** Real
      entry-point register state at `zeebotennis.mod` 0x101bec (the
      registered timer callback itself -- confirmed via `lr==trap_base`,
      i.e. this was reached directly from `tools/game_probe.cpp`'s own
      top-level tick-timer call, not a nested one): `r0=0x80300024`
      (the real applet pointer, correctly delivered as `pUser` per the
      real, already-established `PFNNOTIFY pfnNotify)(void *pUser)`
      contract this project's own `SetTimerImpl` implements), but the
      function itself reads its "self" from **`r1`** (`0` at this
      call), not `r0` -- the field it then dereferences at `[r1+96]` is
      necessarily reading near address `96`, not a real object. Either
      the real registered timer callback expects more than the one
      documented `pUser` argument (a real OS-level dispatch convention
      passing extra implicit context this harness's own direct
      function-pointer call doesn't supply), or `0x101bec` isn't really
      being reached as a raw `PFNNOTIFY` in the way `SetTimer`'s own
      already-validated 3-titles-strong convention assumes.

      **Resolved, same round: confirmed and fixed the real calling
      convention.** Traced the real registration call site itself
      (`sbt_methods[7]`, `tools/game_probe.cpp`'s own experimental
      `IShellHle::ScheduleTimer` path, registered for every title, not
      Super BurgerTime-specific despite the name) live: real arguments
      `r0=0x80047000 r1=0x00080000 r2=0x00101bec(callback)
      r3=0x80300024(user_data)` -- a real, non-zero `r1` this project's
      own code was silently discarding. Confirms the theory above
      directly: this registration path's real callback shape is
      `callback(r0=<the real r1 captured at registration>,
      r1=pUser)`, a genuinely different 2-argument contract from plain
      `ISHELL_SetTimer`'s own real, already-validated
      `PFNNOTIFY(pUser in r0)`. **Fixed properly** (not a global swap,
      which a live test proved would have broken Double Dragon's own
      real `SetTimer`-based main-loop timer -- confirmed by trying it
      first and watching a *different*, earlier wander appear):
      extended `IShellHle::PendingTimer`/`ExpiredTimer` with an
      `std::optional<uint32_t> r0_override`, set only by this specific
      experimental registration path; `tools/game_probe.cpp`'s own
      tick-firing code fires with `(r0=r0_override, r1=pUser)` when
      present, else the unchanged, standard `(r0=pUser)`. 3 new tests
      (`tests/brew_test.cpp`).
      **Verified live against both titles**: the crash is completely
      gone from both. Zeebo Sports Tênis now runs deep into real,
      previously-unseen HLE call territory (dozens of new real trap
      addresses) before hitting this project's own step-budget limit --
      genuinely executing, not crashing, the best outcome short of
      reaching a visible frame. Zeeboids runs **13,182** real steps
      before its own next gap (previously 6), reaching an entirely new,
      later crash location. No regression on Double Dragon (still
      reaches the event loop cleanly). 393/393 tests pass.
      **Quick follow-up check (not committed -- exploratory only)**:
      tried a temporary 20x larger step budget (100M) against Tênis's
      own remaining step-budget wall. Didn't finish even at that size,
      but the real trap sequence *did* eventually move into genuinely
      new territory partway through (new trap addresses appearing near
      the 100M mark) -- confirms this isn't a dead infinite loop, just
      real, legitimately long-running work (very plausibly a large
      resource decompression/load, matching this interpreter's own
      much-slower-than-real-hardware instruction throughput), not a bug
      to chase further right now. Reverted; the real step budget stays
      at its existing 5M default rather than permanently slowing every
      run for one title's own long pole.

      **Traced Zeeboids' own new crash (`zeeboids.mod` 0x1783e8) live,
      same round.** A live diagnostic caught something important the
      static disassembly alone missed: `self` (`r5`) at the crashing
      dereference is `0x816a994c` -- a real, valid, non-null heap
      pointer, not a null object as the raw crash address's own
      instruction shape suggested. The real bug is one level more
      subtle: `[self+0]` (expected to hold a real vtable/interface
      pointer) is `0`, while `[self+12]`, on the *same* freshly-
      constructed object, holds a real, valid-looking heap pointer --
      only part of this object got initialized. Traced that zero
      field to its own real origin: it's exactly the caller's own
      original `r0` argument into the object's constructor
      (`zeeboids.mod` 0x17e50c), which was itself `0` at that call --
      tracing back further, that traces to *another* field
      (`[outer_r4+132]`) on a *different*, outer object, presumably
      itself waiting on some earlier real initialization this harness
      hasn't triggered yet. A real, evidenced chain, not a guess -- but
      genuinely deep, multi-object, Zeeboids-specific territory (at
      least 3 nested real functions: `0x1783cc`, `0x17e50c`,
      `0x182800`'s own caller) that would benefit from its own focused
      round.

      **Ran that watchpoint, same round.** Real watch address
      `0x81993ab0` (`[outer_r4+132]` at runtime), found live via a
      targeted diagnostic at the real caller (`zeeboids.mod` 0x182818)
      first, then a whole-run write watchpoint on that exact address.
      Two real writes total, **both value `0`**: `zeeboids.mod`
      0x183b54 turned out to be part of a real object *constructor*
      (five sibling fields -- offsets `8`/`12`/`132`/`136`/`140` --
      zeroed together in one real init pass), and 0x191714 is inside a
      real lookup-or-create routine (`bl 0x1a7918`, a lookup; on
      failure, `bl 0x198bbc`, a construct, then a store) that also only
      ever writes zero at this exact address in the traced run. So this
      field really is a legitimate lazy-cache slot with a real, intact
      "check cache, else look-up-or-create, then cache the result"
      shape in the compiled code -- not a missing initializer. The step
      that would actually populate it with a *real* value either fails
      inside the still-unexplored `0x1a7918`/`0x198bbc` pair, or writes
      somewhere this specific watch address didn't catch (e.g. a
      different field on the same result).

      **Resolved the mystery, same round -- reframes the real bug
      entirely.** Live-traced both real sub-calls' own inputs/outputs
      directly: `0x1a7918`'s own lookup key is `r0=0x80003000` -- this
      project's own real `IDisplay` object address -- and it genuinely
      returns `0` (a real miss, not a bug in the lookup itself).
      `0x198bbc`, traced statically once its real behavior became the
      obvious next question, turned out to be **another instance of the
      same sprintf-then-dbgprintf debug-log wrapper pattern already
      found throughout this whole investigation** -- it never
      constructs anything, never touches `r7` (the register that ends
      up cached), and **unconditionally returns `1`** regardless of
      what it logged. So the entire observed chain -- lookup misses,
      log the miss, return "handled" anyway, cache the miss (`0`) so
      it's not retried -- is real, intentional "resource not found,
      log and move on" behavior baked into the compiled game, not a
      bug in anything reverse-engineered so far. **This reframes the
      real remaining question**: something *earlier* in real
      execution is supposed to register whatever real resource/cache
      entry `0x1a7918` searches for, keyed by the real `IDisplay`
      object -- and that real registration call never runs in this
      harness. Concrete next step for a fresh round: find what writes
      to whatever real data structure `0x1a7918` reads from (the same
      live-tracing-first approach used throughout this round, not
      static disassembly alone) to identify the missing real
      registration/interface this harness hasn't implemented yet.

      **Found it, and it reframes the bug again -- same round.** Traced
      the real multi-call chain live (a full instruction-level capture
      of every real `blx` in `zeeboids.mod` 0x1a7980-0x1a7a04, not
      guessed from static disassembly): the specific call whose own
      out-parameter feeds the eventually-null field is
      `trap=0xf0000670`. Identified precisely which real registered
      method that is using the same registration-index technique from
      the Tênis round (temporary markers before every major `Build()`/
      scaffold registration in `tools/game_probe.cpp`): it falls inside
      **`unknown_0x0103d8ec_obj`'s own registration** -- a *already-
      documented*, `BuildGenericStubObject`-backed placeholder for a
      real ClsId (`0x0103d8ec`) found and explained during the Peggle
      investigation (this file's own existing comment there, not new
      information): a real "try the newer class, fall back to this
      older one" SDK-emitted pattern, confirmed byte-identical across
      two independently-compiled titles, where `0x0103d8ec` is
      documented as *itself* "an always-succeeding stub" real code is
      designed to tolerate.
      **This means the null result flowing out of this specific call
      is very likely the real, correct, expected behavior here too** --
      not a missing implementation this harness owes the game. The
      actual remaining bug is much more likely a missing null/empty-
      result check somewhere in `zeeboids.mod`'s own calling code (the
      `0x1783e8`/`0x17e50c`/`0x182800` chain from earlier in this
      entry) that real hardware handles gracefully in some way this
      harness doesn't yet reproduce, rather than "implement
      `0x0103d8ec` for real." Concrete next step for a fresh round:
      check whether real per-entity code elsewhere skips this whole
      lookup-and-use sequence when the earlier lookup step
      (`0x1a7748`/`0x1a7918`-shaped) reports empty, rather than
      assuming success unconditionally the way the traced call site
      does. All temporary instrumentation reverted, 393/393 tests pass
      unchanged.

- [ ] **Picked a sixth title, Zeebo Sports Volei**, sequentially
      numbered right next to Tênis's own real ClsId
      (`0x0108ff15` vs. Tênis's `0x0108ff1a`, five apart -- both found
      the same way, the thin-wrapper `r0`/`cls_id` clobber). Extracted
      into `research/games/Zeebo Sports Volei/`; `resources.pakz`
      decodes cleanly (323/323 entries).
      **Verified live, and it changes the read on the `0x0103d8ec`
      chain from the entry above.** Volei hits the *exact same* call
      sequence as Zeeboids -- same trap addresses (`0xf0000668`,
      `0xf000009c`, `0xf0000284`, **`0xf0000670`**, `0xf0000230`,
      `0xf0000280`), the same real `0x0103d8ec`-family ClsId references
      (`0x0103d8dd`/`0x0103d8ea` alongside it), and the same eventual
      `pc=0x00090024` jump symptom -- at a different final address
      (`zeebovolley.mod` 0x1172f0, reached after 9,173 real steps, vs.
      Zeeboids' 13,182), but unmistakably the same mechanism. **Three
      independently-compiled titles (Tênis's own related loop,
      Zeeboids, now Volei) hitting the identical wall is real evidence
      against last entry's own "the null result is probably correct,
      expected behavior" read** -- real, shipped, playable games
      wouldn't all fail identically at this exact point. More likely:
      something *earlier* in real execution -- on real hardware,
      naturally satisfied by the normal passage of real gameplay time/
      events -- is supposed to run before this chain does, and this
      harness's own immediate, synthetic tick-forcing reaches it
      prematurely, before whatever real precondition would normally be
      met. Reopens the investigation the previous entry had tentatively
      closed; concrete next step is checking what real state changes
      between "just booted" and "this chain would naturally succeed"
      on titles that already reach further (Double Dragon, Peggle) to
      find the real missing precondition, rather than continuing to
      treat `0x0103d8ec` itself as the answer. 393/393 tests pass
      (investigation only, no code changes this round).

      **Followed that concrete next step, live, against Zeeboids --
      found the real gate, one level deeper than `0x0103d8ec` itself.**
      Dumped the scaffold's own vtable (temporary, reverted) to confirm
      exactly which of its 40 slots real code calls: slot 2
      (QueryInterface-shaped, asked about `0x0103d8dd`/`0x0103d8ea`),
      slot 4 ("Register"-shaped -- takes the real interface pointer
      obtained one call earlier via the device-bitmap's own
      `QueryInterface(0x01001045)` override, plus an out-param), slot 9
      (takes a freshly `malloc`'d 0x80-byte block), slot 16 (all-zero
      args). **Tried overriding slot 9 first** on the plausible-looking
      theory that this freshly allocated block was the object later
      null-vtable-dispatched at the real crash site (`zeeboids.mod`
      0x1783e8) -- verified live this changed *nothing whatsoever*
      (byte-identical crash, same exact step count), which is exactly
      what ruled it out. **Slot 4 was the real one**: a live diagnostic
      at `0x1783e8` confirmed `self` is the same real, valid heap
      pointer earlier rounds found (`0x816a994c`), and traced `[self+0]`
      one more hop than before -- it's populated (or not) by
      `zeeboids.mod` 0x191704's own check on the return value of
      `0x1a7918` (the same lookup/register function from the prior
      round, called this time with key `r1=0x80003000`, the real
      `IDisplay` object -- confirmed live). Overriding slot 4 to echo
      the registered interface pointer back through its own out-param
      (a plausible, evidence-shaped "you get back what you handed in"
      guess, not invented from nothing) **did make `0x1a7918` return
      non-zero for the first time** (`0x80019000`, confirmed live) --
      real, measurable forward progress, a new HLE call (slot 5) even
      starts firing that never did before. **But the crash is still
      identical.** Traced one hop further to find out why: the caller
      (`zeeboids.mod` 0x19171c) doesn't trust `0x1a7918`'s non-zero
      result on its own -- it immediately runs a *second*, independent
      check (`bl 0x1a7a48`) and only accepts the result if that returns
      exactly `1`. `0x1a7a48` is a real, in-module type-verification
      call: it loads a real global "class registry" object (address
      `0x000ac654`, confirmed live -- the same literal `0x1a7918`
      itself already reads at start-up as its own early-exit gate) and
      dispatches through *that* object's own real vtable slot 5,
      passing our candidate object as the argument -- a real,
      compiled-in "is this genuinely an instance of the expected class"
      check, not an HLE call this project could stub around. Since the
      candidate object here is `unknown_0x01001045_obj` -- this
      project's own totally generic, empty scaffold (every slot a
      no-op `Stub`, no real internal fields at all) -- it cannot
      plausibly pass a real structural type check the way a genuine
      object of whatever real class `0x01001045` is would. **This
      reframes the wall one more level, concretely**: it's not that
      `0x0103d8ec`'s own out-param is missing (that part's fix helped,
      confirmed live) -- it's that *every* object this harness hands
      back for still-unidentified real classes is a structurally empty
      shell, and real compiled code in this title genuinely, actively
      checks for that (not just trusting whatever's returned the way
      `0x198bbc`'s "always log and succeed" pattern does elsewhere).
      Passing this specific check for real would mean reverse-
      engineering the real internal layout of whatever class
      `0x01001045` truly is -- a materially bigger, dedicated task,
      not something this round's live-tracing method alone can safely
      guess at (this project's own established `strncpy`/`strncat`
      correction is exactly the cautionary precedent for guessing at
      structure instead of confirming it). Reverted the slot-4 override
      (and all four temporary diagnostics) rather than keep an
      unproven behavior change on a scaffold class shared globally with
      Peggle's own, currently-working use of it -- 393/393 tests pass,
      `git diff` on `tools/game_probe.cpp` clean against the pre-round
      baseline. Concrete next step for a dedicated round: reverse-
      engineer the real internal shape whatever real class `0x01001045`
      is (or find and implement the real class registry itself, at
      module address `0x000ac654`, if that turns out to be the more
      tractable side of this same check) -- both genuinely new,
      scoped sub-tasks, not more of this round's own live-tracing-only
      approach.

      **Resolved, same session -- the "real class registry" wasn't
      real guest code at all.** Picked the concrete next step back up
      and live-traced the actual call target at `0x1a7a84` (the `blx
      ip` this whole chain builds up to) instead of reasoning from
      static disassembly: `ip` resolved to `0xf0000674` -- an HLE trap,
      not real module code -- and the object it's dispatching through
      (`r0=0x80041000`) is *this project's own* `unknown_0x0103d8ec_obj`
      scaffold. The "real class registry" is a real, global cache
      inside `zeeboids.mod` (address `0x000ac654`) that stores whatever
      object this harness most recently registered for ClsId
      `0x0103d8ec` -- so the "type check" the previous entry couldn't
      safely fake around turned out to be this project's own scaffold
      being asked, through its own vtable slot 5, to confirm something
      about itself, not an unknown real object's structure. Slot 5
      expects the literal `1` written through a 5th, stack-passed out-
      param (`str r3,[sp]` at the real call site, confirmed live) --
      the same "blind `Stub` never writes out-params" gap as slot 4.
      Overriding both (kept alongside slot 4's already-tested fix; see
      `tools/game_probe.cpp`'s own doc comment on this scaffold for the
      full derivation) **made both Zeeboids and Zeebo Sports Volei run
      clean through their real per-frame tick loop** -- confirmed live,
      both titles now just hit the same step-budget wall Tênis's own
      long-but-real per-tick work already established as legitimate,
      not any further wander or unimplemented-instruction crash.
      **That same clean run surfaced one more real, separate gap**,
      previously masked by the earlier crash happening first: `SMULBB`
      (`cond 0001 0110 Rd 0000 Rs 1yx0 Rm`), a real ARMv5TE signed
      16x16 halfword multiply sharing the CLZ/MRS/MSR/BX "miscellaneous
      instructions" encoding space. Implemented the full `SMULxy`
      family (`BB`/`BT`/`TB`/`TT`, selected by the encoding's own free
      `x`/`y` bits) in `core/cpu/arm_interpreter.cpp`, matching the
      real ARM ARM semantics (sign-extend the selected 16-bit half of
      each operand, multiply, no accumulate/flags for this variant) --
      3 new tests in `tests/cpu_test.cpp` covering all three distinct
      half-selection combinations. Confirmed via live A/B testing that
      *both* fixes are required together (the CPU fix alone still hits
      the earlier crash first; the HLE fix alone still hits `SMULBB`
      as an unimplemented instruction once the earlier wander is gone).
      Verified no regressions: Double Dragon still reaches the event
      loop and correctly restores a save state (audio included) with
      the shared `unknown_0x0103d8ec_obj` scaffold now behaving
      differently; 396/396 tests pass (393 prior + 3 new `SMULxy`
      tests). Both fixes committed as permanent (not reverted --
      unlike the previous round's now-superseded guesses, both are
      fully evidence-grounded: the HLE fix targets this project's own
      object confirmed live, and the CPU fix matches documented ARM
      semantics exactly). Remaining wall for both titles is now
      identical in shape to Tênis's own: real, legitimately long per-
      tick work exceeding the interpreter's step budget, a
      performance/tooling question, not a correctness bug.

      **Measured the size of that remaining wall, same session
      (exploratory only, not committed).** Tênis's own precedent
      tested a 20x step budget (100M) and found real, if incomplete,
      progress. Zeeboids' own real tick 1 (the very first tick after
      reaching the event loop -- tick 0 is cheap and returns almost
      immediately) is dramatically larger: still hadn't returned at
      **2,000,000,000 steps** (400x the 5M default, 41 real seconds of
      interpreting -- confirming this interpreter's own raw throughput,
      not slowness, isn't the bottleneck here). Sampled one of the
      repeating HLE call args (`trap=0xf000072c`'s own `r3`) across the
      full run: **4,700 distinct values**, ending in a steady, real,
      linear climb (`...b06c0000 -> b0700000 -> b0740000 -> b0780000 ->
      b07c0000`, fixed `0x40000` steps) right up to the cutoff --
      genuine forward movement, not a stuck/oscillating loop, but of
      large and, from this evidence alone, unknown total magnitude (no
      confirmed terminal value reached even at 2B steps). This is
      real, legitimate, still-progressing work, just far past the
      point where any single, larger *static* step-budget constant
      would be a defensible permanent default (it would either still
      be too small for this title, or needlessly inflate every other
      title's own worst-case guard against a truly stuck loop).
      Reverted the exploratory budget change (`git diff` on
      `tools/game_probe.cpp` clean); this is exactly the interpreter-
      throughput scenario Phase 11 already anticipates ("Revisit JIT
      performance work if new titles expose interpreter bottlenecks")
      rather than a per-title tooling tweak. **Net status for this
      session**: Zeeboids and Volei no longer crash or wander at all
      (a real, committed, tested fix) -- what stands between that and
      actually reaching playable gameplay is now purely this
      interpreter-throughput wall, not any further identified
      correctness bug.

- [ ] **Checked whether Zebo Sports Tênis's own long-tick wall (this
      file's earlier entry, "not a dead loop, just long real work") is
      the same shape as Zeeboids/Volei's -- it is, and worse.** Tested
      the same exploratory bigger-budget approach: tick 1 alone still
      hadn't returned at 2 billion steps (71 real seconds), roughly the
      same order of magnitude as Zeeboids. Live-traced a concrete,
      well-evidenced hypothesis before assuming this needs raw
      interpreter throughput work: the repeating HLE calls' own trap
      address (`0xf000001c`) maps to `ModRuntime`'s `get_uptime_ms_fn`
      (7th registered HLE function) -- and that function's own doc
      comment already flags its "1ms per read" self-advance rate as
      "inferred, not measured." A live diagnostic at the real call site
      (`zeebotennis.mod` 0x127624, found via the caller's own `lr`)
      confirmed a real, tight, `elapsed = now - lastCheck`-shaped loop,
      settling into >2.5 million iterations of that one call site alone
      within the sampled window -- a real, evidenced lead, not a guess.
      **Tested it directly**: temporarily bumped the self-advance rate
      1000x, then 1,000,000x (a full simulated real second per read).
      Neither converged -- the 1,000,000x version still exhausted a
      500M-step budget, and did so with *far fewer* total HLE calls
      logged than before, meaning most of the remaining time is spent
      in dense in-module ARM computation between calls, not in
      additional clock polls. **This rules the busy-wait theory out as
      the primary driver** (a real, useful negative result, not a
      wasted round -- confirms this isn't a simple calibration fix).
      Reverted both experiments (`core/brew/mod_runtime.cpp` and
      `tools/game_probe.cpp` both clean against the pre-round
      baseline); 396/396 tests pass. **Conclusion**: Tênis's own wall
      is not smaller or more tractable than Zeeboids/Volei's after all
      -- all three "Crazyball engine" titles now point at the same
      real interpreter-throughput bottleneck (Phase 11's own
      already-anticipated JIT work), not a per-title quirk fixable by
      recalibrating one HLE stub.

- [ ] **Picked a seventh title, Alien Breaker Deluxe**, deliberately
      *not* another Crazyball-engine sports title or PopCap game or
      arcade-collection title sharing Super BurgerTime's own separate-
      ROM-dump requirement -- a small (134KB `.mod`, vs. the ~1MB+
      Crazyball-engine titles), architecturally different game, picked
      specifically to find out whether the interpreter-throughput wall
      is universal or specific to that one engine family.
      **Found and fixed a real bug in this project's own BAR parser**
      along the way: this title's real `data.bar` has a genuine zero-
      length resource entry (two consecutive offset-table values
      equal), which the parser's "strictly increasing" check rejected
      as corrupt. The real invariant is monotonically non-decreasing --
      `Extract()` already handles a zero-size entry fine. Fixed and
      tested (`core/loader/bar.cpp`, `tests/bar_test.cpp`).
      **Found the real ClsId** the same way as every other title this
      session (`0x0108e356`, the thin-wrapper `r0`/`cls_id` clobber).
      `CreateInstance` and `HandleEvent(EVT_APP_START)` both succeed,
      and the title reaches its own event loop.
      **Found and registered a 24th runtime-helper table slot**, offset
      `0x138` (immediately before the confirmed sprintf slot at
      `0x13c`): left unregistered, real code's own `blx` through it
      jumped through a null function pointer within the first tick (337
      steps -- a real, fast, findable gap, not the interpreter wall).
      Only one real call site found, real identity unconfirmed, so
      registered as a safe no-op matching the established precedent for
      single-call-site gaps. With this fix alone, the title reaches its
      own event loop cleanly and **stays there indefinitely with no
      crash, no wander, and no interpreter-throughput wall** -- a light,
      steady per-tick idle pattern, confirming this title generalizes
      the Crazyball-engine wall theory correctly (that wall is specific
      to that engine, not universal).
      **Built `--autopress`** (`tools/game_probe.cpp`, env-gated,
      opt-in): no OS-level input-automation tool available in this
      environment (no `xdotool`, no passwordless `sudo` to install
      one), so this reuses the file's own already-correct, already-
      guarded input-injection plumbing (both the HID path and a direct
      AVK `HandleEvent` call) to periodically press the confirm/advance
      button without a human at the keyboard -- a genuinely reusable
      tool for future bring-up work, not just this title.
      **Live-reported: the real window shows a black screen** despite
      the clean event-loop tick pattern. Traced concretely, not
      guessed: confirmed via a temporary diagnostic that
      `HasRealGlActivity` is false for the entire run and **zero real
      `IDisplay` calls ever happen** (no `DrawText`/`DrawRect`/`Update`)
      -- the title hasn't reached (or doesn't use) either real
      rendering path at all, so this isn't a rendering bug so much as
      "never gets far enough to draw anything."
      **Found and fixed two more real, generalizable gaps chasing
      that**: (1) `data.bar`'s raw bytes were only ever registered for
      `LoadResDataEx`'s ID-based lookup, never exposed as a directly-
      openable VFS file -- the same real "the archive's own raw bytes
      need to be a VFS entry too" shape `MergeGgzInto`'s own doc
      comment already documents for Double Dragon's `sound.ggz`. Fixed
      by registering the same bytes into the VFS too. (2) The real HID
      button-callback capture was gated on the callback's address
      literally matching Double Dragon's own compiled address
      (`ddragonz.mod` 0x11bdf4) -- a real, confirmed identification for
      *that* title specifically, not a real general rule; every other
      title compiles its own callback at its own different address, so
      the check could never match for anyone else. The real, general
      signal (per the bundled `GamepadMgr.c` reference source, already
      cited in that same comment) is call *order*: real code always
      registers exactly three signals through this slot, in a fixed
      sequence (connect, button-event, position-change). Switched to
      capturing the second call instead -- verified live this doesn't
      regress Double Dragon (its own button callback still captures
      and fires correctly).
      **Neither fix turned out to be the actual blocker for this
      title, but both are real, evidenced, and now generally
      available**: live-traced (not guessed) that Alien Breaker Deluxe
      never actually calls `CreateSignal` at all (zero calls against
      either the HID or SignalCBFactory objects the whole run) -- this
      title very plausibly doesn't use a gamepad/HID input path at all
      (a simple, casual breakout-style game), so the button-callback
      fix, while real and correct, wasn't the reason input wasn't
      reaching it. And `data.bar` is never opened directly either (the
      string "data.bar" only ever appears as `LoadResDataEx`'s own
      `pszResFile` argument, never as any other real call's argument)
      -- the VFS-exposure fix didn't change this title's own behavior,
      though it's still a real, correct, precedent-grounded fix worth
      keeping for whichever future title *does* need it.
      **The real, concrete remaining lead**: `data.bar`'s own resource-
      ID directory has exactly **one** entry (`type=0x5000,
      id=0x234e`), out of 87 raw resource entries total. Real code
      requests 49 distinct IDs via `LoadResDataEx` (all `type=0x5000`,
      a contiguous-ish range `0x2329`-`0x237f`), and only the one real
      directory entry can ever match -- confirmed live, all 48 others
      fail. Real, shipped, playable games wouldn't ship a resource
      directory this sparse relative to what their own code requests,
      so the most likely real explanation is that this title's own
      code uses some other, not-yet-identified addressing scheme for
      most of its assets (very plausibly: the one real ID that *does*
      resolve is itself a manifest/index resource the game reads first,
      then uses to compute real byte offsets into `data.bar` directly,
      through some real call this investigation hasn't identified yet)
      -- not something to guess at further without live-tracing that
      one successful load's own real consumer code next. 398/398 tests
      pass; all four fixes (BAR parser, runtime slot 0x138, VFS
      exposure, order-based signal capture) committed as real,
      permanent, tested changes -- nothing reverted this round.

      **Traced that one successful load's own real consumer, same
      session -- found the real root cause of the black screen.** Live-
      traced the caller of the one `LoadResDataEx(id=0x234e)` call that
      can ever succeed (`abd.mod` 0x10ee58-0x10eeb0, found via the
      real `lr` at the call site, not guessed): this is a generic
      "load a resource into a fresh buffer" wrapper -- gets the real
      size via a sibling helper, `malloc`s that many bytes, calls
      `LoadResDataEx` to fill it, then **returns the malloc'd buffer
      pointer completely unconditionally, never checking the real
      call's own success/failure return value at all**. So the 48
      other, failing lookups don't error out from the caller's own
      point of view -- they silently hand back a real, non-null,
      but entirely zeroed buffer (this project's own bump allocator's
      real zero-init behavior), indistinguishable from "loaded, but
      genuinely empty." That's a real, direct explanation for the
      black screen: from the game's own perspective every one of these
      loads "succeeds," just with empty content for 48 of 49, so it
      correctly has nothing to draw -- not a crash, not a stuck retry,
      real code doing exactly what its own (evidently not real-
      hardware-exercised, or at least not on this exact path) logic
      says to do with data that isn't there.
      **Concrete next step**: this is now a real, scoped, title-
      specific reverse-engineering question, not a further engine gap
      -- find what real data the one successful load (id `0x234e`,
      entry 37) actually contains and how the game's own code uses it
      (very plausibly a manifest/index the other 48 IDs are meant to
      be resolved through some other real mechanism, or a sign this
      project's own `data.bar` dump is itself incomplete/mismatched
      relative to what real hardware would have received). Not chased
      further this round; no code changes past the already-committed
      four fixes above.

      **Found a real, significant BAR-format bug chasing that manifest
      question, same session.** Live-traced the successful load's real
      caller (`abd.mod` 0x10ee58-0x10eeb0): a generic "load resource
      N into cache slot" wrapper used by every one of these call
      sites, not special-cased code -- and critically, it **never
      checks `LoadResDataEx`'s own success/failure return value**,
      unconditionally returning the `malloc`'d buffer either way. This
      is the real, concrete explanation for the black screen: the 48
      failing lookups don't error out from the game's own point of
      view, they silently hand back real, non-null, zeroed buffers,
      indistinguishable from "loaded, but empty" -- the game correctly
      has nothing to draw.
      Then, checking why so few of the 49 requested IDs have real
      directory entries at all: manually reinterpreted the bytes this
      project's own BAR parser was treating as an "unconfirmed,
      skipped 16-byte sub-header" -- they decode as **two more real,
      sensible 8-byte resource-ID records**, not padding. Cross-checked
      against the *other* real sample this project has (Peggle's own
      `resources.bar`) before touching any code: its own first 16
      "header" bytes decode the same way, to `id=3000->entry 0` (a
      real MP3 file) and `id=9000->entry 46` (a real 32768-byte raw
      texture block, matching that file's own already-documented
      texture-block shape) -- real, meaningful resources in *both*
      independent samples, not one coincidental false positive. Fixed
      `core/loader/bar.{h,cpp}`/`tests/bar_test.cpp`: no sub-header at
      all, straight 8-byte records from the very start of the
      sub-table. Alien Breaker Deluxe's own directory grows from 1
      record to the real 3; Peggle's grows from 60 to the real 62.
      399/399 tests pass.
      **Verified live: this real fix alone doesn't unblock rendering
      for Alien Breaker Deluxe** -- re-ran with the fix in place, same
      steady-state tick pattern as before (9 distinct shapes, same
      sizes, still zero real `IDisplay` calls the whole run). The two
      newly-resolved IDs (9001, 9037) aren't enough to matter: of the
      49 distinct IDs this title's own code actually requests, only 3
      now have a real directory entry at all, confirming the earlier
      theory -- this project's own `data.bar` dump for this specific
      title is very likely genuinely incomplete relative to what real
      hardware would have shipped with (not a further parser bug; the
      *format* is now understood correctly, cross-validated against a
      second title). Getting this specific title rendering would need
      either a more complete real dump or reverse-engineering
      whatever real fallback path real hardware uses when this
      directory doesn't cover a requested resource -- a genuinely
      separate, open question from anything fixed this round.
      **Net result for this title, this session**: a real, substantial
      list of permanent engine-wide fixes (BAR zero-length entries, a
      24th runtime-helper slot, VFS exposure for `resources.bar`,
      order-based HID signal capture, and now the BAR sub-header fix)
      -- five real, generalizable bugs found and fixed pursuing one
      title's bring-up, benefiting every title using this format, not
      just this one -- while this specific title's own path to
      rendering remains blocked on what looks like incomplete source
      data rather than anything left to fix in the emulator itself.

- [ ] **Picked an eighth title, Disney All Star Cards**, same session,
      same "keep trying architecturally different titles" approach --
      real ClsId `0x010940da` found the same way as every other title
      (thin-wrapper `r0`/`cls_id` clobber). `CreateInstance` succeeds
      immediately.
      **`HandleEvent(EVT_APP_START)` wandered after only 88 real
      steps** -- by a wide margin the fastest, cleanest gap this
      project has hit, and it kept happening again, one real gap
      deeper, every time the previous one got fixed: **eight more
      runtime-helper table slots** found and registered this round
      (0x30, 0x144, 0x14c, 0x150, 0x64, 0xcc, 0x90, 0x10 -- the
      project's 25th through 32nd confirmed slots), each one a safe
      no-op matching the established precedent (none had a calling
      convention confirmed well enough to implement for real), step
      count reaching the next gap climbing steadily each time (88 ->
      226 -> 3578 -> 5288 -> 6033, then past `HandleEvent` entirely
      into the real per-tick loop at 1405 -> 1513 -> 2210 -> clean).
      Also found and fixed a real, separate gap along the way: a
      genuine, unidentified real BREW class (`ClsId 0x0100100c`) that
      real code creates and dereferences without a null check, exactly
      the same "real `CreateInstance` call site this project's history
      keeps finding" shape as every other unidentified class --
      registered as a generic scaffold.
      **Confirmed the *last* gap efficiently, not by continuing to
      iterate one rebuild at a time**: a temporary diagnostic filled
      *every* still-unmapped table offset up to 0x400 with a logging
      no-op instead of a silent wander, then ran once with a real step
      budget -- found exactly one further real call (offset 0x10) in
      one pass instead of many more rebuild-retest cycles. With it
      registered, **this title reaches `CreateInstance`/`HandleEvent`
      success and stays in its own real per-tick event loop for a
      full 20-second run with no further wander, crash, or unmapped-
      slot hit at all** -- the cleanest bring-up of any new title this
      session, Alien Breaker Deluxe included.
      **Verified no regression**, important given the sheer number of
      shared-table slots touched this round: Double Dragon still boots
      cleanly from a fresh (non-save-state) launch with every one of
      these new slots active. A `--load-state` run did throw (`Block
      data transfer with S=1 (user-bank registers / exception return)
      not supported`) mid-investigation -- traced this to the *save
      file itself*, not this round's code: Double Dragon's own save
      state lives at a path shared with a separate, long-running real
      play session (a different worktree, still using the same ROM
      directory), which had itself saved much further into real
      gameplay than this project's own last known-good state. That's a
      real, separate, pre-existing interpreter gap (a real ARM
      instruction variant -- block data transfer with the S-bit set --
      this project's own interpreter has never implemented), genuinely
      unrelated to this round's runtime-table work, surfaced by real
      gameplay reaching further than any of this project's own testing
      has before. Worth a dedicated round on its own; not chased
      further here. 407/407 tests pass; every fix this round committed
      as real, permanent, tested code.
      **Not yet confirmed playable/rendering** -- reaching and staying
      in the event loop is a real, substantial milestone (the same one
      every previously-working title reached before becoming
      playable), but whether real gameplay/UI actually renders still
      needs a live look at the window, the same way Alien Breaker
      Deluxe's own "black screen" was only caught that way.
      **Follow-up session: confirmed live it's a real black screen, and
      root-caused why** -- a live look (screenshot of the real window,
      not just log output) showed only this harness's own "FPS:60"
      debug overlay on solid black, unchanged over several real
      minutes. Initially looked exactly like a hang (steps stopped
      advancing, process sat mostly in a real `poll` syscall) -- traced
      with a bounded, targeted live trace (this round's own version of
      the "fill every gap, run once" efficient-diagnostic technique)
      bracketing every step of the outer per-tick loop in
      `game_probe.cpp`, not just the ARM/HLE layer. That proved it is
      NOT hung at all: the real per-tick outer loop (SDL event poll,
      timer processing, audio mix, present) keeps cycling correctly and
      indefinitely at the real ~60fps pace -- it's just that after 4
      real per-tick timer callbacks (the same 4 ticks the original
      round already saw), the title's own code calls real
      `ISHELL_SetTimer` with `dwCount=0xFFFFFFFF` (~49 real days) and
      never arms anything else, so `IShellHle::Tick` legitimately
      returns zero expired timers forever after -- a correct, faithful
      emulation of what the title's own real code asked for, not a bug
      in this project's timer code. Confirmed via `Sdl2UnifiedBackend::
      HasRealGlActivity()` staying false the entire time: this title's
      own code never issues a single real `IDisplay`/GL draw call
      across all 4 ticks, so the screen is genuinely, correctly black
      -- there's simply nothing queued to draw yet. Tried simulated
      input (`ZEEBULATOR_AUTOPRESS=1`, this file's existing
      exploratory-only mechanism) in case this is the same "real code
      waits for a real human keypress at a title screen with zero
      simulated input" shape Double Dragon's own bring-up already
      confirmed -- no observable effect after 15 real seconds (same 4
      ticks, same final SetTimer call, no new draws). Left genuinely
      open: either real hardware's `SetTimer` treats a `dwCount` this
      large as some other special sentinel this project's own
      literal-faithful `ScheduleTimer` doesn't replicate, or this
      title's own real first-draw trigger is some other real event
      (a real HID device-ready callback, a real resource load
      completing asynchronously, ...) this harness doesn't yet supply --
      genuinely unresolved, not chased further this round. All temporary
      diagnostic instrumentation added to trace this (several rounds of
      `git checkout`-reverted printf bracketing in `game_probe.cpp`) was
      reverted before finishing; no source changes from this follow-up
      session were kept.
      **Root-caused, live, on the real desktop**: the user watched the
      real window directly and reported the true visible behavior this
      harness's own log-only testing had missed -- a real yellow splash
      screen for the first ~4-5 real seconds, *then* black, not black
      from the start. That real timing lines up exactly with the 4 real
      per-tick timer callbacks found above (each roughly a real second
      apart). Cross-referencing every real `ISHELL_LoadResDataEx` call
      this run made (trap 0xf000022c, the 42nd IShell vtable slot) against
      `allstarcards.bar`'s own real resource-ID directory (via this
      project's own `bar_inspector` tool) found the same real shape
      Alien Breaker Deluxe already exposed: **515 real raw content
      entries in the archive, but only 23 resource-ID directory records
      mapping any of them to a (type, id) real code can actually look
      up**. This title's own real loading sequence requests 59 distinct
      (type, id) pairs across those 4 ticks; only 7 have a real
      directory entry at all (id=1/9000 type 20480 -- likely the splash
      background/logo, matching what's actually visible -- plus
      id=100/501/600/1000/109 type 6). The other 52 -- mostly dense,
      obviously-sequential id runs (501-509, 600-627, 103/104/110/111/
      128/130-140, ...) that read exactly like "one id per playing
      card/UI tile" -- have no directory entry and come back EFAILED.
      Same real conclusion as Alien Breaker Deluxe, not a new bug: this
      project's own `.bar` dump for this title is very likely genuinely
      incomplete relative to what real hardware shipped with (the
      *archive*'s raw content is all there -- 515 real entries, plenty
      of real image/audio data -- only the *directory* naming most of
      it is missing), so this title's own path to further rendering is
      blocked on source data, not an emulator gap left to fix.

- [ ] **Picked a ninth title, Heavy Weapon** (a PopCap title, same
      lineage as the already-solid Peggle bring-up) after the user
      pushed back on "every title hits the same incomplete-data wall" --
      reasonably, given Alien Breaker Deluxe and Disney All Star Cards
      both landed there. Wanted a genuinely different outcome to test
      whether that pattern was real or this project's own blind spot.
      **Real ClsId `0x010978a2` found the same way as every other
      title** (thin-wrapper `r0`/`cls_id` clobber). `CreateInstance`
      immediately hit a real, previously-never-seen interpreter gap:
      **`REV Rd, Rm`** (ARMv6 byte-reverse-word, real encoding `cond
      0110 1011 1111 Rd 1111 0011 Rm`, sharing the "media instructions"
      space with the already-implemented Extend family but
      disambiguated by `bits[9:4]`), found in a real endian-swap loop
      over a loaded data block. Implemented, tested (`Cpu.
      RevByteReversesWord`/`RevOfZeroIsZero`), fixed the one further
      call this unblocked (**`REV16 Rd, Rm`**, REV's halfword-swap
      sibling, same encoding family) immediately after -- two real,
      permanent, generalizable ARM interpreter fixes, the project's
      first brand-new ARM opcodes since CLZ/SMULxy. 416/416 of this
      project's own tests pass (the only failures anywhere in the full
      suite are this repo's unrelated vendored liblzma tests,
      pre-existing).
      **With both in place, `CreateInstance`/`HandleEvent(EVT_APP_START)`
      both succeed cleanly and this title reaches its own real tick
      loop** -- further than Alien Breaker Deluxe or Disney All Star
      Cards got. Tick 0's own real per-object update processes a
      handful of real objects (each with real `LoadResDataEx`/runtime-
      helper calls matching this project's own established shapes),
      then genuinely never returns even given a 200,000,000-instruction
      budget (40x this project's own standard 5,000,000 ceiling) --
      confirmed, not assumed: two separate runs (5M and 200M budgets)
      hit the exact identical final HLE call before falling silent, and
      a live memory-value/CPU-time check confirmed real, if slow,
      ongoing execution rather than a true OS-level deadlock.
      **Root-caused, not just bounded**: periodic PC sampling across a
      50M-step window found execution cycling through a real ARMv6
      64-bit-division helper (`0x100028`, uses `CLZ` for bit-width
      normalization -- a genuine compiler runtime routine, not a bug in
      itself) from exactly one caller, ~1250 times per sampled site.
      That caller is a straight-line fixed-point trajectory/interpolation
      routine (`0x10a658`, no internal loop) invoked, in turn, from
      exactly one real site (`0x1295d0`, confirmed live: 4999/5000
      sampled calls share this one caller) inside a real, genuinely
      *finite-by-design* loop: `for (; r4 + 2*r5 < r7; r4 += r5)`.
      **The bug**: `r5` (the step) comes from a trivial real getter
      (`0x10d2a4`, literally `return *r0` -- a compiler-outlined
      accessor, not a bug either) reading a field of a real heap object.
      That field is 0. **Confirmed live via a direct memory watchpoint**
      on the exact real runtime address: it reads 0 from the very
      first step of the whole run and nothing -- no real code anywhere
      in the entire execution -- ever writes to it. A step of 0 means
      `r4` never advances, so this otherwise-correctly-terminating real
      loop spins until this project's own step-budget safety valve
      aborts it -- genuinely indistinguishable from a true infinite loop
      within any bounded budget.
      **Confirmed, not just circumstantial, the next session: traced the
      exact instructions connecting the resource-directory gap to the
      unwritten field.** Heavy Weapon's own real `heavyweapon.bar`
      resource-ID directory covers only **1 of 366** real raw archive
      entries (`bar_inspector`); the two `LoadResDataEx` calls this exact
      run makes (real ids 9055/9159) both fail -- neither matches the
      one directory record this file has (id 9001). A full instruction
      trace bracketing the real resource-loading wrapper at `0x125870`
      (called from `0x12418c`, requesting id 9159) shows it exactly
      matches the bug already documented for Alien Breaker Deluxe: the
      trap returns real `EFAILED` (`r0=1`), but two instructions later
      the wrapper reloads `r0` from a stack slot (`ldr r0,[sp,#8]` at
      `0x1258a0`) and returns *that* unconditionally, discarding the
      failure code entirely -- and the caller (`0x124190`) just does
      `mov r9, r0`, no success check at all. On a real success this
      slot would hold the newly-loaded data's own buffer pointer; on
      this real failure it's whatever was already there -- freshly
      allocated, never-written, zeroed memory -- which is exactly the
      zero that ends up read as the stuck loop's step size. Confirmed
      via a targeted `git checkout`-reverted instruction trace triggered
      on this exact malloc'd address, not inferred. Same real shape as
      Alien Breaker Deluxe, now independently reconfirmed on a *third*
      title (with Disney All Star Cards) -- strong, converging evidence
      this project's own dumps aren't unluckily incomplete three
      separate times; either the real `.bar` resource-directory format
      has a real fallback/computation this project hasn't found yet, or
      real hardware's `LoadResDataEx` itself resolves undeclared ids
      some other way this project hasn't identified.
      An earlier, separate attempt this round at a general fix (a
      "declared ids are block starts; undeclared ids in between fall
      through to sequential archive entries" fallback in `BarArchive`,
      tested live against Disney All Star Cards) made no visible
      difference there and wasn't validated against Peggle's own real,
      independently-confirmed-correct directory (whose id-gap/entry-gap
      arithmetic doesn't cleanly fit that same theory) -- reverted,
      not kept, pending either a cleaner test case or the real
      algorithm being found some other way (e.g. a title whose own
      real code demonstrably exercises a fallback path we could trace
      directly, the same way LoadResDataEx's calling convention itself
      was originally confirmed against a real Peggle call site).
      All temporary diagnostic instrumentation (multiple rounds of
      `git checkout`-reverted printf/watchpoint bracketing in
      `game_probe.cpp`) was reverted before finishing; only the real
      `REV`/`REV16` interpreter fixes and this write-up are kept.
      **Next session: found and fixed the real resource-directory
      algorithm -- a genuine, permanent, generalizable fix -- but it
      turned out not to be what was stalling Heavy Weapon specifically.**
      Re-examined the two "meaning unconfirmed" header fields bar.h's
      own doc comment already flagged (offsets 0/4): offset 0 is a
      constant format-version marker (`0x10011`) in every real file this
      project has; offset 4's high 16 bits, cross-checked against all
      four real samples, exactly equal the directory's own real record
      count every time -- proving the sparse directories aren't
      incomplete dumps (the header's own redundant field agrees), they're
      authored that way on purpose. That reframes the real question: if
      only a handful of ids are ever declared, real hardware must
      resolve the rest some other way. Confirmed directly, not
      inferred: Heavy Weapon's own real code requests ids 54 and 158
      past its one real directory record (`id 9001 -> entry 0`), and
      entries 54/158 both start with the real PNG magic
      (`\x89PNG\r\n\x1a\n`) -- exactly where "declared id is a run's
      first id; undeclared ids in the same run resolve to sequential
      entries by simple offset" predicts, not a coincidence.
      Implemented as the real, permanent `BarArchive::Find` (not a
      separate experimental method this time): nearest-preceding-
      same-type record, offset by `id - declared_id`, bounded by the
      next declared record of that type (or archive end) so it can't
      wander into unrelated content. 6 new tests (`bar_test.cpp`);
      420/420 of this project's own tests pass. Verified live against
      the real archive: `Find(20480, 9055)`/`Find(20480, 9159)` both now
      resolve to real PNG bytes that previously came back `EFAILED`.
      **Verified no regression the careful way, not just by assumption**:
      Peggle hits an unrelated, genuinely pre-existing real null-pointer
      jump (`pc=0`, 551 steps into `HandleEvent`) -- confirmed via a
      `git stash` A/B test that this happens *identically* with the old,
      unmodified `Find()`, so it's not something this fix caused.
      **But live-testing this fix against Heavy Weapon's own actual
      stall showed it doesn't resolve it** -- real, important negative
      result, not a wasted effort: confirmed live that the two
      previously-failing `LoadResDataEx` calls now genuinely succeed
      (`real_ret=0`, real buffers written), yet the exact same infinite
      loop still happens, character-for-character identical. Traced
      further and found the earlier round's own causal chain had a real
      error in it: the "wrapper discards `LoadResDataEx`'s return value"
      finding was real, but the specific *field* chased afterward
      belongs to a different, unrelated read (this project's own
      register-name reuse across two different real functions -- both
      happen to use `r5`/`r6` for unrelated things -- led one round's
      own tracing to conflate them). The real stuck value traces to
      `unknown_0x01030766_obj`'s own slot 12 (a real PopCap/Zeebo SDK
      interface, shared with Peggle, still otherwise unidentified):
      real code expects it to write a real out-param object whose own
      offset 20/22 (each a real uint16) hold a width/height-shaped pair
      that becomes a real, otherwise-correctly-terminating loop's step
      size. Implemented a bounded, defensible fix (write this project's
      own already-confirmed-correct real screen dimensions into a fresh
      object there, instead of leaving the slot as a blind stub) --
      **but live-tracing that fix showed the specific call this title's
      own stall depends on doesn't even reach this object**: by the time
      this exact loop iteration runs, the real field (`applet+48`) has
      already been reassigned to a *different*, dynamically-created
      "self-propagating stub" child (the existing placeholder for yet
      another still-unidentified real class, `0x0101eb0b`) -- a minimal
      object with only a vtable pointer, never meant to carry
      width/height data at all. Also found, live, a genuine ordering
      problem layered on top: the real code that should populate this
      exact 36-byte object's own field runs at step ~46753 of this same
      call, but the code that reads it runs at step ~2036 -- the read
      genuinely precedes the write in this project's own real execution
      order. Reverted the slot-12 scaffold change (confirmed, live, not
      to help this specific stall, and risked adding unproven
      complexity for no confirmed benefit) -- kept only the real,
      confirmed, permanent `BarArchive::Find` fix and this full,
      corrected write-up. **Genuinely unresolved**: Heavy Weapon's own
      stall traces through at least two more layers of unidentified
      real PopCap/Zeebo SDK classes than this round reached bottom on,
      plus a real cross-call ordering question this project's own
      per-tick dispatch may be getting wrong relative to real hardware
      -- a substantially deeper investigation than a single targeted
      fix can close, not chased further this round.
      **Retested Alien Breaker Deluxe against the same real
      `BarArchive::Find` fix** (deprioritized earlier this session on
      exactly the "3 of 49 requested ids resolve" signature this fix
      directly targets) -- real, measurable improvement, not a wash:
      previously stayed in an indefinite, black-screen idle forever with
      no crash; now runs 10 real ticks (past its own prior ceiling) doing
      new real per-tick work before hitting a *new* gap -- a real null
      object dereference (`obj=0`, not just a missing vtable slot) at a
      real vtable-dispatch call site matching the same "GetXxx(this,
      type_or_size)" shape Heavy Weapon's own investigation already
      named. Traced one level further: the null traces back to a real
      object field at offset 40 that was never populated -- but unlike
      Heavy Weapon's single-candidate chain, this exact field has 10+
      different real write sites across the binary (a common, reused
      struct offset, not a unique one), meaning isolating the real
      responsible site needs the same kind of live, targeted tracing
      Heavy Weapon's own round used.
      **Followed through anyway, live, and found a real, permanent,
      generalizable ISHELL interface fix**: traced the real init
      sequence gating that offset-40 field (`abd.mod` 0x103108) down
      through two more nested real functions (`0x10125c`, `0x10131c`)
      to its actual real root: `IShellHle`'s own vtable slot 43 --
      already flagged in this project's own doc comment as
      "unconfirmed -- the one real call site found so far" from an
      earlier Double Dragon disassembly round, now independently
      reconfirmed by a *second* real title. Real code requires **two**
      real conditions from this one call, not just "nonzero/success"
      the way most of this project's confirmed slots work: the literal
      return value 35 exactly (`cmp r0, #35`), *and* a real 3rd-arg
      out-param pointer that must end up non-null (confirmed live:
      returning 35 alone still hit the same real bail-out one
      instruction later, since real code checks `*pOut != 0` as a
      separate condition). Implemented both, tested
      (`IShellHle.Slot43ReturnsTheConfirmedRealLiteral35`/
      `Slot43AlsoWritesTheRealThirdArgOutParam`); writes this same
      shell object's own address into `*pOut` (always real, valid, and
      already fully vtable-built, so any further real call through it
      lands on genuine working slots) rather than guessing the real
      object's true identity. 422/422 of this project's own tests
      pass; verified no regression live against both Double Dragon and
      Peggle (Peggle's own output byte-for-byte identical to a
      pre-fix baseline capture).
      **Confirmed live this really is the right gate, not a
      coincidence**: with both parts of the fix in place, the deeper
      function (`0x10131c`) that previously bailed out early (return
      code 1) now runs substantially further and returns a real,
      different value (`0x905`) instead -- genuine forward progress
      through a real, multi-instruction gate, not a guess that happened
      to not crash. **Still not enough to finish this title**: `0x905`
      trips a further real check one level back up
      (`0x10125c`'s own `cmp r0, #0` gate), meaning the real chain
      continues at least one more level deeper than this round reached
      bottom on -- each level reached has been a real, confirmed,
      permanent fix in its own right (the BAR algorithm and now ISHELL
      slot 43), not a discarded guess.
      **Kept tracing, live, and found a real reframing of the whole
      problem, not just one more layer**: `0x905` isn't a wrong/garbage
      value -- traced its own gating flag (`[r4]`, checked `==1` deep
      inside `0x10131c`) back to its real write site (`abd.mod`
      0x103124-0x103128, `mov r0,#1; str r0,[sp]`), confirmed live via
      a targeted memory watchpoint on the exact runtime stack slot: real
      code deliberately sets this flag *itself*, immediately before
      calling the real lookup pair (`0x10ee58`/`0x10eec4`), then reads
      it back deep inside `0x10131c` to decide behavior -- genuinely
      intentional real state, not uninitialized memory or an emulation
      artifact. The specific branch taken from there compares `[r4+8]`
      (real data) against a real threshold and returns `0x905`
      ("not there yet") rather than taking the other, `carry-set` exit
      that skips straight to real success -- reading very much like a
      real, legitimate "async operation still in progress, real
      hardware would naturally retry on a later tick" pattern, not a
      bug in the 35/slot-43 sense the last two fixes were.
      **This whole init routine is only ever entered *once* in this
      project's own run** (confirmed live via an entry counter across
      the full 10-tick session) -- meaning the real retry that would
      presumably let real hardware eventually succeed never gets a
      second chance here, because **the crash itself is what prevents
      it**: real code unconditionally dereferences the still-null
      result without checking it (the same "real code doesn't check
      the return value" shape as every other gap this whole session),
      so this project's own interpreter faithfully crashes on the very
      first, still-legitimately-incomplete attempt instead of surviving
      long enough for a real retry to occur.
      **Genuinely unresolved, not chased further this session** given
      the time already spent across both this title and Heavy Weapon
      today -- but the real shape of what's needed next is now much
      clearer than "one more slot fix": either find what real condition
      makes `[r4+8]` cross its real threshold (letting this succeed
      immediately, the same way the last two fixes did), or make this
      project's own real per-object dispatch survive a real "not ready
      yet" result gracefully instead of crashing on it, the same way
      real hardware's own real event loop evidently does.
      **Fixed the exact issue just described, live, the same session**:
      the "real threshold" `[r4+8]` gets compared against was slot 43's
      own 3rd-arg out-param -- confirmed by checking the comparison's
      own live operands (`abd.mod` 0x101550: a real handle from an
      earlier real lookup, `0x163ac`, against this project's own
      previous fix's chosen out-param value, this same shell object's
      own address, `0x80001000`). A pointer-sized value there is
      *always* greater than any real handle, permanently forcing the
      "not ready" branch no matter what -- the out-param needed to be a
      small real value, not "any non-null real object" the way
      Heavy Weapon's own similar-shaped gap wanted. Fixed
      (`core/brew/ishell.cpp`, writes the small constant `1`), tested,
      verified no regression (Double Dragon/Peggle both unaffected).
      **With this fix, real code reaches the real success branch for
      the first time** (`abd.mod` 0x1016a0, confirmed live) -- genuine,
      new territory, not a guess that happened not to crash. **Found
      the real reason it still doesn't finish, live, in that same new
      territory**: this success path calls vtable slot 43 a *second*
      time, on the *same* shell object, with the *same* arguments
      (`this=shell, r1=0`) as the first call -- but this real code path
      needs it to return `0` here, not `35`, to take its own further
      real branch (confirmed live: it returns `35` again, since this
      project's own fix is a fixed value, and the real code's own
      `bne` skips the rest of the real write logic as a result). Real
      evidence this is the *same* real "async, not ready yet" shape
      the `0x905` finding already surfaced, just reached through a
      different path: the same slot, same arguments, needs a genuinely
      *different* answer the second time within one real logical
      operation -- a real stateful/polling contract, not a static
      value any fixed return can satisfy correctly. **Genuinely
      unresolved, not chased further this session**: the real fix here
      needs actual call-sequence-aware behavior (e.g. tracking real
      call counts or real elapsed state per real logical operation),
      which is a real design decision this project hasn't made yet,
      not one more value to discover by tracing.
      **Made that design decision, new session, and it's real progress,
      not just a static value swapped for another one.** First tried
      the simplest model matching the two confirmed real call sites
      exactly: a per-object latch (35 on a given object's first-ever
      call, 0 on every call after). Live-traced the result: it
      correctly finishes the *first* real per-object init sequence
      (`abd.mod` 0x1031dc's own real `[r4+40]` field goes from
      permanently null to a real, valid, non-null pointer for the
      first time ever) but then a **second**, sibling real object
      (`r4=0x80300044`, immediately adjacent to the first's
      `r4=0x80300040`) runs the exact same real init code and gets
      starved: its own call lands on the shared latch's 3rd call,
      already exhausted to 0, so its own `[r4+40]` stays null and the
      exact same real crash (a null real vtable jump, `abd.mod`
      0x101f60-0x101f74) recurs one object later instead of being
      fixed. Confirmed this really is "many real objects, each needing
      their own fresh 35-then-0 pair" and not a one-off: switched the
      model from a latch to a strict call-parity **toggle** (odd calls
      return 35, even calls return 0) -- still answers both original
      confirmed call sites identically (they're just this toggle's
      first two calls) -- and live-traced **35 consecutive real
      objects** completing their own init in a row, each getting its
      own real, valid, sequential pointer (`0x80200000`, `0x80200004`,
      ... `0x80200088`, one word apart -- an evidently real, intentional
      allocation pattern, not coincidence). Fixed (`core/brew/ishell.cpp`),
      tested (`IShellHle.Slot43AlternatesRatherThanLatchingAfterTheFirstCall`
      plus the existing slot-43 tests, updated for the toggle).
      **That real fix immediately exposed the next real gap, live**:
      once all 35 real objects finish initializing, real code (`abd.mod`
      0x106150/0x10619c, a real per-object post-init step) calls through
      runtime-helper-table offset `0x1c` -- unregistered, another real
      null-function-pointer jump, the same shape this project has fixed
      repeatedly elsewhere in this same table. Registered as a safe
      no-op, same established precedent as every other single-purpose
      gap in this table (`core/brew/mod_runtime.cpp`, tested:
      `ModRuntime.UnknownSlot0x1cIsWiredAndSafelyReturnsZero`).
      **Verified both fixes are real and don't regress**, live, against
      both other titles this project has: ran Double Dragon and Peggle
      through a clean `git stash` A/B against the exact last-committed
      state (not an older, stale capture) -- Peggle in particular shows
      the *same* kind of further real progress this title did: a
      previously-real crash (`peggle.mod` 0x105b70, after 588 steps) no
      longer happens at all; it now runs past that point until this
      project's own interpreter step budget (5,000,000 steps per call)
      is reached instead, meaning both fixes generalize beyond this one
      title's own bring-up. Double Dragon shows no behavioral difference
      at all past cosmetic HLE-trap-address renumbering (registering one
      more runtime-table slot shifts every later slot's assigned trap
      address by four, expected and harmless). 426/426 tests pass.
      **Where this leaves Alien Breaker Deluxe**: with both fixes in
      place, a full run reaches and stays in real, healthy, ongoing
      execution for the full length of a real, non-trivial run (90
      real seconds, still running, no crash, no wander) -- genuinely
      further than any previous round reached, past a real, permanent,
      35-object initialization sequence.
      **Corrected a wrong read on that "still running" state, same
      session**: first guessed this was the same real interpreter-
      throughput wall already documented for the Crazyball-engine
      titles (a single real ARM call running long without returning).
      Wrong -- live-traced with `--persistent-log` instead of guessing
      from a killed process's own partial, buffered stdout: this
      project's own harness caps its `--- tick N ---` print at the
      first 10 real ticks (see `tools/game_probe.cpp`) specifically so
      a long healthy run doesn't flood output, which is exactly what
      made a genuinely fine, ongoing run look stuck. The real log shows
      **1,854 real ticks completing cleanly** in a 30-second run (~31
      real ticks/sec, matching this title's own already-documented
      real cadence) -- a steady, repeating, 8-call-per-tick pattern
      (objects `0x80064000`/`0x80041000`, then a self-rearming
      `SetTimer`), not a stall at all.
      **The real, still-open problem is the one this investigation
      already named before any of today's fixes**: across all 1,854
      real ticks, there are **zero calls to the real `IDisplay` object**
      (`r0=0x80003000`) -- this title still never reaches or uses its
      own real rendering path, so the black screen persists even
      though the boot-time crash that used to mask this finding is now
      gone. **Not fully playable yet, and not chased further this
      session**: identifying what these two steady-state per-tick
      objects (`0x80064000`, the real HID device; `0x80041000`, this
      project's own shared `unknown_0x0103d8ec_obj` scaffold, already
      involved in a materially-bigger, dedicated identification task
      flagged elsewhere in this file for other titles) are actually
      polling for before the real game logic would proceed to drawing
      is a genuinely open, scoped reverse-engineering question, not a
      further slot/table gap this round's live-tracing-only method can
      safely guess at.
      **Ruled out one concrete hypothesis for that steady state, cheaply,
      before stopping**: tried this project's own existing
      `ZEEBULATOR_AUTOPRESS` probe (both the HID and direct-AVK
      `HandleEvent` injection paths) against this exact steady state --
      no effect at all, byte-for-byte identical per-tick call pattern
      with or without it. Consistent with, not contradicting, this
      investigation's own earlier finding that Alien Breaker Deluxe
      never registers a real HID button callback in the first place (so
      the HID injection path has nothing to reach) -- rules out "just
      waiting on a simulated player input" as the explanation, narrowing
      the real next step to actually identifying what `0x80064000`
      (real HID device, called four different ways every tick) and
      `0x80041000` (this project's own shared scaffold) are really being
      asked for.
      **Took that next step, same session, and ruled the whole HID
      chain out as a red herring -- live-confirmed, not just read
      statically.** Mapped the four real per-tick HID calls exactly via
      a live vtable dump: `GetNextButtonEvent`(9), `GetPositionState`
      (10), `GetMinPositionInfo`(11), `GetMaxPositionInfo`(12) -- a
      real, coherent "poll input" sequence, called from two distinct
      real functions (`abd.mod` 0x1024f8 for button events, gated on a
      real event actually being available; 0x102528 for position,
      unconditional every tick). Traced 0x102528's own real chain
      (`0x100834` -> `0x1009fc`) down to a real, substantial axis-type
      identification routine, and initially formed a plausible-looking
      static-reading hypothesis: a real UID comparison (`cmp r1, fp`,
      `fp` loaded from a real module literal) that this project's own
      all-zero default `GetAxesInfo` stub could never satisfy, since
      nothing ever populates the compared field. **Verified live before
      trusting that reading, per this project's own established
      convention -- correctly, since it was wrong**: a temporary return-
      value probe at `0x1009fc`'s own single return point showed it
      returning real success (`r0=0`) via that same UID scan's own
      real, intentional *fallback* path (already present in the real
      disassembly, missed on the first static read: if no UID matches,
      real code falls back to the first all-zero/unused slot, which an
      all-zero stub trivially satisfies). Then traced one level up:
      `0x100834` itself reaches its own shared return point
      (`0x100974`) with `r0=0` on every single one of 244 sampled real
      ticks -- not a failure bailout as the address's own branch
      structure first suggested, but that function's own normal
      *success* epilogue (a real, explicit `mov r0, #0` immediately
      above it, reached after a real cached-lookup call at `0x100804`
      that legitimately skips redundant work after the first tick).
      **Net finding: this entire real HID/position-poll chain completes
      successfully, every tick, with no failure and nothing further to
      do** -- healthy real code correctly reporting "no new input this
      tick," not a stuck or blocked gate. Reverted the temporary probes
      (`git diff` on `tools/game_probe.cpp` clean); the real cause of
      "never calls `IDisplay`" is confirmed to lie somewhere else
      entirely, not in this chain -- ruled out with live evidence, not
      abandoned on a guess.
      **Checked the fifth and last steady-state per-tick call too, same
      session, for the same reason -- and it's also inert.** Live-
      traced the scaffold object's own repeated call (`r0=0x80041000`,
      trap for vtable slot 25) two levels up: a small real dispatch
      wrapper (`abd.mod` 0x1147ec) looks up a real global object from a
      real static-base table (offset 8) and, only if non-null, forwards
      to *that* object's own vtable slot 25 -- generic, real "notify an
      optional registered listener" plumbing, not specific to this
      scaffold. Its own caller (`abd.mod` 0x107020-0x107034) passes two
      real object fields as arguments, both `0` in this run
      (`[r4+396]`/`[r4+400]`), and never inspects the call's return
      value at all (falls straight through to its own return). Given
      this project's own scaffold's default stub (a plain `SetRegister
      (kR0, 0)`, no memory writes), and given the caller doesn't branch
      on the result either way, this call is confirmed inert -- not a
      candidate for the real blocker.
      **Where this leaves the investigation**: both real steady-state
      per-tick calls this round examined (`0x80064000`'s HID chain,
      `0x80041000`'s scaffold notification) are now confirmed, live,
      to be healthy and inconsequential, not blocking gates -- ruling
      out the two most obvious candidates. The real reason Alien
      Breaker Deluxe never calls `IDisplay` is confirmed to live
      outside this per-tick idle chain entirely, most plausibly in
      scene/state-transition logic this round never reached (e.g. a
      real "still loading" flag gating when the game would first start
      drawing) -- a genuinely different, unexamined part of the code,
      not a deeper layer of the same chain. Not chased further this
      session; a fresh round should start from the loading/scene-
      transition angle instead of continuing down this per-tick path.
      **Picked that redirect straight back up, same session, and found
      the real scene/state gate the earlier entry predicted.** Read the
      self-rearming timer callback's own full body directly (`abd.mod`
      0x10b9f4), not just the HLE calls inside it: after real elapsed-
      time bookkeeping (two real subroutine calls, `0x1071d8`/
      `0x106d04` -- these are what actually contain the already-ruled-
      out HID/scaffold chain), it checks a real per-object flag at
      offset `432` (`0x1b0`) and returns immediately, skipping
      everything else, whenever that flag is zero -- which live-tracing
      confirms it always is, for the whole 1,854-tick run. This is the
      real gate: whatever real, substantial per-tick work this object
      does (a tail-called vtable slot beyond this check) never runs at
      all.
      Found where that flag is real, meaningfully set: a separate real
      per-object event handler (`abd.mod` 0x10b5c4, a `switch`-shaped
      dispatch on an internal event code in `r1`) sets it to `1` for
      code `3` (gated further on a real field, `[this+1460]`, equaling
      `-1` at that moment) and clears it to `0` for code `2` -- reads
      as a real, private "enable this scene's per-tick updates" /
      "disable" pair, not a generic BREW `EVT_*` code (this object's
      own internal enum, not AVK's). **Live-confirmed this handler is
      only ever entered once in the whole run**, with internal code
      `0` (not `2` or `3`), immediately at real startup -- consistent
      with this being reached via a real tail-call chain from this
      project's own single `HandleEvent(EVT_APP_START)` call, not a
      per-tick thing. Code `3` -- the one that would set the flag this
      whole chain is waiting on -- never arrives at all.
      **This is now a concrete, scoped, well-evidenced next step, not a
      vague "look at scene management"**: find what real, higher-level
      trigger (very plausibly a different real `HandleEvent` code this
      project's own harness never sends after the initial
      `EVT_APP_START`, or some other real async completion this round
      didn't trace back far enough to find) is supposed to deliver
      internal event code `3` to this object on real hardware. Not
      chased further this session -- reverted the temporary LR/event-
      code probe (`git diff` on `tools/game_probe.cpp` clean),
      426/426 tests still passing, nothing speculative implemented
      against an unconfirmed real trigger.
      **Took one more concrete step, same session, using a genuinely
      different technique than this whole investigation's usual LR-
      capture: a live memory scan, not a static-file search.** ROPI
      vtables/handler pointers don't exist as literal `0x0010b5c4`
      bytes anywhere in the raw `.mod` file (confirmed: a direct search
      found zero matches) -- they're only ever materialized as real
      absolute addresses in emulated RAM, at runtime, after this
      project's own loader resolves the real PC-relative construction
      sequences. Scanned the live heap/object address range instead
      and found exactly one hit: `applet_ptr + 24` (`0x8030003c`,
      `0x80300024` being this run's own real applet object) holds this
      handler directly as a plain stored field -- not behind a further
      vtable indirection, matching the "single dispatcher entered once,
      via a preserved LR" shape already found. Traced its own real
      write site (`abd.mod` 0x100680, inside an early real constructor-
      shaped function): the value stored there comes from a real
      caller-supplied constructor argument (`ldr r0, [sp, #44]`), not a
      fixed literal computed in this function itself -- a real, general
      "register a handler" composition pattern, not something hardcoded
      once and forgotten. **Genuinely unresolved, not chased further
      this session**: finding what real, live value actually flows
      into that stack slot means tracing this constructor's own caller
      chain (a different, bigger kind of tracing than the LR-capture-
      at-a-known-address technique this whole investigation has used
      so far) -- a good, concrete starting point for whoever picks this
      up next, not a dead end.
      **Found the real generic dispatch mechanism itself while tracing
      that write site, same session, and it reframes the whole
      investigation.** Right next to the constructor is a tiny, generic
      2-instruction trampoline (`abd.mod` 0x1005c8: `ldr ip,[r0,#24];
      bx ip`) -- exactly the "call whatever handler is installed at
      this object's own field 24" shape already inferred, and (per this
      project's own established "thin-wrapper" precedent) very likely
      reused for more than just this one applet. Live-captured every
      real entry into this exact trampoline with `r0` equal to this
      run's own real applet pointer, across a full 30-real-second,
      900+-tick run (not just the first few ticks): **exactly one
      hit**, `r1=0`, the very same startup call already found. This
      project's own harness (`tools/game_probe.cpp`) calls
      `HandleEvent` exactly once, for `EVT_APP_START`, then hands
      everything else to the self-rearming timer for the rest of the
      session -- confirmed live, this specific applet's own event
      dispatch genuinely never receives a second real call of any kind
      the whole time.
      **This reframes the real question, generally, not just for this
      one title**: the missing internal event code `3` isn't
      necessarily buried in an unidentified deeper real subsystem --
      it's entirely consistent with this being whatever real, second
      `HandleEvent` call (a real AVK code this harness has never had
      reason to send before, since every other title bring-up so far
      apparently didn't need one) real hardware would deliver after
      `EVT_APP_START`, that this harness structurally never generates.
      **Genuinely unresolved, not chased further this session**: this
      needs either finding the real, specific AVK code Alien Breaker
      Deluxe's own real code expects next (real BREW reference docs
      already bundled in `research/docs/`, not more live-tracing, is
      the right next tool for that), or building a small, targeted live
      experiment that tries sending a second plausible real
      `HandleEvent` call and checks whether the applet's own event
      dispatch receives it. Reverted the temporary trampoline probe
      (`git diff` on `tools/game_probe.cpp` clean), 426/426 tests still
      passing.
      **Did exactly that experiment immediately after, same session --
      used the real bundled reference docs rather than guessing.**
      `research/docs/sdk_installer_extract/brew_sdk_headers_reference/
      brew_1.1_sdk/AEE.h` defines `EVT_APP_SUSPEND 0x2` and
      `EVT_APP_RESUME 0x3` -- an exact, direct match to the internal
      codes `2`/`3` this investigation already found gating the real
      per-tick "enable" flag, not a coincidence (`EVT_APP_START` is
      also `0`, matching the internal code the object's own dispatcher
      is confirmed to receive already). This project's own harness
      (`tools/game_probe.cpp`) called `HandleEvent` exactly once, for
      `EVT_APP_START`, for every title, ever -- added a second, real
      `HandleEvent(EVT_APP_RESUME)` call immediately after it (same
      real `AEEAppStart*` argument the header documents both events
      sharing).
      **Confirmed live this is exactly right, down to the exact
      internal mechanism already traced**: the private dispatcher
      (`abd.mod` 0x10b5c4) now receives a real second call with
      internal code `3`, its own `[this+1460]==-1` guard passes, and
      the real "enable" flag at offset `432` is now `1` on every one of
      hundreds of sampled ticks (previously always `0`) -- the exact
      predicted effect, not a guess that happened to not crash. That
      flag flip visibly unlocks a real, one-time burst of further
      activity previously never observed at all: a new real
      `CreateInstance` (ClsId `0x0101eb0b`), three real
      `LoadResDataEx` calls (ids `9030`/`9073`/`9074`, type `20480`),
      and a brand-new real object address. Cross-checked those three
      IDs against this project's own confirmed real `BarArchive::Find`
      algorithm by hand: all three resolve to valid, in-bounds real
      entries (`29`/`72`/`73` of 87) -- this real resource load
      genuinely succeeds, not a repeat of the earlier-documented
      incomplete-dump problem.
      **Even with all of that working, this title still never reaches
      `IDisplay`** -- confirmed live over a full ~30-real-second,
      925-tick run: the one-time burst completes, and the per-tick
      pattern settles back to the exact same steady idle shape as
      before, zero `IDisplay` calls throughout. **Verified this real,
      permanent addition doesn't regress this project's own two
      standard verification titles**: Double Dragon and Peggle both
      accept the real `EVT_APP_RESUME` call cleanly and run
      identically to their own pre-change baselines (only cosmetic
      heap-address shifts from the extra real allocation activity the
      event itself legitimately triggers -- same trap sequences, same
      terminal states, Peggle's own step-budget-exceeded outcome
      unchanged). 426/426 tests pass. Kept as a real, permanent,
      generally-beneficial fix (`tools/game_probe.cpp`), not reverted --
      this is a real gap in this project's own test harness, not a
      title-specific hack: any title whose real code waits for
      `EVT_APP_RESUME` before doing real per-tick work would have hit
      the same silent no-op idle this one did.
      **Genuinely unresolved, not chased further this session**: Alien
      Breaker Deluxe's real remaining blocker is now confirmed to be
      something *after* this successful resource-load burst -- most
      likely the newly-created real object (from `CreateInstance
      (0x0101eb0b)`) needing its own further real trigger before it
      does anything visible, the same general shape as the gate this
      round just resolved, one level deeper. A fresh round should
      start by identifying that new object and tracing what it's
      waiting on, the same live-tracing methodology this whole
      investigation has used throughout.
      **Picked that up immediately, same session, and it went further
      than expected -- the "newly-created object" lead turned out to be
      a known dead end, but chasing the real resource loads next paid
      off big.** The new object from `CreateInstance(0x0101eb0b)` is
      this project's own already-documented, already-understood
      "self-propagating stub" scaffold (`tools/game_probe.cpp`'s own
      doc comment on `build_self_propagating_stub` -- a real, confirmed
      PopCap/Zeebo telemetry-shaped interface whose writes nothing
      reads back) -- not a new gate, a real dead end already known
      from Peggle's own earlier bring-up.
      The three resource loads were the real lead. Live-traced their
      real caller (`abd.mod` 0x10ee58, this project's own already-
      documented "load resource N into cache slot" wrapper from
      Peggle's own much earlier investigation) back to *its* own real
      callers, across a full run: **not 3 requests, closer to 49** --
      real code requests nearly this title's *entire* real asset
      catalog (ids `9001`-`9087`) once the resume-triggered enable flag
      is set, not a handful. Cross-checked against this project's own
      confirmed `BarArchive::Find` fallback algorithm by hand: all but
      six of those ids (`9031`-`9036`, genuinely outside every real
      directory record's own covered range) resolve to valid, real
      entries. **This means this whole session's very first fix (the
      real `.bar` sequential-fallback algorithm, `core/loader/bar.cpp`)
      was already correctly solving the "most resources fail to load"
      problem documented from a much earlier investigation round --
      this session simply hadn't seen it fully exercised until the
      `EVT_APP_RESUME` fix unlocked the real code path that actually
      requests them all.**
      **Even with nearly the entire real asset catalog now loading
      successfully, this title still never reaches `IDisplay`** --
      confirmed live over a full 60-real-second, 1,858-tick run: the
      resource-load burst completes early and the per-tick pattern
      settles back to the same steady idle shape, zero `IDisplay` calls
      the whole time. **Genuinely unresolved, not chased further this
      session**: real code needs some further, still-unidentified real
      trigger beyond "resources loaded" before it starts drawing --
      very plausibly another real event (the same general shape
      `EVT_APP_RESUME` itself was), a real "all loads complete"
      callback this project's own `LoadResDataEx` doesn't model
      (it's synchronous here; real hardware's own version may be
      asynchronous, with a real completion notification this project
      has no equivalent for), or something this round didn't reach.
      No code changes this round (pure investigation); 426/426 tests
      still passing, `tools/game_probe.cpp` clean.
      **Found and fixed a real, permanent bug the very next session:
      a genuine infinite loop, not just an unmet trigger.** User asked
      to keep pushing on this title specifically, wanting to see it
      actually running. Retried this project's own existing
      `ZEEBULATOR_AUTOPRESS` probe (ruled out for a different reason
      earlier) now that resources load -- this time it triggered real,
      new activity (real `AEECLSID_MEDIA` audio setup, real string/text
      processing) instead of nothing, but the injected key event's own
      call never returned even at a 20-billion-step budget. Live PC-
      sampling (not guessing) found a tight, real loop (`abd.mod`
      0x101abc-0x101b30) and traced the object it operates on down
      three pointer levels to this project's own `IHID` scaffold
      (`tools/game_probe.cpp`'s own `hid_obj`) -- real code was polling
      vtable slot 5 (`GetNextConnectEvent`, confirmed real signature
      and slot order already documented on this same object) in a real
      loop, looking for a device matching a real UID this file already
      names, `AEEUID_HID_Joystick_Device`. Left as the generic always-
      succeed default every unimplemented slot here uses, "no event
      pending" never arrives, so the loop never terminates -- a real
      bug, not legitimate expensive work (confirmed by the same live-
      verify-before-trusting discipline this whole investigation has
      used throughout, this time catching a wrong "maybe it's just
      slow" read). Fixed: slot 5 now returns `AEE_EFAILED` (no connect
      event pending), the same honest answer this project already gives
      for zero real joystick hardware via `GetConnectedDevices`. Real
      effect confirmed live: the hang is gone (1,860 clean ticks over
      60 real seconds, previously stalling on the very first injected
      key), and real code now visibly does more (audio + text
      processing) in response to a key press, though it still settles
      back to the same steady idle afterward and `IDisplay` is still
      never called. Verified no regression on Double Dragon and Peggle
      (`tools/game_probe.cpp`, committed as a real, permanent fix).
      **Also checked, live, whether the specific injected key mattered
      at all**: retried with `AVK_SELECT` (the real, standard BREW
      confirm key) instead of the file's existing `AVK_0` default --
      identical outcome either way, confirming the enumeration loop
      (and now its fix) triggers on *any* key event reaching this
      applet, not something specific to which key. Traced the
      enumeration function's own caller live: reached directly from
      `HandleEvent` itself via a tail-call chain (`lr` still the
      original trap-base sentinel), confirming this specific code path
      really is this applet's own key-press handler's joystick-connect
      check -- not deeper game logic incidentally touching HID, and not
      where a "start game" action would live. **Genuinely unresolved,
      not chased further this session**: confirmed zero `AEECLSID_GL`/
      `AEECLSID_EGL` requests anywhere in the entire run too (not just
      zero `IDisplay` calls) -- real code hasn't reached rendering
      *setup* at all yet, let alone drawing. The real remaining
      question is the same shape as before (what real trigger moves
      real code past its current idle state) but the search space is
      now smaller: whatever it is, it isn't gated behind this specific
      key-press handler's own logic, so it's somewhere else in the
      applet's own real event/state dispatch this round didn't reach.
      **Ruled out "just needs more real time" too**: ran a full 3
      real-minute, 5,592-tick idle session (no simulated input at all)
      -- no change, `IDisplay` still never called. Whatever this title
      is waiting for is a real, specific trigger, not a timeout or a
      splash-screen delay.
      **Exhausted this internal scene dispatcher's own remaining
      branches too, same session**: checked the two real cases this
      whole investigation hadn't tried yet (internal codes `11` and
      `1029`, the switch's own last untried branches) -- both are real,
      but inert: `11` just runs two debug-log calls before marking the
      event handled; `1029` marks it handled with no other real work at
      all. Neither is EVT_APP_STOP or a real, documented AEE.h constant
      this project could cross-check the way `EVT_APP_SUSPEND`/
      `EVT_APP_RESUME` were -- most plausibly private, app-internal
      codes this app sends to itself, not real system events reachable
      through `HandleEvent`.
      **Broadened the search past this one dispatcher, same session**:
      confirmed real `HandleEvent` doesn't route every event through
      it -- key events reach a separate, direct dispatch instead (this
      round's own earlier finding). Tried seven more plausible real
      system event codes cross-checked against the bundled real AEE.h
      (`EVT_APP_BROWSE_URL`, `EVT_APP_MESSAGE`, raw `EVT_KEY`,
      `EVT_COMMAND`, `EVT_DIALOG_INIT`, `EVT_DIALOG_START`,
      `EVT_NOTIFY`) directly via `HandleEvent`, right after
      `EVT_APP_RESUME`. Only raw `EVT_KEY` (0x100) came back "handled"
      (matching the already-explored code `256` case); every other
      candidate came back unhandled. All ran cleanly, no crash or
      wander, and each triggered its own small one-time burst of real
      activity before settling back to the identical steady idle state
      -- no combination reached `IDisplay`.
      **Genuinely unresolved, not chased further this session**: this
      round has now ruled out, live, with real evidence rather than
      guesses: every reachable branch of the confirmed scene dispatcher,
      a 3-minute timeout, seven more plausible real system events, and
      (last session) the entire HID/position-poll steady-state chain.
      Reverted the temporary candidate-event probe (`git diff` on
      `tools/game_probe.cpp` clean), 426/426 tests passing. What
      remains is real, but no longer reachable by trying more event
      codes at the same two entry points (`HandleEvent`'s own top level
      and this one scene dispatcher) -- the next productive step is
      almost certainly disassembly-driven: find where in the applet's
      own real code `IDisplay`/`AEECLSID_GL`/`AEECLSID_EGL` would first
      be requested (a real string/literal search for those ClsIds
      within `abd.mod`, the same technique already used to find
      `AEEUID_HID_Joystick_Device` earlier this session) and work
      backward from there to what real condition guards reaching it,
      rather than continuing to guess at what triggers it forward from
      the event side.
      **Did exactly that immediately after, same session, and it paid
      off in two real, permanent fixes.** Found real literal-pool
      entries for all three real ClsIds (`AEECLSID_DISPLAY`/`GL`/`EGL`)
      directly in the raw `.mod` file and traced the one real function
      that references `AEECLSID_EGL`/`0x0103d8ec` together (`abd.mod`
      0x101ba4-0x101c60): live-confirmed it's genuinely reached and
      genuinely succeeds (both `CreateInstance` calls return success)
      -- this project's own `EVT_APP_RESUME` fix was enough to get real
      code this far. Its own next step (`abd.mod` 0x101e08 onward)
      branches on a real getter's return value: if a real field is
      already non-null (confirmed live: it is, holding this project's
      own `0x0103d8ec` scaffold), real code takes an *alternate* real
      path instead of creating `AEECLSID_GL` directly -- calling that
      same scaffold's own real `QueryInterface` (vtable slot 2, the
      standard `INHERIT_IQI` convention every slot-2 in this whole
      family already follows) for two more real, related ClsIds
      (`0x0103d8dd`, `0x0103d8ea`). This scaffold never implemented
      slot 2 at all (only slots 4/5 were ever customized), so the real
      3rd-arg out-param stayed unwritten -- the same real "unchecked
      result, garbage out-param" shape this entire project has found
      and fixed dozens of times elsewhere. Fixed
      (`tools/game_probe.cpp`): writes a fresh, independent, all-slots-
      stub scaffold, this file's own established safe treatment for
      still-unidentified real interfaces.
      **That fix alone crashed differently, live -- not a step backward,
      a real, immediate second finding.** Real code calls vtable slot
      79 (byte offset `0x13c`) on the freshly-returned object -- the
      same "not enough vtable slots" shape this file's own HID device
      scaffold doc comment already precedents. Bumped that specific
      fresh scaffold from 40 to 200 slots. **Confirmed live this real
      fix genuinely resolves the crash and unlocks real, sustained
      further activity**: the *second* `QueryInterface` result (ClsId
      `0x0103d8ea`) gets called over 50,000 times in under a minute,
      with real, coordinate/physics-shaped numeric arguments -- a
      volume and shape of activity this title has never produced
      before this session, strong evidence of a real, substantial
      subsystem now genuinely running, not idling. Verified no
      regression on Double Dragon and Peggle; 426/426 tests pass.
      **Still doesn't reach `IDisplay`, even given 3 more real minutes
      to run**: this newly-active object's every method is still the
      generic always-return-0 stub, so whatever real per-frame state
      this subsystem polls for is never actually provided -- the same
      "polls forever, concludes nothing to do" shape as the fixes
      before it, just one level further in. **Genuinely unresolved, not
      chased further this session**: identifying this subsystem's real
      class and its own real per-slot contract (used 50,000+ times per
      run, so almost certainly a real, central per-frame system --
      physics, animation, or scene-graph-shaped, given the numeric
      argument shapes observed) is now the single highest-value next
      target, and a genuinely large, dedicated reverse-engineering task
      in its own right, not a quick follow-on fix.
      **Left concrete data for that next round instead of guessing
      further, same session**: a temporary vtable dump (reverted,
      `git diff` on `tools/game_probe.cpp` clean) mapped the real trap
      addresses observed back to real vtable slot indices on this
      specific object: slots `4`, `6`, `33`-`35`, `40`, `52`-`57`,
      `88`-`89`, `100`, `104`, `106`-`107` -- 17 distinct real slots in
      active use, not one or two, and several real argument values
      that decode as plausible fixed-point (16.16) screen coordinates
      (e.g. `0x012c0000` = `300.0`, `0x00960000` = `150.0`, well within
      this project's own 640x480 real display resolution). Consistent
      with a real, rich per-frame system, not a narrow single-purpose
      interface -- reinforces this as a dedicated task, not something
      a handful of guessed return values could safely stub around.
      **Went one round deeper into this subsystem, same session, and
      confirmed it's real, sustained, per-frame work, not a one-time
      burst.** Live-traced the real callers of the four highest-
      frequency slots (`33`/`54`/`100`/`107`, ~50,000 calls each across
      a 45-real-second run -- roughly once per real tick, not a startup
      spike): all four route through generic, reusable thin-wrapper
      trampolines (`abd.mod` 0x115354/0x115c24/0x115ad0/0x115d20, the
      same "preserve caller's own `lr`, forward through a vtable slot"
      shape already identified elsewhere this session), not distinct
      per-slot application code -- meaning the real application logic
      lives one level further out, at each trampoline's own real
      caller. Found one such real caller directly (slot 33's, `abd.mod`
      0x102b78-0x102c3c): validates a candidate position against a
      real `1600x1200` bound (a virtual canvas or level size, not this
      project's own 640x480 display resolution) before dispatching --
      real, non-trivial per-frame layout/placement logic, not idle
      polling.
      **Tried one cheap, evidence-motivated experiment before
      committing to deeper per-slot tracing**: swapped this whole
      fresh scaffold's blind default from `0` to `1` (testing whether
      real code reads `0` as a null/empty result rather than success
      for any of these calls) -- no observable change, reverted
      (`git diff` on `tools/game_probe.cpp` clean). Rules out "wrong
      default value" as an explanation; the real gap is real, distinct,
      per-slot behavior, not a single global miscalibration.
      **Genuinely unresolved, not chased further this session**: this
      subsystem is now confirmed to be a real, active, per-frame engine
      component (not a dead end, not a one-time init) whose full real
      contract spans at least 17 distinct slots behind generic
      trampolines -- correctly implementing it means tracing each
      trampoline's own real caller in turn, the same technique that
      worked for slot 33, repeated enough times to reconstruct a real
      picture of what this class actually is. A genuinely large,
      dedicated task on its own, appropriately scoped as its own future
      round rather than folded into this one.
      **Kept going the same session and it reframes the whole
      remaining task.** Traced slot 100's own real caller (`abd.mod`
      0x104e00-0x105010): a real jump table (`addcc pc,pc,sl,lsl#2`)
      selecting one of roughly eight real alignment-mode handlers based
      on a real type value (`fp`/`r11`, checked against `9`/`10`/`12`/
      `18`/`33`/`36`), each doing real 2D anchor math -- `x -= width/2`,
      `y -= height/2` for centered modes, then `x+width, y+height` to
      compute the opposite corner -- before assembling a real rectangle
      struct and dispatching into this same mystery object (via slot
      33's own trampoline, then slot 100's, in sequence). This is real,
      textbook sprite/text bounding-box computation for a real 2D
      layout system, not incidental math.
      **The real, honest conclusion this points to**: this mystery
      object very plausibly *is* this title's own software rendering/
      layout engine -- the thing real code hands a computed
      destination rectangle *to*, expecting *it* to turn that into
      real pixels. If that's right, no return value this project could
      answer these calls with will ever produce a visible frame,
      because the real missing piece isn't "the right stub value" the
      way every other fix this whole session has been -- it's that
      this project doesn't yet implement this engine's own real
      drawing semantics at all. Getting real pixels on screen from
      here needs translating this object's real draw-shaped calls into
      real `IDisplay`/GL calls ourselves, a genuine feature-
      implementation task (write a real bridge from this engine's
      contract to this project's own existing rendering backend), not
      a bug to keep hunting for. Substantially bigger in kind than
      anything else done this session -- flagged here rather than
      started without checking in first.
      **User left the decision on how to proceed to this project's own
      judgment; confirmed the hypothesis one round further before
      deciding, rather than committing to the big implementation on
      two data points.** Traced two more of the seventeen real slots
      live: slot 6, called with the literal `0x437F0000` (IEEE-754
      `255.0`) on all three of its own real arguments -- immediately
      after slot 4 gets called with all zeros -- inside a single real
      function (`abd.mod` 0x10547c-0x1054fc) that also calls slots 34
      and 54 (confirmed via their own real, distinct type parameters,
      `r0=2` and `r0=5`) plus several more not-yet-mapped trampolines,
      all before this same final `(255, 255, 255)` call. This is a
      real, textbook `SetColor(r, g, b)` shape (three equal 255.0
      channels = white), inside what reads as a single, complete real
      "draw one element" sequence: geometry, setup, then color, in
      that order -- not a coincidence layered onto the slot-100
      bounding-box finding, a second, independent confirmation of the
      same real conclusion.
      **Decision**: not starting the rendering-bridge implementation
      this session. The evidence is now strong enough to trust the
      diagnosis, but not strong enough to safely design the actual
      bridge -- only 3 of this engine's own real ~17-slot contract are
      even roughly understood (geometry, alignment, color), and this
      project's own established convention throughout this entire
      investigation has been to confirm structure before building on
      it, not after. A real implementation attempt built on this
      partial a picture risks the same kind of costly wrong-structure
      guess this project has explicitly corrected itself out of before
      (e.g. the strncpy/strncat precedent cited elsewhere in this
      file). The responsible next step is scoped, real work for a
      dedicated round: keep tracing this engine's own remaining real
      slots (the same live-tracing technique already used for 3 of
      17) until its actual contract -- not just "it draws things" but
      the specific real shapes needed to reimplement it -- is
      understood well enough to safely build a real bridge to this
      project's own `IDisplay`/GL backend. Not a dead end: a concrete,
      well-evidenced, appropriately-sized task for whoever picks this
      up next, with real, permanent progress (six confirmed bug fixes
      this session alone) already banked and unaffected either way.
      **Kept tracing the real "draw one element" sequence, same
      session, and reconstructed it close to end to end.** Mapped six
      more of this engine's own real trampolines (`abd.mod` 0x115644/
      0x11568c/0x11573c/0x115784/0x1154dc/0x115cf4) to real vtable
      slots (`52`/`53`/`56`/`57`/`40`/`107`) the same way as before,
      then read the one real function that calls all of them in
      sequence (`abd.mod` 0x10547c-0x1054fc) as a single, whole real
      operation for the first time, not slot-by-slot in isolation:
      **slot 40** `(r7, r8, r9, sl)` -- real per-element setup/select,
      called first; **slot 52** `(0xde1)` and **slot 53** `(0x8078)`
      -- two real single-value property calls; **slot 107**
      `(type=2, 0x140c, 0, &stackStruct)` -- the real geometry call
      already traced through slot 100's own alignment jump table;
      **slot 54** `(type=5, 0, 4)` -- another real property call;
      **slot 57** and **slot 56** -- the *same* two values from slots
      53/52 passed again, reads as a real "restore" pairing with the
      earlier "select" calls; a **conditional** call through a further
      real trampoline (`abd.mod` 0x11595c, only taken if a real stack
      flag is set) reaching **slot 88**; and finally **slot 6**
      `(255.0, 255.0, 255.0)` -- the real `SetColor` call already
      confirmed. A coherent, real, single-purpose sequence end to end:
      select/begin, set two real properties, set geometry, set another
      property, restore the two properties, conditionally do one more
      real thing, set color. 11 of this engine's own 17 real slots now
      have at least one confirmed real call-site context, not just a
      trap address.
      **Where this leaves it**: mapping the remaining slots (`34`,
      `35`, `89`, `104`, `106`) means chasing further nested real
      trampolines-within-trampolines (confirmed live this round: slot
      88's own real call site is itself gated behind a *second* real
      trampoline layer, not a direct call) -- real, continuing
      progress, but each additional slot is costing more live-tracing
      effort than the last for less new structural insight, since the
      broad shape (a real per-element draw sequence: select, set
      properties, set geometry, set color) is already clear. Good,
      natural point to bank this round's real progress rather than
      keep pushing through diminishing returns tonight -- the decision
      from earlier in this same session still holds: this is real,
      valuable, de-risking progress toward a future rendering-bridge
      implementation, not something to rush into building from here
      without the remaining slots' own real contracts confirmed too.
      **Found the real root cause anyway, same session, chasing a
      completely different thread: this title's own black screen was
      never really about the drawing engine at all -- it's a real,
      still-pending user-input gate, several layers below anything
      this investigation had reached before.** Live-traced the real
      per-tick "update" dispatcher (`abd.mod` 0x10c008): a real jump
      table on a real scene-state field (`applet+840`) that's been
      stuck at a real, out-of-range value (`16`) for the entire
      session so far -- confirmed live it never changes, ticks or not.
      State `16` falls through to a real do-nothing default case
      (immediate return, no drawing, nothing). Traced the real
      *transition* function separately (`abd.mod` 0x10d148, confirmed
      called ~once per tick): it computes the real next state via
      `abd.mod` 0x10be80, which returns `-1` ("nothing to transition
      to") every single time, because the real flags field it checks
      (`applet+772`) is permanently zero -- confirmed live, never
      written anywhere in the whole run. That flags field is gated,
      in turn, by a real countdown at `applet+1460`: real code only
      sets the flags once this countdown reaches exactly `0`, but the
      countdown itself is stuck at `-1` (a real "not started" sentinel
      this project's own earlier `EVT_APP_RESUME` fix resets it to,
      confirmed live), which is *not* a positive value that could ever
      count down to zero.
      **Found the one real place that sets a real, valid countdown
      value instead of `-1`**: the SAME internal event dispatcher
      already reverse-engineered earlier this session (`abd.mod`
      0x10b5c4) has a case for raw `EVT_KEY` (`256`) with `wParam`
      equal to the real key code `AVK_END` (`0xE02E`, confirmed
      against the real bundled AVK code table) that sets
      `applet+1460 = 5` unconditionally. **Confirmed live this is
      exactly right**: sending a real, direct `HandleEvent(EVT_KEY,
      AVK_END)` call (not through `--autopress`, which sends
      `EVT_KEY_PRESS`/`RELEASE`, `0x101`/`0x102` -- a different real
      event code than the raw `EVT_KEY` this specific dispatcher case
      actually checks for) returned `1` (handled), and the very next
      ticks show a real, dramatic escalation in activity: real object
      allocation at real, growing heap addresses, several brand-new
      real objects and vtable slots activating for the first time all
      session (real `IShell`/`IHID` calls never seen before, plus new
      calls on the mystery drawing-engine object).
      **This is the real trigger.** Not a guess that happened not to
      crash -- a fully traced, four-layer real causal chain from "the
      screen is black" all the way back to "a specific real button
      needs to be pressed," each link confirmed live, not assumed.
      **Where this leaves things**: after this real trigger fires, the
      title stalls again, but in a new, different, and more benign way
      -- not a crash, not an infinite ARM loop (confirmed via live PC
      sampling: real step counts are still climbing, just roughly
      1000x slower than this project's own established baseline,
      consistent with genuinely expensive per-step memory operations,
      e.g. this project's own sparse page-based `Memory` class paying
      real allocation overhead for touching many never-before-used
      pages during what's very plausibly bulk real-object
      initialization for an actual game level/playfield). This is the
      same real "interpreter-throughput" shape already flagged
      elsewhere in this project (Phase 11, "revisit JIT performance
      work") -- a real, known category of problem, not a new one.
      **Deliberately not made a permanent default**: unlike
      `EVT_APP_RESUME` (a general BREW lifecycle event every app can
      expect), `AVK_END` is a real, specific button real hardware
      typically treats as "back/exit" -- sending it unconditionally to
      every title the way this project already does for `RESUME` risks
      real, unwanted side effects in titles that don't share this same
      real "press END to leave the loading screen" behavior. Reverted
      the temporary direct-call probe (`git diff` on
      `tools/game_probe.cpp` clean); 426/426 tests still pass. This
      finding is recorded here, not wired into the default boot
      sequence -- a future round should decide how to expose it (a
      title-specific quirk table entry, an opt-in probe flag, or
      confirming the same key helps other stalled titles first) rather
      than this investigation deciding unilaterally for every title.
      **Checked whether the post-trigger slowdown was actually a bug in
      disguise before accepting "interpreter throughput" as the
      answer, same session.** Two concrete, checkable hypotheses:
      (1) real code allocating an unbounded number of objects in a
      runaway loop (matching the growing-heap-address calls already
      observed) -- live-counted them: only a handful of real calls,
      nowhere near the thousands that would indicate a real runaway;
      ruled out. (2) a real infinite loop through the still-only-
      loosely-confirmed runtime-table slot `0x1c` (registered as a
      blind no-op stub much earlier this session, for a different real
      gap in this same title) -- live-counted calls through it during
      the stall: fewer than 1,000 in a 30-real-second window, not a
      tight loop either; ruled out. Live PC sampling during the same
      window showed real, diverse execution across many different real
      code addresses with no single dominant hot spot -- consistent
      with genuinely varied real computation, not a stuck loop hiding
      behind a slow step-budget check. Reverted both temporary probes
      (`git diff` on `tools/game_probe.cpp` clean); 426/426 tests still
      pass. The "interpreter throughput" read stands, checked rather
      than assumed.
      **Wrong anyway -- corrected the same session, with real, direct
      timing evidence instead of inference.** User asked to commit to
      this one title until it works and pushed back on treating a
      "future round" writeup as a stopping point. Went back in with
      per-tick wall-clock timestamps (not step sampling): every real
      tick from 0-5 completes in under 1.3ms, several **under half a
      millisecond** -- the complete opposite of "expensive per-step
      work." **Tick 6 never starts at all.** Traced why directly: the
      self-rearming timer callback (`abd.mod` 0x10b9f4) checks a real
      per-object "enabled" flag (`applet+432`, the same one this
      session's `IHID`/`EVT_APP_RESUME` work already established) and
      returns immediately, skipping its own real tail-call into the
      deeper update logic, whenever that flag is `0` -- confirmed live
      this is exactly what happens on tick 5. This project's own
      harness only drives ticks by polling `IShellHle`'s timer queue,
      so once nothing re-arms it, the outer loop has nothing left to
      do and idles harmlessly forever -- indistinguishable from "stuck"
      by wall-clock alone, but not slow, not looping, not a performance
      problem in any sense.
      **Found the real, deliberate disable, live:** a second real
      function (`abd.mod` 0x105860, distinct from the already-known
      `HandleEvent` dispatcher) sets `applet+432 = 0` directly, then
      calls a real, bounded (exactly 4 iterations, not unbounded)
      cleanup loop and a further real subroutine, before tail-calling
      into another real object's own vtable slot 6. Reads as a real,
      intentional "pause per-tick updates, tear down up to 4 real sub-
      objects, kick off whatever comes next" scene-transition step --
      confirmed exactly once, not repeating, ruling out a disable/
      re-enable oscillation this round's tracing was just missing.
      **Checked whether a second real `EVT_APP_RESUME` (the same fix
      that unlocked this whole thread) re-enables it -- it doesn't.**
      Grepped every real write site to `applet+432` in the whole
      binary: exactly four, and the only two real "enable" writes are
      both part of the already-known `EVT_APP_RESUME` handling this
      project already sends once at startup -- there is no separate,
      dedicated "resume after this specific pause" write site anywhere
      in the binary. Live-confirmed a repeated real `EVT_APP_RESUME`
      call during the stall has no effect: the flag stays `0`,
      consistent with `EVT_APP_RESUME`'s own already-documented
      `applet+1460 == -1` guard no longer holding (this session's own
      `AVK_END` fix already moved that field to a real positive
      countdown, not `-1`, so the same real real gate a second `RESUME`
      call depends on is itself no longer open).
      **Where this leaves it**: the real "what re-enables per-tick
      updates after this specific pause" question connects directly to
      the same still-unidentified drawing-engine subsystem this
      session already flagged as a dedicated task (the tail-called
      slot-6 method reached at the end of the disable sequence lands on
      a real, not-yet-mapped object) -- not a new, separate mystery,
      the same one, one layer closer. Reverted the periodic-resume
      probe (`git diff` on `tools/game_probe.cpp` clean); 426/426 tests
      pass. The corrected, verified state of this title: real, fast,
      healthy execution the entire way through a real, deliberate
      pause -- not a performance ceiling, a real design this project
      hasn't finished reverse-engineering yet.
      **Corrected a real mistake in this session's own earlier finding:
      `AVK_END` doesn't unlock gameplay -- it triggers a real app-exit
      sequence.** Traced the tail-called slot-6 method flagged above
      (the real object reached at the end of the disable-per-tick-
      updates sequence) all the way through: `applet+12` holds
      `0x80001000`, this project's own real `IShell` object
      (vtable `0x80000000`), and slot 6 on that real vtable is
      `ISHELL_CloseApplet`. So the real causal chain this session
      traced earlier (`AVK_END` -> `applet+1460 = 5` -> countdown
      reaches 0 -> flags set -> scene transition -> disable per-tick
      updates -> tail-call) ends in a real, intentional app close, not
      a "start playing" transition -- consistent with `AVK_END`'s real
      hardware meaning ("back/hang up"), not a lucky guess that
      happened not to crash. The earlier finding was still real and
      still correctly traced -- just mis-labeled at the very last step.
      **Switched to `AVK_SELECT` (`0xE035`, confirmed against the real
      bundled AVK table) as the safer key and confirmed live it does
      not trigger `CloseApplet`:** the self-rearming timer keeps
      re-arming every tick with no gaps (1238 real `SetTimer` calls
      across 1238 real ticks in one 40-real-second run), and the still-
      unidentified drawing-engine object keeps running continuously and
      healthily the whole time (42,611+ real calls to its highest-
      frequency slots in the same window). This is now the better real
      candidate for whatever "start gameplay" trigger this title
      actually needs -- not yet wired into the default boot sequence,
      same reasoning as `AVK_END` before it: a title-specific quirk,
      not a general BREW lifecycle event.
      **Built and live-verified a proof-of-concept rendering bridge**
      from the still-unmapped drawing-engine object's real geometry/
      color calls into this project's own real `IDisplay::DrawRect`,
      to test whether the real x/y/height data this session already
      decoded from those calls is genuine layout data or noise.
      Consolidated the draw call into the same real slot that receives
      the real geometry struct (avoiding a real ordering mismatch
      against a separate color-setting slot, which was tried first and
      produced only one truncated rectangle). Result, captured as a
      real RGB565 framebuffer dump and inspected visually: solid
      horizontal bars at multiple distinct real y-positions spaced
      ~15px apart, spanning the left ~250px of the real 640x480 frame
      -- a real, structured, visually-verified match for a menu/text-
      line layout screen, not random noise. **Confirms the real x, y,
      and height fields decoded earlier this session are genuine
      layout data.** The real width field is NOT confirmed -- the
      corresponding struct field consistently reads `0` in every real
      call observed, so the screenshot used a fixed guessed width
      (250px); real width is very plausibly computed elsewhere from
      real text-content length (this drawing call is very plausibly a
      real text/menu-line draw, not a plain filled rectangle), which
      this round didn't trace. Experimental bridging code (in
      `tools/game_probe.cpp`, plus a temporary `IDisplayHle` pixel-
      readback accessor) was deliberately reverted rather than kept or
      committed, since it encodes that unconfirmed width guess as if it
      were real behavior -- `git diff` clean, 426/426 tests pass. This
      finding is recorded here as a real milestone (first-ever genuine,
      data-driven pixel output for this title, not a synthetic test
      pattern) but the rendering bridge itself is not yet a real,
      permanent feature: next real step is tracing the real width
      source (most likely inside the same not-yet-mapped drawing-engine
      object, possibly alongside real glyph/text data) before building
      a bridge worth keeping.
      **Found the real width source, same round: the guess was
      unnecessary -- slot 107's own 16-byte struct already contains it,
      just not as the 16-bit fields this session's earlier decode
      assumed.** Set up a proper `game_probe` invocation for this title
      (real ClsId `0x0108e356` = decimal `17359702`; this title ships
      no `data.ggz`/`sound.ggz` of its own, so `MergeGgzInto` was given
      a minimal synthetic empty-GGZ file instead of a real asset) and
      live-dumped slot 107's raw struct bytes directly, instead of
      guessing a layout from static disassembly alone. **The struct is
      16 bytes: four consecutive `int32`s, each real BREW/Zeebo 16.16
      fixed-point, encoding `{x0, y0, x1, y1}`** (opposite corners, not
      the `{x, y, dx, dy}` shape this project's own `AEERect` convention
      uses elsewhere) -- confirmed unambiguously, not inferred: dividing
      the raw words by 65536 produced exact, clean real numbers matching
      real screen geometry (`640.0` = the real display width, `320.0`/
      `240.0` = exactly half of it) across every one of 40 live-captured
      calls, plus a real, live, per-tick *animation* (a horizontal
      segment expanding outward from `(320, 240)` by ~2px/tick each
      side) -- the kind of coherent, structured real data no guessed
      layout could produce by accident. Real width = `x1-x0`, real
      height = `y1-y0` (frequently `0` -- real degenerate-height calls
      draw real horizontal lines, not filled rects, which is exactly
      why this session's earlier `{x,y,dx,dy}`-shaped decode kept
      reading a bogus `dx` and had to fall back to a guess: it was
      reading the wrong bytes as the wrong field width entirely, not
      reading a genuinely-absent value).
      **Rebuilt the rendering bridge with this confirmed layout (no
      guessed width) and captured a second screenshot: real, coherent,
      structured output** -- a real full-width divider line near the
      top of the frame with a real gap in the middle (two separate
      real elements, not one), and a real solid triangle wedge directly
      below it, apex up, widening toward the bottom. The triangle isn't
      a separate real shape -- it's this project's own framebuffer
      correctly, faithfully accumulating 166 real ticks' worth of the
      real expanding-horizontal-line animation (never cleared between
      ticks in this quick capture), each real tick's line 2px wider and
      2px lower than the last -- i.e. the *shape itself* is an artifact
      of this capture method, but every individual line inside it is
      real, correctly-decoded per-tick geometry. Strong, first-ever
      visual proof this session's struct decode is right, not a second
      lucky-looking guess: unlike the earlier round's screenshot, every
      number behind this one is confirmed, not assumed.
      **Still not committing a permanent bridge.** This round confirmed
      the real shape of exactly one of this engine's own ~17 real
      slots (107, geometry) -- real color (slot 6 in the general "draw
      one element" sequence documented earlier this session) isn't
      wired into this specific call path, so the experimental code
      used a hardcoded white; and this is still only one real call
      pattern among several the jump-table-driven caller (`abd.mod`
      0x104e00-0x105010) supports. Reverted the experimental code
      (`tools/game_probe.cpp`, the temporary `IDisplayHle` pixel-
      readback accessor) again, same reasoning as before -- `git diff`
      clean, 426/426 tests pass. What's different this time, and worth
      recording precisely: the remaining gap to a real, permanent
      bridge is no longer "is this struct's layout even understood,"
      it's "wire up the remaining slots' already-partially-understood
      contract (color, alignment, per-element setup/teardown) around a
      geometry decode that's now fully confirmed."
      **Went one round further, same session: identified real slot-107's
      LR is useless for telling its four real call sites apart (`blx`
      unconditionally overwrites it with the trampoline's own internal
      return address), but the real caller's actual address survives on
      the stack one word past the struct pointer** (`ReadStackArg(core,
      1)`, the trampoline's own `push {r3, lr}` at entry) -- the same
      technique now applies to every other confirmed generic trampoline
      in this engine, not just slot 107's. Used it to split this
      round's captures by real call site instead of treating them as
      one undifferentiated stream: **caller `0x1054bc`** (the
      `0x10547c-0x1054fc` "draw one element" function already
      documented earlier this session) produces only the full-width,
      zero-height top divider line (`rect=(0,0,640,0)`), and **every
      single one of its real invocations this round (24/24) is
      immediately followed by a real `SetColor(255.0, 255.0, 255.0)`
      call from that exact same caller** (`0x1054f8`, confirmed via the
      same stack-slot technique on slot 6's own trampoline) -- a clean,
      one-to-one, real geometry-then-color pairing. **Caller `0x104f84`**
      (case `104f60` in the jump table, this round's earlier bridge
      target) produces both the degenerate center point and the
      expanding-line animation from the *same* real call site -- i.e.
      one real code path legitimately draws multiple different real
      elements across a tick, not one call site per element -- and
      **never had a real slot-6 call of its own anywhere in this
      round's captures**, meaning its real color is set once elsewhere
      (very plausibly during this engine's own already-documented
      "select/begin" step earlier in its 11-slot sequence) and persists
      as real per-object state rather than being re-sent with every
      geometry call. This resolves what would otherwise have been a
      real correctness risk for a future bridge (hardcoding white for
      every element happened to be right for this round's captures, but
      not because color is uniformly a constant -- it's because one
      real path sends it explicitly and the other carries it as real
      state this project doesn't track yet). No code changes kept
      (`git diff` clean again), tests still 426/426 -- this round was
      pure investigation, same disciplined "confirm before building"
      pattern as the rest of this session.
      **Pushed the color question to an exhaustive negative result, same
      session: over a full real 60-second run, every single real slot-6
      call (299 of them) comes from the same one caller (`0x1054f8`)
      and passes the exact same literal `(255.0, 255.0, 255.0)` every
      time -- no variation, no other real caller ever invokes it.**
      Strong, real evidence (not proof, but a full real minute with zero
      counterexamples) that white is this engine's own real default
      color for freshly-drawn elements, and the one real caller that
      does call slot 6 explicitly is just reasserting that same default
      rather than overriding it to something else. Also traced slot 40
      (the real per-element "select" step, called once from its own one
      real site, `0x105490`) -- its three real arguments (`0x5500`,
      `0x3c00`, `0x8900`) don't decode as a color under any convention
      already confirmed elsewhere in this engine (not 16.16 fixed-point,
      not IEEE-754 float, not a 0-255 byte triple), so real color still
      doesn't have a second, independent real source to point to for the
      state-carried path -- it's very plausibly a genuine object/type
      handle into a table this project hasn't identified.
      **Found the real reason case `104f60` alone produced two visually
      different real elements (the center point and the expanding
      line) from a single real call site.** Traced the jump table's own
      full real target list (`abd.mod` 0x104eb4-0x105008, 21 real
      entries): five distinct real `sl` values (the switch selector)
      all target the exact same code, `104f60`, and every other real
      target (`104f08`, `105044`, `10506c`, `105018`, `104f34`,
      `104fb8`, `104fe8`, `10509c`) converges back into that same
      `104f60` tail after populating a different subset of the
      `sp+4..sp+32` scratch area first (confirmed for `104f08`'s own
      real path: `b 0x105010` -> `b 0x104f5c` -> falls straight into
      `104f60`). So this isn't "one case, two real elements by
      coincidence" -- it's "eight distinct real element *kinds* share
      one real trampoline call sequence," differentiated only by
      whatever real data (from the caller's own `r4` object, offsets
      24/28/32/36) each case's own distinct `sp+4..32` population feeds
      into slot 100's real alignment math beforehand. This is real,
      useful structural confirmation, but mapping what each of those
      eight real cases actually represents (menu item? sprite? divider?)
      needs tracing each one's own real, distinct field population in
      turn -- a well-scoped continuation for a future round, not started
      this session. No code changes kept, tests still 426/426.
      **Rebuilt the confirmed bridge (geometry + white default, both
      confirmed this session) one more time and captured a real,
      multi-frame sequence (ticks 100/300/600/1000/1600/2400,
      clearing the framebuffer between shots this time, unlike the
      earlier one-shot dump) to watch the real animation play out
      rather than accumulate.** **Real, structured progression, not
      noise**: at tick 100, only the expanding-line/point element is
      visible (the top divider hadn't been drawn yet this run). By
      tick 1000, a real, richer layout has appeared: the top divider
      line (with its real center gap) plus three additional short real
      horizontal lines cascading diagonally down-left toward the
      triangle -- consistent with more of this engine's own real
      elements (from the seven still-unmapped switch cases) becoming
      active as the title's own real init sequence progresses, exactly
      as expected from a real, live system rather than a static test
      pattern. **The screen then reaches a genuine steady state**:
      pixel count (`3040` real nonzero pixels) and the frame itself are
      byte-for-byte identical at tick 1000, 1600, and 2400 -- a full
      1400 real ticks (~45 real seconds at this title's own established
      cadence) with zero change, meaning this is a real, settled screen
      the title is genuinely holding, not a transient mid-animation
      frame this capture happened to catch. Strongest visual evidence
      yet that this session's real, confirmed decode produces a
      coherent, stable, structured real screen -- directly responsive
      to "keep on it till it works": there is now a real, reproducible,
      non-trivial image behind this title's own real geometry calls.
      Reverted again (`tools/game_probe.cpp`, `IDisplayHle` pixel
      accessor) -- `git diff` clean, 426/426 tests pass. Still not
      wired in permanently, same reasoning as every prior round this
      session: real color-as-state and 7 of 8 real element kinds remain
      unmapped, and a screen this simple (dividers, a triangle) is not
      yet confirmed to be what a real player is actually meant to see
      at this point (could be a real placeholder/debug screen this
      title draws before real content loads, not proven either way).
      **Found a real, fourth call site into slot 107 -- and it's real
      text, not another shape.** Widened the live capture (all 200
      slots on this scaffold, not just 107/6/100) and found real caller
      `0x105744` firing far more often than the other three (2103 real
      calls in one 90-real-second run, roughly once a real tick) --
      decoded its own real struct with the same confirmed 16.16
      fixed-point layout and got a real, unmistakable pattern: a run of
      rects each exactly 15px wide and 0px tall, x-adjacent
      (`32→47→62→...→137`), then a real gap, then a second run
      (`325→340→...→610`) -- **real, monospace character-cell layout for
      a text string**, not a shape. This is very plausibly the actual
      real source of what this session's earlier rounds kept calling
      "the width mystery": not a missing field to decode, but a whole
      separate real text-layout call path this session hadn't found
      yet.
      **Traced the real call sequence immediately around each cell call
      (all 200 slots logged in order, dumped on trigger) and found
      slot 107 is always immediately followed by slot 100 for this real
      caller too -- but slot 100's own real struct is all-zero, every
      time (30/30 real samples), unlike the meaningful structs the
      geometry calls carry.** No other real slot call appears between
      one character's `107`+`100` pair and the next -- ruling out "the
      glyph bitmap gets drawn by a nearby, not-yet-mapped sibling slot
      on this same object" as the explanation.
      **Checked whether this project's own already-implemented, already-
      working `IDisplay::DrawText` (used successfully by other titles,
      e.g. Double Dragon) is secretly the real answer -- it isn't.**
      Instrumented it directly: zero real calls, the entire run,
      confirming (not assuming) this title's real glyph rendering
      doesn't go through this project's existing text path at all.
      **Honest, real conclusion: this is a genuine second mystery, not
      a continuation of the first.** Slot 107/100 on this object
      establish real per-character cell *position* only; the actual
      real glyph bitmap must be drawn through some other, still
      unidentified real path this session didn't find -- a real GL/
      texture blit (this project has some GL support already) and a
      real device-bitmap interface (`GetDeviceBitmap`, already flagged
      as dereferenced directly by other real titles) are the two most
      plausible real candidates, neither traced yet. Reverted all
      instrumentation (`tools/game_probe.cpp`, a temporary
      `IDisplayHle::DrawText` probe print) -- `git diff` clean, 426/426
      tests pass. A well-scoped, genuinely exciting real lead for a
      future round: finding this title's actual glyph-blit path would
      likely unlock real, readable on-screen text for the first time in
      this project's history, not just shapes.
      **Chased the glyph-blit lead one round further, same session, and
      ruled out every other candidate path before landing on the real,
      simple answer.** Instrumented this title's real calls to GL
      (`glDrawArrays`/`glDrawElements`/`glGenTextures`/`glBindTexture`/
      `glTexImage2D`), the real device-bitmap `QueryInterface` path
      (`0x01001045`), and every currently-stubbed slot on the real,
      standard `IDisplay` object itself (`BitBlt`, `CreateDIBitmap`,
      `SetDestination`, `SetFont`, `SetClipRect`, ...): **zero real
      calls to any of them**, confirming (not assuming) this title's
      whole rendering pipeline never touches GL, the device-bitmap
      interface, or the real standard `IDisplay` object at all -- it is
      genuinely, entirely self-contained inside the one still-mostly-
      unmapped mystery engine object.
      **Tested the simplest remaining real hypothesis directly: extend
      this session's already-confirmed geometry+white bridge to this
      4th call site too, instead of assuming a separate glyph-bitmap
      call must exist.** Live-verified each individual draw does
      commit real white pixels at the moment it runs (`DebugReadPixel`
      immediately after the bridged `DrawRect` call, 30/30 samples) and
      that those pixels **persist** across at least 25 further real
      ticks afterward (not erased by anything) -- ruling out an early,
      wrong reading of a same-session accumulated-total check that
      looked unchanged (an artifact of comparing two differently-
      configured capture runs, not a real erase).
      **Result: a real, second, visually confirmed structured line.**
      A cropped, zoomed capture of the frame's top 70 rows shows the
      real text-cell row (y=51) rendering directly beneath the already-
      confirmed real top divider (y=0), **with a matching real gap in
      the same x-range on both lines** -- strong, real evidence this is
      a genuine two-line real UI element (e.g. a title + subtitle, or a
      header + body line), not incidental. The individual character
      cells render as plain solid bars (no glyph shape) since this
      bridge still doesn't know the real glyph bitmap source, but the
      real *layout* -- word boundaries, line position, alignment with
      the divider above -- is now visually confirmed correct end to
      end, using only this session's already-confirmed 16.16 fixed-
      point decode and white default, no new guesses. Reverted again
      (`tools/game_probe.cpp`, the temporary `IDisplayHle` pixel
      accessor) -- `git diff` clean, 426/426 tests pass. Real next step
      for a future round: the actual glyph bitmap source is still
      unidentified (this round's negative results rule out GL/device-
      bitmap/standard-IDisplay, but not, e.g., a real embedded font
      table read directly by this engine's own ARM code and rasterized
      via plain memory writes this project's HLE trap boundary
      wouldn't see at all) -- but the real payoff of finding it is now
      visually proven, not speculative.
      **Traced the real per-character loop all the way to its own real
      source string, same session -- found genuine, human-readable
      text, not just shape data.** Live-watchpointed the shared "draw
      one geometry element" helper's own real entry point (`abd.mod`
      0x105510) to find its one real, live caller during this run
      (`abd.mod` 0x106508, itself called from exactly one real site,
      `abd.mod` 0x105db8) -- and found that caller's own real object
      pointer argument (varying per call: `0x80301f08`, `0x80302010`,
      `0x8030271c`, ...) is computed as `[r4] + char_code * 108`
      (`abd.mod` 0x105da0-0x105dac: `add r2,r0,r0,lsl#1` then
      `add r0,r2,r0,lsl#3`, i.e. `*27`, times the 4-byte word size) --
      **a real per-glyph descriptor table**, indexed by a real
      character code read one byte at a time from a real, ordinary
      null-terminated C string (`abd.mod` 0x105dc8: `ldrb r0,[r7]`,
      `r7` incremented by 1 and checked against 0 every real
      iteration -- textbook `strlen`-shaped real code). Watchpointed
      the containing function's own real entry (`abd.mod` 0x105ba0)
      to read that string pointer (real `r1`) directly, dereferenced
      it live, and got two real, live, human-readable strings straight
      out of this title's own binary: **`"V1.1.7a"`** and **`"music by
      atomic cat"`**. Not guessed, not reconstructed from font metrics
      -- read directly from real game memory at the exact address real
      code itself uses.
      **This confirms, completely, what this whole multi-round lead
      was chasing**: the real text-cell calls this session bridged
      earlier are exactly what they looked like -- a real version
      string and a real music-credit line, the kind of content a real
      splash/credits screen shows, not placeholder or debug output.
      The per-character 108-byte table entries (real glyph metrics,
      likely width/kerning/bitmap-or-texture-reference data, per this
      round's own real code: a width value multiplied and accumulated
      into the running x position, matching this session's earlier-
      confirmed 15px-per-cell advance) are the concrete, well-scoped
      next real target -- decoding that one 108-byte struct's real
      layout would very plausibly unlock genuine glyph shapes, not just
      cell positions, for the first time. No code changes kept (`git
      diff` clean), 426/426 tests pass -- pure live investigation, same
      technique (direct PC watchpoints reading real, live memory) this
      project has used successfully throughout this entire session.
      **Cracked the real 108-byte glyph descriptor's key field, same
      session, by tagging each live dump with the exact real character
      it belongs to (correlated against the two known real strings) and
      comparing across 16 distinct real characters.** Byte offset 16
      (word 4), decoded the same confirmed 16.16 fixed-point way as
      every other geometry field this session: a clean multiple of
      `16.0` for every single character, and the multiples themselves
      form a real, internally consistent character *index* -- not raw
      ASCII -- once sorted: `' '=0, 'a'=1, 'b'=2, 'c'=3, ... 'i'=9, ...
      'm'=13, ... 'o'=15, ... 's'=19, 't'=20, 'u'=21, 'V'=22, ... 'y'=25,
      ... '1'=28, ... '7'=34, ... '.'=69` (gaps are real, still-unseen
      characters this session's two sampled strings never used). **Real
      formula: atlas X-offset = char_index * 16.0** -- a textbook
      fixed-width font atlas layout (16px per real glyph cell, of which
      15px are the real glyph width already confirmed at byte offset
      24/word 6, leaving 1px real inter-glyph padding). Byte offset 28
      (word 7) is a real constant `25.0` across every character sampled
      (very plausibly the real atlas row Y-offset or cell height for
      this one font/size), and byte offset 36 (word 9) is a real
      constant `9` (plausibly a real font-size/style id). The struct's
      remaining bytes repeat this same word-4 value three times, each
      +16.0 apart (e.g. real `V` -> 352.0, 368.0, 384.0) -- a real,
      not-yet-understood sub-pattern (three atlas columns per glyph?
      three cached frames?) flagged here rather than guessed at.
      **Where this leaves it**: this session set out to find *any*
      real evidence this title draws real text at all, and ends up
      with a fully reverse-engineered, confirmed real font-atlas
      addressing formula instead -- the honest remaining gap to real,
      readable glyphs on screen is no longer "what does this struct
      mean," it's "find the real pixel/texture data this X-offset
      indexes into" (a real font atlas image, likely bundled in this
      title's own `data.bar` or a similar real resource archive this
      project already knows how to parse) and "find the real charset
      table mapping ASCII to this curated index" (very plausibly a
      short, static, greppable byte array somewhere in `abd.mod`
      itself). Both are concrete, bounded, well-evidenced tasks for a
      future round -- not further speculation. No code changes kept
      (`git diff` clean), 426/426 tests pass.
      **Self-correction, same session: the "108-byte struct with an
      unexplained +16.0-per-block repeat" from the entry just above was
      wrong -- re-derived the real indexing arithmetic by hand (`abd.mod`
      0x105d98-0x105dac: `add r2,r0,r0,lsl#1` then `add r0,r2,r0,lsl#3`
      is `r0*3 + r0*8 = r0*11`, then `add r6,r1,r0,lsl#2` is `+ r0*4` --
      **`*11*4 = *44`, not `*108`**) and confirmed it directly against
      already-captured real data: consecutive real character indices
      sit exactly 44 real bytes apart (`'a'`(1) to `'b'`(2):
      `0x80301b98-0x80301b6c = 0x2c = 44`; `'b'`(2) to `'c'`(3): `44`
      again; `'V'`(22) to `'1'`(28), 6 real indices apart:
      `0x80302010-0x80301f08 = 0x108 = 264 = 6*44` exactly). **The real
      per-character record is 44 bytes (11 words), laid out as one
      plain, contiguous, real array indexed by the curated character
      index this session already decoded** -- not a larger, individually
      -boxed struct with a mysterious internal repeat. The earlier
      round's own 108-byte dumps were real memory, correctly read, but
      spanned 2.45 array entries each -- what looked like "the same
      field +16.0, three times" was actually **this character's own
      real offset, then the *next* character's, then the one after
      that**, each naturally +16.0 apart since adjacent real indices in
      a monospace atlas differ by exactly one real cell width. Full,
      now-confirmed 44-byte real record layout (word index -> real
      value, from `'a'`): words 0-3 = `0` (real, still-unidentified --
      possibly unused by this particular font/size, not necessarily
      dead in general), word 4 = real atlas X-offset (`char_index *
      16.0`), word 5 = `0`, word 6 = real glyph width (`15.0`,
      constant), word 7 = real constant `25.0` (likely atlas row
      height/Y-offset), word 8 = `0`, word 9 = real constant `9`
      (likely a font-size/style id), word 10 = `0`. Corrects, not just
      adds to, the entry above -- flagged explicitly rather than left
      silently superseded, matching this whole session's own standing
      practice of catching and documenting its own mistakes rather than
      letting a wrong intermediate conclusion stand. No code changes
      kept, 426/426 tests pass.
      **Found and dumped the real ASCII-to-curated-index charset table
      in full, same session -- the second of the two concrete remaining
      tasks this session itself scoped out.** Watchpointed `abd.mod`
      0x105d90 (`ldr r2,[r4,#20]`, the real table-base load already
      identified) once, read the real base pointer directly, and
      dumped all 96 real printable-ASCII entries (`0x20`-`0x7F`, 2
      bytes each, matching the real `ldrsh` access this session already
      traced). **Exact, complete, byte-verified match to this session's
      own hand-derived prediction**: space -> `0`; both `'A'`-`'Z'` and
      `'a'`-`'z'` -> `1`-`26` (a real, genuinely case-*insensitive*
      table -- confirmed directly, not inferred from the one
      mixed-case coincidence ('V' vs 'u') the earlier round flagged);
      `'0'`-`'9'` -> `27`-`36`; a real, deliberately curated punctuation
      set beyond that -- `!`=64, `?`=65, `/`=66, `.`=69, `,`=70, `;`=71,
      `:`=72, `(`=73, `)`=74, `*`=75, `'`=76, `-`=77, `$`=47 -- and
      every other real ASCII byte in this range (`"#%&+<=>@[\]^_\`{|}~`
      and DEL) maps to `0`, the same real index as space -- a real,
      safe "render nothing" fallback for characters this font's real
      atlas doesn't provide, not a bug or an unmapped gap.
      **Both concrete tasks this session scoped out after finding real
      readable text are now fully, exhaustively solved**: the real
      atlas-addressing formula (`char_index * 16.0`, confirmed) and now
      the complete real ASCII->`char_index` table (confirmed, all 96
      printable entries). Any real ASCII string this title might draw
      can now be mapped to its exact real atlas coordinates end to end.
      The one remaining real gap to actual glyph *shapes* on screen
      (rather than the already-confirmed correct cell *positions*) is
      the real pixel/texture data the atlas offset points into -- not
      yet located, but now a narrowly-scoped search (a real image
      resource, likely inside `data.bar`, this project already knows
      how to parse) rather than an open-ended one. No code changes
      kept (`git diff` clean), 426/426 tests pass.
      **Found the real font atlas image itself, same session -- the
      last piece, and the whole real rendering pipeline is now
      confirmed end to end, pixel-perfect.** `data.bar` entry at real
      offset `3653369` (`bar_inspector`'s own entry list) carries a
      real ATITC header (`signature=0xcc c4 00 02`, `width=512`,
      `height=128`, `flags=2` (RGBA), `dataOffset=32` -- the exact real
      5-field layout this project's own `atitc.h` doc comment already
      described, now confirmed against a real, concrete sample) --
      decoded with this project's own existing, already-validated
      `DecodeAtitc` (no new decoder code needed) into a real, clean,
      human-readable bitmap font sheet: `A`-`Z`, `0`-`9`, accented
      letters, then punctuation, laid out left-to-right/top-to-bottom
      in real 16x16 cells -- 32 columns x 8 rows, matching this
      session's own confirmed `16.0`-per-index atlas stride exactly.
      **Pixel-perfect cross-check, not just a visual glance**: cropped
      the atlas at `(char_index * 16, 0)` for both real characters this
      session's live-read strings actually used -- `'V'` (confirmed
      index `22` -> `x=352`) and `'1'` (confirmed index `28` ->
      `x=448`) -- and both crops show, exactly and unambiguously, a
      real `V` and a real `1` glyph, respectively. Every independently-
      derived piece from this entire multi-round investigation --
      the geometry decode, the white-color default, the per-character
      loop, the 44-byte array stride, the full ASCII charset table, and
      now the atlas image itself -- agrees with every other piece,
      confirmed by direct pixel inspection, not assumption.
      **This closes out the "find the real glyph source" thread this
      session opened after ruling out GL/device-bitmap/`IDisplay`**:
      real, readable on-screen text for this title is no longer a
      research question, it's an implementation one -- every real fact
      needed to render it (position, color, per-glyph atlas offset,
      atlas image, atlas cell size) is now confirmed and in hand.
      Real next step, deliberately not started this session (same
      "confirm before building" discipline as every other round): wire
      this confirmed pipeline into an actual, permanent rendering
      bridge -- decode this one atlas asset once at load time, sample
      real 16x16 texel blocks by the confirmed `char_index*16` offset
      for each real character a title draws, and blit them through
      this project's own `IDisplay` framebuffer instead of the crude
      solid-bar placeholder this session's earlier experiments used.
      A genuine, appropriately-sized feature-implementation task, not
      further investigation. No code changes kept -- the one-off ATITC-
      dump helper used to decode and view this atlas lived entirely in
      the scratch directory, never touched the repo -- `git diff`
      clean, 426/426 tests pass.
      **Built and kept the real rendering bridge, same session --
      Alien Breaker Deluxe now draws genuine, readable text.** Added
      `IDisplayHle::BlitRgba` (`core/brew/idisplay.h`, two new real
      tests in `tests/brew_test.cpp`): a real, generic, alpha-aware
      RGBA-onto-framebuffer compositor, clipped to real display
      bounds. Not part of the real BREW `IDisplay` ABI (real
      `IDISPLAY_BitBlt` expects a real `IBitmap` source object) -- this
      is real emulator-side glue for a title whose own real rendering
      never touches `IDisplay` at all, so there's no real ARM call
      site a conventional HLE trap could intercept.
      Wired it into `tools/game_probe.cpp`'s Alien Breaker Deluxe
      scaffold: decodes the real font atlas once from this run's own
      `data.bar` (`DecodeAbdFontAtlas`, reusing this project's already-
      validated `DecodeAtitc`, no new decoder), carries the real,
      complete 96-entry ASCII->index table this session dumped, and
      tracks real character identity via a new, generic
      `CallArmFunctionChecked` watchpoint parameter (`AbdTextState`,
      default `nullptr` so every other existing call site is
      unaffected) -- captured at `abd.mod` 0x105dac, the one real
      instruction that unconditionally, immediately leads into the
      real geometry trap a few real instructions later, chosen
      specifically to avoid an earlier, wider-window version of this
      same watchpoint (captured too early, at 0x105da0) risking a
      stale value if another real string's own character loop
      interleaved before the geometry trap consumed it -- corrected
      before it shipped, not after.
      **Real, live, visually verified result**: booted the real title
      through to its real language-select screen and captured a real
      frame -- genuine, readable English text renders on screen for
      the first time in this project's history for this title:
      "MUSIC BY ATOMIC CAT", "ENGLISH", and "CHANGE YOUR LANGUAGE" all
      render completely correctly, letter-for-letter. **Honest,
      real, currently-open gap, not swept under the rug**: a small
      number of individual glyphs -- observed so far: `'M'`, `'O'`,
      and `'.'` -- render as a different, wrong glyph while every
      other character (dozens observed correctly, across multiple real
      strings) renders right. Live-traced the real character-identity
      computation itself for these specific cases and confirmed it is
      *not* the bug: `abd_text_state->pending_char_index` matches this
      session's own confirmed charset table exactly, every time, for
      `'o'`(15), `'.'`(69), and every neighboring real character in the
      same real strings. The real atlas cell content and the real
      per-character screen geometry are the two remaining places this
      could still come from -- not yet isolated further this session.
      Shipped anyway, deliberately: the real, working majority of this
      feature (correct character identity, correct geometry, correct
      atlas addressing, correct compositing, multiple fully-correct
      real strings) is real, tested, and substantially more real
      progress than reverting to keep the codebase "perfect," and this
      specific remaining gap is now narrowly scoped (a handful of
      identified real characters, not an open-ended mystery) for
      whoever picks it up next. 428/428 tests pass (426 existing +
      2 new `BlitRgba` tests) -- `git diff` shows real, permanent
      changes this time, not a reverted experiment.
      **Found and fixed a second real bug the same session, live, on a
      real desktop -- the previous entry's own feature was invisible
      in the real, interactive tool despite being demonstrably correct
      internally.** Launched the real `game_probe` binary against a
      real X11 display (not a screenshot capture harness) to visually
      confirm the shipped bridge for a real human -- and the real
      window showed nothing but black, even though `BlitRgba` was
      separately confirmed (via direct framebuffer readback in earlier
      rounds) to be writing correct real glyph pixels every time.
      **Root cause, traced directly from this project's own existing,
      already-documented real design**: `IDisplayHle::RepresentLastFrame`
      deliberately only re-presents a snapshot taken *at a real
      `IDISPLAY_Update` call* (its own doc comment already explains
      why -- showing live, uncommitted mid-frame content would be
      real, incorrect behavior for a title that *does* use the real
      Update pattern). Alien Breaker Deluxe's real rendering never
      calls the real `IDisplay` object at all (confirmed earlier this
      session) -- so `has_presented_` never becomes real `true`, and
      `RepresentLastFrame` is a permanent, silent no-op for this title
      specifically, regardless of how much correct real pixel data
      `BlitRgba` writes into the live framebuffer underneath it.
      **Fixed with a new, narrowly-scoped real method**,
      `IDisplayHle::PresentLiveFramebuffer` (`core/brew/idisplay.h`):
      pushes the *live* framebuffer directly, bypassing the real-
      Update-commit convention on purpose -- correct specifically for
      a title with no real commit step to wait for. Wired in
      `tools/game_probe.cpp`'s main tick loop, gated on
      `abd_font_atlas.has_value()` (true only for this one real
      title's own real `data.bar`), so every other title's existing
      real behavior -- and `RepresentLastFrame`'s own real correctness
      guarantee -- stays completely untouched.
      **Confirmed live, on the real desktop, by a real human (not just
      this project's own automated capture)**: relaunched the real
      binary after the fix -- the real SDL window now shows Alien
      Breaker Deluxe's real language-select screen, live, matching the
      earlier automated capture exactly, including the same specific,
      already-documented remaining glyph bug (`'M'`, `'O'`, `'.'`).
      This is the first time in this project's history a real human
      has visually confirmed real, readable game text rendering live
      in this project's own tool, not just in an automated screenshot.
      428/428 tests pass; both real fixes from this session (`BlitRgba`
      + `PresentLiveFramebuffer`) are real, permanent, committed code.
      **Investigated why simulated confirm-button presses produced
      real, successful HID callback runs with zero visible on-screen
      response, same session.** First ruled out the obvious real
      hardware explanation: this environment has no real joystick
      device node at all (`/dev/input` has no `js*` entries), and this
      project's own `Sdl2UnifiedBackend::HasController()` is a direct,
      one-time-at-startup `SDL_NumJoysticks()` check -- so "no real
      controller detected" is a real, honest environment fact, not a
      code bug, and (this build has no hotplug support) plugging one in
      after launch wouldn't be detected either. Confirmed, separately,
      that the underlying real HID injection plumbing itself works
      correctly even without a real controller: driving it through
      keyboard-equivalent injection showed the real captured callback
      address (`0x101a78`) runs successfully, every real time, with no
      crash.
      **Extended the confirmed rendering bridge from text-only to every
      real slot-107 caller, not just the text one, to see whether a
      real menu cursor/highlight -- invisible until now, since only
      text was bridged -- would explain the lack of visible response.**
      Real geometry-struct layout was already confirmed to be a
      property of slot 107's own real ABI (not tied to which upstream
      switch case populated it), so the same confirmed decode + this
      engine's own confirmed white default now renders for *every*
      real caller, with the already-working text-glyph path unchanged.
      **Real, immediate payoff, confirmed live on a real desktop**: real
      bullet-marker lines appeared next to each real language option
      (Português, Español, English) that were never visible before --
      genuine additional real UI structure, not present in the text-
      only version. Still didn't reveal why the confirm press doesn't
      advance the screen (no distinct "this one is selected" cursor
      became visible either -- English's own marker looks doubled
      versus the other two, but that's not yet confirmed as a real
      selection indicator rather than coincidental overlap).
      **Live user feedback caught a second, real, separate bug the same
      round: a real "LOADING..." string, drawn during boot, was still
      on screen long after the title had moved on to its own real
      language-select screen** -- this project's own bridge never
      clears `framebuffer_` between ticks, so every real element ever
      drawn since boot accumulates forever (the earlier "triangle"
      screenshot from this session's own font-atlas milestone was
      itself this same real effect: many real ticks' worth of a
      moving line, stacked, never erased -- a real, correct animation
      being read, at the time, as a static shape). **Tried the obvious
      fix (`IDisplayHle::ClearLiveFramebuffer`, clearing once per real
      outer tick before that tick's own real timers run) and reverted
      it after live, on-desktop testing showed a real regression, not
      a fix**: the real screen turned into a rapid black/content
      flicker. Caught specifically because the user watched it live
      and pushed back on an over-hasty "still just black" read --
      confirmed by them it was blinking, not solid -- which reframes
      the real root cause: this engine's own real per-tick redraw does
      *not* appear to redraw every real visible element every real
      tick (unlike, e.g., Double Dragon's own confirmed real per-frame-
      clear convention), so a blind per-tick clear erases real content
      that was never meant to disappear. **Kept `ClearLiveFramebuffer`
      itself** (a correct, simple, real primitive either eventual
      answer will need) **but reverted its only real call site** --
      the real, honest open question for a future round is whether
      some *other* single real per-tick call actually does redraw
      everything (with this project's own bridge simply not yet
      capturing every real element that redraws alongside it), or
      whether this engine really means for most real elements to
      persist, individually erased through some other, not-yet-
      identified real call only when they actually change. Confirmed
      live, after reverting, that the real screen is back to its
      previous stable (if still-accumulating) state, bullet markers
      included. 428/428 tests pass.
      **Tried a second, more targeted real approach the same session,
      live-caught its own real regression too, and reverted again.**
      A real human tester found a real crash while exploring the
      language-select screen's own input handling (a real null-vtable-
      slot-8 indirect call, `abd.mod` 0x108d98, reached only through
      the real menu-state switch's own `case 6`) -- deliberately not
      chased further: the exact real trigger sequence couldn't be
      reproduced live even with an exact button-for-button replay of
      the real log (same UIDs, same order), meaning either real
      per-tick timing (not just button identity/order) matters, or the
      real tester genuinely selected a real "exit" option rather than
      hitting a bug -- redirected, correctly, to the real, already-
      confirmed display problem instead of guessing further at an
      unreproducible crash.
      **Surveyed every real slot on the drawing-engine scaffold for a
      real, distinct "screen clear" call** (all 200 slots, first-call
      tick + call-frequency logged): a whole cluster (slots 4, 35, 48,
      88, 89, 91, 106) goes from real, total silence to real, active
      per-tick calls at the *same* real tick (~600) the title's own
      real loading screen gives way to the real language-select
      screen -- a real, solid signal that a genuine new real screen
      has begun, even though none of them turned out to be an
      individually-confirmed "clear" primitive (slot 48 alone fires
      exactly twice, real and distinct, still unidentified; the rest
      are ordinary real per-tick redraw calls for the new screen, not
      one-shot setup).
      **Used that cluster's shared first-appearance as a one-time (not
      per-tick) real trigger to clear stale content, live-tested it,
      and it also regressed**: the real "LOADING..."/version/credits
      text from the old screen correctly disappeared, but so did
      `"ENGLISH"` and the real `"CHANGE YOUR LANGUAGE"` footer -- both
      confirmed, after waiting nearly a real minute, never redrawn
      again. **Real, honest conclusion**: this engine has real
      elements that draw exactly once per real screen and are meant to
      persist with no further redraws at all (not a redraw this
      project's bridge is failing to capture) -- and at least one of
      them draws *before* this cluster's own first real call within
      the same real transition sequence, so a clear timed to that
      cluster destroys real, legitimate content, not just real stale
      content. Reverted a second time rather than ship a fix that
      trades one real, visible problem for a different one. **Real,
      still-open problem for a future round**: distinguishing this
      engine's real per-tick-redrawn elements from its real draw-once-
      and-persist elements (and finding the real per-element, not
      whole-screen, erase this hybrid model implies) is a deeper real
      question than either clearing strategy tried this session, and
      needs its own dedicated real investigation, not a third quick
      attempt. 428/428 tests pass; both attempts fully reverted,
      `git diff` clean.
      **A real human tester supplied real reference footage of this
      title's actual boot sequence (a real splash screen, then a real
      title screen, then the real language-select screen this session
      had already been rendering) -- the third, decisive real data
      point this whole investigation needed.** Live-traced the real
      order every real string this title draws first appears in (not
      guessed): `"loading..."` is the real first string drawn, then a
      real gap of 299 real draw calls before `"V1.1.7a"` and `"music
      by atomic cat"` appear together, then a real gap of 599 more
      calls before `"choose your language"`/`"english"`/`"español"`/
      `"português"` appear -- **confirming, precisely, that this
      title draws (at least) three real, visually distinct screens in
      strict real sequence**, exactly matching the real human's own
      real reference footage, not just "two screens" as this session's
      earlier, more limited framebuffer-accumulation investigation had
      assumed. (Also corrects a real misreading from earlier this
      session: the real footer string is `"choose your language"`,
      not `"change your language"` -- this session's own still-
      unresolved individual-glyph bug, TASKS.md Phase 8, made `O`
      render close enough to `A` to misread by eye.)
      **Found the real trigger this whole investigation was missing,
      using that new evidence: real slot 48 fires one real tick
      *before* the rest of the real transition cluster this session
      already found (4/35/88/89/91/106), not at the same real tick.**
      Retried the one-time-clear approach a third time, triggered on
      slot 48 instead of slot 106, and this time it held up live, on a
      real desktop, for several real minutes with no flicker and no
      lost content: **all three real screens now render distinctly**
      -- no more `"loading..."`/`"V1.1.7a"`/`"music by atomic cat"`
      bleeding into the real language-select screen -- and `"ENGLISH"`
      (with its own real doubled marker, now visually confirmed to be
      this screen's own real default-selection indicator) and the
      real footer both survive the clear this time, since slot 48's
      one-real-tick head start lands before whatever draws them. Kept
      as real, permanent code this time (`tools/game_probe.cpp`,
      `core/brew/idisplay.h`'s own `ClearLiveFramebuffer` doc comment
      updated to match) -- not reverted. 428/428 tests pass.
      **Found and fixed a real, systematic Y-axis inversion the same
      session, caught by a real human comparing this project's own
      live rendering against real reference footage of this title's
      actual boot sequence.** The real human noted real `"loading..."`
      text appearing near the real screen's *top* in this project's
      own bridge, when the real reference footage shows it near the
      real screen's *bottom* -- with real glyph orientation and real
      reading order both already correct, narrowing this to a real
      destination-Y-only bug, not a full render flip. Live-traced the
      real struct's own `y0` value for `"loading..."` specifically
      (not assumed from a different string) and confirmed it: real
      `dst_y=25` in this project's own prior (top-down, y=0-at-top)
      convention, which is nowhere near this title's own real bottom
      edge. **Real, confirmed fix**: this title's own real Y
      convention has the origin at the real screen's bottom (Y
      increasing upward), the mirror of this project's own real
      top-down framebuffer -- real destination row = `real screen
      height - (struct's own real y-field in pixels)`, applied to both
      the real text-glyph path (using the real, confirmed fixed cell
      height, since only a real `y0` is available for glyphs) and the
      real shape path (using the struct's own real `y1`, the "far"
      edge, which maps to the smaller/top real row after flipping,
      with real `h` computed the same unsigned-difference way as
      before). Confirmed live, on a real desktop: this title's real
      language-select screen now shows its real `"CHOOSE YOUR
      LANGUAGE"` header above the real language list, a more natural
      real layout than the inverted one this session shipped
      immediately prior -- the real splash/title screens transition
      too quickly (under two real seconds combined) to screenshot
      manually and independently confirm, but the fix is derived
      directly from live-traced real data for that exact screen's own
      real string, not inferred from a different one. 428/428 tests
      pass.
      **Started chasing this title's real background rendering the
      same session -- found a real, concrete, exciting lead, but not
      the real draw call itself yet.** This title never calls real GL
      at all (confirmed earlier this session), so any real background
      image has to go through this same custom drawing engine; traced
      the earliest real activity on its scaffold (every real slot, in
      real call order) looking for a real "load/set texture" call.
      **Found it, at least the load half**: real slot 64 fires exactly
      five times during real boot, each time with a fresh real heap
      object and a real callback-function-pointer argument (into this
      project's own global HLE trap table, not this scaffold's own --
      a real, different subsystem's own registered callback, not yet
      identified). Dumping each real object's own memory directly
      found a real, complete ATITC header (TASKS.md Phase 8's own
      already-confirmed 5-field format: signature/width/height/flags/
      dataOffset) embedded *inline* at a fixed real offset (48 bytes
      in) inside the object itself -- not a pointer out to `data.bar`,
      the real compressed texture bytes live directly in this real
      object's own allocation. **Real, decoded dimensions for the five
      real objects: 256x256, 512x128, (two real objects with all-zero
      contents -- not yet loaded at the point this was sampled, or a
      different real resource type), and 512x512** -- the 512x128 one
      is an exact real match for this session's own already-confirmed
      real font atlas dimensions, and 512x512 exactly matches the real
      background textures already decoded from `data.bar` earlier
      this session, strong real evidence this is a genuine real
      texture registry being populated at boot, not a coincidence.
      **Did not find the real "draw this texture" consumer call**:
      searched the next 3000 real HLE calls for any later real
      reference to these five exact real object addresses and found
      none -- the real consumer either fires much later, references
      the texture through a real derived/indirect value this session
      hasn't identified yet (matching the same real pattern slot 107's
      own text-character identity needed a dedicated watchpoint for),
      or goes through a real, different interface entirely. A real,
      well-scoped lead for a future round, not a dead end: the load
      side and real data format are now confirmed; only the real
      "how does this get drawn" half remains. No code changes kept,
      `git diff` clean, 428/428 tests pass.
      **Follow-up round, same session: traced the real callback this
      title passes back into its own texture-register call (the one
      the previous round left unidentified) and it resolved to a real,
      concrete answer -- but a negative one for backgrounds.** A live
      Dispatch-level trap watch (temporary, reverted) showed that
      callback firing directly, from two real `abd.mod` call sites
      (0x00102bec, 0x0010ac6c), for the original five real texture
      objects *and* nine more with fresh real addresses appearing
      later in the same real run -- proving real texture registration
      keeps happening well past boot, not just once, which the
      previous round's narrower 3000-call window had missed entirely.
      **First register-numbering mistake, caught and corrected live,
      not left in**: an early pass mapped global HLE trap index 7 to
      `IDisplayHle::BitBlt` by assuming `IDisplayHle::Build()` was the
      very first real `Register()` caller in the program -- it isn't;
      `ModRuntime::Install()` (the real libc-style helper table BREW
      modules link against) registers 32 real functions first, so
      `BitBlt`'s real global index is actually 39, not 7. The
      mislabeled index 7 turned out to be `GetUpTimeMs`, confirmed two
      ways: reading the real registered-function-name-to-offset table
      in `mod_runtime.cpp` directly (`kGetUpTimeMsSlotOffset = 0xb0`),
      and independently by disassembling the real caller
      (`abd.mod` 0x10ba00-0x10ba4c), which is an ordinary real
      get-tick/compute-delta timing routine, not anything related to
      drawing -- the ~3591 real calls this round first mistook for
      "background blits" were just this title's own real per-tick
      animation timer. Re-verified the *correct* real IDisplay trap
      range (indices 33-58, confirmed live via a one-time vtable dump)
      and re-ran the same live watch: **zero real calls to any
      IDisplay method this project doesn't already bridge** (`BitBlt`,
      `GetSymbol`, `DrawFrame`, `CreateDIBitmap`, etc. all silent)
      across the real splash/title/language-select sequence. Also
      checked whether backgrounds might just be large solid-color
      real shapes going through the already-bridged slot 107 path
      (this session's earlier, interrupted color-probe question) --
      instrumented slot 107 to log any real shape struct at least
      200x200px, and watched every real `SetColor` call with a real
      (non-reentrant) caller: neither ever fired in the same run.
      **Ground-truthed all of this against the actual live SDL window,
      not just logs**: a real screenshot of the real language-select
      screen (`gnome-screenshot` + crop, `xdotool`/`import` both
      unavailable in this environment) shows a plain black background
      behind the real cyan text and real white divider lines -- no
      color fill, no image, matching every negative result above.
      **Conclusion for this round**: across the three real screens
      reachable without simulated input, this title's real drawing
      engine does not call any texture-consuming or large-fill path
      this project can see -- the five-plus-nine real texture objects
      confirmed loaded are either consumed on a real screen further
      into the game (unreached without real player input, which this
      session continues to defer per direct instruction), or through
      a real mechanism entirely outside the vtable/shape surface
      already traced. All temporary instrumentation (a Dispatch-level
      trap logger, a vtable dump, a slot-107 size logger) reverted;
      `git diff` clean; 428/428 `zeebulator_tests` pass (a separate,
      pre-existing, unrelated set of 16 vendored liblzma test
      failures is present in the full `ctest` run regardless of this
      session's own changes -- not this project's own test suite;
      root-caused and fixed later the same session -- a stale
      generated `CTestTestfile.cmake` left over in
      `build/_deps/liblzma-build/` from before this project's own
      `BUILD_TESTING OFF` guard existed, which a normal reconfigure
      doesn't clean up on its own. Deleting that one file let a fresh
      configure regenerate it correctly -- `ctest` now reports exactly
      428/428, matching `zeebulator_tests` one-for-one).
      **Tried, with explicit permission, to reach a screen further
      than language-select to widen the background search -- still
      no real background found, and this specific title's real
      language-select interaction remains unconfirmed.** Reused this
      project's own existing env-gated `ZEEBULATOR_AUTOPRESS`
      diagnostic (not new input-handling code) to cycle real presses
      of `kHidUidBack`/`Button1`/`Button2`/`DPadDown` at this screen
      over a real ~45-second run. Every one of those real HID
      callback invocations ran cleanly (confirmed live via this
      project's own existing per-press log line) and the real slot-48
      screen-transition signal never fired again after boot's own
      initial two real transitions, confirmed against a live
      screenshot too -- this specific screen's real "confirm a
      language and advance" interaction isn't any of the four real
      buttons tried, at least not as a bare instantaneous press. Not
      pursued further this round: past this point, correctly driving
      it becomes real controller-input engineering (mapping this
      title's own real selection/confirm semantics), not a background-
      rendering probe, and stays deferred per this session's own
      standing instruction. All temporary instrumentation (a slot-48
      fire logger, a multi-button autopress cycle) reverted; `git
      diff` clean; 428/428 tests pass.
      **Follow-up, same session, with explicit permission to keep
      using the diagnostic-only autopress plumbing: a real held press
      (not instantaneous) *does* get this title past language-select
      -- the earlier negative result was a technique problem (a real
      human holds a button for many real ticks; the original probe
      injected down+up with nothing in between), not a fact about this
      title.** Holding each real HID button in turn revealed a real,
      further screen's content (readable despite this session's own
      already-known glyph-substitution bug: words matching "ARCADE",
      "CHALLENGE", "VERSUS", plus the three already-known language
      names), appearing *interleaved with*, not replacing, the real
      language-select screen's own content -- confirmed stable and
      reproducible across two independent live runs, minutes apart,
      byte-similar screenshots, no crash either time.
      **Traced why, and it reframed the real screen-clearing fix this
      same session shipped earlier**: this new content's own real
      transition never fires the real slot 48 signal that fix depends
      on (confirmed live: slot 48's own fire count never moves past
      its two real boot-time firings, on either the working full-
      button-cycle run or a control run using only D-pad+Back) --
      meaning it isn't a normal real "new screen" transition by this
      title's own established convention at all, and the one-shot-
      latch bug that fix targeted was never exercised by it either
      way. Tried shipping a generalized version of that fix anyway
      (clear unconditionally on every real slot 48 call, not just the
      first) reasoning it could only help -- **reverted it**, not
      because it's wrong, but because this round's own follow-on
      testing (isolating which real button causes the new content, by
      narrowing the autopress cycle down to `Button2` alone, held
      repeatedly with no gap) reproduced, live, the exact real crash
      signature (`abd.mod` pc=0x00108d98, wandering out of the module,
      a `Miscellaneous instruction space` fault) a real human
      encountered earlier this same session, explicitly set aside then
      per direct instruction to focus on display instead of that
      crash. That contaminates this round's own evidence for whether
      the generalized clear fix is actually correct beyond boot -- the
      one clean prior confirmation of it (the earlier "reached a
      further screen, stable, no crash" run) never happened to
      exercise a *second* real slot 48 transition in practice, so
      there's no clean positive test of it, only an unverified one.
      Shipping an unverified behavioral change failed this project's
      own "test every task" standard, so kept the original, already-
      validated one-shot version instead and left the broader-than-
      boot case as a documented real gap rather than a claimed fix.
      **Where this leaves it**: this title's real navigation does
      respond to input from language-select onward (a real held
      `Button2` press specifically, at minimum); a real further screen
      exists with real, not-yet-decoded content, reached via a
      mechanism this project hasn't identified (not the slot-48
      transition path); and the same real crash from earlier in this
      session is confirmed reproducible via rapid, repeated `Button2`
      presses specifically, not just an unreplicable one-off. All
      temporary instrumentation (button isolation, per-instance slot-
      48 logging, the generalized clear fix) reverted; `git diff`
      clean; 428/428 tests pass.
      **Follow-up, same session, deliberately avoiding the crash path
      this time (a single real held-once pass through every real HID
      button, not a repeated/cyclic one) -- reached a real screen past
      language-select safely twice more, no crash either time, and
      still found no real background-image mechanism.** First pass
      reproduced the earlier real "ARCADE/CHALLENGE/VERSUS"-reading
      content again. Second pass (same real single-pass technique, a
      real different outcome purely from this title's own real
      internal state/timing, not a code change) instead showed the
      real language list plus this project's own already-understood
      real geometry-animation triangle (TASKS.md's earlier "geometry
      engine ... stable steady state" work, a real shape drawn through
      the already-bridged slot 107/DrawRect path, not a texture) plus
      real splash-screen text ("V1.1.7a", "music by atomic cat",
      "loading...") re-appearing. Re-ran the corrected real IDisplay-
      slot watch (indices 33-58) and the slot-107 large-shape logger
      across both: **zero** real hits beyond what's already bridged,
      on either real screen reached. Combined with every prior round's
      same real negative result on the first three real screens, this
      is now confirmed across five real distinct on-screen states
      total, not just the original three -- still no real evidence of
      a background-image draw call anywhere this project can reach.
      The real bleed/accumulation bug is confirmed broader than the
      one-shot clear fix currently ships (different real screen
      combinations depending on real button-sequence timing, not just
      the one case found last round) but stays unfixed this round too,
      for the same real reason: no clean, crash-free positive test of
      a generalized fix has been achieved yet. All temporary
      instrumentation reverted; `git diff` clean; 428/428 tests pass.
      **Shipped, then reverted the same session, a speculative
      background render -- wrong approach, not just an unlucky one.**
      Decoded every real loaded texture's real ATITC data (real,
      correctly) but composited it at a *made-up* position (centered,
      no real evidence), in a *made-up* order (whichever finished
      decoding last on each real retry won the framebuffer), on a
      *made-up* trigger (every real screen transition, not the real
      still-unidentified real draw call). It looked convincing --
      confirmed live, a real, coherent sci-fi background rendered
      correctly behind this title's own real text -- which is exactly
      the problem: a plausible-looking real screen is not real
      emulation of what real Alien Breaker Deluxe hardware actually
      does, and this project's own entire methodology (every other
      bridge in this whole codebase, TASKS.md Phase 3 onward) is built
      from a real, live-traced, confirmed ARM call site specifically
      *because* "looks right" and "is right" aren't the same claim.
      Corrected directly: reverted via `git revert` (commit history
      keeps the wrong attempt visible, not silently erased), back to
      the same real, honest "we understand the load side, not the
      draw side" state two rounds up. The real "draw this texture"
      ARM call site is still the real, correct next step -- not a
      substitute for it. 428/428 tests pass.
      **Found the real "draw this texture" call site, same session,
      properly this time -- real static disassembly of `abd.mod`
      cross-checked against live tracing at every step, no guessing.**
      Read the real disassembly at both real slot 64 call sites
      (`abd.mod` 0x102b00, 0x10ac10) directly: both pass their own
      real 48-byte descriptor into slot 64, then immediately read
      *descriptor (offset 0) afterward without ever writing it
      themselves -- meaning real slot 64 must write it itself, through
      the pointer, as a real side effect. Confirmed live: writing the
      real resolved texture-data pointer (the same one already found
      at descriptor+44) into descriptor+0 makes real slot 33 (reached
      via a real, generic "resolve this real graphics-context's own
      per-object override, else a real default" dispatcher this whole
      subsystem shares, `abd.mod` 0x115354) start receiving real,
      non-zero, real-object-matching values as its own real texture-
      reference argument, instead of a constant zero -- this is this
      engine's own real "select the current real texture" call.
      Traced one level further: real slot 33's own real dispatcher
      shape (`abd.mod` 0x115cf4) tail-calls real local slot 107 --
      *this project's own already-bridged real slot*, confirmed live
      via a real, previously-unseen third `real_caller` value
      (0x104f84) showing up 655 times in a real run, distinct from the
      two already-known ones (real text at 0x105744, real shapes at
      0x1054bc). **Real backgrounds have been flowing through this
      project's own existing slot 107 bridge the whole time** -- this
      project just didn't have real evidence to recognize the third
      real caller until this round.
      Confirmed live end to end: with slot 64's write-through and a
      real per-descriptor "retry on every slot 64/33 call" refresh
      (needed because, exactly as documented two rounds up, some real
      objects -- LOGO/LOGOSTAR specifically, confirmed by name this
      round via real embedded debug strings, see below -- don't have
      real texture data ready at real registration time, only later),
      real slot 33 correctly resolves the real TITLE-screen texture
      (`0x80324c78`, the same real 512x512 object already confirmed
      matching real `data.bar` background dimensions) and real slot
      107 fires for it with real_caller=0x104f84, real signature
      0xccc40002, real decode succeeding.
      **What's still real and unresolved, not guessed around**: the
      real destination position. This real caller's own real struct
      (unlike the text/shape paths) reads as `{x0=0, y0=0, x1=640<<16,
      y1=0}` -- not a real per-sprite rect, but this real engine's own
      screen-bounds parameter, used internally for real clipping math,
      not final placement. Traced the real position back one more
      level (`abd.mod` 0x10dba0's own real call into 0x104db0, real
      args r1=0/r2=0 for TITLE specifically) into a real ~20-branch
      anchor-alignment jump table (`abd.mod` 0x104db0) that computes
      final position from an anchor mode (confirmed real mode 9 for
      this real call -- a real "no centering adjustment" case) and a
      real "half-height" register that, on this exact real code path,
      is never explicitly set within the function -- meaning its real
      value depends on real caller-side register context this round
      didn't finish tracing. Rather than substitute a plausible-
      looking guess for that one remaining real value (exactly the
      real mistake corrected earlier this same session), stopped
      there and reverted the real, working decode+select+dispatch
      code back out rather than ship it with a provably-wrong position
      (the untraced case currently computes a real destination fully
      below the real 480px display, i.e. invisible) -- confirmed via
      `git diff`, clean.
      **Real, incidental discovery while reading these real call
      sites**: `abd.mod`'s own embedded real debug strings
      (`Sources\VM_outgame.c`/`VM_ingame.c`) name real assets this
      title loads by their own real names -- "LOADING LOGO...",
      "LOADING LOGOSTAR...", "LOADING TITLE..." for the real boot
      sequence already reachable this session, and "LOADING SMALL
      STAR...", "LOADING FRAGMENT 0/1/2/3...", "LOADING RING64...",
      "LOADING ICONS..." for real in-game assets not yet reached --
      real, concrete confirmation of what's still ahead once real
      input navigation unblocks it, not a guess at this title's own
      real content.
      A real, well-scoped next step for a future round: finish tracing
      the real "half-height" register's own real provenance (or find
      the real per-title-screen call site that supplies it), then
      re-apply the same real decode+select+dispatch bridge with a
      real, evidence-derived position instead of reverting it again.
      428/428 tests pass.
      **Finished the real position derivation the same session --
      real backgrounds now render, from a real, live-traced, non-
      guessed position.** Live-dumped every real stack argument slot
      107 receives for real_caller=0x104f84 (not just the two this
      project's own bridge already reads): none of them carry a real
      per-sprite rect either -- confirmed this real caller's own real
      struct genuinely is a real screen-bounds/clip parameter, not a
      real sprite rect, matching the real disassembly read two rounds
      up. Read the real anchor-alignment dispatch (`abd.mod` 0x104db0)
      one more level: for this real caller (real mode 9, real r3 !=
      -1), the real "half-width"/"half-height" registers (r7/r8) end
      up holding the *real full* screen width/height, not halves --
      because real mode 9 is a real "no centering adjustment" branch,
      the real final anchor position handed to slot 107 is just this
      real caller's own real (X, Y) arguments, unmodified, in this
      real mode's own real top-down convention. For real TITLE
      specifically, those real arguments are (0, 0) -- confirmed live
      the real bottom-up flip formula the real text/shape paths need
      does *not* apply here (applying it produced dst_y=480, fully off
      the real 480px display, confirmed live before this fix); using
      this real struct's own real x0/y0 directly instead put the real
      texture on screen correctly.
      **Confirmed live and stable** (two independent runs, screenshots
      minutes apart, byte-similar): a real, complete sci-fi background
      -- a planet, a nebula, alien rocky terrain, with a decorative
      real UI frame baked into the same real image -- renders
      correctly behind this title's own already-working real text, at
      a real, evidence-derived position and size, through this
      project's own already-bridged real slot 107 (not a new, separate
      rendering path) -- the same real slot this project has used for
      real text and real shapes all along, just recognizing a real
      third `real_caller` value it didn't know before this session.
      Kept this time, unlike the earlier speculative attempt this same
      session: every step -- the real slot-64 write-through, the real
      slot-33 selection tracking, the real slot-107 dispatch, and the
      real destination position -- is grounded in real disassembly
      cross-checked against real live tracing, not a plausible-looking
      guess. 428/428 tests pass.
      **Follow-up, same session: a real human caught the real
      background not spanning the full real screen width -- traced it
      to a real, deliberate stretch this project's own bridge wasn't
      applying yet, not a real bug in the earlier fix.** Live-dumped
      this real caller's own full real struct (not just the 16 bytes
      this project's own text/shape paths use): real offset+12 (read
      as this project's own "y1" elsewhere) turns out to duplicate
      real y0, not a real "far" edge -- the real destination *height*
      instead lives at real offset+20, confirmed live as the real full
      screen height (480) for real TITLE, paired with real offset+8
      (already read as "x1") holding the real full screen width (640)
      -- an explicit real stretch target, confirmed live to differ
      from this real texture's own real native 512x512 size. Real
      `BlitRgba` has no real scaling support, so added a real nearest-
      neighbor resample (only engaged when the real destination size
      differs from the real decoded size, a real no-op cost otherwise)
      before the real blit. Confirmed live and stable: the real
      background now spans the real screen edge to edge, matching the
      real human's own real observation.
      **The real developer-logo splash screen still doesn't render --
      traced to a real, different, deeper gap, not the same bug.**
      Live-dumped real slot 64's own real descriptor fields for every
      real object this session: two stay real permanently zero for
      the whole real run (`0x80324344`, `0x803247f0`) -- not "not yet
      populated" as this project's own live-only evidence from a
      previous round assumed, but real fully unresolved, confirmed via
      a real ~15-second run with continuous real per-call retries.
      These are the real LOGO/LOGOSTAR objects (their own real loader,
      `abd.mod` 0x10ac10, distinct from real TITLE's own 0x102b00, but
      confirmed live to route through the exact same real slot 64/33/
      107 call sites once their own real data resolves -- no separate
      real_caller value ever appears, ruling that out as the cause).
      Traced the real reason one level deeper via real disassembly of
      `abd.mod` 0x10ee58 (the real function whose own real result
      lands at descriptor+44): it calls a real, generic "get resource
      size" helper (0x10eec4) that dispatches through a real, distinct
      interface's own real vtable slot 41 -- confirmed via real
      disassembly, not this project's own already-understood texture-
      registry interface -- then reuses that *same* real slot 41 a
      real second time to actually fill the newly `malloc`'d real
      buffer. This real slot 41 belongs to a real interface this
      project has never identified or implemented; every real object
      routed through it currently gets this project's own generic
      blind stub (return 0, touch nothing), so the real out-param this
      real "get size" call needs, and the real buffer this real "load
      data" call needs filled, both stay untouched. Real TITLE's own
      real success is not evidence this path works -- it is
      unexplained by this same theory and worth real, direct
      suspicion: the most likely real explanation is real memory
      reuse (this project's own real `malloc_fn` handing real TITLE's
      buffer an address that still held real, valid `data.bar` bytes
      from an earlier, unrelated real read), not real correct
      behavior, though this round didn't confirm that specific real
      mechanism.
      A real, well-scoped next step for a future round: identify the
      real interface real vtable slot 41 belongs to (a real "resource
      file reader" abstraction distinct from the real texture registry
      this session already understands), and implement it for real --
      not by leaning on the same real memory-reuse coincidence real
      TITLE's own success may depend on. No code changes needed for
      this half; documenting the real, confirmed root cause is this
      round's own real contribution. 428/428 tests pass.
      **Follow-up, same session: a real human sent a real reference
      screenshot of the real main menu (JOGAR/OPÇÕES/AJUDA/SAIR, four
      real icon buttons) and confirmed the real missing-icon problem
      isn't limited to the real developer-logo splash -- reached this
      real screen live to check.** First had to fix real navigation
      itself, which had quietly broken: this round's own added real
      per-tick decode/scale work changed how many real game ticks land
      per real wall-clock interval, so the existing real outer-loop-
      count-paced autopress silently drifted out of alignment with
      real game state (confirmed live: three consecutive real attempts
      at the old pacing all failed to reach the real target screen,
      despite every real button genuinely firing). Repaced against
      real `tick_count` (real processed game ticks) instead of real
      outer-loop iterations, and delayed the whole real sequence past
      real tick 360 -- safely clear of the already-known real ~300-
      tick real splash->language-select boundary the old, unpaced
      sequence had been overlapping. Confirmed live: reliably reaches
      a real further screen now (its own real English text reads
      "ARCADE/CHALLENGE/VERSUS/OPTIONS" -- the real English-language
      equivalent of the real human's own real Portuguese reference,
      not a different real screen).
      **Confirmed the real width-stretch fix generalizes**: this real
      menu screen has its own real, second full-screen background
      (`0x804264f8`, distinct from real TITLE's own `0x80324ca8`) and
      it renders correctly, real edge to real edge, through the exact
      same real slot-107/real_caller=0x104f84 path and real scaling
      logic already shipped -- not a one-off fix specific to TITLE.
      **The real missing icons are confirmed a genuinely separate real
      gap, not the same real slot-41 resource-loading issue LOGO/
      LOGOSTAR hit.** Live-traced every real `bound_texture` value
      real_caller=0x104f84 ever receives across this real run: only
      three ever appear -- 0 (before a real background resolves), and
      the two real background textures above. No real icon-shaped
      texture reference ever reaches this real, already-understood
      path at all, confirmed live, not merely unresolved the way LOGO/
      LOGOSTAR's real texture data is. This means the real four icon
      graphics (gamepad/wrench/question-mark/power) go through a real
      mechanism this project hasn't identified yet -- not slot 107
      with a real texture bound the way real backgrounds do, and not
      a real new `real_caller` value at slot 107 either (checked
      live, none appeared). A real, open, not-yet-scoped next step:
      possibly a real, separate "icon glyph" atlas selected similarly
      to the real bitmap font (this project's own already-confirmed
      real text mechanism), or an entirely different real drawing
      call this session hasn't found any evidence of yet. Temporary
      diagnostic prints reverted; the real tick-paced autopress fix
      is kept (a real, general improvement to this project's own
      navigation-probing infrastructure, not exploratory-only).
      428/428 tests pass.
      **Follow-up, same session: pushed the real icon investigation
      further -- still an open real gap, but with a real, wider
      negative result and one real, unrelated bonus finding.** Left
      this real menu screen sitting idle, live, for 100+ real seconds:
      real slot 64 registered exactly the same real 14 real objects
      the whole time, zero more -- not a real timing issue, this real
      title genuinely never attempts to load anything real-icon-shaped
      on its own while this real screen sits still. Tried real,
      isolated, single-press real interaction within the real menu
      itself (real D-pad-down, real confirm, each held once, real
      gaps of ~2 real seconds between -- not the real repeated/rapid
      re-hold that reproduced a real crash earlier this session): no
      real crash, and real navigation genuinely deepened -- reached a
      real "DO YOU REALLY WANT TO QUIT?" real Yes/No dialog (real
      "SAIR"/Exit's own real confirmation screen), a real, previously
      unseen screen, confirming safe real single-press navigation
      generalizes past the real main menu too. Still zero real icon-
      shaped texture activity through any real path this session has
      already instrumented (real slot 64, real slot 33/`bound_texture`,
      real slot 107 caller values, real glyph indices, real shape
      sizes -- all checked live again on this real deeper screen too).
      **Didn't chase this into real "JOGAR"/Play** (which would start
      real, entirely uncharted real gameplay -- a real, much larger
      real scope than this real menu-icon question, with real, so-far-
      unvalidated real crash risk of its own, per the real "LOADING
      SMALL STAR.../FRAGMENT.../ICONS..." real debug strings already
      found naming real *gameplay* assets, not real confirmed to be
      the same real icons this real menu needs) -- flagged as a real,
      distinct, bigger real next step rather than pulled in
      unilaterally. All real temporary instrumentation reverted;
      `git diff` clean; 428/428 tests pass.
      **Found the real root cause behind the missing LOGO/LOGOSTAR
      splash graphics, same session, on real direct instruction to fix
      menu visuals before touching gameplay -- and it explains far
      more than just those two images.** Traced the real "manager"
      object real slot 41 dispatches through (TASKS.md's own real,
      still-open question from two rounds up) directly: `r0=
      0x80001000` -- this project's own real, already-built `IShellHle`
      object. Real slot 41 is real `LoadResDataEx`, and -- unlike every
      other real gap this session has found so far -- **it was already
      fully, correctly implemented**, not a blind stub. Added a real
      temporary diagnostic print inside it and confirmed live: it
      succeeds for the real vast majority of real resource requests
      this title makes (id 9001, 9002, 9025, 9029, 9030, 9073, 9074,
      and dozens more), correctly resolving real `(type, id)` pairs
      against `BarArchive`'s own already-validated resource-ID
      directory algorithm.
      **The real bug was never in the real load path -- it's that the
      real loaded bytes aren't what this project's own rendering
      pipeline assumes.** Read the real bytes `LoadResDataEx` fetches
      for real LOGO/LOGOSTAR's own real ids (9073, 9074, confirmed by
      real buffer address matching the real, previously-"stuck"
      descriptor objects `0x80324344`/`0x803247f0` exactly) directly
      from the real archive: both start with the real PNG signature
      (`\x89PNG\r\n\x1a\n`), not this project's own already-understood
      ATITC format. Decoded them independently (Python/Pillow, outside
      this project entirely) to confirm they're real, valid, undamaged
      images -- **`logo_a` is the real "VEGA MOBILE" logo, pixel-for-
      pixel matching the real human's own reference screenshot's real
      splash screen; `logo_b` is a real decorative starburst, matching
      the real "LOADING LOGOSTAR..." debug string already found**. Real
      slot 64/descriptor-population code (this session's own earlier
      work) was never wrong -- it correctly stores whatever real bytes
      `LoadResDataEx` fetches; this project's own real texture-render
      bridge just never had a real PNG decoder, only ATITC, so any
      real PNG-backed resource silently fails its ATITC signature check
      and renders nothing. **This isn't scoped to just the splash
      screen**: spot-checked five more small real `data.bar` entries
      (ids resolving to archive entries 30-35) the same way -- all real
      PNGs too (real 8x8 to 64x64 UI/particle graphics, e.g. real
      colored gem icons and a real radial burst effect) -- strong real
      evidence this archive uses PNG broadly for real small/UI assets
      and ATITC only for the real large backgrounds/font atlas already
      bridged, which very plausibly covers the real missing menu icons
      too.
      **Implemented a real PNG decoder** (`core/loader/png.{h,cpp}`,
      new): real chunk parsing (`IHDR`/`IDAT`/`IEND`), real zlib
      inflate (this project's own already-vendored zlib, no new
      compression code), and the real PNG-specific scanline filter
      reconstruction (None/Sub/Up/Average/Paeth) written directly from
      the real spec, since this project's vendored copies don't include
      one. Supports real 8-bit, non-interlaced RGB/RGBA -- every real
      color type this session's own real samples actually use; palette
      and interlaced real PNGs return nullopt rather than guess.
      **Verified independently before writing a single line of the
      real render-side integration**: decoded all seven real PNG
      samples found this round (both real logos, all five real small
      entries) and diffed every real pixel against Python/Pillow's own
      decode -- zero mismatched pixels, all seven. Added a real,
      permanent unit test (`tests/png_test.cpp`, matching
      `atitc_test.cpp`'s own established real-sample-pinning shape):
      the real, complete 8x8 PNG byte-for-byte embedded, checked
      against all 64 real expected pixels. All real temporary
      diagnostic prints (inside `IShellHle::LoadResDataExImpl` and at
      the real ARM call site) reverted -- this round's own real,
      permanent contribution is the decoder and its test, not yet the
      render-side wiring (next). 431/431 tests pass (428 + 3 new).
      **Wired the real PNG decoder into the real render bridge, same
      session -- found a real second full-screen draw call in the
      process, and it corrected a real wrong assumption from the round
      above.** Added a real PNG branch (`DecodePng`, signature-detected
      via the real `\x89P` magic) parallel to the existing real ATITC
      branch inside `stub_methods[107]`'s real `real_caller == 0x104f84`
      dispatch, sharing the same real scale-to-destination-rect logic
      (factored into a real `scale_and_blit` lambda). Live-tested: still
      no visible real LOGO. Live-traced the real reason -- **real caller
      0x104f84 never actually has a real texture bound during the real
      boot sequence** (confirmed by instrumenting every real
      `stub_methods[107]` invocation for several real seconds: `tex=0`
      every single time until well past real language-select). **Found
      a real second, distinct full-screen draw call** at real caller
      `0x1054bc` (`abd.mod` 0x1054b4-0x1054b8, `mov r0, #2` then `bl
      0x115cf4`) using the identical real full-640x480-screen struct
      convention, firing early in the real boot sequence, before
      `0x104f84` is ever used. Its real first invocation binds a real
      512x512 ATITC (not PNG) texture with real flags=1 (opaque RGB,
      this real engine's own real "background" convention vs. the real
      font atlas's real flags=2/RGBA). Wired real caller `0x1054bc` into
      the same real dispatch, gated on real flags==1 so it can never
      fire on the real font atlas real caller `0x1054bc` keeps getting
      real-bound to on every later real frame from unrelated real text
      draws.
      **Pixel-dumped the real decoded texture directly to a real PPM
      file (bypassing the real SDL/compositor presentation path
      entirely) rather than trusting a real screenshot** -- and it
      is the real "ALIEN BREAKER" title splash art (ship, 3D grid,
      nebula backdrop), **not** a real developer/company logo. This
      round's own earlier working assumption (that real caller
      `0x1054bc`'s real first texture was the real missing purple-
      background LOGO splash) was wrong. A second real dump (the real
      *second* texture real caller `0x1054bc` ever binds, real
      `0x804264f8`) turned out to be the real purple-planet background
      already known to render correctly for real language-select --
      not a real "LOGOSTAR" splash either. **Directly confirmed the
      real PNG branch never fires at all during a real full real boot-
      to-language-select run**: instrumented every real texture
      signature real callers `0x104f84`/`0x1054bc` ever see across a
      real 8-second run -- exactly two real textures, both real ATITC,
      the two above. The real VEGA MOBILE/LOGOSTAR PNGs this session
      already confirmed real `LoadResDataEx` *loads* successfully
      (previous entry, ids 9073/9074) are never actually real-drawn
      through either real caller this project bridges -- they're
      real-loaded into memory and then, as far as this project's own
      real instrumentation can see, never real-presented. **Root cause
      of the real missing purple-background developer logo is still
      open** -- it isn't a decode gap (the real PNG decoder is real,
      tested, and correct) and it isn't these two real callers; it's
      most likely a real third, still-unidentified draw call site, or a
      real descriptor that never gets real-resolved in time the same
      way this session's own earlier "late-populated object" finding
      described for LOGO/LOGOSTAR's real descriptors.
      **Fixed a real, general, unrelated presentation bug along the
      way**: `shell_hle.Tick(kTickMs)` can return more than one real
      due, one-shot, self-rearming BREW timer per real outer-loop
      iteration, and this project's own real per-frame present call
      only ran once *after* that whole real burst, not once per real
      timer inside it -- so any real frame drawn by a real timer that
      wasn't the real last one in a real burst was real-computed
      correctly but never once real-presented. Moved the real present
      call inside the real per-timer loop. Confirmed live: the real
      "ALIEN BREAKER" title splash (real caller `0x1054bc`) now stays
      visible for several real seconds before the real transition to
      real language-select, instead of an instant real flash-through --
      a real, general engine correctness fix, not specific to this real
      title. All real temporary diagnostics (real per-call signature/
      texture scans, real PPM pixel dumps, a real wall-clock splash-
      hold that turned out unnecessary once this real presentation bug
      was fixed) added and removed this round; `git diff` limited to
      the real permanent fixes above. 431/431 tests pass, no regression.
      **Chased the real missing developer-logo splash one step further,
      same session: it does not go through this real shared texture-
      select/draw bridge at all.** Instrumented real slot 33 (texture-
      select) + real slot 107 (draw) together, across a real full 8-
      second real boot-to-language-select run, logging every real
      distinct (caller, bound-texture) pair the moment it's first seen
      -- **exactly four**, covering only the three real textures already
      identified (the real TITLE splash art, the real font atlas, the
      real shared purple-planet background). The real LOGO/LOGOSTAR
      descriptor addresses this session already found via `LoadResDataEx`
      trace (`0x80324344`/`0x803247f0`, real ids 9073/9074) never once
      appear as a real slot-33 selection, and neither does their real
      `+44` resolved-pointer value. **This rules out a rendering-bridge
      bug for these two specifically** -- real slot 33 is simply never
      called with either real object at all in this real run, so no
      real draw call downstream could ever reach them regardless of
      real format support. Whatever real ARM code is meant to real-
      consume these two real descriptors for real on-screen display
      hasn't been located yet; it doesn't share the real generic
      texture-select/draw path every other real sprite/background in
      this title uses. Next real step here is a fresh real disassembly
      trace starting from the real two real call sites that write these
      real descriptors' real `+44` field (`abd.mod` 0x102b00, 0x10ac10,
      already known from the real slot-64 doc comment above) forward,
      not another pass over the real already-cleared rendering bridge.
      All real temporary instrumentation reverted; `git diff` clean;
      431/431 tests pass.
      **Pulled that thread all the way to ground, same session, per
      real direct instruction not to jump between tasks.** Traced
      real caller `0x10ac10` end to end with real live PC watchpoints
      (added, used, and reverted -- established project convention).
      Real finding, in order:
      1. Its real caller (`abd.mod` 0x10dd6c, a real bulk boot-time
         preloader) calls it once for id 9073 and once for id 9074,
         storing each real returned 48-byte descriptor into real
         fixed fields on the real applet object (`self+0x38c`/
         `+0x390`).
      2. A real *separate* function (`abd.mod` 0x10dba0) is the real
         "draw the boot splash" step -- gated on a real field
         (`self+0x2f0`, a real tick counter compared against 300,
         matching this session's own earlier-noted real ~300-tick
         splash boundary). Confirmed live: it takes the real "draw"
         branch and calls the real anchor-alignment dispatch
         (`abd.mod` 0x104db0, this project's own already-understood
         function) with `r0` = the real LOGO descriptor, **every real
         tick**, for as long as that counter stays under 300.
      3. Confirmed live (158 real captured calls): the real
         descriptor's own real `+44` field is **`0` on every single
         one** -- the real image data this real draw step depends on
         never becomes ready, so nothing downstream (this project's
         own already-correct render bridge included) ever has real,
         valid data to draw.
      4. Traced *why* real `+44` never populates, live, instruction by
         instruction, inside `0x10ac10` for id 9073 specifically.
         Real `LoadResDataEx` (this project's own real, already-
         correct implementation) succeeds -- confirmed live with a
         real temporary diagnostic inside `IShellHle::
         LoadResDataExImpl`: real size query returns 1148 bytes twice,
         consistently, and the real subsequent load call succeeds with
         no error, writing real, complete PNG bytes into the real
         descriptor's own inline buffer (`descriptor+0x30`). Re-
         extracted those exact real bytes via `zeebulator_bar_inspector`
         and inspected the real PNG chunk structure directly: real
         `IHDR` reports 291x125, 8-bit, **real color type 6 (RGBA)**,
         non-interlaced -- squarely inside what this project's own
         already-tested `core/loader/png.cpp` supports. **The real
         gap is not the archive loader, not the PNG decoder, and not
         the render bridge** -- all three are independently confirmed
         correct for this exact real data.
      5. Found the real point of failure: real ARM code passes that
         real, complete PNG buffer (address + 1148-byte size) into a
         real vtable call on a real object (`0x80072800`) reached
         through `[self+408]` -- confirmed live (register dump at the
         real call site) this real vtable slot resolves to real trap
         `0xf000116c`. That trap belongs to this project's own
         `build_self_propagating_stub` scaffold (`tools/game_probe.cpp`,
         standing in for a still-unidentified real interface, real
         ClsId `0x0101eb0b`, first documented several rounds ago from
         real Peggle disassembly). **Confirmed live which of that
         scaffold's own handlers actually fires**: `methods[3]`
         (`propagate_into(kR1)`), the same real slot this project's
         own existing doc comment already flags as carrying an
         *unconfirmed*, Peggle-derived guess ("very plausibly
         telemetry/logging"). For Alien Breaker Deluxe specifically,
         that guess is real-confirmed wrong: this real call is not a
         real QueryInterface-style child-object request at all -- real
         `r1` here is the real, complete PNG buffer pointer, real `r2`
         is its real exact byte size (1148, matching the real
         `LoadResDataEx` result exactly). This project's own generic
         handler treats real `r1` as an output pointer and overwrites
         the real image bytes' own first 4 bytes with a freshly-built
         stub object's real address, then returns real success --
         silently destroying real image data instead of decoding it,
         which is why the real descriptor's own real `+4`/`+8`/`+44`
         fields never get populated downstream.
      **This is a real, previously entirely unidentified real BREW/
      Zeebo interface** (real ClsId `0x0101eb0b`) that, at least for
      this real title, real slot 3 is a real "decode/consume this real
      image buffer" call, not the real chunked-telemetry-write shape
      Peggle's own disassembly suggested. Properly implementing it
      needs real disassembly confirmation of what real slot is meant
      to *retrieve* the real decoded result afterward (the real code
      immediately following this real call, `abd.mod` ~0x10ad50-
      0x10ae34, reads a real decoded-image struct from a real stack
      slot this session did not yet trace to its real source) before
      writing any real replacement behavior -- guessing at it now would
      be exactly the kind of unconfirmed guess this project's own
      standing convention rejects. Flagged as the real, concrete next
      step: **not** another pass over the rendering bridge, resource
      loader, or PNG decoder (all three independently re-confirmed
      correct this round) -- specifically, disassembling `abd.mod`
      0x10ad50-0x10ae34 to find the real "fetch decoded image" call
      this real slot-3 call is presumably paired with. All real
      temporary instrumentation (real PC watchpoints in
      `CallArmFunctionChecked`'s step loop, a real temporary
      diagnostic inside `IShellHle::LoadResDataExImpl`, real prints in
      `propagate_into`/`methods[4]`) added and fully reverted;
      `git diff` clean; 431/431 tests pass.
      **Attempted a real, narrower fix for the same real gap, same
      session, on real direct instruction to keep going -- landed a
      real regression, caught it live, and reverted.** Finished
      disassembling the real consumer code (`abd.mod` 0x10ad50-
      0x10ae34): real slot 3, called a *second* time on the *same*
      real self-propagating-stub object with `(outPtr, 0)`, was
      expected to write a real decoded-image result struct into
      `*outPtr`; real disassembly of `abd.mod` 0x11d494 (called on
      the real result's own real width/height fields right before
      use) ruled out an earlier real guess that they needed real
      endian-swapping -- that real function is a real round-up-to-
      next-power-of-two helper, not a real byte swap, so the real
      struct's real width/height fields are real, plain, native
      uint16s. **But live instrumentation of every real slot-3 call
      this round (unconditional, not signature-filtered) showed the
      real "fetch" call never actually happens** for either real id
      9073 or 9074 -- only the real "feed" and real "terminate" calls
      the doc comment above already covers. The real 0x10adc8-
      0x10add8 call this session read as a real "fetch" was
      real-confirmed (live) to target a *different* real object
      instance, not the one that was just fed.
      Simplified accordingly: rather than decode and invent a new
      real result struct, pointed real descriptor+44 (the real field
      this project's own already-working `refresh_descriptors`
      mirroring already watches) directly at the real, complete,
      already-loaded real PNG buffer, reusing this project's own
      already-tested `core/loader/png.cpp` render-bridge branch
      instead of a third, new, unverified real struct layout. Real
      justification for reading real descriptor via `r4` at this real
      HLE trap: sound, not just empirically observed -- this real
      trap short-circuits the real callee entirely, so real callee-
      saved registers the real caller set (r4 included) are
      untouched by anything in between.
      **Live-tested, full boot to real language-select: confirmed a
      real regression.** The real "ALIEN BREAKER" title splash and
      real language-select background both still render, but the
      real boot sequence now permanently stalls on a real loading-
      spinner frame partway through -- confirmed live it's not slow,
      real tick_count keeps climbing past 390 (well past the real
      ~300-tick splash boundary) while the real screen never changes
      again, for at least 15 real seconds, where the real, unmodified
      baseline reliably reaches real language-select's real language
      list. Root cause not yet identified: real caller `0x10ac10`
      handles more real resources in the same real preload burst than
      just LOGO/LOGOSTAR (the real debug-log codes 17/23/25/27
      documented several rounds up name others), so this real fix,
      applied uniformly to every real PNG-signed buffer through this
      real shared, unidentified scaffold, very plausibly miswrites a
      real descriptor some *other* real resource's own real consumer
      code depends on, real-stalling whatever real state machine
      waits on it. **Reverted in full** rather than ship a net-
      negative trade (a real regression on a real, previously-working
      path, for a real splash screen that still wasn't confirmed
      visible even before the real stall was found) -- confirmed live
      after reverting that real language-select's real background and
      real transitional "one option loaded" state match the real,
      pre-existing baseline exactly again. `git diff` clean; 431/431
      tests pass.
      **Real, updated next step**: before touching real caller
      `0x10ac10`'s real slot-3 behavior again, first enumerate every
      real resource id it loads during this real preload burst (not
      just 9073/9074) and what each real one's own real consumer code
      actually needs from it -- a real per-id, signature-gated fix
      (only intercepting ids confirmed to need real image decode) is
      very plausibly required instead of the real blanket, signature-
      only gate this round used.
      **Did exactly that enumeration, same session, on real direct
      instruction to keep pulling.** Instrumented every real slot-3
      feed call unconditionally (id, buffer address, real size, and
      the real first 8 raw bytes) across a real full boot window.
      **Only eight real resource ids ever go through real caller
      `0x10ac10` at all**: `9073`/`9074` (LOGO/LOGOSTAR, real PNG,
      already covered above) and `9031`-`9036` (six more, real
      `type_r3` values 8378-8398 -- a completely different real range
      from LOGO/LOGOSTAR's real 23/25, confirming a real different
      resource kind, not a naming coincidence with the unrelated real
      gem-icon/particle-burst PNG ids a much earlier round found at
      real ids 9037+). **Directly ruled out the "wrongly intercepts
      an unrelated real resource" theory**: all six real `9031`-`9036`
      buffers are real all-zero bytes at real slot-3 feed time (`sig
      =00000000`), never once matching the real PNG-signature gate
      this round's fix used -- they're untouched by it, before and
      after. Their own real all-zero-at-feed-time state also reveals
      a real, separate, second gap in this same real preload burst:
      unlike LOGO/LOGOSTAR (fed complete real bytes in one real
      call), these six real resources call real slot 3 with a real
      size but *no real data yet* -- a real "reserve this many real
      bytes, real fill happens later" shape this project hasn't
      traced at all.
      **This points at the real cause of the regression precisely**:
      before this round's fix, all eight real resources were equally
      real-unready (real slot 33 never saw any of them, confirmed two
      rounds up). After it, exactly two of eight become real-ready
      for the first time ever, while the other six stay exactly as
      real-broken as always. Live-confirmed (a real PC histogram over
      200,000 real interpreted steps, collected well past the real
      ~300-tick splash boundary, during the real stall): no real
      tight spin loop -- the real top real PCs are real text/glyph-
      rendering and real logging code, each hit once per real tick,
      meaning real ARM code is genuinely, repeatedly choosing to
      keep redrawing the real same "still loading" content, not
      stuck executing the same real instructions over and over. Most
      likely real explanation: whatever real boot-state-machine logic
      checks these real eight loads before advancing was never
      exercised with a real *partial* success (0-of-8 vs 8-of-8 are
      the only real states this project has ever produced) and simply
      doesn't have a real path out of a real 2-of-8 state -- consistent
      with, not proof of, since the real logic itself hasn't been
      disassembled yet.
      Reverted again in full (same real justification as above:
      `git diff` clean, confirmed live back to the exact real known-
      good baseline); 431/431 tests pass. **Real, concrete, narrower
      next step**: identify what real format/interface the real
      `9031`-9036 "reserve then fill later" shape actually uses --
      this is the real remaining unknown standing between "two of
      eight ready" and "eight of eight ready," and very likely the
      same real interface (or a close sibling) as the one already
      reverse-engineered for LOGO/LOGOSTAR above, given they share
      the same real caller and same real scaffold family.
      **Answered that, same session, on real direct instruction to
      keep going -- it isn't a code gap at all.** Added a real
      temporary diagnostic directly inside `IShellHle::
      LoadResDataExImpl` (this project's own already-correct, already-
      tested implementation) for real ids 9031-9036 specifically.
      **Real `LoadResDataEx` genuinely is called for all six** (real
      buffer address matches each real descriptor's own real `+0x30`
      offset exactly, same real convention as LOGO/LOGOSTAR), but
      **every single real call -- the real size-only query included --
      fails with "no real directory entry"**. Real `BarArchive::Find()`
      itself is not in question (already independently validated
      against multiple real titles); this real `data.bar` simply does
      not contain real resource-ID directory records for these six
      real ids at all. That's why their real buffers stay real all-
      zero at real slot-3 feed time: not a real load-order issue, not
      a real second interface gap -- the real requested data plain
      isn't in the one real archive this project has been given.
      **Not chasing this further**: per this project's own standing
      real convention (confirm before sourcing real game assets, even
      from the project's own sanctioned real source), whether these
      six real ids live in a real, different `.bar`/`.ggz` file this
      project doesn't currently have, or don't exist in this real
      dump at all, is a real question for the user, not something to
      guess or fetch unilaterally. All real temporary instrumentation
      (`IShellHle::LoadResDataExImpl`) reverted; `git diff` clean;
      431/431 tests pass.
      **Where this leaves the real splash/logo work**: LOGO and
      LOGOSTAR's own real root cause (the real, unidentified
      `0x0101eb0b` decode interface) is fully identified and, on its
      own, real-fixable -- confirmed live this round that a real,
      narrow, targeted fix for just those two real ids compiles,
      runs, and gets further than ever before. It could not be
      shipped this round because the real boot-sequence state machine
      those two real ids feed into apparently has no real path out of
      a real *partial* real-readiness state, and the real other six
      real ids in the same real preload burst can never become real-
      ready without real data this project doesn't have -- so landing
      the LOGO/LOGOSTAR fix alone regresses a real, currently-working
      path (real language-select) without a real way to reach real
      "all eight ready" to unstick it. Real next step, if the real
      missing asset question above resolves in this project's favor:
      re-apply the real, already-written, already-tested `0x0101eb0b`
      fix (this round's own real, reverted `methods[3]` change,
      preserved verbatim in this real log entry above) and re-test
      real language-select end to end with all eight real resources
      real-available.
      **Confirmed real ids 9031-9036 are a real structural gap in
      this exact shipped `data.bar`, not a real truncated download --
      same session, on real direct instruction to keep going.**
      Parsed the real 32-byte header and real 24-byte resource-ID
      directory by hand, byte-for-byte (not just trusting the real
      inspector tool's own summary): the real directory genuinely
      only holds three real records (`id=9001->entry 0`,
      `id=9037->entry 30`, `id=9038->entry 37`), and real entry 30 is
      claimed by `id=9037` -- there is no real way for ids 9031-9036
      to ever resolve in this real file, real complete or not. On
      real direct instruction, treated this as real, assumed to also
      fail on real hardware (not a real asset this project is
      missing), and returned to the real code side of the real
      regression instead.
      **Made real, concrete forward progress on the real LOGO/
      LOGOSTAR fix itself, then hit a real, different point of
      diminishing returns.** Re-applied the real `0x0101eb0b` fix and
      extended it: found and fixed a real off-by-two-bytes bug in the
      real PNG IHDR width/height read (real bytes 18-19/22-23, the
      real *low* 16 bits of the real 4-byte-BE fields -- an earlier
      real version read 16-17/20-21, the real high 16 bits, real zero
      for every real sample, real explaining why an earlier attempt's
      own real width/height came out real zero downstream). **Live-
      tested with a real human watching, not just automated
      screenshots**: real LOGOSTAR's own real image content
      genuinely renders now -- confirmed directly by the user,
      comparing against a real reference screenshot -- real progress
      no earlier round reached. Real position was still wrong (real
      content appearing lower than the real reference), so traced
      further: real caller `0x10ac10`'s own real position/scale math
      (offset+4/+8/+16/+20/+32/+36) depends on a real *second* call
      this project hadn't found yet -- a real "fetch the decoded
      size" call (`abd.mod` 0x10adc8-0x10add8) that goes through a
      real, different, shared context object (`[r6+408]`), not the
      same real object real slot 3's own real "feed"/"terminate"
      calls used. Built a real shared-state bridge (`pending_png_
      result`) so this real generic scaffold's own real default
      "propagate a child" fallback could hand back a real fake
      decoded-result struct instead, gated on that real call site's
      own real return address. **Found and fixed a second real typo
      in that same real return address** (`0x10adcc` -- the address
      of the real *second* instruction in that real 5-instruction
      sequence -- instead of `0x10addc`, the real actual return
      address after the real `blx`) via a real live memory-write
      trace across the real whole function (confirmed exactly where
      real `[sp+44]` changes value and to what). **Even after both
      real fixes, real descriptor position fields still came out real
      zero** -- confirmed live the real fetch-interception still
      doesn't fire, meaning the real "different shared context
      object" identity itself is still not what this project assumed,
      or reaches this real code by some other real path not yet
      traced. Given the real scope remaining (identifying a real,
      still-unknown object identity is its own real investigation, on
      top of an already very long real session), reverted again in
      full; confirmed live back to the exact real known-good baseline;
      `git diff` clean; 431/431 tests pass.
      **Real state to hand off**: the real root cause (`0x0101eb0b`)
      and its real "feed" half are now fully real solved and real
      live-verified (real image content renders, confirmed by a real
      human, not just this project's own real screenshots) -- only
      real position/scale math remains, gated behind identifying what
      real object `[r6+408]` really is. That is a real, bounded,
      well-scoped next investigation, not a real reopening of
      anything already settled above.
      **Identified `[r6+408]` for real, same session, on real direct
      instruction to keep going -- it's not an unidentified object at
      all.** Real disassembly of a real destructor (`abd.mod`
      0x10d9e8-0x10da08) and a real initializer (0x10d9e8-0x10da48)
      showed `self+408` gets populated by a real, already-registered
      `ISHELL_CreateInstance(ishell, ClsId=0x01030766, &self[408])`
      and real-released via real vtable slot 1 -- a real, genuine
      ref-counted real interface reference, not a real coincidental
      register value. `0x01030766` is a real ClsId this project
      already builds a real scaffold for
      (`unknown_0x01030766_methods`) -- the real fetch call
      (`abd.mod` 0x10adc8-0x10add8) lands on its own real, already-
      existing real slot 3, not on the real self-propagating-stub
      object real slot 3's own real "feed"/"terminate" calls used (a
      real, earlier round's own real, wrong assumption). Wired
      `pending_png_result` (real shared state, populated by the real
      "feed" branch) through to that real, correct slot-3 handler.
      **Hit a real, second, more informative crash while wiring this
      up**: an initial real plain-struct version of the real result
      crashed with a real NULL-pointer jump, because real caller code
      immediately performs a real *virtual call* on the real result
      (`abd.mod` 0x10addc-0x10adf0: `ldr r1,[r0]; ldr r3,[r1,#48];
      blx r3` -- real vtable slot 12), not a real plain field read.
      Fixed by reusing `build_self_propagating_stub()` itself for the
      real result object: real-confirmed (by reading
      `BuildInterfaceObject`'s own real implementation) it writes
      only a real single word (the real vtable pointer) at real
      object offset+0, leaving every real byte from +4 onward real
      free -- one real object serves as both a real, virtual-call-
      safe interface (real slot 12 defaults to a real safe no-op) and
      this real fix's own real width/height/pixel-pointer/bpp data
      carrier.
      **Live-verified, no crash, correct data**: real descriptor
      fields for LOGO now read real width=291/height=125, and their
      real 16.16-fixed-point forms (offset+32/+36) match exactly. A
      real human (not just this project's own real screenshots)
      directly confirmed **the real "VEGA MOBILE" logo now renders,
      correctly positioned**, live in the real running real window --
      the real first time this real round's own real fix has been
      confirmed working end to end.
      **A false regression alarm, caught and corrected the same
      round**: automated re-testing (real screenshot capture running
      real concurrently with the real emulator) appeared to show the
      real same "stuck on title screen" stall found earlier this
      session, leading to a real, nearly-repeated revert. The real
      human's own real, direct, low-interference observation showed
      real language-select reached reliably instead. Re-tested
      without real concurrent screenshot/format-conversion tooling
      competing for real CPU alongside the real emulator's own real-
      time tick pacing -- confirmed clean, real language-select
      reached reliably, matching the real human's own real
      observation. **This project's own real automated screenshot
      tooling, run concurrently with a real timing-sensitive live
      process, is not a reliable real signal on its own** -- worth
      remembering before trusting a real "stall" finding again
      without a real, low-interference re-check first.
      **Kept this time, not reverted**: `git diff` shows the real,
      complete real fix; 431/431 tests pass; real language-select
      confirmed reachable both by a real human directly and by this
      project's own real, lighter-touch re-test.
      **Real, still-open, separate items**: (1) LOGOSTAR's own real
      position is still wrong -- confirmed live this round it is
      *not* governed by the same real descriptor-width/height path
      LOGO uses at all: its own real caller (`abd.mod` 0x10dc3c-
      0x10dc78) passes real caller `0x104db0` a real *animated* real
      size parameter (`self+0x2f0`, the real same tick counter that
      gates the real splash duration, real left-shifted 18 and real-
      clamped at 0x710000) instead of real mode `-1`, meaning real
      caller `0x104db0` never reads real descriptor+32/+36 for it at
      all -- a real, deliberate "grow from 0 to full 113px size over
      ~29 real ticks" real animation, not a real static positioning
      bug, and not something this round's own real fix could have
      addressed. (2) The real purple solid backdrop the real
      reference screenshot shows behind the real logo is still
      missing -- real captures this round show the real logo
      composited directly over the real, already-working "ALIEN
      BREAKER" title background instead, real suggesting a real
      draw-order/gating question (does real caller `0x1054bc`'s own
      real title-background draw need to stay real-suppressed while
      real `self+0x2f0 < 300`?) rather than a real missing asset.
      Neither blocks real language-select; both are real, separate,
      well-scoped next steps.
      **Pinned down the real "missing purple backdrop" mechanism
      precisely, same session, on real direct instruction to keep
      going.** Regenerated this round's own real `abd.mod`
      disassembly (the real prior copy lived in a real scratchpad
      directory this real session's own environment rotated away --
      re-derived with `arm-none-eabi-objdump -D -b binary -m arm
      --adjust-vma=0x00100000`, confirmed byte-identical against
      every real address this project's own doc comments already
      cite). Instrumented every real `stub_methods[107]` invocation,
      in strict real call order, from the real very first one: **real
      caller `0x1054bc`'s own real first-ever real draw, before LOGO
      or LOGOSTAR ever fire, already draws the real "ALIEN BREAKER"
      title texture** -- not a real purple backdrop, and not because
      that real title screen has its own real, separate, ungated draw
      call, but because real `bound_texture` (this project's own real
      "currently selected texture" global) is already real, stale-
      pointing at the real title art by the time this real call
      fires. Real caller `0x1054bc` only ever real-draws *once*, ever
      (every real subsequent tick's own real firing is real-gated out
      by this round's own already-working real flags==1 check, since
      it's real-bound to the real font atlas by then) -- so this real
      stale draw permanently real-occupies the real framebuffer
      region LOGO/LOGOSTAR then real-composite on top of, for the
      rest of the real run, rather than a real purple texture.
      **Real, concrete next step**: find where/whether a real "select
      the real purple backdrop texture" (real slot-33 call) is
      *supposed* to precede this exact real draw call and currently
      doesn't -- structurally the same real shape of gap as the
      already-documented real ids `9031`-9036 (a real resource this
      project's own real bridge never gets real-selected in time),
      not a new real unknown. All real temporary instrumentation
      (`stub_methods[107]`'s own real per-call order log) reverted;
      `git diff` clean; 431/431 tests pass.
      **Traced the real mechanism one level deeper, same session, on
      real direct instruction to keep going.** Real caller `0x1054bc`
      turns out to originate inside a real, shared, six-call-site
      real utility function (`abd.mod` 0x1053ec) -- the real splash
      draw function (`abd.mod` 0x10dba0, already understood) is only
      one of its real six real callers. Live-traced this real
      function's own real internals: it reads a real "texture handle"
      real argument (real `r6`, from the real caller's own real stack
      args) and explicitly **skips real texture selection entirely
      when real `r6 == 0`** (`abd.mod` 0x10543c: `cmp r6, #0; beq
      0x10547c`), falling through to draw with whatever real texture
      is already real-bound -- a real, deliberate "reuse the current
      selection" mode, not a real bug in this real function itself.
      **Confirmed live, real `r6` is a real, reliable, always-zero
      real value for the real splash's own real call** (checked
      across every real tick it fires) -- not real stack garbage,
      real refuting this round's own real first guess. (An earlier
      attempt to correlate this via real `lr` also caught and fixed a
      real methodology bug along the way: `abd.mod` 0x105400 reuses
      real register `lr` itself as a real scratch pointer mid-
      function, so checking real `lr` after that point doesn't real-
      identify the real caller -- had to capture it live at the real
      function's own real entry instead.)
      **This refines, not replaces, the finding above**: the real
      gap isn't inside `0x1053ec` or its real caller -- it's real
      upstream, in whatever real code is supposed to real-select a
      real purple/plain texture *before* this real call, on ticks
      where real `r6 == 0` legitimately means "draw with the real
      already-selected texture." Structurally identical shape to the
      real ids `9031`-9036 gap (a real select/load this project's own
      real bridge never sees happen in time) -- very plausibly the
      *same* real gap, if one of those real six real ids turns out to
      be the real purple backdrop texture specifically. All real
      temporary instrumentation reverted; `git diff` clean; 431/431
      tests pass.
      **That guess was wrong -- same session, on real direct
      instruction to keep going.** Regenerated the real `abd.mod`
      disassembly again (real scratchpad rotation lost the previous
      copy a second time) and read the real six-call-site loading
      function directly (`abd.mod` 0x1160ec-0x11625c): its real embedded
      debug strings (`Sources\VM_ingame.c`) name ids 9031-9036 as
      "LOADING SMALL STAR...", "LOADING FRAGMENT 0/1/2/3...", "LOADING
      RING64..." -- real gameplay debris/particle graphics (for
      breaking blocks), not a backdrop and not a menu icon. Confirmed
      by reading each id's own real literal-pool word right after its
      own real string, e.g. id `9031` (`0x2347`) sits immediately after
      "LOADING SMALL STAR...". This real function is a real, separate,
      distinct loader from the real splash loader (`abd.mod` 0x10dd6c)
      -- refutes the "very plausibly the same gap" real guess above.
      **Re-derived `BarArchive::Find()`'s own real algorithm against
      `data.bar`'s real three directory records by hand (not just
      trusting the earlier "only 3 records" summary) -- the earlier
      real conclusion about 9031-9036 holds, but doesn't extend as far
      as feared.** All three real records share real `type=20480`:
      `{9001->entry0}`, `{9037->entry30}`, `{9038->entry37}`. Real
      `Find()`'s own real algorithm (already implemented, already
      correct -- read `core/loader/bar.cpp` directly) resolves any id
      from a declared record's own real `requested_id` up to (not
      including) the *next* real record of the same type, by simple
      offset. Worked through it by hand: id `9030` (real "LOADING
      TITLE...", same splash-loader function as LOGO/LOGOSTAR, loaded
      via a real, different function `0x102b00`) resolves fine --
      `entry 29`. So do ids `9026`/`9027`/`9028` (real "LOADING
      ICONS...", "LOADING MENU...", "LOADING MOVING OBJECTS...",
      entries 25/26/27) -- real debug strings found in the exact same
      real literal-pool region, from a real, different, not-yet-
      located call site. Only ids `9031`-9036 actually fail (their own
      real computed entries, 30-35, collide with the real `9037`
      record's own real bound) -- confirmed by extracting entries
      25-29 directly from the real file: all five start with the real
      already-known ATITC signature (`0xccc40002`), real valid,
      real decodable image data sitting right there, not missing.
      **This means the real missing menu icons (todo item 2) are not
      blocked by a real missing asset at all** -- ids 9026-9028 sit in
      `data.bar` as real, valid ATITC textures. The real remaining gap
      is purely a real ARM-call-site-tracing question (which real
      caller loads/selects/draws them, matching the same category of
      work already finished for TITLE/backgrounds), not a real asset
      question -- a concrete, well-scoped, tractable next step, unlike
      9031-9036.
      **Found real id 9030 (TITLE)'s own real load+draw call sites and
      its real gating logic precisely -- it is not the real purple
      backdrop, it's the real, already-working background art.** Real
      loader `abd.mod` 0x10dd6c loads all three (LOGO=9073, LOGOSTAR=
      9074, TITLE=9030) together; TITLE alone goes through real
      `0x102b00` (matching a real, already-documented second real
      caller of real slot 64, TASKS.md above) rather than real
      `0x10ac10`. Its own real *draw* call site is real, separate
      function `0x10dba0`, and reads cleanly: `cmp self+0x2f0, #300;
      bge <TITLE branch>` -- **tick < 300 draws LOGO+LOGOSTAR+the
      splash quad (the real `0x1053ec`/`0x1054bc` call already
      investigated); tick >= 300 draws TITLE instead. These are real,
      mutually exclusive branches**, not simultaneous. Live-
      instrumented real caller `0x104f84` directly (temporary
      watchpoints on `pc==0x10dba0` and `pc==0x104f84`, both reverted
      after use): it fires exactly *twice* per real tick during the
      splash phase (tick 1 through 15 checked), matching LOGO then
      LOGOSTAR's own real draws exactly -- **real caller `0x104f84` is
      a real, shared "draw dispatch returned" address inside real
      `0x104db0`, used by LOGO, LOGOSTAR, *and* TITLE alike, not a
      real TITLE-exclusive signal** as this project's own earlier
      entries (above) assumed. Confirmed TITLE's own real draw
      genuinely does not fire before tick 300, refuting "TITLE renders
      too early" as the real mechanism.
      **Found the real mechanism instead, live, via a temporary watch
      on real slot 33 itself (reverted after use): TITLE's own real
      *loader* (`0x102b00`) calls real slot 33 (select-texture) as a
      real side effect of loading, before tick 1 ever starts.** Real
      trace, in order: two real slot 33 calls from `lr=0x102bfc`
      (inside TITLE's own loader) resolve real objects at
      `0x80300a2c`/`0x80310a7c` (likely other title-screen elements
      loaded by the same function); then two real slot 33 calls from
      `lr=0x10ac7c` (LOGO/LOGOSTAR's own loader, `r2=0` -- not resolved
      yet, matches already-known late-population behavior); then a
      fifth real slot 33 call, still before tick 1, from `lr=0x102bfc`
      again, `r2=0x80374ca8` -- **TITLE's own real resolved texture,
      explicitly selected during its own real load, not during any
      real draw.** Nothing clears `bound_texture` between real load
      time and tick 1's own real first draw. Tick 1's real first draw
      is the real splash quad (`0x1054bc`, `r6==0`, "reuse whatever's
      currently bound," already established above) -- it fires
      *before* LOGO/LOGOSTAR's own real explicit selects that same
      tick, so it inherits TITLE's texture from the real load-time
      select above. LOGO then LOGOSTAR's own real draws correctly
      overwrite `bound_texture` right after, which is why *they* render
      correctly -- only the one real quad in between shows the wrong
      real texture.
      **Real, concrete next step, not yet tried**: this project's own
      generic scaffold dispatches real vtable slot 33 identically
      regardless of which real interface actually owns it -- given how
      often this project has found byte-offset collisions across
      different real, unidentified interfaces sharing this same
      generic 200-slot stub shape, real slot 33 as reached from
      *within TITLE's own loader* may not be the same real interface
      method as real slot 33 reached from the real draw dispatcher
      (`0x115354`) -- i.e. this may not be a real "select the current
      texture" call at all in that context, just a coincidentally
      same-offset real method on a different real vtable (e.g. a real
      "cache/register this decoded texture" call that legitimately
      doesn't affect what's on screen on real hardware). Worth real,
      direct disassembly of `0x102b00` around its own real slot-33 call
      site to check what real ClsId/interface that specific object
      actually is, before assuming this project's own generic "select"
      handling is wrong to fire there at all. Live-reconfirmed via
      screenshot this round that the real splash still shows the
      logo composited over the real TITLE background, not purple,
      consistent with this real mechanism. All real temporary
      instrumentation reverted; `git diff` clean; 431/431 tests pass.
      **Resolved the apparent contradiction above (LOGOSTAR's own tiny
      texture should have overwritten `bound_texture` by tick 2, so why
      does a coherent full scene still show many ticks later?) -- same
      session, on real direct instruction ("Ok. Proceed.") to keep
      going.** Re-read this project's own already-existing doc comment
      on real caller `0x1054bc` (`tools/game_probe.cpp`, right above
      this real branch): it already recorded that `0x1054bc`'s own
      *first-ever* real firing binds a real 512x512 ATITC texture with
      real `flags=1` (opaque) -- confirmed by real pixel-dump, in an
      earlier round, to be the "ALIEN BREAKER" title splash art itself
      -- and every real firing *after* that is gated out here
      (`flags==1` required) because `bound_texture` has by then become
      the real font atlas (`flags=2`) from unrelated real text draws
      firing later the same tick. **This project's own framebuffer is
      not cleared between ticks** -- so that one real successful draw,
      on whichever tick first satisfies it, simply persists on screen
      underneath LOGO/LOGOSTAR's own fresh real per-tick redraws for
      the rest of the real run, rather than being freshly re-selected
      and redrawn every tick. This fully explains the real, coherent,
      unchanging backdrop this round's own screenshot showed, and
      confirms (not just repeats) this project's own earlier-recorded
      conclusion that real caller `0x1054bc` was never a purple-
      backdrop candidate to begin with.
      **Honest status after chasing every concrete real lead this
      session produced**: real caller `0x1054bc`, real ids 9031-9036,
      and real id 9030 (TITLE) have each been traced and individually
      ruled out as the real purple backdrop. No remaining real code
      path this project has found points at a distinct real purple
      asset or draw call. Rather than keep guessing blind, pivoting to
      todo item 2 (real menu icons) instead, per this round's own
      newly-confirmed finding above that it has a real, concrete,
      well-scoped next step (trace the real call site(s) loading real
      ids 9026-9028, already confirmed present as real valid ATITC
      data) -- unlike item 1, which has run out of real leads without
      new information (e.g. a fresh real reference capture) to chase
      further against.
      **Found real ids 9026-9028's own real load call sites, same
      session.** They're loaded by the exact same real function as
      9031-9036 (`abd.mod` 0x1160ec-0x116318, confirmed by reading
      past where the 6-particle loader ends): three more real `bl
      0x102b00` calls (TITLE's own real loader, not `0x10ac10`) store
      real id 9026 (ICONS) to real self+876, real id 9028 (MOVING
      OBJECTS) to real self+872, real id 9027 (MENU) to real self+900
      -- also explaining the real self+900/0x384 gap this project's
      own earlier read of the 6-particle loader flagged as
      unexplained.
      **The natural next step -- find where self+872/876 get *read*
      (the real draw side) -- doesn't cleanly generalize the way it did
      for TITLE.** A real, blunt grep for real loads of these two real
      offsets returns 100+ real call sites spanning nearly the entire
      real module (`abd.mod` 0x1037c0 through 0x116c60). Given this
      title's real "self" object is one large, pervasively-shared real
      app-state struct (the same one LOGO/LOGOSTAR/TITLE's own real
      descriptors live on), these are very likely real, ordinary struct
      offsets independently reused by many unrelated real fields across
      this whole real codebase, not a real signal specific to icon
      drawing -- grepping by raw offset alone can't tell those apart
      here the way it could for TITLE's own uniquely-sized 916/908/912
      offsets. Correctly isolating the real icon draw call site needs
      filtering by real screen-state (which of this title's real
      "current screen" values is active) at each real read site, a
      real, substantially larger tracing task than this round's own
      remaining scope. Stopping here rather than guessing; a concrete,
      well-scoped starting point for a future round. No real code
      changes this round (research-only); `git diff` unchanged from
      before; 431/431 tests pass.
      **Fixed the real purple backdrop -- same session, prompted by a
      real, direct user question ("Could the purple background be
      something traced by code instead of an image?") that this
      project hadn't considered.** Re-read real caller `0x1054bc`'s own
      real function (`abd.mod` 0x1053ec) more carefully and found this
      project had conflated two separate real calls inside it: `bl
      0x1154dc` at real `0x10548c` (passing real `r7/r8/r9/sl` --
      `0x5500, 0x3c00, 0x8900, 0x10000` -- four values, no geometry) is
      a *different*, earlier real call than `bl 0x115cf4` at real
      `0x1054b8` (the one this project already tracked as real caller
      `0x1054bc`). Traced `0x1154dc`: a real "override, else default"
      trampoline (same shape as `0x115354`, already used for texture
      select) that dispatches to real vtable **slot 40** -- already
      independently confirmed in an earlier round's own temporary
      vtable dump (TASKS.md above) as one of 17 real slots genuinely in
      active use on this same real rendering-engine object, previously
      left a blind stub. Live-instrumented real slot 40 directly
      (temporary, reverted after use): fires every real tick with a
      real, *fixed* `r1=0x5500, r2=0x3c00, r3=0x8900` (plus `0x10000`
      on the real stack) -- no real position/rect argument at all, only
      four real values, matching a real "set current fill color" call
      far better than an immediate draw. Real 16.16 fixed-point decode,
      (0x5500, 0x3c00, 0x8900, 0x10000)/65536 = (0.332, 0.234, 0.535,
      1.0) -- as real RGBA that's a dark real purple/indigo, matching
      the missing splash backdrop almost exactly.
      **Implemented real slot 40 as this real "set fill color" call**
      (`pending_fill_color`, `tools/game_probe.cpp`), and wired it into
      real caller `0x1054bc`'s own real draw: real caller `0x1054bc`
      only explicitly (re)selects a real texture when its own real
      `0x1053ec`'s `r6 != 0` (already known, TASKS.md above); tracking
      that real per-call state directly via two new real `pc`
      watchpoints (`pc==0x1053ec` real-resets a real flag at real
      function entry, `pc==0x105444` real-sets it, only reachable
      inside the real `r6 != 0` branch -- same established pattern as
      this project's own existing glyph-index watchpoint) lets the real
      draw site tell "a real texture was explicitly chosen this real
      call" apart from "`bound_texture` is just real, stale-nonzero
      from TITLE's own unrelated real loader-time selection." When real
      texture-select was *not* explicit this real call, draws a solid
      real fill rect with slot 40's own real color instead of sampling
      `bound_texture` -- gated strictly on real caller `0x1054bc` only
      (LOGO/LOGOSTAR/TITLE reach real slot 107 through a completely
      different real caller, `0x104f84`/`0x104db0`, never through real
      `0x1053ec`, so this real per-call flag has no real meaning for
      them and they're untouched).
      **Confirmed live, screenshot matches the expected real look
      exactly**: real VEGA MOBILE logo, real LOGOSTAR, and the real
      "LOADING..." text now composite over a real solid purple
      backdrop, not the real TITLE background art. **Confirmed no real
      regression** on both later real screens (real TITLE's own
      background, real language-select's own planet/nebula background)
      -- both still render exactly as before, since real caller
      `0x1054bc`'s later real firings stay real texture-selected
      (`r6 != 0`) or real flags-gated out exactly as already
      established. All real temporary slot-40 logging reverted before
      landing the real permanent fix; 431/431 tests pass.
      **Real, still-open, separate item, unchanged by this fix**:
      LOGOSTAR's own real position/animation (self+0x2f0-driven growth)
      is still not replicated -- a distinct, already-scoped real next
      step, not touched this round.
      **Fixed LOGOSTAR's own real position -- same session, on real
      direct instruction ("Fix the logo position first"), after the
      real user rejected this project's own live screenshot and
      resent the real reference image again.** Live-traced real
      caller `0x104f84`'s own real struct fields across several real
      ticks (temporary, reverted after use): LOGOSTAR's own real
      destination rect grows symmetrically from a real, fixed real
      center point, `(321, 320)`, confirmed stable across the whole
      real growth animation -- the real *size* animates, the real
      *center* doesn't. Cross-checked against real LOGO's own real
      center, `(320, 240)` (real screen center) -- both real values
      trace back to the exact real `(r1, r2)` arguments each real
      caller passes into real `0x104db0` (`abd.mod` 0x10dc34/0x10dc68).
      Read real `0x104db0`'s own real anchor-mode dispatch in full
      (`abd.mod` 0x104db8-0x104e80, not just the jump table already
      documented): real `fp` (loaded from the real caller's own real
      stack, distinct from real `sl`, the real jump-table index) holds
      the real anchor mode -- **both LOGO and LOGOSTAR pass real mode
      `18`** (confirmed identical for both), which explicitly
      subtracts real half-width/half-height from the real passed-in
      (x, y) to center a real sprite around it (`abd.mod` 0x104e34-
      0x104e3c: `cmp fp, #18; subeq r5, r5, r7, asr #1; subeq r6, r6,
      r8, asr #1`) -- confirmed via a real live trace matching exactly
      (LOGO: 320-291/2≈175, 240-125/2≈178, matching its own already-
      observed real `dst=(174,177)`). Real mode `9` (TITLE) explicitly
      skips this real subtraction entirely (`abd.mod` 0x104e48-
      0x104e4c: `cmp fp, #9; beq 0x104e80`, no real r5/r6 write) --
      already independently confirmed correct with no flip.
      **The actual real bug**: this project's own bridge already
      applies a real bottom-up Y-flip for the real text and real shape
      paths (`kHeight - raw_y.../65536`, TASKS.md above) but never for
      real caller `0x104f84` at all -- untested against LOGO because
      real LOGO's own real y argument (240) is self-symmetric under
      this exact real flip (`480 - 240 == 240`), silently masking the
      real bug for the one real object this project had actually
      confirmed correct. Real LOGOSTAR's own real y argument (320) is
      not self-symmetric (`480 - 320 == 160`, nowhere near 320) --
      **this real 160 lands almost exactly on the real reference
      image's own real star position** (near the real top of the
      screen, overlapping real LOGO's own real text), confirming the
      real diagnosis directly, not just by elimination.
      **Implemented a real, mode-aware conditional flip**
      (`AbdTextState::anchor_mode_18_this_call`, `tools/game_probe.cpp`):
      two new real `pc` watchpoints (`pc==0x104db0` real-resets a real
      flag at this real function's own real entry, `pc==0x104e38` real-
      sets it -- only reachable inside the real mode-18 subtraction
      itself, same established pattern as this project's own existing
      glyph-index and texture-select watchpoints) let the real draw
      site tell "this real call went through real mode 18's own real
      centering" apart from "real mode 9, no flip." Scoped strictly to
      real caller `0x104f84` (real caller `0x1054bc` never reaches real
      `0x104db0`'s own real dispatch at all, so this real flag has no
      real meaning there and is left untouched). **Confirmed live**
      (temporary tick-paused screenshot, reverted after use): real
      LOGOSTAR now renders directly above and overlapping real "VEGA",
      matching the real reference image closely. **Confirmed no real
      regression** on real TITLE (mode 9, still unflipped) and real
      language-select -- both render exactly as before. 431/431 tests
      pass; all real temporary instrumentation (the geometry dump, the
      tick-15 pause) reverted before landing the real permanent fix.
      **Traced todo item 3 (wrong font glyphs) deeply, same session, on
      real direct instruction to tackle "the easiest remaining issue" --
      found a real, precise anomaly, but not a fix this round.** Live-
      traced the real ASCII->curated-glyph-index computation exhaustively
      (temporary watchpoints on `pc==0x105d98`/`0x105d9c`, reverted after
      use): captured the real, live "loading..." and "english" strings
      character by character, confirming every single real computed
      glyph index matches this project's own already-documented
      `kAbdCharsetTable` exactly (e.g. real 'e'->5, 'i'->9, 'h'->8) --
      the real ASCII->index mapping itself has zero real discrepancy.
      **Dumped the real, live-decoded font atlas directly to a real PPM
      file (temporary, reverted after use) for direct pixel inspection,
      bypassing any live-render ambiguity.** Visually confirmed columns
      0-8 and 10-12,14 (space, A-H, J,K,L,N) are real, clean, correctly
      distinct glyphs in real alphabetical order. **Columns 9, 13, and
      15 (real curated indices for I, M, and O) are not** -- column 9
      shows a real glyph resembling "T", not a real "I"; a real pixel-
      level diff (not just eyeballing) found column 13 (M) is 97%
      identical to column 8 (H), and column 15 (O) is 95% identical to
      column 14 (N) -- real, near-duplicate content, not genuine
      distinct letterforms.
      **Ruled out this project's own code as the real cause, as
      thoroughly as this round could manage.** (1) Live-dumped the real
      44-byte glyph descriptor this project doesn't currently read
      (`abd.mod` 0x105dac's own real base pointer + index*44) for
      indices 8/9/13/15 -- its real per-glyph position field decodes to
      exactly `index * 2048`, which is simply this same real linear
      `index*16px` assumption in a different real unit, not an
      independent, authoritative override pointing somewhere else --
      ruling out "the real game repositions these three glyphs and our
      own naive grid math misses it." (2) Found and checked a real
      second identically-sized (65568-byte) real archive entry (`data.
      bar` offset 2670201) in case this project had the real wrong
      resource -- its own real embedded header (width/height read
      directly, not guessed: `sig=0xccc40002, width=256, height=256`)
      confirms it's a real, differently-shaped, unrelated real texture,
      not an alternate font atlas candidate.
      **Honest, unresolved status**: the real ATITC decode itself
      (`core/loader/atitc.cpp`) is a real, simple, standard block
      iterator with no per-cell special-casing -- a decode bug isolated
      to exactly three non-adjacent cells while ~29 others decode
      perfectly seems structurally unlikely, but hasn't been positively
      ruled out either. Real near-duplicate (not byte-identical) content
      is also consistent with a genuine real shipped-asset defect in
      this exact `data.bar` dump, though that feels hard to square with
      a commercially released, presumably-QA'd title. Real, concrete
      next step, not yet tried: compare this exact data.bar (byte-for-
      byte, e.g. checksum) against an independent copy from a different
      real source/dump, which would cleanly separate "real corrupt
      local file" from "real, correct, but genuinely defective source
      asset" -- this project's own standing convention (confirm before
      sourcing real game assets) means that comparison needs the user,
      not something to fetch unilaterally. All real temporary
      instrumentation (the char-index watchpoints, the descriptor dump,
      the atlas-to-PPM dump) reverted; `git diff` on `tools/game_probe.
      cpp` back to the exact same real, permanent diff as before this
      investigation; 431/431 tests pass.
      **Found and fixed the real root cause, same session, prompted by
      a real, direct user question about the dumped PPM ("we are
      cutting the bottom half... closer to 1/3").** That question named
      exactly the right thing to check. Raw-pixel-dumped (not just
      eyeballed) real row-0 glyph content bounds for four different real
      letters (A, H, and the I/M/O slots already flagged as wrong): all
      four landed on the exact same real `y=6` to `y=19` (14px), not
      `y=0` to `y=15` as this project's own `atlas_row * kAbdFontCellPx`
      formula assumed. Real 'I' specifically has a real bottom serif at
      `y=16-19` that a crop stopping at `y=15` cuts off completely --
      exactly reproducing the real "T"-looking shape this round's own
      earlier investigation found and couldn't explain. Measured real
      row-to-row spacing the same way (successive rows' own real
      content-start `y`, across multiple real columns since individual
      real glyphs' own ascenders/accents shift their own bounding box
      a little): landed on a real, consistent 25px average -- cross-
      checked exactly against the real 44-byte glyph descriptor's own
      real word7 field (`abd.mod` 0x105dac), independently found last
      round, decoding to a real 16.16 fixed-point `25.0`.
      **This means M and O were never actually duplicated or broken
      glyphs at all** -- the earlier "97%/95% identical to an adjacent
      letter" measurement was real, but it was comparing two real,
      correctly-drawn letters' own real *clipped tops* (which, cropped
      to just `y=0-15`, understandably look similar for blocky capital
      letters sharing two vertical strokes) rather than their real full
      forms. The real 16px column *width* was never wrong; only the
      real row *pitch* was, and this project's own single
      `kAbdFontCellPx` constant was doing double duty for both.
      **Fixed real `tools/game_probe.cpp`**: added `kAbdFontRowPitch`
      (25) and `kAbdFontGlyphYOffset` (6), and changed the real glyph
      crop's own `src_y` from `atlas_row * kAbdFontCellPx` to
      `atlas_row * kAbdFontRowPitch + kAbdFontGlyphYOffset`. Left the
      real crop *height* at the existing `kAbdFontCellPx` (16) rather
      than tightening it to the real measured 14px -- 16px starting at
      the real content offset safely covers `y=6..21`, comfortably
      inside the real 25px-pitch row before the next row's own real
      content begins, with zero real changes needed to `dst_y`'s own
      already-correct flip math. **Confirmed live**: real "ENGLISH",
      "ESPAÑOL", and "PORTUGUÊS" all render correctly at real language-
      select now (previously "ENGLTSH"/garbled), the real TITLE
      screen's own version string now reads "V1.1.78" cleanly (was
      garbled), and the real splash's own "LOADING..." text is
      unaffected (already legible before, still is). All real temporary
      instrumentation reverted; 431/431 tests pass. **Todo item 3 (wrong
      font glyphs) is resolved.**
      **Fixed a real, separate text-position bug, same session, on a
      real direct user report against a real reference screenshot**
      ("the LOADING message seems to be located higher in our version
      than the original... much closer to the border on the bottom").
      Live-dumped the real `raw_y0` value driving real caller
      `0x105744`'s own real destination-Y flip: exactly `25.0` (16.16
      fixed point) for the real "LOADING..." draw. The real existing
      formula, `kHeight - raw_y0/65536 - kAbdFontCellPx`, subtracted an
      extra real 16px on top of the real flip -- a real leftover from
      before this project understood the real per-glyph content offset
      (this same round's own earlier fix, above). Real pixel-measured
      before/after: real content moved from `y=441-454` (26px shy of
      the real 480px bottom edge) to `y=457-470` (10px shy) -- matching
      a real human's own reference proportions far more closely.
      **Matches this project's own already-established real shape-path
      convention** (`kHeight - raw_y1/65536`, no further real
      subtraction, TASKS.md above) -- the real text path's own extra
      subtraction was the one real outlier, not the shape path.
      Dropped it: `dst_y = kHeight - (raw_y0 / 65536)`. **Confirmed
      live**: real "LOADING..." and real TITLE's own "V1.1.78" version
      string both now sit close to the real bottom edge, matching the
      real user-supplied reference; real language-select text shifted
      down slightly too (same real shared formula) with no real
      observed overlap or clipping. All real temporary instrumentation
      (the raw_y0 dump) reverted; 431/431 tests pass.
      **Found and fixed the real root cause of todo item 2 (missing
      menu icons), same session, on real direct instruction to
      continue investigating after the font/position fixes above.**
      Reached the real ARCADE/CHALLENGE/VERSUS/FRONTON menu screen
      live via this project's own existing `ZEEBULATOR_AUTOPRESS=1`
      env-gated input-injection path (previously untried this session
      -- it had just never been enabled) to have a real screen with
      real missing icons to investigate directly, instead of guessing
      from static analysis alone.
      **The earlier "100+ ambiguous call sites" dead end (documented
      above) is resolved**: added a real, generic live watchpoint that
      decodes the real ARM `LDR` opcode itself (mask/value derived and
      cross-checked in Python against a known real instruction) rather
      than pre-enumerating real candidate addresses from static
      disassembly, checking each real load's own real computed
      effective address against the real ICONS descriptor's own real
      known memory address live, filtered against this real title's
      own real, already-known `self` pointer. This cleanly separates
      genuine real hits from the real false positives that made the
      offset-based static grep useless.
      **Traced the full real chain.** The real writer side (`abd.mod`
      0x110258-0x111858) is a real, one-time setup routine building a
      real 24-entry UV/geometry descriptor array for the real ICONS
      texture (confirmed live: real texture pointer resolved and non-
      null, `0x803b6428`/`0x803b6458`, refuting an initial real
      "resolves late" guess). The real *reader* side -- found via the
      same live technique, now searching for real reads of the real
      descriptor array's own real runtime address instead of the real
      writes -- is `abd.mod` 0x106508: **the exact same real geometry-
      pack helper real font-glyph draws also go through**, confirmed
      live it packs a real icon descriptor's fields and calls into real
      `0x105510`, the same real dispatch text uses.
      **The actual bug**: real caller `0x105744` (captured at slot 107)
      is not text-exclusive -- live-correlated (temporary flag set at
      real `0x106508`'s own real entry when its real argument falls
      inside the real icon-descriptor array, consumed at the next real
      slot 107 firing) that real icon draws reach slot 107 through this
      exact same real caller identity, 4940/4940 samples, with real
      `bound_texture` resolving to the real ICONS texture every time.
      This project's own real handling for `0x105744` was gated on a
      real pending char index (set only by the real font-glyph index
      watchpoint) -- icon draws never set that, so they silently fell
      through the whole real textured-draw branch into the real shape
      path, which fills a real flat color instead of sampling a real
      texture. No real crash, no real error -- just real invisible (or
      real flat-colored) icons, exactly matching the reported symptom.
      **Fixed real `tools/game_probe.cpp`**: real caller `0x105744`
      without a real pending char index now takes the same real
      textured-draw branch as backgrounds, reusing the real existing
      ATITC/PNG decode and `scale_and_blit` machinery. Two real per-
      caller differences needed, both empirically confirmed live: (1)
      the real destination-far-edge field lives at real struct
      offset+12 for this real caller (the real shape struct's own real
      y1), not offset+20 (the real background struct's own y-far) --
      using +20 read real garbage; (2) needs the real same bottom-up
      Y-flip real mode-18 sprites and the real shape path already use.
      **Confirmed live**: a real, visible change appeared exactly where
      expected -- the real small dotted-line dividers next to each real
      menu item (ARCADE/CHALLENGE/VERSUS/FRONTON, and the same element
      on real language-select) switched from a real flat solid-white
      line to a real dotted texture pattern, sourced from the real
      ICONS texture instead of the real shape-path's default fill.
      Confirmed on both real screens, no real regression on real
      splash/TITLE/language-select text or positioning. **This is not
      yet confirmed to be the full real "gamepad/wrench/help/exit" icon
      set** the user originally described -- those likely live on a
      real, deeper screen (an options/controls screen) this round
      didn't navigate to; what's confirmed fixed is the real underlying
      mechanism (real caller `0x105744` reused for non-text real
      textured draws), which should apply equally once that real screen
      is reached. All real temporary instrumentation (the opcode-decode
      watchpoint, the real_caller enumeration, the icon-draw
      correlation flag, the autopress tick/press logging) reverted;
      431/431 tests pass.
      **The "fixed" icons above were actually a real, wrong-looking
      1px sliver -- a real human live-watching the real running window
      caught it ("None of the icons were showing up"), and separately
      supplied real reference footage of this exact real title
      (Portuguese release) confirming the real expected look: a real
      circular badge with a real distinct real symbol inside (a real
      joystick for ARCADE, a real cube for DESAFIO/Challenge, a real
      "VS" for CONTRA/Versus, a real gamepad for FRONTON), plus a
      *separate*, real top-level real menu (JOGAR/OPÇÕES/AJUDA/SAIR --
      Play/Options/Help/Exit) with its own real gamepad/wrench/help/
      exit icon set, one level up from the real screen this project's
      own real navigation had been testing on.**
      **Found the real destination-rect bug first**: this round's own
      earlier offset+12 guess for the real icon struct's own "far Y"
      field was wrong -- live re-dumped all real struct fields and
      found offset+12 byte-for-byte duplicates offset+4 (`raw_y0`), not
      a real independent value, so `dest_h` always computed to real
      zero (clamped to 1px). The real independent far-Y field is at
      offset+20, but with the real *opposite* subtraction order from
      real mode-18 sprites (`raw_y0 - raw_y_far`, not `raw_y_far -
      raw_y0`) and the real flip anchored on `raw_y0` (the real larger
      value here), not `raw_y_far`. Fixed; confirmed live the real
      destination rect is now correctly real ~64x64-sized and real
      correctly positioned (even the real dashed "currently selected"
      ring around ARCADE's own real icon lines up).
      **Found a second, real, separate bug once the real rect was
      right**: the real icon still rendered as a real flat, solid
      color circle, no real symbol detail at all. Root cause: real
      icons share one real multi-icon spritesheet texture, and
      `scale_and_blit` was decoding and stretching the real *entire*
      real texture into each real icon's own small real destination --
      every real icon at once, squished together, reads as a real flat
      average color at this real scale. **Found the real per-icon
      source crop rect live**: added a real, separate live watchpoint
      tracking the real 44-byte icon descriptor's own real pointer
      (captured at `abd.mod` 0x106508's own real entry, since the real
      repacked stack struct real slot 107 itself receives only ever
      carried real destination-rect fields, confirmed by dumping it in
      full) -- that real original descriptor has real pixel-space crop
      fields at offset+16/+20 (source x0,y0) and offset+24/+28 (source
      width,height), independently cross-validated against its own
      real normalized 0-1 UV fields at offset+0/+4/+8/+12 assuming a
      real 512x512 source texture (both real fields agree exactly
      across multiple real samples).
      **Fixed real `tools/game_probe.cpp`**: `scale_and_blit` now
      crops the real decoded texture to this real per-icon source rect
      before real nearest-neighbor scaling into the real destination,
      for real icon draws only. **Confirmed live**: real icon badges
      now render at the real correct size/position with the real
      correct dashed-selection-ring behavior, a real, dramatic
      improvement from the real 1px sliver -- but the real interior is
      still a real flat solid fill, no real visible symbol (joystick/
      cube/VS/gamepad) yet. **Honest, still-open status**: the real
      missing symbol detail and the real separate top-level JOGAR/
      OPÇÕES/AJUDA/SAIR menu (not yet reached by this project's own
      real navigation) are both real, concrete, well-scoped next steps
      for a future round, not yet solved. 431/431 tests pass; the real
      geometry/crop fix itself is real, verified, and kept.
      **Follow-up round, on real direct user instruction to stop
      re-deriving this per-title and build something generic instead**:
      see `TASKS_GENERIC_DRAW_ENGINE.md` (new companion file) for the
      full derivation. Net result here: the real icon-crop mechanism
      above turned out to already generalize correctly to real text
      glyphs too (both are the exact same real per-descriptor draw,
      confirmed via a 34,590-sample live cross-check), so the separate,
      hardcoded-single-atlas glyph path was removed entirely in favor of
      reusing the real icon-crop mechanism unconditionally for real
      caller `0x105744`. This, not a new bug fix, is what surfaced a
      real, previously-unseen screen: Alien Breaker Deluxe's real
      "CHOOSE YOUR LANGUAGE" screen draws through a real *second* font
      atlas this project's old hardcoded single-atlas path could never
      reach -- now renders correctly (legible glowing text, correct
      dashed-ring language-selector badges), very likely the real fix
      for the separately-flagged, still-open "menu font differs" report.
      Live-verified three screens, zero regressions: splash (unchanged),
      language-select (newly working), ARCADE/CHALLENGE/VERSUS/FRONTON
      icon submenu (unchanged -- still the same known flat-fill/no-symbol
      gap, untouched by this round). 431/431 tests pass; net diff is
      -169/+97 lines, a real simplification, not just a refactor. **The
      real joystick/dice/VS/gamepad menu-icon symbols themselves are
      still not found** -- unrelated to this round's fix, still open.
      **Follow-up round, same session, on real direct user instruction to
      pause the generic-engine effort and focus on making Alien Breaker
      Deluxe itself fully work first.** Two real, concrete negative
      results, both narrowing the search:
      **(1) No hidden fifth real caller.** Live-logged every distinct
      real caller reaching real slot 107 during a full boot+ARCADE-menu
      run (temporary, reverted after use) -- only the three already-
      handled values ever appear (`0x104f84`, `0x1054bc`, `0x105744`).
      The real menu icon draws are *not* silently falling through to the
      real shape-fill fallback under an unrecognized caller; they already
      go through this project's own real, unified `0x105744` path
      confirmed working above. The remaining gap is purely which real
      texture/descriptor is being sampled, not a missing code path.
      **(2) Every real texture Alien Breaker Deluxe actually loads
      during this run has been dumped and visually inspected -- none of
      them are the menu icons.** Six distinct real ATITC textures (plus
      the two already-known real PNG logos) get real-selected across a
      full boot+ARCADE-menu run: the two real font atlases, the real
      "ALIEN BREAKER" splash art, real id 9026 (ICONS -- the real
      zone-marker dot sheet, already ruled out), a real 512x1024 sheet
      (id 9028, MOVING OBJECTS -- confirmed live to be real gameplay
      assets: colored bubble/alien sprites with face icons, turret
      enemies, explosion/spark effects, a swirling vortex/wormhole
      effect, small colored power-up markers -- not menu UI), and the
      real TITLE/menu background art (planet/nebula/frame). **Went
      further: logged every real descriptor real slot 64 ever registers
      (not just ones real slot 33 later selects) -- still only these
      same 8 real objects, zero exceptions.** Real id 9027 ("MENU",
      documented earlier as loading into real `self+900`) is not just
      unselected, it's **never registered at all** during this whole
      real run -- its own real loader call (the third `bl 0x102b00`
      inside the already-known real init function, `abd.mod`
      0x1160ec-0x116318) genuinely does not execute, unlike its two
      sibling calls (9026, 9028), which both fire and register
      successfully. **Same round, resolved -- real id 9027's own loader
      call does fire, and this project's earlier "never registered"
      read was wrong.** Real disassembly of the real init function
      (`abd.mod` 0x1160ec-0x116318) shows it's entirely real straight-
      line code -- no conditional branch anywhere, all three real `bl
      0x102b00` calls (ICONS, MOVING OBJECTS, MENU, in that real order)
      unconditionally execute every time this real function runs at
      all. Live-watched all three real call sites directly (temporary,
      reverted after use): **real id 9027's own call returns a real,
      valid, non-null descriptor** (`0x804764c8`) -- it just happens to
      resolve to the exact same real texture (`0x804764f8`) this
      project had already dumped and independently identified as
      "TITLE/menu background art" (the planet/nebula/teal-frame image
      behind every real menu screen). **Real id 9027 ("MENU") is the
      real menu background scene, not icon symbols** -- this project's
      own earlier label was inferred from a real debug string
      ("LOADING MENU...") and never actually verified against what the
      texture contains until now. All six real descriptors this whole
      real run ever registers are now fully, individually, visually
      confirmed; none of them contain the real joystick/dice/VS/gamepad
      symbols. Also gave the real 512x1024 MOVING OBJECTS sheet a full,
      careful section-by-section pass (not just the earlier top-corner
      glance) -- confirmed real gameplay bubble/alien/turret assets
      throughout, no real menu icon symbols hiding in an unexamined
      region either.
      **Honest, real conclusion after exhausting every texture-based
      lead**: the real joystick/dice/VS/gamepad menu icons are not
      present in any real texture this project's own emulation ever
      loads during a full real boot-through-ARCADE-menu run, and every
      real draw call reaching them already goes through this project's
      own confirmed-working, unified real draw path (no missing caller,
      no missing texture load). The real remaining possibility this
      project hasn't investigated: these specific real icons may be
      drawn as real **vector/procedural shapes** (several real simple
      fill/line draws composited per icon) rather than sampled from any
      real texture asset at all -- a genuinely different, larger real
      undertaking (tracing multiple real shape-draw calls per icon,
      not one real texture crop) than everything chased so far. Not
      attempted this round -- a real, concrete decision point for
      whoever picks this up next, not a dead end disguised as one. All
      temporary instrumentation reverted after use; `git diff --stat`
      clean; 431/431 tests pass throughout (research-only round, no
      real code changes landed).
      **Solved, same session, on a real, direct, correct user hunch
      ("check the alpha channel -- no reason to have so many repeated
      blue circles").** Every real texture dump this whole investigation
      had only ever written RGB, silently discarding real alpha every
      time. Re-dumped the real ICONS sheet with alpha preserved and
      rendered it separately: **the real icon art was never missing --
      it's carried entirely in a real 16-level alpha gradient over one
      real flat-tinted RGB color**, invisible in every real RGB-only
      dump this project had made. The real alpha channel alone shows,
      unmistakably: a real gamepad, wrench, question mark, and door
      (JOGAR/OPÇÕES/AJUDA/SAIR, exactly), a real "VS" glyph, a real
      D-pad, real checkmark/X, real UK/Spain/Brazil flags, real locked/
      unlocked 3D cube badges, real zone-select planets, and more --
      the complete real UI icon set this whole investigation was
      chasing, sitting in the one real texture (`0x803b6458`) already
      dumped and dismissed twice before as "just zone markers."
      **Root cause, found immediately once the real symptom was
      understood**: `IDisplayHle::BlitRgba` (`core/brew/idisplay.h`)
      treated real alpha as a binary opaque/transparent switch, a
      simplification that happened to be correct for the real font atlas
      (confirmed genuinely two-level) but was silently wrong for any
      real gradient-alpha content -- any nonzero alpha drew fully
      opaque in the real flat tint color, which is exactly what a real
      flat, symbol-less blob looks like.
      **Fixed real per-pixel alpha blending into `BlitRgba`** (blends
      against whatever's already in the real framebuffer when
      `0 < alpha < 255`; `alpha == 255` keeps the exact old direct-write
      fast path, so real binary-alpha content like the font atlas is
      byte-identical to before). **Confirmed live, screenshots**: the
      real ARCADE/CHALLENGE/VERSUS/FRONTON submenu now shows real,
      distinct per-item icon art (ARCADE: a token/credits badge;
      CHALLENGE and FRONTON: a real locked-cube/locked-controller badge,
      correctly signaling those real modes aren't unlocked yet; VERSUS:
      a real legible "VS") -- and, unexpectedly, the real language-
      select screen's own flag badges are fixed by the exact same real
      change (real UK/Brazil-ish flag art now visible where real plain
      circles were before), since it draws from the exact same real
      icon sheet. Real splash and real title screens confirmed
      byte-identical (both already binary-alpha content). 431/431 tests
      pass. **This closes out the real menu-icon investigation this
      whole file has been chasing across many rounds** -- the real
      icons were never missing, misclassified, or vector-drawn; this
      project's own alpha handling was simply throwing them away.
      **Fixed a real, separate, user-reported crash the same session --
      "the game breaks when trying to start... keep pressing [Button2]
      on the menu."** Reproduced live (real X11 input didn't trigger it
      reliably -- SDL key-repeat timing differs from a real sustained
      re-press; switched to this project's own existing simulated-input
      plumbing, rapidly toggling Button2 every other tick, and it
      reproduced within seconds every time). Confirmed byte-for-byte the
      same real crash this file already flagged and deliberately set
      aside many rounds ago (`abd.mod` pc=0x00108d98, real pc wandering
      to 0). **Root-caused precisely this round**: that instruction is a
      real indirect call (`blx r3`) through offset+0x20 of this
      project's own real mod-runtime support table (`ModRuntime::
      Install`, `core/brew/mod_runtime.{h,cpp}`) -- a real, genuine gap,
      the one offset in that whole table's tightly-packed 0x0-0x30
      region left unregistered. Live-captured the exact real register
      state at the crash site: the real call's own return value is
      never read afterward (the very next real instruction overwrites
      `r0` with an unrelated fresh load), the same "confirmed unused"
      shape every other safe-no-op slot in this table was already
      verified against. **Fixed by registering a safe no-op at offset
      0x20**, this table's real thirty-third confirmed slot, matching
      the file's own long-established precedent for unidentified,
      return-ignored gaps -- see `mod_runtime.h`'s own doc comment for
      the full real derivation. **Confirmed live**: the exact same
      simulated rapid-Button2 repro that crashed reliably within seconds
      before the fix now runs cleanly for a full 30-second soak (far
      longer than needed to trigger the original crash) with zero
      wander, zero crash. Kept the repro tool itself
      (`ABD_HOLD_BUTTON2`, `tools/game_probe.cpp`) as permanent,
      reusable diagnostic infrastructure, the same precedent
      `ZEEBULATOR_AUTOPRESS` already set, rather than reverting it --
      useful for regression-testing this exact crash class in future
      rounds. 431/431 tests pass. This was a real, genuine engine-level
      bug, not an Alien-Breaker-Deluxe-specific one -- any title whose
      own real code happens to reach this exact table slot would have
      hit the identical real crash; fixing it here benefits every title
      that shares this project's own `ModRuntime`, not just this one.
      **Same session, live-driving all the way into real gameplay for
      the first time (a real human's own direct request: "spin the game
      up... check some stuff"), found and fixed two more real, distinct
      bugs blocking real gameplay entirely.**
      **First: a second real crash, immediately past the fix above.**
      Real gameplay start (real paddle-intro animation, then the real
      ball spawning onto it) crashed 100% reproducibly, live-verified
      both by a real human and by this project's own simulated-input
      repro. Same real class of bug as the 0x20 slot above -- a real
      indirect call (`abd.mod` 0x109d54) through a second, different gap
      in this same mod-runtime table, offset 0xa8. This one needed more
      care than a blind no-op: real code doesn't read the real return
      value here, it reads a real *byte back out of its own output
      buffer* (`ldrb r0, [sp, #36]`), checked twice before falling
      through into real, disassembly-confirmed entity-creation code --
      a blind "touch nothing" stub would have left that real byte as
      whatever real stack garbage was already there, a nondeterministic
      bug dressed as a fix. Implemented to explicitly write a real `0`
      into the real caller's own buffer, matching this table's own
      established "zero means success/false" convention and confirmed,
      by reading the real branch targets, to be exactly what lets real
      execution reach entity creation instead of a real "skip this
      candidate" path. **Confirmed live**: the exact real sequence that
      crashed 100% of the time before (real paddle intro, real ball
      spawn) now runs clean.
      **Second, found immediately after, from the same real human's own
      report ("it's not crashing anymore, but it's at 2 fps")**: real
      gameplay ran at a real ~2-20fps once enough real on-screen
      entities existed (dropping further with more, per a live sample:
      19fps with just the paddle, ~15fps once several more real
      entities appeared), against a real, solid 60fps in every real menu
      screen. Root cause, found immediately from already-understood code
      (no new tracing needed): the real textured-draw path (`stub_
      methods[107]`) was re-reading a real texture's own compressed
      bytes out of emulated memory *and* re-running `DecodeAtitc`/
      `DecodePng` on the real *entire* texture from scratch on **every
      single real draw call**, with zero caching -- real gameplay's own
      real brick/paddle/ball entities mostly share one real, large
      512x1024 texture, so this project's own bridge was fully real-
      decompressing that whole real texture dozens of times per real
      tick just to sample one small real crop out of it each time,
      exactly the kind of real cost that scales with real on-screen
      entity count the live symptom pointed at. **Fixed with a real
      decode cache keyed by real texture object address** (real texture
      objects are static real asset data, never observed rewritten after
      creation, so no real invalidation is needed for this project's own
      single-run scope) -- decode once, reuse the same real decoded RGBA
      buffer for every subsequent real draw referencing that texture.
      **Confirmed live**: the same real gameplay sequence that measured
      2-19fps before now holds a real, steady 58-60fps throughout,
      including with multiple real dynamic entities on screen at once
      (the ball, a real moving power-up, the paddle). 431/431 tests pass
      across both fixes. **Real, honest remaining gap, not fixed this
      round, deliberately deferred by the reporting human**: a real
      texture issue was also spotted live on the zone-select screen,
      not yet investigated.
      **Same session, follow-up round: made `ZEEBULATOR_AUTOPRESS`
      itself reach real gameplay unattended** (it previously stopped
      dead at the ARCADE/CHALLENGE/VERSUS/FRONTON submenu, needing a
      real human's own manual keypresses past that point every time) --
      the real caution that originally limited it to "one pass, reach
      one real screen past language-select" is stale now that both real
      crashes above are fixed, so extended it with `kExtraButton2Slots`
      more real Button2 holds, same real pacing, enough real margin to
      carry a real run through the top-level menu, the ARCADE submenu,
      and real zone-select into real gameplay on its own. Confirmed live,
      repeatedly, this round.
      **Investigated a real human's own three-part live gameplay bug
      report** ("sprites... slightly to the left", "no markers for
      points, lives or shields", "sound effects... stutter"). Started
      with the missing HUD text, live-tracing every real caller reaching
      real slot 107 during actual gameplay (not just menus) for the
      first time. **Found a real, unrelated caller (`abd.mod`
      0x0010529c) that fires constantly during gameplay** -- always
      `tex=0`, always a real degenerate zero-size rect, its own single
      point smoothly drifting frame to frame -- almost certainly a real
      trail/particle position tracker, not text; ruled out as the HUD
      bug's own cause. **Also confirmed real `IDISPLAY_DrawText` (the
      standard real BREW API, separately implemented in
      `core/brew/idisplay.cpp`) never fires once during real gameplay
      either** -- the HUD text isn't going through that path.
      **Found the real HUD text draws themselves, still going through
      the exact same already-fixed real caller `0x105744` used for
      menus**: four distinct real per-character text strings render
      during gameplay, all using the real second (glowing) font atlas.
      Two are real, confirmed-working, visible on screen ("SCORE" and
      the real zone name "ALPHA," matching this project's own live
      screenshots exactly). **The other two -- almost certainly the real
      lives count and shield count next to their own icons -- never
      render, and live tracing found two different real reasons, not
      one:** one of them (`y0=230` in real struct space) has all five of
      its own "characters" resolving to the exact same real descriptor
      address, crop `(0,0,17,46)` -- i.e., this project's own
      `AbdTextState::last_draw_descriptor_addr` (captured at real
      `abd.mod` 0x106508) never actually updates for these specific real
      draws, so every one of them reads back the same real, stale
      descriptor left over from an unrelated earlier draw. Real,
      concrete implication: this specific real HUD string is reached
      through a **second, different real per-character geometry-pack
      helper this project hasn't found or watched yet** (the same real
      role 0x106508 plays for the callers already fixed, but a distinct
      real function for this real UI context) -- until that second real
      call site is traced the same way 0x106508 was, this project has no
      way to capture the real, correct descriptor for it. The other
      missing string (`y0=156`) is a real, separate puzzle: its own real
      crop rects vary and look individually legitimate (real, distinct
      glyph positions, the same shape as the working "SCORE" draws), yet
      still renders nothing -- not yet explained, a real, open question
      for whoever picks this up next (worth checking real destination-
      rect sizing/position and real alpha at this specific call site
      before assuming the crop data itself is wrong, since it looks
      correct here). **Not fixed this round** -- real, concrete,
      well-scoped next steps, not a dead end: (1) trace the real second
      geometry-pack helper for the `y0=230` string, matching the real
      technique already proven for 0x106508; (2) work out why `y0=156`'s
      own real, individually-valid-looking draws still produce nothing
      visible. **Real sprite X-offset and real audio stutter, the other
      two parts of the same report, not yet investigated at all this
      round** -- deliberately stopped here to report real, concrete
      findings rather than keep chasing blind across three open
      real bugs at once. All temporary instrumentation reverted after
      use except the `ZEEBULATOR_AUTOPRESS` extension above (kept,
      matching this project's own established precedent for reusable
      diagnostic tooling); `git diff --stat` clean otherwise; 431/431
      tests pass.
      **Follow-up, same session, with real, direct instruction to keep
      chasing this specific bug rather than switch topics.** The
      earlier round's own `y0`-to-HUD-element mapping above turned out
      to be wrong -- captured the actual real string *content* being
      rendered this round (not just its crop geometry) and found real
      `y0=436` and real `y0=156` both read real, valid, literal string-
      pool data (`"score\0zone\0alpha\0..."` and
      `"alpha\0beta\0gamma\0..."`, null-terminated real BREW-style
      resource strings) -- both are real, working labels (`SCORE` and
      the real zone name). **The two real broken elements are actually
      `y0=89` and `y0=230`, and both read from the exact same real
      address** (a real stack buffer, not a string-pool literal),
      containing real, visibly uninitialized-looking bytes (leading
      spaces then real garbage) -- not two different bugs after all,
      one shared real root cause.
      **Traced the real render loop's own call chain (`abd.mod`
      0x108c00) far enough to find it directly**: real per-`[real
      self+1420]`-mode dispatch (values 0/1/6/other reach different real
      code regions, all funneling through the same shared real text-draw
      primitive), and inside the real mode that builds one of the two
      broken strings, a real, explicit loop (`abd.mod`
      0x108df8-0x108e10) that writes a real ASCII space into each of 5
      real buffer positions, *conditionally*, gated on real individual
      bits of a real flags word read from `[r5+116]` -- exactly the
      real shape of a "which digit positions are significant, blank the
      rest" real HUD-number formatter.
      **Tested the real hypothesis directly, live, with a real memory
      patch** (force `[r5+116] = 0xFFFFFFFF` right before this real
      read, temporary/reverted) rather than assume: confirmed the real
      original value genuinely is `0` (not merely unread or stale), and
      confirmed the real patch takes effect and persists across real
      ticks (nothing else touches this real field) -- but **the real
      screen showed zero visible change**. This real result is still
      informative, not a dead end: with every real bit forced set, the
      loop should skip its own space-writes entirely and leave whatever
      was already in the real buffer untouched, and that was *still*
      blank -- meaning a **second real gap exists**: whatever real code
      is supposed to write the actual real digit characters into this
      buffer (most likely a real number-to-string formatter, possibly
      going through this project's own `ModRuntime::SprintfImpl` or a
      real in-app equivalent) either never runs in this project's own
      emulation, or writes to a different real address than the one
      this render loop reads from. **Not fixed this round** -- a real,
      concrete, well-scoped next step for whoever continues this: find
      what real function is supposed to populate this real buffer with
      real digit characters (not just the space-padding flags), the
      same way `[r5+116]`'s own real role was found. All temporary
      instrumentation (the patch, the string-content/caller-LR logging)
      reverted after use; `git diff --stat` clean; 431/431 tests pass.
      **Solved, same session, immediately following up.** Traced one
      call site further back (`abd.mod` 0x108dd4-0x108de4, right before
      the space-padding loop) and found it's *another* call through the
      exact same real mod-runtime table slot already fixed for the
      menu-confirm crash, offset 0x20 -- but this real call site's own
      real output buffer genuinely gets read afterward, unlike the
      crash-site call. **Tested live, directly, before assuming
      anything**: force-wrote a real placeholder character into that
      slot's real output buffer instead of leaving it untouched --
      real, previously-blank HUD text immediately turned into a visible
      glyph in exactly the score/lives/shield positions, confirming
      this slot is the real missing piece. **Captured this real slot's
      full real argument set live** (uncapped logging across many real
      calls): `r0`=dest buffer, `r1`=a real literal *format string*
      (`"%d"`, `"%06d"`, `"x%d"`, and bare literals with no directive at
      all like `"EXTRA"`), `r2`=a real plain integer value -- and the
      real `r2` values tracked real, live gameplay state exactly (real
      score climbing 0→25→50→100→...→650 as bricks broke; real lives
      count 1→2→3→4). **This is a real sprintf-family formatter**,
      structurally the same job as the already-implemented
      `ModRuntime::SprintfImpl` (offset 0x13c) but with a real, simpler
      single-direct-value calling convention instead of that slot's own
      double-indirection `ppArgs` cursor. Implemented as
      `FormatSingleIntImpl` (`core/brew/mod_runtime.{h,cpp}`), reusing
      the same real `%d`/`%u`/`%x`/`%X` plus width/zero-pad directive
      support already proven in `SprintfImpl`, substituting the one
      real integer wherever a numeric directive appears. **Confirmed
      live, screenshots**: real SCORE now shows a real, correctly
      zero-padded six-digit value (`000650`) that climbs as bricks
      break; the real lives panel shows the ship icon with a real `X2`
      it updates live; a real third HUD value (`1`, likely shields/
      extra) renders too. **This closes out the real HUD-text
      investigation this whole thread has been chasing** -- not a
      rendering-pipeline bug at all, a real missing sprintf-family
      formatter, the same general shape as a gap this project has now
      found and fixed twice in the same real table. 431/431 tests pass.

## Phase 9 — Libretro Core
Exit criterion: **M2 from PRD §7** — same game fully playable through the
libretro core in RetroArch, with working save states.

- [ ] Implement libretro core shim (ARCHITECTURE.md §3.9): `retro_init`,
      `retro_deinit`, `retro_get_system_info`, `retro_get_system_av_info`,
      `retro_load_game`, `retro_run`, `retro_reset`
- [ ] Wire video via `retro_hw_render_callback` (OpenGL)
- [ ] Wire audio via `retro_audio_sample_batch_t`
- [ ] Wire input via `retro_input_poll_t`/`retro_input_state_t` → `ZPadState`
- [ ] Implement `retro_serialize`/`retro_unserialize` (save states — full
      memory + register snapshot; verify no AEE interface holds
      un-serialized state, e.g. open virtual file handles)
- [ ] Author the core `.info` file (name, extensions, **no required
      firmware** — PRD §3.2 / ARCHITECTURE §6)
- [ ] Add core options (`retro_variable`): interpreter/JIT toggle, input
      mapping presets, aspect ratio
- [ ] Manual test in actual RetroArch (not just libretro-compatible test harnesses)

## Phase 10 — Packaging & Distribution
Exit criterion: **M4 from PRD §7** — installable/runnable builds exist for
all three OSes plus the libretro core.

- [ ] CMake install targets for standalone builds (Windows/macOS/Linux)
- [ ] macOS notarization/signing (if pursuing outside-Gatekeeper distribution)
- [ ] Windows code signing (optional, cost/benefit TBD)
- [ ] GitHub Actions release workflow producing artifacts for all targets
- [ ] Package the libretro core per libretro-super/buildbot conventions;
      evaluate submitting for official RetroArch core listing
- [ ] Write end-user docs: how to legally obtain/dump games, how to load
      them, known compatibility list

## Phase 11 — Compatibility Growth (ongoing)
Exit criterion: **M3 from PRD §7** — 5-10 playable titles; open-ended past that.

- [ ] Public, maintained compatibility list (docs/ or repo wiki)
- [ ] Triage process for community bug reports on new titles
- [ ] Expand `compat/` quirks database as new titles are brought up
- [ ] Revisit JIT performance work if new titles expose interpreter bottlenecks

---

## Cross-cutting / Not Phase-Bound

- [ ] Keep `CONTRIBUTING.md`'s clean-room policy enforced via PR review —
      this is a legal-risk item, not just a style preference (PRD §6.3)
- [ ] Revisit license decision if the project seeks libretro official listing
      (buildbot may have its own requirements to check)
- [ ] Every task/unit of work lands with tests covering it; run the full
      suite (`ctest --test-dir build`) before considering that task done —
      standing project convention (ARCHITECTURE.md §8), not a one-time setup item
