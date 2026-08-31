#pragma once

#include <cstdint>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Wires up the ARM RVCT "ROPI" (Read-Only Position Independent) static-
// base convention real compiled .mod code expects. See PHASE8_LOG.md
// for how this was found: real disassembly of Double Dragon's real
// ddragonz.mod (arm-none-eabi-objdump, not manual hex decoding) shows
// AEEStaticMod_New -- the standard AEEModGen.c helper every module's
// AEEMod_Load calls -- computing its own load address via PC-relative
// addressing, then reading a pointer from 4 bytes *before* that address,
// then calling a function pointer read from offset 0x68 within whatever
// that points to, passing one integer argument (a byte count) and
// expecting a pointer back. That matches AEEModGen.c's own
// `MALLOC(nSize + sizeof(IModuleVtbl))` call exactly -- IModuleVtbl is 4
// function pointers (16 bytes on this ABI), and the disassembly computes
// r0 = nSize + 16 immediately before that indirect call.
//
// A second slot, offset 0x6c (immediately after MALLOC), is FREE: real
// disassembly of AEEApplet_New's cleanup path (called when
// ISHELL_CreateInstance(AEECLSID_DISPLAY) fails, see PHASE8_LOG.md) reads
// a function pointer from there and calls it with one pointer argument,
// matching the reference source's `FREE(pme)` exactly.
//
// A third slot, offset 0xc0, is by far the most heavily used (138
// distinct real call sites in ddragonz.mod alone, found by scanning the
// whole binary for the "read static base, index table" instruction
// pattern -- see PHASE8_LOG.md). Every site follows an identical
// shape: call this slot with no meaningful argument, twice in a row
// (compiler doesn't common-subexpression-eliminate it, so it's genuinely
// called twice), read offset+12 from the returned pointer both times,
// dereference that once more to reach a vtable, and call vtable slot 2
// (CreateInstance) on it. That's exactly the shape of
// `ISHELL_CreateInstance(pIShell, ClsId, ppObj)` where `pIShell` is
// fetched from an ambient "current app context" rather than passed in
// explicitly -- the mechanism real AEEStdLib helper macros that don't
// take an IShell parameter rely on internally. `SetShellInstance()`
// supplies the real pointer this slot should expose.
//
// The same context struct has a second confirmed field, offset +20
// (sibling to the IShell pointer at +12): a real call site
// (`ddragonz.mod` offset 0x1b5e0) reads it and stores it directly into
// a field inside the applet's own struct, which real code elsewhere
// (offset 0x23a18) later dereferences and calls vtable slot 18 on with
// `pRect=NULL` -- matching real `AEEIDisplay.h`'s `SetClipRect(po,
// pRect)` at that exact slot index precisely. This field is the current
// app's `IDisplay` pointer. `SetDisplayInstance()` supplies it.
//
// The same context struct has a third confirmed-to-exist field, offset
// +0x2c -- found probing Peggle (`peggle.mod`, TASKS.md Phase 8): real
// code reads it and calls through it with ARM RVCT's "ROPI" relative-
// vtable convention (see scaffold_object.h's
// BuildGenericRelativeVtableStubObject doc comment), unlike every other
// confirmed real interface in this codebase. Unlike the Shell/Display
// fields, *what real interface this points at* isn't known yet -- only
// that real code expects something non-null and callable there.
// `SetThirdContextObject()` supplies a placeholder (a relative-vtable
// stub, safe to call but does nothing real) so calling through it
// resolves rather than wandering into unmapped memory; replace with a
// real implementation once the interface's identity is understood.
//
// The same context struct has a fourth confirmed-to-exist field, offset
// +0x24 -- found continuing the Peggle investigation (TASKS.md Phase 8)
// into why its per-tick timer callback never re-armed itself the real
// self-rearming-`SetTimer` way Double Dragon's does. Real disassembly
// of that callback (`peggle.mod` offset `0x32db0`) shows it reads
// `*(context[0x24] + 20)` and skips its entire timer-rearming path
// unconditionally if that's zero -- which it always is, since nothing
// in this codebase ever wrote a real object there. Unlike the Shell/
// Display/third-object fields, this one is accessed as a plain,
// directly-read-and-written data struct, not through a vtable (the
// same callback also reads and writes a 64-bit timestamp at
// `+24`/`+28` and another field at `+0x2a0`) -- so it isn't a BREW
// *interface* at all, more likely an app- or OS-provided "scheduler"/
// timing-state block. Its real identity and full real layout are not
// known. `SetFourthContextObject()` supplies a real, writable memory
// block (zeroed, with only the one confirmed-load-bearing field,
// `+20`, pre-set non-zero to unlock the gate) -- an educated, minimal
// enabling stub, not a confirmed-correct implementation of whatever
// this real struct actually is.
//
// The same context struct has a fifth confirmed-to-exist field, offset
// +0x28 -- found continuing the Peggle investigation past the fourth
// field's arena gate (TASKS.md Phase 8). Real disassembly (`peggle.mod`
// offset 0x132dfc, reached from tick 0's own callback with no null
// check beforehand) reads it unconditionally and passes it to a small
// real subroutine (offset 0x131fac) that calls through it using the
// exact same ARM RVCT "ROPI" relative-vtable convention as the third
// field (`ldr r1,[r4]; ldr r2,[r1]; add r2,r2,r1; bx r2`) -- so, like
// the third field, it's a real interface pointer, not a plain data
// struct, but its real identity is equally unknown.
// `SetFifthContextObject()` supplies the same kind of relative-vtable-
// safe placeholder as the third field, for the same reason (resolve
// the call rather than wander into unmapped memory).
//
// A fourth slot, offset 0xb0 (4 real call sites, far rarer than 0xc0),
// is GETUPTIMEMS: called with no argument, twice, around a chunk of
// per-tick work, with `second_result - first_result` used immediately
// after as an elapsed-time delta (`sub r1, r0, r7` in the real
// disassembly of Double Dragon's timer-tick callback, TASKS.md Phase
// 8) -- the standard real BREW pattern for measuring elapsed
// milliseconds around a piece of work. `Tick()` advances the millisecond
// counter this slot returns.
//
// A fifth slot, offset 0x4 (the very first slot after the table's own
// base, and one of the most frequently hit), is MEMSET: real
// disassembly of a real call site (`ddragonz.mod` offset 0x1f9b0) shows
// exactly `memset(r4+46, 0, 10)` -- r0=dest, r1=0 (the fill value),
// r2=10 (the byte count), matching ANSI `void *memset(void*, int,
// size_t)` precisely. Real ROPI-compiled code apparently routes even
// plain C library calls through this table, not just AEE/BREW-specific
// ones.
//
// A sixth slot, offset 0x14, is STRLEN: a real call site
// (`ddragonz.mod` offset 0x23b00) calls it with one argument (a string
// pointer) and immediately does `add r1, r0, #1` on the return value --
// the classic `strlen(s) + 1` idiom for computing a buffer size
// including the null terminator, matching ANSI `size_t strlen(const
// char *s)` exactly.
//
// A seventh slot, offset 0xe4, is a bounded string copy: the one real
// call site (`ddragonz.mod` offset 0x23b18, right after the STRLEN call
// above) passes `(src=r0, n=strlen(src)+1, dest=r2=a stack buffer,
// cap=r3=0x200)` -- i.e. copy up to `n` bytes from `src` to `dest`,
// never more than `cap`. Unlike MALLOC/FREE/GETUPTIMEMS/MEMSET/STRLEN,
// this one wasn't matched against a specific named reference function
// (the argument order doesn't cleanly match a standard `strlcpy(dest,
// src, len)`) -- but the copy semantics themselves are unambiguous from
// the calling convention alone, so it's implemented as exactly that:
// `n = min(strlen_plus_one, cap); memcpy(dest, src, n)`.
//
// An eighth slot, offset 0x0 (the table's very first word), is MEMCPY:
// found while tracing a real key-input event (evt 0x101, TASKS.md Phase
// 8) -- a real call site (`ddragonz.mod` offset 0x223b4) reads offset 0
// specifically (`ldr r3, [r0]`, no displacement) and calls it with
// `(dest, src, n=36)`, the classic `memcpy(dest, src, n)` shape,
// matching MEMSET's placement at the very next slot (0x4).
//
// A ninth slot, offset 0x8, is STRCPY: also found while tracing a real
// key-input event (a different jump-table branch than the one that
// found MEMCPY -- see PHASE8_LOG.md). A real call site
// (`ddragonz.mod` offset 0x1a3e0 onward, tail-called `ldr r2,[r0,#8]`
// off the same table) passes just `(dest=r5+45, src=r9)` -- two
// pointers, no length -- matching `char *strcpy(char *dest, const char
// *src)` exactly (copies through the null terminator, unbounded).
//
// A tenth slot, offset 0xe8 (immediately after the bounded-copy slot),
// is STRSTR: a real call site (`ddragonz.mod` offset 0x1d838 onward,
// part of the same graphics-init routine documented in TASKS.md Phase
// 8) calls `eglQueryString(dpy, EGL_EXTENSIONS)` then feeds the result
// straight into this slot along with a literal extension-name string
// (`"EGL_QUALCOMM_COLOR_BUFFER"`, read directly out of the real file's
// bytes at that literal's address) with no null check in between --
// exactly the standard `if (strstr(exts, "SOME_EXTENSION")) {...}`
// idiom, matching ANSI `char *strstr(const char *haystack, const char
// *needle)` precisely.
//
// An eleventh slot, offset 0x13c, is a sprintf-family formatter: a real
// call site (`ddragonz.mod` offset 0x23d0c, part of the per-tick loop
// -- see PHASE8_LOG.md) calls it with `(dest=a stack buffer,
// fmt=a real literal string, ppArgs=&(a stack slot holding a pointer to
// a second stack buffer))`; reading the format string directly out of
// the real file's bytes at that literal's address gives `"ERROR
// CODE:%d"`. The double indirection on the third argument (a pointer to
// a cursor, not a plain `va_list`) is unlike a standard `vsprintf` --
// implemented as `int Func(char *dest, const char *fmt, void **ppArgs)`
// where `*ppArgs` is advanced 4 bytes (this ABI's word size) per
// consumed argument. See ModRuntime::SprintfImpl for which format
// directives are supported.
//
// A twelfth slot, offset 0x9c, is a debug-logging function: found while
// probing a different real game (Peggle's `peggle.mod`, the same real
// static-base table layout as Double Dragon -- see TASKS.md Phase 8 for
// why a second title was brought in). Real call sites near the very
// start of `AEEMod_CreateInstance` invoke it twice in a row with two
// different argument shapes; reading the literal-pool addresses passed
// as arguments directly out of the real file's bytes shows why: the
// first call's destination buffer literally contains the ASCII string
// `"*dbgprint"` and its source argument is a real Windows build-machine
// path (`"e:\Peggle\..."`), while the second call's first argument is a
// real literal format string (`"CREATING APPLET: %x"`, `"FAILED TO
// CREATE APPLET %x"`, `"FAILED TO ALLOCATE MEMORY %x"` at other call
// sites) -- exactly BREW's real `DBGPRINTF` macro family (which expands
// to different fixed-arity helper calls depending on how many
// substitution arguments the format string needs, all funneling through
// this one static-base slot). No caller ever inspects this slot's
// return value -- confirmed by checking every real call site found (the
// instruction immediately after each call always overwrites r0 before
// it could be read) -- so it's implemented as a pure no-op.
//
// A thirteenth slot, offset 0x44, is a second MEMCPY-equivalent: found
// the same way as the twelfth (real Thumb-mode `AEEMod_CreateInstance`
// code on Peggle, once Thumb decoding existed to trace it). The one real
// call site unambiguously reachable through the confirmed static-base-
// table-fetch idiom (`peggle.mod` offset `0xc718`, immediately after
// re-fetching the table pointer via the same "PC-relative literal, add
// pc, deref -4" sequence every other slot is found through) calls it
// with exactly memcpy's calling convention -- `(dest=sp+28, src=sp+4,
// count=24)`, a plain 24-byte stack-struct copy -- and its return value
// is read into R0 immediately after, matching `void *memcpy(dest, src,
// n)`'s "returns dest" contract too. Most likely a real ARM EABI helper
// symbol (e.g. `__aeabi_memcpy`) a compiler can emit separately from a
// user-callable `memcpy` for compiler-generated struct copies, but
// behaviorally identical -- implemented by registering the same
// MemcpyImpl a second time rather than duplicating it.
//
// A fourteenth slot, offset 0x74, is REALLOC: found continuing the same
// Peggle trace, once the class's own third context field (see above)
// was given a safe stub to call through. Two independent real call
// sites (`peggle.mod` offsets `0x3b038` and `0x3b0dc`) -- two separate
// growable-array template instantiations, one with 56-byte elements,
// one with 4-byte ones -- both call it with `(old_ptr=the array's
// current buffer, read from the array struct, new_size=new_element_
// count * element_size)` and check the result for non-null before
// overwriting their own buffer pointer, exactly `void *realloc(void
// *ptr, size_t size)`'s real contract. See ReallocImpl for how it's
// implemented against this allocator's no-free-list bump allocator.
//
// A fifteenth slot, offset 0x40, is unidentified: found probing a
// third real game (Super BurgerTime -- TASKS.md Phase 8), the first
// static-base call reached once a real stack/module address collision
// in tools/game_probe.cpp's own scratch-stack placement (unrelated to
// this table) was fixed. The one real call site found so far
// (`supbtime.mod`, reached from `HandleEvent(EVT_APP_START)`) calls it
// with `(this=the current real applet pointer, buffer=a 512-byte local
// stack buffer, size=0x200)` -- a shape consistent with some kind of
// "fill this buffer, up to size bytes" real BREW API, but not yet
// matched to any specific confirmed one (the established string/memory
// helpers at other slots all take fixed, non-"this"-shaped argument
// lists). Registered as a safe no-op (matching the twelfth slot's own
// precedent) rather than guessed at further -- unblocks real execution
// past this specific call site without claiming to know what it does.
//
// A sixteenth slot, offset 0xc, is also unidentified: found immediately
// after the fifteenth (Super BurgerTime's `HandleEvent`, once the
// fifteenth slot's own call site no longer wandered). Reached through a
// small standalone trampoline (`supbtime.mod` offset `0x11b200`-
// `0x11b214`: fetch the static-base table via the same real relocated-
// literal idiom, then `ldr pc,[table,#0xc]`) rather than the more common
// direct call shape, but functionally identical. Sits in the same
// tightly-packed cluster as the confirmed MEMCPY(0x0)/MEMSET(0x4)/
// STRCPY(0x8)/STRLEN(0x14) slots, and its one real call site passes
// `(dest=the same 512-byte stack buffer the fifteenth slot was given,
// src=a real, low, module-relative literal address)` -- a shape
// consistent with a sibling C-runtime string function (STRCAT/STRCMP
// are both plausible given the two-pointer-argument shape) but not
// confirmed. Registered as a safe no-op, same rationale as the
// fifteenth slot.
//
// A seventeenth slot, offset 0xd0, was found one real level deeper:
// reached only once the real per-frame callback this title's
// `HandleEvent` registers (see tools/game_probe.cpp's own real
// evidence for that, and IShellHle::ScheduleTimer) actually starts
// running. Its one real call site passes `(name="boot", heap_object)`
// -- `name` points directly at a real, in-module string table
// (`supbtime.mod` offset 0x18ee50: `"boot\0boot.rom\0zupa_p1.rom\0
// zupa_s1.rom..."`, a literal ROM manifest -- "zupapa" is this
// arcade original's real Japanese title, confirming this is the
// generic arcade-core's own real romset-loading code, not a guess),
// and `heap_object` is a real pointer this codebase's own MALLOC slot
// (0x68) had just returned moments earlier. Very likely something
// like "register/hash this named ROM chunk" given the shape, but not
// confirmed -- registered as a safe no-op, same rationale as every
// other unidentified slot above.
//
// An eighteenth slot, offset 0x184, was found continuing that same
// real per-frame callback's own execution once the seventeenth slot
// stopped it from wandering: 2,788 more real steps in (up from the
// seventeenth slot's own 95, itself up from nothing before that --
// each fix unlocking substantially more real, evidenced execution,
// including a real repeating pass through the classic-arcade-core's
// own ROM-manifest string table found earlier in this investigation,
// e.g. real calls with `"roms"`/`"roms\neogeo"`/etc. as arguments).
// Its one real call site passes just `(flag=1, 0, table)`, too thin a
// shape to identify -- registered as a safe no-op, same rationale as
// every other unidentified slot above.
//
// A nineteenth slot, offset 0xdc, was found in Double Dragon (not
// Super BurgerTime, unlike the last few above) once the real GGZ
// short-read padding fix let execution run measurably further: a real
// call site (`ddragonz.mod` offset 0x11de1c) in a small guard function
// that first rejects a null pointer argument (-3) and an
// already-initialized context field at +8 (-1), then calls this slot
// with that one pointer argument, treating a 0 return as success --
// sits in the same tightly-packed real cluster as GetAppContext (0xc0)
// and the seventeenth slot (0xd0), one word after a 4-byte gap.
//
// **Identified for real** (TASKS.md/PHASE8_LOG.md Phase 8, a later
// round): real **decompress this real gzip-compressed resource stream
// in place**. Confirmed two ways, not guessed: (1) with this slot
// stubbed to "always succeed, do nothing," the caller's own subsequent
// `memcpy`-based struct reads (byte offsets +2/+3/+4/+6 off the same
// pointer) pull real, live gzip header bytes -- e.g. the compression-
// method byte at +2 (always 8 for deflate) feeding directly into a
// real `internalformat` argument downstream, provably an arithmetic
// artifact (`0x8b95 + 8 = 0x8b9d`, the exact, always-identical
// "unknown format" this project kept rejecting), not real image data;
// (2) those exact same byte offsets (+2 flag, +3 bpp, +4 width, +6
// height, both uint16 LE) match this project's own independently
// reverse-engineered real OBM1 header layout
// (`core/loader/obm1.h`) byte-for-byte -- real code is provably
// already-correct OBM1 header parsing, just fed the wrong (compressed)
// input. Implemented as `DecompressGzipInPlaceImpl`: real zlib
// inflate (matching this project's own already-established real gzip
// handling in `core/loader/ggz.cpp`), streamed from/to emulated
// memory, overwriting the same address with the decompressed result
// (the real caller's own buffer is already sized for the decompressed
// content -- confirmed by matching real `IFILE_Read` "want" sizes
// against the real compressed stream's own true length in an earlier
// round of this same investigation).
//
// A twentieth slot, offset 0x1b4, was found in Double Dragon once the
// app-context fix (TASKS.md Phase 8, this same investigation --
// SetContextAddress no longer masking the third/fourth/fifth context
// fields with placeholders) let it run measurably further than any
// commit before that fix, well past its previous steady state (`ddragonz.
// mod` offset 0x11f870, reached from a real per-array-element
// initialization loop: two sibling loops immediately before/after this
// call process consecutive 4-byte-stride array entries at fixed offsets
// `+0x3000`/`+0x4000` off the same base).
//
// **Identified for real** (TASKS.md/PHASE8_LOG.md Phase 8, the sprite
// z-ordering investigation's fix round): originally guessed as an
// "array-of-N-constructor" RVCT/EABI helper and stubbed as a no-op,
// since a raw element size wasn't visible. That guess was wrong --
// tracing the one real call site directly (`ddragonz.mod` 0x11f870,
// inside the real per-frame "draw every registered character" function)
// shows this is a real, generic **sort**: called with `(base=this+0x4234,
// count=<the real per-frame character count>, size=4, compar=<a real
// ARM function pointer>)`, i.e. exactly `void SORT(void *base, int
// count, int size, int (*compar)(const void*, const void*))` --
// `size=4` (pointer-sized elements) is what rules the old "construct N
// objects" guess out: a real constructor helper would need the real
// per-object size (hundreds of bytes for an entity), not 4. Disassembled
// the real comparator itself (`ddragonz.mod` 0x10b918): both arguments
// are pointers to array slots (each slot holding an `Entity*`), and it
// compares the pointed-to entities' real fields at `+0x7c` (primary
// key) then `+0x50` (tie-break -- the same field this project already
// found gates which of the two render categories an entity lands in),
// returning +1 when the first entity sorts before-or-equal the second
// and -1 otherwise (this real comparator's own polarity, not the
// standard C `qsort` convention). This is very likely the real fix for
// the sprite z-ordering bug this whole investigation chased: without a
// real implementation here, the character render list stays in
// whatever order entities were registered this frame (their real
// per-entity `Update()` call order, itself governed by a fixed 8-slot
// pool array with no ordering relationship to on-screen position), so
// draw order -- and with it, the depth value each character's vertices
// get stamped with -- never reflects real relative depth at all.
// Implemented as `SortPointerArrayImpl`: a real in-place insertion sort
// over the `count` pointer-sized slots at `base`, calling back into the
// real ARM `compar` function (via `HleRuntime::CallArmFunction`, saving
// and restoring `LR` around each nested call since `CallArmFunction`
// repurposes it as its own return sentinel) for every comparison --
// deferring entirely to the real game's own comparison logic rather
// than reimplementing whatever `+0x7c`/`+0x50` actually mean.
//
// A twenty-first slot, offset 0xc8, was found in Zeebo Sports Tênis
// (TASKS.md Phase 8, the ClsId-discovery investigation's follow-up
// round) tracing the wander hit immediately after first reaching that
// title's own event loop. Real call site (`zeebotennis.mod` 0x1013f8)
// calls it right after the already-known sprintf-family formatter
// (offset 0x13c) with `(dest, sprintf's-own-scratch-buffer,
// maxlen-1)`; a second, unambiguous real call site (0x136bf0) shows
// `(dest=<a fixed offset inside a freshly-constructed object>,
// src=<a plain string>, maxlen=32)` -- a plain constant, not a
// computed length, ruling out a concat/append interpretation. Both are
// consistent with real, standard **`strncpy(dest, src, maxlen)`**
// semantics: implemented as `StrncpyImpl`.
//
// A twenty-second slot, offset 0x18, was found continuing the same
// Zeebo Sports Tênis round, immediately past where the `strncpy` fix
// let real execution advance to (`zeebotennis.mod` 0x104320). Real
// call site: `(s=<a string pointer>, c=43 /* '+' */)`, with the return
// value checked for null immediately after and the string's own first
// byte separately re-read right after that -- exactly real, standard
// **`strchr(s, c)`** semantics: implemented as `StrchrImpl`.
//
// A twenty-third slot, offset 0x140, was found in Zeeboids (TASKS.md
// Phase 8, picked as a Zeebo Sports Tênis "sibling" sharing the same
// real `TTDMemoryManager.cpp` engine code -- confirmed live: the one
// real call site found so far sits immediately before a real
// `dbgprintf` (offset 0x9c) call whose own arguments reference that
// exact real source file string). Real calling convention: `(dest=a
// 64-byte local buffer, size=64, arg2=<a caller-supplied value>,
// arg3=<a pointer to a small local struct>)` -- a shape consistent
// with a sprintf-family formatter (feeding `dbgprintf`'s own `%s`
// argument), but only one real call site, not enough independent
// corroboration the way offset `0xc8`'s `strncpy` identification had.
// Since `dbgprintf` is itself already a complete no-op that discards
// its message entirely, whatever this slot actually computes never
// affects anything downstream regardless -- registered as a safe
// no-op (matching the fifteenth slot's own precedent) rather than
// guessed at, unblocking real execution past this call site without
// claiming to know what it does.
//
// A twenty-fourth slot, offset 0x138 (immediately before the sprintf-
// family slot at 0x13c), was found picking a new, architecturally
// different title (Alien Breaker Deluxe) to try after Zeeboids/Zebo
// Sports Volei/Tênis all converged on the same interpreter-throughput
// wall -- this title's own real per-tick code calls it once, right
// after a real `malloc(48)`, with `(dest=a stack buffer, src=an
// adjacent stack buffer)` and a small, per-tick-incrementing third
// register value (23, then 25 one tick later) left over from loading
// the callee's own address into it, not confirmed as a real third
// argument. Left unregistered, this was a real, confirmed null
// function pointer (`blx` through a never-written table slot,
// producing the exact "pc=0" wander this project's own tooling
// already catches). Only one real call site found so far, and its
// own result isn't visibly consumed by the immediately-following real
// code -- not enough evidence to guess a real identity, so this is a
// safe no-op (matching the fifteenth/twenty-third slots' own
// precedent) rather than a guess, just to unblock real execution past
// this specific call site.
//
// A twenty-fifth slot, offset 0x30, was found bringing up a fresh
// title (Disney All Star Cards) after Alien Breaker Deluxe's own
// bring-up stalled on incomplete source data rather than an engine
// gap: real code calls it right in `HandleEvent(EVT_APP_START)`
// itself (88 real steps in, the fastest any of this project's own
// runtime-slot gaps has been hit), immediately after a real call
// through a *different*, per-instance vtable (offset 0x44 there,
// unrelated to this table's own identically-numbered MEMCPY-alias
// slot). Its own real return value is used as a loop counter
// (`add r0,r0,#1; cmp r0,r4; ble ...` immediately follows), suggesting
// something enumerable/iterable, but only one real call site found so
// far and not enough evidence to guess further -- registered as a
// safe no-op matching the established precedent for single-call-site
// gaps.
//
// A twenty-sixth slot, offset 0x144 (immediately after the sprintf-
// family slot at 0x13c and the twenty-third slot at 0x140), was found
// continuing that same Disney All Star Cards round: real code calls it
// with a shape that looks sprintf-adjacent but isn't quite this file's
// own confirmed `SprintfImpl` convention -- `(dest=a stack buffer,
// size=32, fmt=a real literal string address)`, an explicit size
// parameter between dest and fmt that `SprintfImpl`'s own confirmed
// `(dest, fmt, ppArgs)` shape doesn't have, very plausibly a real
// `snprintf`-shaped sibling rather than the same function reused.
// Only one real call site found so far and the size/format mismatch
// means reusing `SprintfImpl` outright would risk silently wrong
// behavior rather than just an unimplemented gap -- registered as a
// safe no-op instead, matching the same established precedent.
//
// A twenty-seventh slot, offset 0x14c (immediately after the
// twenty-sixth), and a twenty-eighth, offset 0x150 (immediately after
// that), were found continuing the same Disney All Star Cards round,
// each one appearing the moment the previous gap in this same tight
// cluster (0x138-0x150) got fixed -- real, evidenced forward progress
// each time (step count reaching the next gap grew from 88 to 226 to
// 3578 to 5288 across this whole run), not a guess. Neither has a
// calling convention confirmed well enough to identify; both
// registered as safe no-ops matching the same established precedent.
//
// A twenty-ninth slot, offset 0x64 (immediately before the confirmed
// MALLOC slot at 0x68), was found the same round, one real gap deeper
// still (5288 steps in): a real 4-argument call
// (`dest`/`src`-shaped first two registers plus two more) with no
// confirmed identity yet -- also a safe no-op.
//
// Three more slots -- offset 0xcc (immediately after the confirmed
// STRNCPY slot at 0xc8, a thirtieth), offset 0x90 (immediately before
// the confirmed DBGPRINTF slot at 0x9c, a thirty-first), and offset
// 0x10 (right after MEMCPY/MEMSET, a thirty-second) -- were found
// finishing the same Disney All Star Cards round, the last of them
// (0x10) reached only *after* real code got past `HandleEvent
// (EVT_APP_START)` into its own first real per-tick timer callback --
// this title's own `CreateInstance`/`HandleEvent` now both succeed
// cleanly, a real, substantial milestone none of this round's earlier
// fixes reached alone. None of these three have a confirmed calling
// convention; all three are safe no-ops matching the same established
// precedent. Confirmed via a temporary, exhaustive diagnostic (every
// still-unmapped table offset up to 0x400 filled with a logging no-op
// instead of guessing one gap at a time) that 0x10 was the *last* real
// gap this title's own boot sequence needs: with it registered, a full
// 20-second run reaches and stays in the real per-tick event loop with
// no further wander, crash, or unmapped-slot hit at all.
//
// A thirty-third slot, offset 0x20 (immediately after the confirmed
// eighteenth slot at 0x1c), was found in Alien Breaker Deluxe -- not
// during bring-up, but as the real root cause of a real, reproducible
// crash a real human hit live, mashing the menu confirm button
// (`abd.mod` 0x108d98, `blx` through this exact table slot, real pc
// landing on 0 -- this table's own unwritten-memory default -- and
// wandering out of the module). Registered as a blind no-op at first
// (the crash-site call's own return value is genuinely unused, and
// that fix alone was enough to stop the crash), but a real, separate
// live investigation the same session -- chasing a real, direct human
// report that gameplay's own SCORE/lives/shield HUD text never
// rendered at all -- found this same slot reached again, at several
// other real call sites, in a shape the blind stub couldn't satisfy:
// real code passes `(dest=a stack buffer, fmt=a real literal format
// string, value=a real plain integer, not a ppArgs cursor)` and reads
// the real *formatted result* back out of that buffer afterward. Real
// literal format strings observed live: `"%d"`, `"%06d"` (real score
// values climbing 0/25/50/100/125/175/250/350/475/625/650 as bricks
// broke), `"x%d"` (real lives count 1-4), and bare literals with no
// `%` at all (`"EXTRA"`, passed through unchanged). **This is a real
// sprintf-family formatter, structurally the same job as the
// already-implemented `SprintfImpl` (offset 0x13c) but with a real,
// simpler single-direct-value calling convention** instead of that
// slot's own double-indirection `ppArgs` cursor -- confirmed live by
// literally writing a placeholder character into the buffer and
// watching real, previously-blank HUD text turn into a visible glyph
// in exactly the positions real reference footage shows real
// score/lives/shield digits. Implemented as `FormatSingleIntImpl`,
// reusing the same real `%d`/`%u`/`%x`/`%X` plus width/zero-pad
// directive support `SprintfImpl` already has, substituting the one
// real integer argument wherever a numeric directive appears.
//
// A thirty-fourth slot, offset 0xa8, was found the same way, chasing a
// second real crash the same round: Alien Breaker Deluxe's own real
// ball-spawn sequence (`abd.mod` 0x109d54, `blx` through this table
// slot) -- reached once, exactly when a real new gameplay entity (the
// ball) is about to be created, right after a real human got a real
// level's paddle-intro animation on screen. Unlike the thirty-third
// slot (and every earlier one), this real call's return value in `r0`
// is *not* what the real caller reads -- real code passes `(buffer=a
// stack address, flag=1)` and reads a real byte back out of that
// buffer itself (`ldrb r0, [sp, #36]`), checked twice (`cmp r0,#0` then
// `tst r0,#7`) before falling through into real, disassembly-confirmed
// entity-creation code. A blind "touch nothing" stub would leave that
// real byte as whatever real stack garbage happened to already be
// there, making the outcome nondeterministic rather than a clean pass-
// through. Implemented by writing a real `0` into the caller's own
// buffer explicitly (`CheckObjectFlagImpl`-shaped, offset
// `kCheckObjectFlagSlotOffset0xa8`) -- both real checks then read as
// "condition not met," matching this table's own established "zero
// means success/false uniformly" convention, and real execution
// reaches the real entity-creation call live disassembly shows follows
// immediately, rather than branching into what real disassembly shows
// is a "skip this candidate, advance to the next linked entity" path
// instead.
//
// Only these thirty-four table slots are confirmed by real
// disassembly so far. Every other offset is left unmapped -- a real
// .mod hitting one would fetch from unwritten memory, which
// tools/game_probe.cpp's wandered-outside-module check exists
// specifically to catch and report loudly rather than silently
// misbehave.
class ModRuntime {
 public:
  // `heap_region`/`heap_size` bound a simple bump allocator the MALLOC
  // slot hands memory out from. FREE is a no-op (leaks) -- consistent
  // with having no free-list, which is fine for a single game-session
  // emulator run. `context_address` is where the small "app context"
  // struct the offset-0xc0 slot returns is stored; must not overlap the
  // module, the heap region, or any other memory region in use.
  ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size,
             uint32_t context_address);

  // Sets the IShell object pointer the offset-0xc0 "get app context"
  // slot should expose at the confirmed field offset (+12). Safe to call
  // before or after Install().
  void SetShellInstance(uint32_t shell_ptr);

  // Sets the IDisplay object pointer the offset-0xc0 "get app context"
  // slot should expose at the confirmed field offset (+20). Safe to call
  // before or after Install().
  void SetDisplayInstance(uint32_t display_ptr);

  // Sets the object pointer the offset-0xc0 "get app context" slot
  // should expose at the confirmed-to-exist, but not-yet-identified,
  // field offset (+0x2c) -- see the class doc comment. Safe to call
  // before or after Install().
  void SetThirdContextObject(uint32_t object_ptr);

  // Sets the object pointer the offset-0xc0 "get app context" slot
  // should expose at the confirmed-to-exist, but not-yet-identified,
  // field offset (+0x24) -- see the class doc comment. Safe to call
  // before or after Install().
  void SetFourthContextObject(uint32_t object_ptr);

  // Sets the object pointer the offset-0xc0 "get app context" slot
  // should expose at the confirmed-to-exist, but not-yet-identified,
  // field offset (+0x28) -- see the class doc comment. Safe to call
  // before or after Install().
  void SetFifthContextObject(uint32_t object_ptr);

  // Redirects the offset-0xc0 "get app context" slot to return a
  // different address than the one passed to the constructor. Found
  // necessary tracing Peggle (TASKS.md Phase 8): `IModule::CreateInstance`
  // returns a real, dynamically-malloc'd `IApplet*` (this codebase's own
  // heap allocator, not a fixed address known up front), and real code
  // reads/writes the third/fourth/fifth context fields directly *on that
  // applet instance*, not on a separate fixed struct -- so GetAppContext
  // needs to expose the applet pointer once it's known, not before. Safe
  // to call any time; takes effect on the next GetAppContext call.
  //
  // Only re-primes the Shell/IDisplay fields onto the new address, NOT
  // the third/fourth/fifth placeholder objects -- also found tracing
  // Peggle: real code gates its own real construction of those fields
  // on the fifth field itself already reading non-zero (it doubles as
  // an "already initialized" flag -- see the class doc comment), so
  // pre-filling it with this codebase's own placeholder here would
  // make real code believe it's already been constructed and skip its
  // own real constructor entirely, permanently freezing that field at
  // a fake placeholder instead of a real object. Leaving all three
  // fields untouched on the new (freshly allocated, zero-filled)
  // address lets real code run its own real construction the first
  // time it actually needs to.
  void SetContextAddress(uint32_t context_address);

  // Advances the millisecond counter the offset-0xb0 GETUPTIMEMS slot
  // returns. Deterministic and tick-driven (not a real wall-clock read)
  // to match how the rest of the emulator's timing works (see
  // IShellHle::Tick). GETUPTIMEMS itself also self-advances this same
  // counter by a small synthetic amount on every read, independent of
  // Tick() -- see GetUpTimeMsImpl's doc comment for why a purely
  // externally-driven clock isn't enough.
  void Tick(uint32_t elapsed_ms);

  // Writes `table_address` at `module_base - 4` and populates the
  // table's known slots. Must be called after the module itself has
  // been loaded at `module_base`; `table_address` must not overlap the
  // module or any other memory region in use.
  void Install(uint32_t module_base, uint32_t table_address);

 private:
  void MallocImpl(IArmCore& core);
  void MemcpyImpl(IArmCore& core);
  void MemsetImpl(IArmCore& core);
  void StrlenImpl(IArmCore& core);
  void StrcpyImpl(IArmCore& core);
  void BoundedStrcpyImpl(IArmCore& core);
  void StrncpyImpl(IArmCore& core);
  void StrchrImpl(IArmCore& core);
  void StrstrImpl(IArmCore& core);
  void SprintfImpl(IArmCore& core);
  void FormatSingleIntImpl(IArmCore& core);
  void GetAppContextImpl(IArmCore& core);
  void GetUpTimeMsImpl(IArmCore& core);
  void ReallocImpl(IArmCore& core);
  void DecompressGzipInPlaceImpl(IArmCore& core);
  void SortPointerArrayImpl(IArmCore& core);

  // Shared bump-allocation core used by both MallocImpl and
  // ReallocImpl. Returns 0 (NULL) if `size` doesn't fit in the
  // remaining heap.
  uint32_t Allocate(uint32_t size);

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t heap_cursor_;
  uint32_t heap_end_;
  uint32_t context_address_;
  uint32_t shell_ptr_ = 0;
  uint32_t display_ptr_ = 0;
  uint32_t third_context_object_ = 0;
  uint32_t fourth_context_object_ = 0;
  uint32_t fifth_context_object_ = 0;
  uint32_t uptime_ms_ = 0;

  // GetAppContextImpl only re-writes a field into the context struct
  // when its Set*() call is pending (tracked here), rather than on
  // every single call. Found necessary tracing Peggle (TASKS.md Phase
  // 8): unconditionally rewriting every field on every call (the
  // original design, so host-side Set*() calls always took effect
  // regardless of ordering against Install()) silently clobbered real
  // code's own direct writes into the third/fourth/fifth fields
  // (confirmed real -- see the class doc comment) on the very next
  // GetAppContext call. Since every current Set*() call site in this
  // codebase happens once, up front, before any real ARM code runs,
  // "write once, then leave real code's own writes alone" preserves
  // the original behavior for host-driven setup while no longer
  // fighting the game's own runtime writes.
  bool shell_pending_ = false;
  bool display_pending_ = false;
  bool third_pending_ = false;
  bool fourth_pending_ = false;
  bool fifth_pending_ = false;
};

}  // namespace zeebulator
