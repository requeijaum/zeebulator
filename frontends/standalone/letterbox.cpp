#include "frontends/standalone/letterbox.h"

namespace zeebulator {

ViewportRect ComputeLetterboxedViewport(int window_width, int window_height, int logical_width,
                                         int logical_height) {
  if (window_width <= 0 || window_height <= 0 || logical_width <= 0 || logical_height <= 0) {
    return ViewportRect{};
  }

  double window_aspect = static_cast<double>(window_width) / window_height;
  double logical_aspect = static_cast<double>(logical_width) / logical_height;

  ViewportRect rect;
  if (window_aspect > logical_aspect) {
    // Window is relatively wider than logical -- pillarbox (bars on the
    // sides), full window height.
    rect.height = window_height;
    rect.width = static_cast<int>(window_height * logical_aspect + 0.5);
    rect.x = (window_width - rect.width) / 2;
    rect.y = 0;
  } else {
    // Window is relatively taller than logical (or exactly matching) --
    // letterbox (bars top/bottom), full window width.
    rect.width = window_width;
    rect.height = static_cast<int>(window_width / logical_aspect + 0.5);
    rect.x = 0;
    rect.y = (window_height - rect.height) / 2;
  }
  return rect;
}

}  // namespace zeebulator
