// Text on Linux: glyph bitmaps made with stb_truetype, in the layout the
// Android build gets from SmasActivity.renderGlyphs (see video_gles.cpp's
// buildFont): meta[0] ascent, meta[1] line height, then per code point the
// advance x 64 (-1: no glyph), the bitmap's left and top from the pen and the
// baseline, its width and height; `bits` the 8-bit coverage bitmaps in order.
//
// The fonts: the "fonts" folder beside the program (ui-regular.ttf,
// ui-bold.ttf, ui-fallback.ttf, ui-symbols.ttf: build.sh copies the system's there, and the
// AppImage carries them), else the system's own (fc-match), and Luckiest Guy
// (LuckiestGuy-Regular.ttf beside the program) for the launcher's title.
#pragma once
#include <cstdint>
#include <vector>

bool renderGlyphs(const std::vector<uint32_t>& cps, float px, bool bold, bool title, std::vector<int>& meta,
                  std::vector<uint8_t>& bits);
