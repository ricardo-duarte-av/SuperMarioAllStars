// Super Mario All-Stars port: the Linux side (SDL window and event loop, frame
// pacing, where files are kept, the ROM picker). It shares the Android build's
// SDL / OpenGL ES 3.0 layer (platform/android: video, sound, controllers,
// touch); the application itself is app/app.cpp.
//
// Files: the settings, saves, states and screenshots go beside the AppImage
// (portable: the folder it is in, when that can be written to), beside the
// program when a file named "portable" is there, else to
// $XDG_DATA_HOME/SuperMarioAllStars (~/.local/share/SuperMarioAllStars). The ROM is looked
// for there (and in a "roms" folder in it), beside the program and up to three
// folders above it, beside the AppImage and in the current folder.
#include <SDL.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include "app/app.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include "core/png.h"

namespace {
std::wstring g_resDir;  // the program's folder (fonts, a bundled ROM)
// --capture <file.png> <seconds>: the window's picture written after that long, then quit (checks, CI)
std::string g_capturePath;
double g_captureAfter = 0;

// Refreshes per game frame when the display rate is a whole multiple of the
// game's 60.1 fps, else 0.
int lockedRefreshes(double hz) {
  int k = (int)std::lround(hz / Snes::kFrameRate);
  return k >= 1 && std::fabs(hz / k - Snes::kFrameRate) / Snes::kFrameRate < 0.025 ? k : 0;
}

std::wstring trimSlash(std::wstring d) {
  while (d.size() > 1 && d.back() == L'/') d.pop_back();
  return d;
}

std::wstring envDir(const char* name) {
  const char* v = getenv(name);
  return v && *v ? trimSlash(widenUtf8(v)) : std::wstring();
}

void makeDirs(const std::wstring& path) {
  std::string p = narrowUtf8(path);
  for (size_t i = 1; i <= p.size(); i++)
    if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
}

std::vector<std::wstring> romDirs(const std::wstring& data) {
  std::vector<std::wstring> dirs = {pathJoin(data, L"roms"), data};
  std::wstring d = g_resDir;
  for (int up = 0; up <= 3 && !d.empty(); up++) {
    dirs.push_back(d);
    dirs.push_back(pathJoin(d, L"roms"));
    if (d == L"/") break;
    d = pathDir(d);
    if (d.empty()) d = L"/";
  }
  std::wstring appimage = envDir("APPIMAGE");
  if (!appimage.empty()) {
    dirs.push_back(pathDir(appimage));
    dirs.push_back(pathJoin(pathDir(appimage), L"roms"));
  }
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) {
    dirs.push_back(widenUtf8(cwd));
    dirs.push_back(pathJoin(widenUtf8(cwd), L"roms"));
  }
  std::vector<std::wstring> unique;
  for (auto& x : dirs)
    if (std::find(unique.begin(), unique.end(), x) == unique.end()) unique.push_back(x);
  return unique;
}

bool hasRomExt(const std::wstring& n) {
  std::wstring lower = n;
  for (wchar_t& c : lower) c = (wchar_t)towlower(c);
  for (const wchar_t* ext : {L".sfc", L".smc", L".swc", L".fig"})
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ext) == 0) return true;
  return false;
}

bool haveCommand(const char* name) {
  std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
  return system(cmd.c_str()) == 0;
}

// The file picker (zenity or kdialog) runs on its own thread; the main loop
// keeps drawing meanwhile and looks at the answer each pass.
struct Picker {
  std::thread th;
  std::atomic<int> state{0};  // 0 idle, 1 open, 2 answered
  std::mutex mu;
  std::string path;
} g_picker;

void runPicker() {
  std::string cmd;
  if (haveCommand("zenity"))
    cmd = "zenity --file-selection --title='Choose the Super Mario All-Stars (USA) ROM' "
          "--file-filter='SNES ROMs | *.sfc *.smc *.swc *.fig *.SFC *.SMC' 2>/dev/null";
  else if (haveCommand("kdialog"))
    cmd = "kdialog --title 'Choose the Super Mario All-Stars (USA) ROM' --getopenfilename \"$HOME\" "
          "'*.sfc *.smc *.swc *.fig *.SFC *.SMC|SNES ROMs' 2>/dev/null";
  std::string out;
  if (!cmd.empty())
    if (FILE* p = popen(cmd.c_str(), "r")) {
      char buf[4096];
      while (size_t n = fread(buf, 1, sizeof buf, p)) out.append(buf, n);
      pclose(p);
    }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  {
    std::lock_guard<std::mutex> lock(g_picker.mu);
    g_picker.path = cmd.empty() ? std::string("\x01") : out;  // (\x01: no picker on this system)
  }
  g_picker.state = 2;
}
}  // namespace

void openUrl(const char* url) { SDL_OpenURL(url); }

void App::applyDisplayMode() {
  if (!window) return;
  SDL_SetWindowFullscreen(window, cfg.displayMode == DisplayMode::Windowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowCursor(cfg.displayMode == DisplayMode::Windowed || inLauncher || menu.open() ? SDL_ENABLE : SDL_DISABLE);
}

bool App::findRom(std::wstring* out) {
  for (const std::wstring& dir : romDirs(exeDir)) {
    std::vector<std::wstring> names;
    if (!listFiles(dir, names)) continue;
    std::sort(names.begin(), names.end());
    for (const std::wstring& n : names) {
      if (!hasRomExt(n)) continue;
      std::wstring p = pathJoin(dir, n);
      std::vector<uint8_t> data;
      if (readAll(p, data) && isSmasRom(data, nullptr)) {
        *out = p;
        return true;
      }
    }
  }
  return false;
}

// A ROM chosen in the picker or dropped on the window: copied into the data
// folder's "roms" (so it is found again however it was opened).
static bool adoptRom(App& app, const std::string& src) {
  std::vector<uint8_t> data;
  if (!readAll(widenUtf8(src), data) || !isSmasRom(data, nullptr)) return false;
  std::wstring dir = pathJoin(app.exeDir, L"roms");
  makeDirs(dir);
  std::wstring name = pathName(widenUtf8(src));
  if (!writeAll(pathJoin(dir, name), data.data(), data.size())) return false;
  app.romPoll = 0;
  app.pollRom(0);
  return app.snes != nullptr;
}

void App::pickRom() {
  if (pickingRom || g_picker.state != 0) return;
  pickingRom = true;
  g_picker.state = 1;
  if (g_picker.th.joinable()) g_picker.th.join();
  g_picker.th = std::thread(runPicker);
}

void App::pollPick() {
  if (!pickingRom || g_picker.state != 2) return;
  g_picker.th.join();
  g_picker.state = 0;
  pickingRom = false;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(g_picker.mu);
    path = g_picker.path;
  }
  if (path == "\x01") {
    menu.notify("Put the ROM in " + narrow(pathJoin(exeDir, L"roms")) + " or drop it on this window", 6.0f);
    return;
  }
  if (path.empty()) return;
  if (!adoptRom(*this, path)) menu.notify("That isn't the Super Mario All-Stars ROM - see the game card", 4.0f);
}

// Taps and drags (a touch screen, or the mouse on the launcher and the menus);
// the on-screen pad's buttons.
void App::touchFrame() {
  touch.layout((float)video.width(), (float)video.height());
  touch.inGame = !inLauncher && snes && !menu.open() && !(game && game->levelLoading());
  touch.visible = touchActive ? cfg.touchMode != 2 : cfg.touchMode == 1;
  touch.showFF = cfg.fastForward && !achLocked();
  touch.showRewind = cfg.rewind && !achLocked();
  if (touch.takePressed(TouchControls::BtnMenu) && touch.inGame) openMenu();
  float dy = touch.takeDrag(), x, y;
  bool tapped = touch.takeTap(&x, &y);
  if (menu.open()) {
    if (dy != 0) menu.drag(dy);
    if (tapped) menu.tap(x, y);
  } else if (inLauncher && tapped) {
    launcherTap = launcher.tap(x, y);
  }
}

bool App::init() {
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
  SDL_SetHint(SDL_HINT_APP_NAME, "Super Mario All-Stars");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Super Mario All-Stars");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  {
    char* base = SDL_GetBasePath();
    g_resDir = trimSlash(widenUtf8(base ? base : "."));
    SDL_free(base);
  }
  // where the settings and saves go: beside the AppImage (portable) when that
  // folder can be written to; beside the program when a "portable" file is
  // there; else the user's data folder
  const std::wstring appimage = envDir("APPIMAGE");
  const std::wstring appimageDir = appimage.empty() ? std::wstring() : pathDir(appimage);
  if (!appimageDir.empty() && access(narrowUtf8(appimageDir).c_str(), W_OK) == 0) {
    exeDir = appimageDir;
  } else if (appimage.empty() && fileExists(pathJoin(g_resDir, L"portable"))) {
    exeDir = g_resDir;
  } else {
    std::wstring xdg = envDir("XDG_DATA_HOME");
    if (xdg.empty()) xdg = pathJoin(envDir("HOME"), L".local/share");
    exeDir = pathJoin(xdg, L"SuperMarioAllStars");
  }
  makeDirs(exeDir);
  g_log = openFile(pathJoin(exeDir, L"smas_port.log"), "w");
  logf("starting, program dir %s, data dir %s", narrow(g_resDir).c_str(), narrow(exeDir).c_str());
  configPath = pathJoin(exeDir, L"smas_port.ini");
  saveDir = pathJoin(exeDir, L"saves");
  shotDir = pathJoin(exeDir, L"screenshots");
  makeDir(saveDir);
  makeDir(pathJoin(exeDir, L"roms"));
  if (!cfg.load(configPath)) cfg.save(configPath);
  cfg.vsync = true;

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  // the windowed size: the game's 4:3 picture at the window scale, inside the desktop
  int scale = std::clamp(cfg.windowScale, 1, 8);
  int ww = 292 * scale, wh = 224 * scale;
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0)
    while (scale > 1 && (ww > dm.w * 0.9 || wh > dm.h * 0.9)) ww = 292 * --scale, wh = 224 * scale;
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  if (cfg.displayMode != DisplayMode::Windowed) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  window = SDL_CreateWindow("Super Mario All-Stars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, flags);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetWindowMinimumSize(window, 320, 240);

  if (!video.init(window)) {
    logf("video init failed: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Super Mario All-Stars", "OpenGL ES 3.0 could not be started.",
                             window);
    return false;
  }
  logf("video ok %dx%d", video.width(), video.height());
  if (!audio.init(cfg.audioLatency)) menu.notify("Audio device unavailable");
  input.init();
  menu.setCanvas(video.canvas());
  touchActive = false;

  const bool haveRom = loadRom();
  logf(haveRom ? "rom loaded: %s" : "rom not found%s", narrow(romPath).c_str());
  achStore.load(pathJoin(exeDir, L"achievements.ini"));
  launcher.setVersion(kAppVersion);
  launcher.setAllStarsOnly(cfg.launcherAllStars);
  launcher.selectGame(Launcher::cardGame(cfg.lastGame));
  playing = Launcher::cardGame(cfg.lastGame);
  if (haveRom) romReady();
  applyConfig();
  inLauncher = true;
  if (haveRom && !startGame.empty()) {
    static const wchar_t* kIds[Launcher::kCards + 1] = {L"smb1", L"smbll", L"smb2", L"smb3", L"allstars"};
    for (int i = 0; i <= Launcher::kCards; i++)
      if (wcscasecmp(startGame.c_str(), kIds[i]) == 0) launchGame(Launcher::cardGame(i));
  }
  if (!achStore.noticeShown) {
    achStore.noticeShown = true;
    achStore.save();
    if (cfg.achievements) achWelcome();
  }
  return true;
}

void App::shutdown() {
  saveSram();
  cfg.save(configPath);
  music.shutdown();
  launcher.releaseGpu();
  menu.setCanvas(nullptr);
  audio.shutdown();
  input.shutdown();
  video.shutdown();
  if (window) SDL_DestroyWindow(window);
  window = nullptr;
  if (g_picker.th.joinable()) g_picker.th.detach();  // (a picker still open: left to close by itself)
  if (g_log) fclose(g_log);
  g_log = nullptr;
  SDL_Quit();
}

// One pass per display refresh when SDL_GL_SwapWindow waits for it (VSync):
// the refresh rate is the median of recent intervals; on a whole multiple of
// the game's 60.1 fps each game frame gets a steady number of refreshes, else
// the game time owed decides. Where swapping doesn't wait (no VSync, a
// hidden window), the passes are timed instead.
void App::run() {
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  const double period = 1.0 / Snes::kFrameRate;
  float intervals[48] = {};
  int nIntervals = 0;
  const int64_t kMouseFinger = -1000;
  bool mouseDown = false;
  double timerDeadline = 0;
  const Uint64 started = last;
  double slept = 0;  // (timed passes: how long the last one waited, left out of the intervals)
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      const float W = (float)video.width(), H = (float)video.height();
      // (mouse coordinates are in window points; the drawable may be larger on HiDPI)
      int winW = 1, winH = 1;
      SDL_GetWindowSize(window, &winW, &winH);
      const float mx = W / std::max(1, winW), my = H / std::max(1, winH);
      switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window, &w, &h);
            video.resize(w, h);
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            focused = true;
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            focused = false;
            input.clearKeys();
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          if (e.key.repeat) break;
          const bool down = e.type == SDL_KEYDOWN;
          // Alt+Enter: windowed / fullscreen (F11 is the Fullscreen binding)
          if (down && e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
            cfg.displayMode =
                cfg.displayMode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
            applyDisplayMode();
            break;
          }
          int vk = vkFromScancode(e.key.keysym.scancode);
          if (vk) input.onKey(vk, down);
          if (down) touchActive = false;
          break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED: input.rescanDevices(); break;
        case SDL_CONTROLLERBUTTONDOWN: touchActive = false; break;
        case SDL_CONTROLLERAXISMOTION:
          if (std::abs(e.caxis.value) > 20000) touchActive = false;
          break;
        case SDL_FINGERDOWN:
          touchActive = true;
          touch.onDown((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H);
          break;
        case SDL_FINGERMOTION: touch.onMove((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        case SDL_FINGERUP: touch.onUp((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        // The mouse works the launcher and the menus as a finger would (not the on-screen pad).
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button != SDL_BUTTON_LEFT) break;
          if (inLauncher || menu.open()) {
            mouseDown = true;
            touch.onDown(kMouseFinger, e.button.x * mx, e.button.y * my);
          }
          break;
        case SDL_MOUSEMOTION:
          if (e.motion.which == SDL_TOUCH_MOUSEID) break;
          if (mouseDown) touch.onMove(kMouseFinger, e.motion.x * mx, e.motion.y * my);
          if (cfg.displayMode != DisplayMode::Windowed) SDL_ShowCursor(SDL_ENABLE);
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button != SDL_BUTTON_LEFT || !mouseDown) break;
          mouseDown = false;
          touch.onUp(kMouseFinger, e.button.x * mx, e.button.y * my);
          break;
        case SDL_MOUSEWHEEL:
          if (menu.open() && e.wheel.y) menu.drag(e.wheel.y * H * 0.06f);
          break;
        case SDL_DROPFILE:
          if (e.drop.file) {
            std::string f = e.drop.file;
            SDL_free(e.drop.file);
            if (adoptRom(*this, f))
              menu.notify("ROM loaded", 3.0f);
            else if (!snes)
              menu.notify("That isn't the Super Mario All-Stars ROM - see the game card", 4.0f);
          }
          break;
        default: break;
      }
    }
    if (!running) break;
    pollPick();
    if (cfg.displayMode != DisplayMode::Windowed && !inLauncher && !menu.open()) SDL_ShowCursor(SDL_DISABLE);
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - last) / freq;
    last = now;
    intervals[nIntervals++ % 48] = (float)std::max(0.0, dt - slept);
    slept = 0;
    double hz = 60.0;
    bool swapWaits = true;
    if (nIntervals >= 48) {
      float d[48];
      std::copy(intervals, intervals + 48, d);
      std::nth_element(d, d + 24, d + 48);
      if (d[24] > 1.0f / 400) hz = 1.0 / d[24];
      // (passes much quicker than any display: swapping doesn't wait, or the window is hidden)
      swapWaits = d[24] > 1.0f / 400 && hz < 400;
    } else {
      SDL_DisplayMode m;
      if (SDL_GetWindowDisplayMode(window, &m) == 0 && m.refresh_rate > 0) hz = m.refresh_rate;
    }
    int frames = 0;
    if (!swapWaits) {
      if (pacing.mode != kPaceTimer) {
        pacing = Pacing();
        pacing.mode = kPaceTimer;
        timerDeadline = (double)now / freq;
        logf("pacing: swapping doesn't wait for the display, timed at the game's rate");
      }
    } else if (pacing.mode != kPaceVsync || lockedRefreshes(hz) != pacing.perFrame) {
      pacing = Pacing();
      pacing.mode = kPaceVsync;
      pacing.perFrame = lockedRefreshes(hz);
      logf("pacing: %.2f Hz display, %d refreshes per frame (0 = uneven)", hz, pacing.perFrame);
    }
    pacing.hz = hz;
    if (pacing.mode == kPaceTimer) {
      // a frame each period, waiting for its moment
      const double t0 = (double)SDL_GetPerformanceCounter() / freq;
      double t = t0;
      if (timerDeadline - t > 0.002) SDL_Delay((Uint32)((timerDeadline - t) * 1000 - 1));
      while ((double)SDL_GetPerformanceCounter() / freq < timerDeadline) {
      }
      t = (double)SDL_GetPerformanceCounter() / freq;
      slept = t - t0;
      while (timerDeadline <= t && frames < 3) {
        timerDeadline += period;
        frames++;
      }
      if (t - timerDeadline > 0.25) timerDeadline = t + period;  // (stalled: no catching up)
    } else {
      int vb = std::max(1, (int)std::lround(dt * hz));  // refreshes since the last pass
      if (vb > 6) {  // stalled: resume without fast-forwarding
        vb = 0;
        pacing.vblanks = 0;
        pacing.accum = 0;
        frames = 1;
      }
      if (pacing.perFrame) {
        pacing.vblanks += vb;
        while (pacing.vblanks >= pacing.perFrame) {
          pacing.vblanks -= pacing.perFrame;
          frames++;
        }
      } else {
        pacing.accum += vb / hz;
        while (pacing.accum >= period) {
          pacing.accum -= period;
          frames++;
        }
      }
    }
    frames = std::min(frames, 3);
    frame(dt, frames);
    render();
    if (!g_capturePath.empty() && (double)(SDL_GetPerformanceCounter() - started) / freq >= g_captureAfter) {
      std::vector<uint32_t> px;
      int w = 0, h = 0;
      bool ok = video.capture(px, &w, &h) && writePng(g_capturePath, px.data(), w, h, w);
      logf("capture %s: %s", g_capturePath.c_str(), ok ? "written" : "failed");
      running = false;
    }
    video.present();
  }
}

int main(int argc, char** argv) {
  std::unique_ptr<App> app = std::make_unique<App>();
  g_app = app.get();
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) app->startGame = widenUtf8(argv[++i]);
    else if (a == "--capture" && i + 2 < argc) {
      g_capturePath = argv[++i];
      g_captureAfter = atof(argv[++i]);
    } else if (a == "--help" || a == "-h") {
      printf("Usage: %s [--game smb1|smbll|smb2|smb3|allstars] [--capture file.png seconds]\n", argv[0]);
      return 0;
    }
  }
  int rc = 0;
  if (!app->init())
    rc = 1;
  else
    app->run();
  app->shutdown();
  g_app = nullptr;
  return rc;
}
