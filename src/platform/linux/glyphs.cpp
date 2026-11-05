// Text on Linux: see glyphs.h.
#include "platform/linux/glyphs.h"
#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace {

struct FontFile {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  bool ok = false;
};

std::string baseDir() {
  static std::string dir = [] {
    char* p = SDL_GetBasePath();
    std::string d = p ? p : "./";
    SDL_free(p);
    return d;
  }();
  return dir;
}

bool loadFile(const std::string& path, FontFile& f) {
  if (path.empty()) return false;
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) return false;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (n <= 0 || n > (64l << 20)) {
    fclose(fp);
    return false;
  }
  f.data.resize((size_t)n);
  bool read = fread(f.data.data(), 1, (size_t)n, fp) == (size_t)n;
  fclose(fp);
  if (!read) return false;
  int off = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
  f.ok = off >= 0 && stbtt_InitFont(&f.info, f.data.data(), off);
  return f.ok;
}

// The system's font for a fontconfig pattern (empty if fc-match isn't there).
std::string fcMatch(const char* pattern) {
  std::string cmd = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return {};
  char buf[1024];
  std::string out;
  while (size_t n = fread(buf, 1, sizeof buf, p)) out.append(buf, n);
  pclose(p);
  return out;
}

// The first of these that loads.
std::unique_ptr<FontFile> openFirst(std::initializer_list<std::string> paths) {
  auto f = std::make_unique<FontFile>();
  for (const std::string& p : paths)
    if (loadFile(p, *f)) return f;
  return nullptr;
}

FontFile* uiFont(bool bold) {
  static std::unique_ptr<FontFile> regular, boldFace;
  static bool triedR = false, triedB = false;
  const std::string fonts = baseDir() + "fonts/";
  if (bold) {
    if (!triedB) {
      triedB = true;
      boldFace = openFirst({fonts + "ui-bold.ttf", fcMatch("sans-serif:semibold"), fcMatch("sans-serif:bold"),
                            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"});
    }
    if (boldFace) return boldFace.get();
  }
  if (!triedR) {
    triedR = true;
    regular = openFirst({fonts + "ui-regular.ttf", fcMatch("sans-serif"),
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                         "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"});
  }
  return regular.get();
}

// For symbols the UI font lacks (arrows, stars; the circled button letters,
// which DejaVu doesn't have): tried in order.
const std::vector<FontFile*>& fallbackFonts() {
  static std::vector<std::unique_ptr<FontFile>> owned;
  static std::vector<FontFile*> list;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const std::string fonts = baseDir() + "fonts/";
    for (auto&& paths : {std::initializer_list<std::string>{fonts + "ui-fallback.ttf", fcMatch("DejaVu Sans"),
                                                            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                                            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"},
                         std::initializer_list<std::string>{fonts + "ui-symbols.ttf", fcMatch("sans-serif:charset=24b6")}})
      if (auto f = openFirst(paths)) {
        list.push_back(f.get());
        owned.push_back(std::move(f));
      }
  }
  return list;
}

FontFile* titleFont() {
  static std::unique_ptr<FontFile> f;
  static bool tried = false;
  if (!tried) {
    tried = true;
    f = openFirst({baseDir() + "LuckiestGuy-Regular.ttf", baseDir() + "fonts/LuckiestGuy-Regular.ttf"});
  }
  return f.get();
}

}  // namespace

bool renderGlyphs(const std::vector<uint32_t>& cps, float px, bool bold, bool title, std::vector<int>& meta,
                  std::vector<uint8_t>& bits) {
  FontFile* main = title ? titleFont() : nullptr;
  if (!main) main = uiFont(bold || title);  // (no title font: the UI font, bold, as on Windows)
  if (!main) return false;
  // (Android's Paint text size is the em size)
  auto scaleOf = [px](FontFile* f) { return stbtt_ScaleForMappingEmToPixels(&f->info, px); };
  const float sc = scaleOf(main);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&main->info, &asc, &desc, &gap);
  meta.assign(2 + cps.size() * 5, 0);
  bits.clear();
  meta[0] = (int)std::ceil(asc * sc);
  meta[1] = (int)std::ceil((asc - desc + gap) * sc);
  for (size_t i = 0; i < cps.size(); i++) {
    int* m = &meta[2 + i * 5];
    const int cp = (int)cps[i];
    FontFile* f = main;
    int g = stbtt_FindGlyphIndex(&f->info, cp);
    for (FontFile* fb : fallbackFonts()) {
      if (g) break;
      f = fb;
      g = stbtt_FindGlyphIndex(&f->info, cp);
    }
    if (!g && cp != ' ') {
      m[0] = -1;
      continue;
    }
    if (!g) f = main;
    const float s = scaleOf(f);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
    m[0] = (int)std::lround(adv * s * 64);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&f->info, g, s, s, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) continue;  // (a space: no bitmap)
    const int w = x1 - x0 + 2, h = y1 - y0 + 2;  // a pixel of room for anti-aliasing on each side
    std::vector<uint8_t> bmp((size_t)w * h, 0);
    stbtt_MakeGlyphBitmap(&f->info, bmp.data() + w + 1, w - 2, h - 2, w, s, s, g);
    bits.insert(bits.end(), bmp.begin(), bmp.end());
    m[1] = x0 - 1;
    m[2] = y0 - 1;
    m[3] = w;
    m[4] = h;
  }
  return true;
}
