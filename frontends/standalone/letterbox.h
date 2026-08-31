#pragma once

namespace zeebulator {

// A pixel rect within a real window, in that window's own coordinate
// space (origin top-left, matching SDL/GL viewport convention).
struct ViewportRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Computes the largest centered rect within a `window_width` x
// `window_height` real window that preserves `logical_width` x
// `logical_height`'s own aspect ratio -- used to blit a fixed-resolution
// offscreen framebuffer onto a real window of arbitrary size (window
// resizing, or an explicit 2x/3x/4x scale) without distorting it.
// Letterboxes (bars top/bottom) or pillarboxes (bars left/right)
// depending on which dimension is the tighter constraint; degenerate
// inputs (zero/negative window or logical size) return an empty rect at
// the origin rather than dividing by zero.
ViewportRect ComputeLetterboxedViewport(int window_width, int window_height, int logical_width,
                                         int logical_height);

}  // namespace zeebulator
