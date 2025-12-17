// main.cpp — Conway (SDL2) as a Windows Screen Saver (.scr)
//
// Screen saver args (Windows):
//   /s              run full screen
//   /p <HWND>       preview inside the provided window handle
//   /c              config dialog (shows a simple message)
//   (no args)       config dialog (common when double-clicking)
//
// Interaction (as requested):
//   - LEFT mouse: paint/spawn live cells
//   - RIGHT mouse: erase cells
//   - ESC: exit (ONLY key that exits)
//
// Build on Windows: compile as a GUI app (Subsystem: Windows), then rename output to .scr.

#include <SDL2/SDL.h>
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
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#ifdef _WIN32
  #ifdef min
    #undef min
  #endif
  #ifdef max
    #undef max
  #endif
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
                if (!alive) nxt[i] = 1; // newborn
                else        nxt[i] = (age < cap) ? (uint8_t)(age + 1) : cap;
            }
        }
    }
}

static void randomize(std::vector<uint8_t>& g, double density, std::mt19937& rng) {
    std::bernoulli_distribution d(std::clamp(density, 0.0, 1.0));
    for (auto& cell : g) cell = d(rng) ? 1 : 0;
}

static void clearGrid(std::vector<uint8_t>& g) { std::fill(g.begin(), g.end(), 0); }

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

// ---------------- Windows screen saver argument handling ----------------

enum class SaverMode { Config, Run, Preview };

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

struct SaverArgs {
    SaverMode mode = SaverMode::Config; // no-args default
    uintptr_t preview_parent_hwnd = 0;
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
            "  /s  Fullscreen\n"
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

    const bool isPreview = (sargs.mode == SaverMode::Preview);

    SDL_Window* window = nullptr;

#ifdef _WIN32
    HWND previewParent = (HWND)sargs.preview_parent_hwnd;
    HWND previewChild = nullptr;
#endif

    if (isPreview) {
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
    } else {
        window = SDL_CreateWindow(
            "Conway Screen Saver (SDL2)",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1280, 720,
            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP
        );
    }

    if (!window) {
        std::cerr << "SDL window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!isPreview) {
        SDL_ShowCursor(SDL_ENABLE); // keep it usable for painting
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
                // As requested: ONLY ESC exits.
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
        if (isPreview) {
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
