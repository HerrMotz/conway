// main.cpp — Conway (SDL2) as a Windows Screen Saver (.scr) spanning ALL monitors
//
// Screen saver args (Windows):
//   /s              run full screen (spans all monitors via virtual desktop window)
//   /p <HWND>       preview inside the provided window handle
//   /w [WxH]        windowed preview (not fullscreen)
//   /c              config dialog (shows a simple message)
//   (no args)       config dialog
//
// Interaction:
//   - LEFT mouse: paint/spawn live cells
//   - RIGHT mouse: erase cells
//   - ESC: exit (ONLY key that exits)
//
// Build on Windows: compile as a GUI app (Subsystem: Windows), then rename output to .scr.

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

struct Config {
    int cell_px = 16;
    int ms_per_step = 1000;
    double density = 0.18;
    bool wrap = true;
    int max_age = 30; // 1..255
};

static int mod(int a, int m) { int r = a % m; return (r < 0) ? r + m : r; }
static inline int idx(int x, int y, int w) { return y * w + x; }

// --- Colour helpers (HSV -> RGB) ---
static SDL_Color hsvToRgb(float h_deg, float s, float v) {
    h_deg = std::fmod(h_deg, 360.0f);
    if (h_deg < 0) h_deg += 360.0f;

    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h_deg / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float r1 = 0, g1 = 0, b1 = 0;
    if      (h_deg < 60)  { r1 = c; g1 = x; b1 = 0; }
    else if (h_deg < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h_deg < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h_deg < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h_deg < 300) { r1 = x; g1 = 0; b1 = c; }
    else                  { r1 = c; g1 = 0; b1 = x; }

    auto to8 = [](float f) -> uint8_t {
        int vv = (int)std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f);
        return (uint8_t)vv;
    };

    SDL_Color out;
    out.r = to8(r1 + m);
    out.g = to8(g1 + m);
    out.b = to8(b1 + m);
    out.a = 255;
    return out;
}

static SDL_Color colorForAge(uint8_t age, int max_age) {
    if (age == 0) return SDL_Color{0, 0, 0, 255};

    int ma = std::max(1, max_age);
    float t = (ma == 1) ? 0.0f : (float)(std::min<int>(age, ma) - 1) / (float)(ma - 1); // 0..1

    float hue = 200.0f * (1.0f - t); // 200 -> 0
    float sat = 1.0f;
    float val = 1.0f - 0.65f * t;    // 1.0 -> 0.35

    return hsvToRgb(hue, sat, val);
}

static int countNeighbors(const std::vector<uint8_t>& g, int x, int y, int w, int h, bool wrap) {
    int c = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (wrap) {
                nx = mod(nx, w);
                ny = mod(ny, h);
                c += g[idx(nx, ny, w)] ? 1 : 0;
            } else {
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                c += g[idx(nx, ny, w)] ? 1 : 0;
            }
        }
    }
    return c;
}

static void stepLife(const std::vector<uint8_t>& cur, std::vector<uint8_t>& nxt,
                     int w, int h, bool wrap, int max_age) {
    uint8_t cap = (uint8_t)std::clamp(max_age, 1, 255);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = idx(x, y, w);
            int n = countNeighbors(cur, x, y, w, h, wrap);

            uint8_t age = cur[i];
            bool alive = (age != 0);

            bool nextAlive = alive ? (n == 2 || n == 3) : (n == 3);

            if (!nextAlive) {
                nxt[i] = 0;
            } else {
                if (!alive) nxt[i] = 1;
                else        nxt[i] = (age < cap) ? (uint8_t)(age + 1) : cap;
            }
        }
    }
}

static void randomize(std::vector<uint8_t>& g, double density, std::mt19937& rng) {
    std::bernoulli_distribution d(std::clamp(density, 0.0, 1.0));
    for (auto& cell : g) cell = d(rng) ? 1 : 0;
}

static void setCell(std::vector<uint8_t>& g, int w, int h, int gx, int gy, bool alive) {
    if (gx < 0 || gx >= w || gy < 0 || gy >= h) return;
    g[idx(gx, gy, w)] = alive ? 1 : 0;
}

static void resizeGridToWindow(SDL_Window* win, const Config& cfg,
                               int& grid_w, int& grid_h,
                               std::vector<uint8_t>& cur, std::vector<uint8_t>& nxt) {
    int win_w_px = 0, win_h_px = 0;
    SDL_GetWindowSize(win, &win_w_px, &win_h_px);

    int cell = std::max(1, cfg.cell_px);
    int new_w = std::max(1, win_w_px / cell);
    int new_h = std::max(1, win_h_px / cell);

    if (new_w == grid_w && new_h == grid_h && (int)cur.size() == grid_w * grid_h) return;

    std::vector<uint8_t> new_cur(new_w * new_h, 0);
    std::vector<uint8_t> new_nxt(new_w * new_h, 0);

    bool old_ok = (grid_w > 0 && grid_h > 0 && (int)cur.size() == grid_w * grid_h);
    if (old_ok) {
        int copy_w = std::min(grid_w, new_w);
        int copy_h = std::min(grid_h, new_h);
        for (int y = 0; y < copy_h; ++y) {
            for (int x = 0; x < copy_w; ++x) {
                new_cur[idx(x, y, new_w)] = cur[idx(x, y, grid_w)];
            }
        }
    }

    grid_w = new_w;
    grid_h = new_h;
    cur.swap(new_cur);
    nxt.swap(new_nxt);
}

// ---- Virtual desktop bounds (span all monitors) ----
static SDL_Rect getVirtualDesktopBoundsFallback() {
    // Safe fallback if display queries fail
    SDL_Rect r;
    r.x = SDL_WINDOWPOS_UNDEFINED;
    r.y = SDL_WINDOWPOS_UNDEFINED;
    r.w = 1280;
    r.h = 720;
    return r;
}

static SDL_Rect getVirtualDesktopBounds() {
    int n = SDL_GetNumVideoDisplays();
    if (n <= 0) return getVirtualDesktopBoundsFallback();

    SDL_Rect b{};
    if (SDL_GetDisplayBounds(0, &b) != 0) return getVirtualDesktopBoundsFallback();

    int minX = b.x;
    int minY = b.y;
    int maxX = b.x + b.w;
    int maxY = b.y + b.h;

    for (int i = 1; i < n; ++i) {
        SDL_Rect bi{};
        if (SDL_GetDisplayBounds(i, &bi) != 0) continue;
        minX = std::min(minX, bi.x);
        minY = std::min(minY, bi.y);
        maxX = std::max(maxX, bi.x + bi.w);
        maxY = std::max(maxY, bi.y + bi.h);
    }

    SDL_Rect r{};
    r.x = minX;
    r.y = minY;
    r.w = std::max(1, maxX - minX);
    r.h = std::max(1, maxY - minY);
    return r;
}

// ---------------- Windows screen saver argument handling ----------------

enum class SaverMode { Config, Run, Preview, WindowedPreview };

static std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
}

static uintptr_t parseHandle(const std::string& s) {
    try {
        size_t pos = 0;
        int base = 10;
        if (startsWith(lower(s), "0x")) base = 16;
        unsigned long long v = std::stoull(s, &pos, base);
        (void)pos;
        return (uintptr_t)v;
    } catch (...) {
        return 0;
    }
}
static bool parseInt(const std::string& s, int& out) {
    try {
        size_t pos = 0;
        long v = std::stol(s, &pos, 10);
        if (pos == 0) return false;
        // Keep it sane: avoid huge windows from accidental inputs.
        if (v < 1 || v > 16384) return false;
        out = (int)v;
        return true;
    } catch (...) {
        return false;
    }
}

static void parseWxH(const std::string& s, int& w, int& h) {
    // Accepts "800x600"
    auto x = s.find('x');
    if (x == std::string::npos) return;
    int tw = 0, th = 0;
    if (parseInt(s.substr(0, x), tw) && parseInt(s.substr(x + 1), th)) {
        w = tw;
        h = th;
    }
}

struct SaverArgs {
    SaverMode mode = SaverMode::Config;
    uintptr_t preview_parent_hwnd = 0;
    int window_w = 1280;
    int window_h = 720;
};

static SaverArgs parseSaverArgs(int argc, char** argv) {
    SaverArgs out;
    if (argc <= 1) { out.mode = SaverMode::Config; return out; }

    std::string a1 = lower(argv[1]);
    if (!a1.empty() && (a1[0] == '/' || a1[0] == '-')) a1 = a1.substr(1);

    if (startsWith(a1, "s")) { out.mode = SaverMode::Run; return out; }
    if (startsWith(a1, "c")) { out.mode = SaverMode::Config; return out; }

    if (startsWith(a1, "p")) {
        out.mode = SaverMode::Preview;
        auto colon = a1.find(':');
        if (colon != std::string::npos && colon + 1 < a1.size()) {
            out.preview_parent_hwnd = parseHandle(a1.substr(colon + 1));
        } else if (argc >= 3) {
            out.preview_parent_hwnd = parseHandle(argv[2]);
        }
        return out;
    }

    if (startsWith(a1, "w")) {
        out.mode = SaverMode::WindowedPreview;

        // Defaults can be overridden via:
        //   /w:800x600
        //   /w 800 600
        out.window_w = 1280;
        out.window_h = 720;

        auto colon = a1.find(':');
        if (colon != std::string::npos && colon + 1 < a1.size()) {
            parseWxH(a1.substr(colon + 1), out.window_w, out.window_h);
        } else if (argc >= 4) {
            int w = 0, h = 0;
            if (parseInt(argv[2], w)) out.window_w = w;
            if (parseInt(argv[3], h)) out.window_h = h;
        }
        return out;
    }

    out.mode = SaverMode::Run;
    return out;
}

#ifdef _WIN32
static HWND createPreviewChild(HWND parent) {
    RECT rc{};
    GetClientRect(parent, &rc);
    int w = (int)std::max<LONG>(1, rc.right - rc.left);
    int h = (int)std::max<LONG>(1, rc.bottom - rc.top);

    HWND child = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, w, h,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    return child;
}
#endif

#ifdef _WIN32
static void makeWindowTopmost(SDL_Window* win, const SDL_Rect& r) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(win, &info)) {
        HWND hwnd = info.info.win.window;
        SetWindowPos(hwnd, HWND_TOPMOST, r.x, r.y, r.w, r.h,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}
#endif

int main(int argc, char** argv) {
    Config cfg;
    SaverArgs sargs = parseSaverArgs(argc, argv);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    if (sargs.mode == SaverMode::Config) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION,
            "Conway Screen Saver",
            "This is a Conway (SDL2) screen saver.\n\n"
            "Run modes:\n"
            "  /s  Fullscreen across all monitors\n"
            "  /p <HWND> Preview\n"
            "  /c  Config (this dialog)\n\n"
            "Controls:\n"
            "  Left mouse  = paint live cells\n"
            "  Right mouse = erase cells\n"
            "  ESC         = exit\n",
            nullptr
        );
        SDL_Quit();
        return 0;
    }

    const bool isEmbeddedPreview = (sargs.mode == SaverMode::Preview);
    const bool isWindowedPreview = (sargs.mode == SaverMode::WindowedPreview);
    const bool isFullRun = (sargs.mode == SaverMode::Run);

    SDL_Window* window = nullptr;
    SDL_Rect virtualBounds = getVirtualDesktopBounds();

#ifdef _WIN32
    HWND previewParent = (HWND)sargs.preview_parent_hwnd;
    HWND previewChild = nullptr;
#endif

    if (isEmbeddedPreview) {
#ifdef _WIN32
        if (!previewParent || !IsWindow(previewParent)) {
            SDL_Quit();
            return 0;
        }
        previewChild = createPreviewChild(previewParent);
        if (!previewChild) {
            SDL_Quit();
            return 0;
        }
        window = SDL_CreateWindowFrom((void*)previewChild);
#else
        SDL_Quit();
        return 0;
#endif
    } else if (isWindowedPreview) {
        window = SDL_CreateWindow(
            "Conway Screen Saver (SDL2) - Preview",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            sargs.window_w, sargs.window_h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
        );
    } else {
        // One borderless window that spans the entire virtual desktop (all monitors).
        window = SDL_CreateWindow(
            "Conway Screen Saver (SDL2)",
            virtualBounds.x, virtualBounds.y,
            virtualBounds.w, virtualBounds.h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS
        );
    }

    if (!window) {
        std::cerr << "SDL window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    if (isFullRun) {
        // Make sure it stays above the taskbar / other windows.
#ifdef _WIN32
        makeWindowTopmost(window, virtualBounds);
#endif
    }

    if (isFullRun || isWindowedPreview) {
        SDL_RaiseWindow(window);
    }

    SDL_Renderer* ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());

    int grid_w = 0, grid_h = 0;
    std::vector<uint8_t> cur, nxt;

    resizeGridToWindow(window, cfg, grid_w, grid_h, cur, nxt);
    randomize(cur, cfg.density, rng);

    bool running = true;
    bool mouse_left = false, mouse_right = false;

    auto last_step = std::chrono::steady_clock::now();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    resizeGridToWindow(window, cfg, grid_w, grid_h, cur, nxt);
                }
            }

            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) running = false;
            }

            if (e.type == SDL_MOUSEBUTTONDOWN) {
                if (e.button.button == SDL_BUTTON_LEFT)  mouse_left = true;
                if (e.button.button == SDL_BUTTON_RIGHT) mouse_right = true;
            }
            if (e.type == SDL_MOUSEBUTTONUP) {
                if (e.button.button == SDL_BUTTON_LEFT)  mouse_left = false;
                if (e.button.button == SDL_BUTTON_RIGHT) mouse_right = false;
            }

            if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                int cell = std::max(1, cfg.cell_px);
                int gx = mx / cell;
                int gy = my / cell;
                if (mouse_left)  setCell(cur, grid_w, grid_h, gx, gy, true);
                if (mouse_right) setCell(cur, grid_w, grid_h, gx, gy, false);
            }
        }

#ifdef _WIN32
        if (isEmbeddedPreview) {
            if (!previewParent || !IsWindow(previewParent)) {
                running = false;
            } else if (previewChild && IsWindow(previewChild)) {
                RECT rc{};
                GetClientRect(previewParent, &rc);
                int w = (int)std::max<LONG>(1, rc.right - rc.left);
                int h = (int)std::max<LONG>(1, rc.bottom - rc.top);
                MoveWindow(previewChild, 0, 0, w, h, TRUE);
            }
        }
#endif

        auto now = std::chrono::steady_clock::now();
        bool time_to_step = (cfg.ms_per_step == 0)
            ? true
            : (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_step).count() >= cfg.ms_per_step);

        if (time_to_step) {
            stepLife(cur, nxt, grid_w, grid_h, cfg.wrap, cfg.max_age);
            cur.swap(nxt);
            last_step = now;
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        SDL_Rect r{0, 0, cfg.cell_px, cfg.cell_px};
        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                uint8_t age = cur[idx(x, y, grid_w)];
                if (!age) continue;

                SDL_Color c = colorForAge(age, cfg.max_age);
                SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);

                r.x = x * cfg.cell_px;
                r.y = y * cfg.cell_px;
                SDL_RenderFillRect(ren, &r);
            }
        }

        SDL_RenderPresent(ren);
        SDL_Delay(1);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
