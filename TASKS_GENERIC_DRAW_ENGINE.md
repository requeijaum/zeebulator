# Zeebulator — Generic Draw-Engine Task Breakdown

Companion to `TASKS.md` (per-title emulation-correctness work) and
`PHASE8_LOG.md` — this file tracks reimplementing the shared "mystery
engine" interface (real BREW ClsId `0x0103d8ec` family) generically,
dispatched by real vtable slot + real argument/state, instead of by
matching literal ARM return addresses inside one title's own compiled
`.mod`. Exists because this interface is confirmed shared, byte-for-byte,
across at least five independently-compiled titles — continuing to
re-derive its behavior per title (the current approach in
`tools/game_probe.cpp`) doesn't scale, and the user explicitly asked for
something uniform that works across games instead of repeating this
investigation from scratch every time.

## Context: what's already there

- Real ClsId `0x0103d8ec` (plus sibling `QueryInterface` targets
  `0x0103d8dd`/`0x0103d8ea`) is a real, unidentified BREW class — not in
  any bundled SDK header, not found via public web search (tried this
  round: no hits). Confirmed shared byte-for-byte, same
  `ISHELL_CreateInstance`-then-fallback-to-`AEECLSID_EGL` instruction
  sequence, across Peggle, Super BurgerTime, Alien Breaker Deluxe,
  Zeeboids, and Zeebo Sports Volei (`TASKS.md`, multiple entries). Treated
  as one real, shared 2D draw engine used by several titles, not
  title-specific code.
- **Not the same pipeline Double Dragon uses.** Double Dragon renders
  through real `AEECLSID_GL`/`AEECLSID_EGL` directly — a separate,
  already-working real OpenGL ES path this file's own work doesn't touch
  at all. Not a valid cross-check target for this specific effort; kept
  only as a "did this change break anything else" regression smoke test
  below, since some of the plumbing it shares (`BuildInterfaceObject`,
  `HleRuntime`) is generic infrastructure other titles' interfaces use too.
- Today's implementation (`tools/game_probe.cpp`,
  `unknown_0x0103d8ec_methods[2]`'s 200-slot scaffold) is entirely Alien
  Breaker Deluxe-specific: real slot semantics were correctly derived
  (33 = select texture, 40 = set fill color, 107 = draw geometry element),
  but *which draw path a given call represents* (text glyph / icon /
  shape) is currently decided by matching literal ARM return addresses
  inside `abd.mod` (`real_caller == 0x105744`, etc.) — meaningless for any
  other title's own compiled binary — and the whole branch is gated on
  `abd_font_atlas.has_value()` (ABD-only).
- **New finding this round**: real vtable slot 100 fires immediately
  after every real slot 107 draw, unconditionally, regardless of which
  real game code triggered it — confirmed for both text-glyph and icon
  draws. Its 8-value 16.16 fixed-point struct is shaped like real rect
  geometry: for glyphs, only the X pair varies call to call (Y stays
  constant within one row of text); for icons, both X and Y vary
  meaningfully (matching the icons' own real 2D spritesheet layout,
  already confirmed via a full texture dump). Strong evidence this is the
  real, generic "set source rect" state call — if confirmed and
  implemented properly, it would replace both of ABD's existing crop
  hacks (the `0x106508` PC-watchpoint icon-descriptor sniff, and the
  manual atlas-row/column arithmetic used for glyphs) with one real,
  data-driven mechanism sourced entirely from real interface calls.
- **Not yet resolved**: exact byte semantics of the struct's second half
  (offset+16..+28) — looks like duplicate-X plus a single Y baseline, but
  the arithmetic relationship to the first half (offset+0..+12) wasn't
  consistent across every sample captured so far. Needs more targeted
  live sampling before code is written against it.
- **Also not yet generalized**: destination-rect anchor/Y-flip selection
  (`AbdTextState::anchor_mode_18_this_call`, and the differing flip
  conventions for text/shape vs. mode-18 sprites vs. icons) still depends
  on `real_caller` matching — a second, separate piece of state a later
  phase here needs to find a generic real source for too. Not blocking
  Phase A/B below, which only tackle the source-rect half.

## Phase A — Pin down slot 100's real semantics

Exit criterion: struct fields decoded with the same confidence level this
project already has for the icon descriptor's own fields (cross-checked
against known-good values, not just pattern-matched from a handful of
samples).

**Progress this round (live-instrumented, all temporary code reverted
after — `git diff --stat` clean, 431/431 tests pass):**

- Confirmed real slot 100 fires unconditionally after every real slot 107
  draw, for both text-glyph and icon-shaped draws alike, regardless of
  which real game code triggered it — same real call site (`lr`) either
  way. Its 8-value 16.16 fixed-point struct splits cleanly into two
  4-value halves: offset+16 always exactly duplicates offset+0, and
  offset+24 always exactly duplicates offset+8 (X unchanged between
  halves, confirmed with zero exceptions across every sample). Offset+20
  and offset+28 are always exactly equal to each other too (a single Y
  value, not a min/max pair).
- **Major, unplanned finding while chasing this**: one of the two
  "icon-shaped" real draw clusters found this round turned out to bind a
  real, previously-unexamined 256x256 ATITC/RGBA texture
  (`0x80300a2c`, loaded alongside TITLE at boot) — dumped and visually
  confirmed to be a **second real font atlas** (full A-Z, digits, and
  accented Portuguese characters Ç/Á/É/Í/Ó in a glowing-cyan style), not
  menu icons. Root cause found: real slot 107 draws for this atlas *do*
  go through the real per-character `pending_char_index` mechanism (same
  as ordinary text), but this project's own code unconditionally decodes
  glyph cells from the single, pre-loaded `abd_font_atlas` buffer
  (populated once at startup from a fixed resource id, entirely outside
  the real slot-33 select-texture / `bound_texture` mechanism) instead of
  the real texture actually bound at draw time. This is very likely the
  real, concrete mechanism behind the user's separately-flagged, still-
  open "menu font differs from what we show" report from earlier in this
  project's history — worth fixing as part of Phase B below, not a new
  parallel bug.
- Also confirmed **both** real font atlases (the original English/
  Portuguese one, a real 512x128 ATITC/RGBA texture at `0x80310a7c`, and
  this newly-found 256x256 one) are selected through the exact same real
  slot-33 call every other sprite texture uses — there is nothing
  structurally special about "the" font atlas on real hardware; this
  project's own separate `abd_font_atlas` pre-load is a project-side
  shortcut built before this was understood, not something real hardware
  does.
- **Genuinely unresolved, not yet safe to build on**: which half of the
  struct (offset+0..+12 "local" or offset+16..+28 "adjusted") is the real
  usable source rect. Tested both against the newly-dumped 256x256 font
  atlas: using offset+0..+12 directly landed a sampled glyph cleanly at
  row 1 (Y=0) and a plausible row 2 (Y≈47px, matching the atlas's own
  visible row height); using offset+16..+28 (the "+K" adjusted half)
  would have placed that same row-2 glyph around Y≈93px instead, which
  does not land as cleanly on the visible grid. These two results
  contradicted each other under a single "always use this half" rule.

**Resolved, same round, with a rigorous cross-check instead of more
eyeballing**: compared each candidate crop rect's own pixel width/height
against the *destination* rect's own pixel width/height for the same
draw event (real slot 107's own struct, which independently already uses
this same "offset+20, not offset+12" convention for its own destination
height — see `dest_h` in the draw code). Live-instrumented 34,590 real
samples of real caller `0x105744` (every text-glyph and icon draw across
a full boot+menu-navigation run): **100.0% exact match, zero exceptions**,
once the struct is read as a real 4-corner quad rather than two
redundant halves -- offset+16 always exactly duplicates offset+0 and
offset+24 always exactly duplicates offset+8 (X unchanged top/bottom,
axis-aligned), while the real usable Y range spans offset+4 (top) to
offset+20 (bottom) -- i.e. **X from either half (they're identical); Y0
from the first half, Y1 from the second**. This is exactly the field
layout Alien Breaker Deluxe's own real 44-byte descriptor (read via
`AbdTextState::last_draw_descriptor_addr`, formerly named
`last_icon_descriptor_addr`) already exposes at its own offset+16
(x0)/+20 (y0)/+24 (w)/+28 (h) -- meaning the mechanism this project had
already built and verified for icons was *already* the correct, generic
mechanism; it only needed to stop being gated behind "no pending char
index."

- [x] Resolved via a same-draw destination-vs-crop pixel cross-check
      (100% exact match, 34,590 samples) rather than a same-context
      multi-row sample -- turned out to be more direct
- [x] Confirmed real caller `0x105744`'s own already-existing crop
      mechanism (44-byte descriptor, offset+16/20/24/28, pixel-space)
      already correctly generalizes to both real text glyphs and real
      icons -- no new slot-100-based state needed after all; slot 100
      was a valuable independent cross-check, not a new production
      mechanism
- [x] Documented the confirmed layout in `tools/game_probe.cpp`
      (`AbdTextState::last_draw_descriptor_addr`'s own doc comment, and
      the real textured-draw branch inside `stub_methods[107]`)

## Phase B — Generic source-rect + draw dispatch (Alien Breaker Deluxe)

Exit criterion: ABD's text glyphs and icons both still render correctly
(no regression vs. current screenshots) with `real_caller`/pending-char-
index matching removed from crop-selection specifically — driven only by
the real, generic per-descriptor crop mechanism confirmed in Phase A, no
separate glyph-cell-copy code path.

**Done, same round.** Removed entirely: the separate glyph-draw branch
(hardcoded `abd_font_atlas` cell copy, `atlas_row`/`atlas_col`
arithmetic, `kAbdFontCellPx`/`kAbdFontRowPitch`/`kAbdFontGlyphYOffset`
constants), `AbdTextState::pending_char_index` and its own `0x105dac`
watchpoint (confirmed, live, to be nothing more than an index into the
same real descriptor array real icon draws already use — see Phase A).
Every real caller-`0x105744` draw now takes one real, generic path:
decode whichever real texture is bound, crop it per the real descriptor
at `AbdTextState::last_draw_descriptor_addr`, blit. Net diff: -169/+97
lines — a genuine simplification, not just a rename.

**Found something real and unplanned while implementing this**: Alien
Breaker Deluxe's real "CHOOSE YOUR LANGUAGE" screen (ENGLISH/ESPAÑOL/
PORTUGUÊS, the real screen immediately after language selection reaches
the menu) had never been seen rendering at all before this round — it
draws entirely through the real second font atlas (`0x80300a2c`, Phase A
findings above) that this project's old, hardcoded-single-atlas glyph
path silently couldn't reach. It now renders correctly: legible glowing
text, correct circular language-selector badges with the real dashed
selection ring. This is very likely the concrete, real fix for the
user's separately-flagged, still-open "menu font differs from what we
show" report from earlier in this project's history.

- [x] Replaced `AbdTextState::last_icon_descriptor_addr`-gated-on-icons
      crop logic with a real, generic version driven by real caller
      `0x105744` alone (no pending-char-index gate)
- [x] Removed the separate hardcoded-atlas glyph-cell-copy path entirely
      (superseded, confirmed dead)
- [x] Live-verified three real screens unchanged/fixed: splash
      (LOGO/LOGOSTAR/LOADING, confirmed byte-for-byte same as before this
      round), language-select (previously unreachable/broken, now
      renders correctly for the first time), ARCADE/CHALLENGE/VERSUS/
      FRONTON icon submenu (confirmed unchanged — same badge geometry and
      dashed-ring behavior as before this round; flat solid-color icon
      fill is still present, a separate, already-tracked open item, not
      something this round touched)
- [x] Full `ctest` suite green throughout (431/431)

## Phase C — Cross-title validation (Peggle)

Exit criterion: Peggle — a second, independently-compiled title already
confirmed instantiating this exact same ClsId family and reaching
thousands of real ticks without stalling (unlike Zeeboids/Zeebo Sports
Volei, which get stuck elsewhere in their own tick loops) — renders real
on-screen content through this same generic Phase B implementation, with
*zero* new Peggle-specific code added.

- [ ] Run Peggle far enough (`tools/game_probe.cpp` or successor) to reach
      its own real calls into slot 33/100/107 on this class family
- [ ] Confirm real pixels appear on screen sourced from real texture data,
      not a flat/blank/garbled result
- [ ] If it doesn't work out of the box, record exactly what differed
      (real struct shape, real slot numbering, a genuinely different real
      interface hiding behind the same ClsId) rather than silently adding
      Peggle-specific matching — that would defeat the point of this file

**Attempted, same round. Blocked before reaching this engine's own draw
calls at all — a real, pre-existing, already-documented Peggle issue,
not something today's ABD refactor touched.** Ran `peggle.mod` (real
ClsId `17407190`/`0x01099cd6`, the folder number `278962` does *not*
work — see `tools/game_probe.cpp`'s own usage text) the same way as
ABD. Real boot proceeds normally (13,861 real HLE calls logged, no
errors) and then hits one real call that runs 5,000,000 real interpreter
steps without returning, tripping this project's own step-budget
safety abort (`CallArmFunctionChecked`'s own guard) and stopping the
whole run before a screenshot could be taken — no window was even left
open to check. This matches a real, already-documented, separate Peggle
blocker from earlier in this project's history (`TASKS.md`, the
"romset loading remains the next real undertaking" entry): a real
second wait loop that genuinely needs ROM-data readiness, not just
elapsed time, to resolve — this project's existing clock-advance fix
only resolves the *first* such loop. **Not a regression from today's
work, and not evidence against the shared-engine hypothesis either —
Peggle simply doesn't run long enough yet, on its own, independent of
this file's own scope, to reach the tick range where its own menu/UI
drawing would fire.**

Also worth correcting this section's own original exit criterion
while it's fresh: even once Peggle *does* run far enough, "zero new
Peggle-specific code" was optimistic as originally scoped. Phase B's
own implementation still classifies which draws are textured
(`real_caller == 0x104f84 || 0x1054bc || 0x105744`) by Alien Breaker
Deluxe's own literal compiled ARM addresses -- meaningless in
`peggle.mod`'s own, separately-compiled binary. Real Phase C, once
Peggle runs far enough, will still need Peggle's own equivalent
addresses traced (the same category of work already done for ABD, not
avoidable yet) *unless* a follow-on phase finds a way to classify
"is this a textured/glyph draw" from real, address-independent state
alone (e.g. `bound_texture != 0`, already tracked) instead of by
caller identity -- a real, concrete, not-yet-attempted next step, not
this round's own scope.

- [ ] Real, concrete blocker to clear first, not this file's own scope:
      resolve Peggle's own separate romset/ROM-data-readiness wait loop
      (`TASKS.md`) so it reaches real gameplay/menu ticks at all
- [ ] Once unblocked, trace Peggle's own real equivalent addresses for
      select-texture/draw call sites (same category of work as ABD's,
      unavoidable under the current caller-identity classification)
- [ ] Longer-term, real alternative worth investigating instead of
      repeating per-title address tracing forever: classify "textured
      draw" purely from address-independent real state (`bound_texture
      != 0`, a real descriptor having just been consumed) rather than
      caller identity -- would need care not to reintroduce the real
      "stale nonzero `bound_texture`" bug this project already found and
      fixed once for real caller `0x1054bc` specifically

## Regression guard

- [ ] Double Dragon (separate real GL/EGL pipeline) smoke-tested after
      any change here, since this refactor touches shared generic HLE
      scaffold plumbing other titles' interfaces also use, even though it
      doesn't exercise this specific draw path
- [ ] Full `ctest` suite stays green throughout every phase above, not
      just at the end
