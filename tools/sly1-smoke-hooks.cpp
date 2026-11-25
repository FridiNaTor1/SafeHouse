// Sly 1's smoke hooks (cmake/sly1.cmake, Linux): a hidden window, screenshots, scripted keys, mouse and a virtual pad
// and a timed exit for the game from the Sly1/ submodule, without patching it. The link wraps GLFW calls
// (-Wl,--wrap=...); with SAFEHOUSE_SLY1_SMOKE unset every wrapper is a plain call through.
//
//   SAFEHOUSE_SLY1_SMOKE="out=DIR;every=N;frames=N;keys=KEY@FIRST-LAST,...;shots=F,F,...;mouse=...;pad=FAMILY;..."
//
//   out      where the screenshots go: DIR/frame-NNNNNN.png (the back buffer just before each Nth swap), and DIR/log.txt
//   every    a screenshot every N swaps (default 300; 0: none)
//   shots    screenshots at these swaps too
//   frames   exit (code 0) after N swaps (default: never)
//   clock    "game": every, frames, shots and every schedule's FIRST / LAST are milliseconds of the game's clock instead
//            of swaps (the same run at every frame-rate cap; the game's safehouse/smoke.cpp ShSmokeGameTime). Each
//            screenshot's log line then carries the game time, and a "stat" line each second of wall time gives the
//            frames drawn and the game time gone by. Every screenshot's line carries the player's position and the scene
//            (the game's ShSmokeScene: "world=<level> ctx=<any|turret|van|tank|puffer|jetpack|rhythm|vault>").
//            The game may hold keys of its own too (its ShSmokeAutoKey: the debug menu's rhythmauto=1 test step).
//   size     the window's size WxH (default the game's own, 1280x720): e.g. 3840x1080 for 32:9
//   keys     GLFW key codes held from swap FIRST to swap LAST (257 = Enter, the PS2 Start; 75 = K)
//   mouse    GLFW mouse buttons held: BUTTON@FIRST-LAST,... (0 left, 1 right)
//   move     the cursor moved DX:DY window units every swap FIRST..LAST: DX:DY@FIRST-LAST,...
//   cursor   the cursor placed at X:Y (window units) for swaps FIRST..LAST: X:Y@FIRST-LAST,...
//   wheel    a wheel turn of N notches (+ up) at a swap: N@FRAME,...
//   pad      a virtual controller in joystick slot 1 (the only one the game sees): xbox, ps3, ps4, ps5, switch, deck,
//            steamcontroller (the family's USB IDs in an SDL-style GUID, so SafeHouse's controller layer identifies it)
//   padbtn   its GLFW gamepad buttons held (0 A .. 14 d-pad left; 1000 / 1001 the left / right trigger): B@FIRST-LAST,...
//   padaxis  its axes held at a value (percent, -100..100; 0 LX, 1 LY, 2 RX, 3 RY): A:V@FIRST-LAST,...
//
// The window is created hidden (GLFW_VISIBLE false) and reported focused, so the game takes the scripted input; the
// real keyboard, mouse and controllers never reach a smoke run, and the cursor mode is only recorded (a hidden test
// window never grabs the desktop's pointer). Used by tools/sly1-smoke.sh; never by the launcher.
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <stb/stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" {
// (the game's, safehouse/smoke.cpp; absent from a build without it)
double ShSmokeGameTime() __attribute__((weak));
int ShSmokePlayerPos(float* xyz) __attribute__((weak));
const char* ShSmokeScene() __attribute__((weak));
int ShSmokeAutoKey(int key) __attribute__((weak));
GLFWwindow* __real_glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*);
void __real_glfwSwapBuffers(GLFWwindow*);
int __real_glfwGetKey(GLFWwindow*, int);
int __real_glfwGetWindowAttrib(GLFWwindow*, int);
int __real_glfwGetMouseButton(GLFWwindow*, int);
void __real_glfwGetCursorPos(GLFWwindow*, double*, double*);
void __real_glfwSetInputMode(GLFWwindow*, int, int);
int __real_glfwGetInputMode(GLFWwindow*, int);
GLFWscrollfun __real_glfwSetScrollCallback(GLFWwindow*, GLFWscrollfun);
int __real_glfwJoystickPresent(int);
int __real_glfwJoystickIsGamepad(int);
int __real_glfwGetGamepadState(int, GLFWgamepadstate*);
const char* __real_glfwGetJoystickGUID(int);
const char* __real_glfwGetJoystickName(int);
const char* __real_glfwGetGamepadName(int);
const float* __real_glfwGetJoystickAxes(int, int*);
const unsigned char* __real_glfwGetJoystickButtons(int, int*);
const unsigned char* __real_glfwGetJoystickHats(int, int*);
}

namespace
{
struct Range { int a, b, first, last; };   // (a, b: the key / button / axis and its value)
struct Pad { const char* id; const char* guid; const char* name; };
constexpr Pad kPads[] = {
    { "xbox", "030000005e0400008e02000010010000", "Microsoft X-Box 360 pad" },
    { "ps3", "030000004c0500006802000011810000", "Sony PLAYSTATION(R)3 Controller" },
    { "ps4", "030000004c050000cc09000011810000", "Sony Interactive Entertainment Wireless Controller" },
    { "ps5", "030000004c050000e60c000011810000", "Sony Interactive Entertainment DualSense Wireless Controller" },
    { "switch", "030000007e0500000920000011810000", "Nintendo Switch Pro Controller" },
    { "deck", "03000000de2800000512000011010000", "Steam Deck" },
    { "steamcontroller", "03000000de2800000211000011010000", "Valve Software Steam Controller" },
};
struct Smoke
{
    bool on = false;
    std::string out;
    long every = 300, frames = -1, frame = 0;
    std::vector<long> shots;
    std::vector<Range> keys, mouse, move, cursor, wheel, padbtn, padaxis;
    const Pad* pad = nullptr;
    int width = 0, height = 0;
    double x = 320, y = 240;
    int cursorMode = GLFW_CURSOR_NORMAL;
    GLFWscrollfun scroll = nullptr;
    FILE* log = nullptr;
    bool gameClock = false;
    long nextShot = 0;
    // (clock=game: the stats)
    std::chrono::steady_clock::time_point statStart{};
    long statFrames = 0;
    double statGame = 0.0;
    // the schedule's time: swaps, or milliseconds of game time
    long now() const { return gameClock ? (ShSmokeGameTime ? long(ShSmokeGameTime() * 1000.0) : 0) : frame; }
};

// "A@F-L,..." / "A:B@F-L,..." / "A@F" (a single swap) lists
std::vector<Range> ParseRanges(const std::string& value)
{
    std::vector<Range> ranges;
    size_t p = 0;
    while (p < value.size()) {
        size_t comma = value.find(',', p);
        if (comma == std::string::npos) comma = value.size();
        const std::string item = value.substr(p, comma - p);
        Range r{};
        if (std::sscanf(item.c_str(), "%d:%d@%d-%d", &r.a, &r.b, &r.first, &r.last) == 4 ||
            (r.b = 0, std::sscanf(item.c_str(), "%d@%d-%d", &r.a, &r.first, &r.last) == 3))
            ranges.push_back(r);
        else if (std::sscanf(item.c_str(), "%d@%d", &r.a, &r.first) == 2)
            r.last = r.first, ranges.push_back(r);
        p = comma + 1;
    }
    return ranges;
}

Smoke& State()
{
    static Smoke smoke = [] {
        Smoke s;
        const char* spec = std::getenv("SAFEHOUSE_SLY1_SMOKE");
        if (!spec || !*spec) return s;
        s.on = true;
        std::string text = spec;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(';', start);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(start, end - start);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
                if (key == "out") s.out = value;
                else if (key == "every") s.every = std::atol(value.c_str());
                else if (key == "frames") s.frames = std::atol(value.c_str());
                else if (key == "clock") s.gameClock = value == "game";
                else if (key == "size") std::sscanf(value.c_str(), "%dx%d", &s.width, &s.height);
                else if (key == "keys") s.keys = ParseRanges(value);
                else if (key == "mouse") s.mouse = ParseRanges(value);
                else if (key == "move") s.move = ParseRanges(value);
                else if (key == "cursor") s.cursor = ParseRanges(value);
                else if (key == "wheel") s.wheel = ParseRanges(value);
                else if (key == "padbtn") s.padbtn = ParseRanges(value);
                else if (key == "padaxis") s.padaxis = ParseRanges(value);
                else if (key == "pad") {
                    for (const Pad& pad : kPads)
                        if (value == pad.id) s.pad = &pad;
                } else if (key == "shots") {
                    size_t p = 0;
                    while (p < value.size()) {
                        size_t comma = value.find(',', p);
                        if (comma == std::string::npos) comma = value.size();
                        s.shots.push_back(std::atol(value.substr(p, comma - p).c_str()));
                        p = comma + 1;
                    }
                }
            }
            start = end + 1;
        }
        if (!s.out.empty()) s.log = std::fopen((s.out + "/log.txt").c_str(), "w");
        s.nextShot = s.every;
        return s;
    }();
    return smoke;
}

bool Within(const Range& r, long now) { return now >= r.first && now <= r.last; }

void Shot(Smoke& s, GLFWwindow* window)
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    if (w <= 0 || h <= 0 || s.out.empty()) return;
    std::vector<unsigned char> pixels(size_t(w) * h * 3);
    GLint readFramebuffer = 0, readBuffer = 0, packAlignment = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // (RGBA: the Vulkan renderer's GL layer reads only that; packed to RGB here)
    std::vector<unsigned char> rgba(size_t(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i)
        pixels[i * 3] = rgba[i * 4], pixels[i * 3 + 1] = rgba[i * 4 + 1], pixels[i * 3 + 2] = rgba[i * 4 + 2];
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    glReadBuffer(readBuffer);
    char name[64];
    std::snprintf(name, sizeof name, s.gameClock ? "/time-%06ld.png" : "/frame-%06ld.png", s.gameClock ? s.now() : s.frame);
    stbi_flip_vertically_on_write(1);
    stbi_write_png((s.out + name).c_str(), w, h, 3, pixels.data(), w * 3);
    // (mean brightness: a black picture is easy to spot in the log)
    unsigned long long sum = 0;
    for (unsigned char c : pixels) sum += c;
    if (s.log) {
        std::fprintf(s.log, "shot %ld %dx%d mean=%.1f", s.frame, w, h, double(sum) / double(pixels.size()));
        float pos[3];
        if (s.gameClock) std::fprintf(s.log, " game=%.3f", ShSmokeGameTime ? ShSmokeGameTime() : -1.0);
        if (ShSmokePlayerPos && ShSmokePlayerPos(pos)) std::fprintf(s.log, " player=%.2f,%.2f,%.2f", pos[0], pos[1], pos[2]);
        if (ShSmokeScene) std::fprintf(s.log, " %s", ShSmokeScene());
        std::fprintf(s.log, "\n");
        std::fflush(s.log);
    }
}

bool FakePad(int jid) { return State().on && State().pad != nullptr && jid == GLFW_JOYSTICK_1; }
}

namespace
{
GLFWwindow* s_window = nullptr;   // (the last window made: Vulkan's present has none to hand over)

// Before each frame's present: the count, the screenshots, the timed exit, the next frame's mouse.
void BeforePresent(GLFWwindow* window)
{
    Smoke& s = State();
    if (s.on) {
        ++s.frame;
        const long now = s.now();
        bool shot = false;
        if (s.gameClock) {
            const auto wall = std::chrono::steady_clock::now();
            const double game = ShSmokeGameTime ? ShSmokeGameTime() : 0.0;
            if (s.statFrames == 0) s.statStart = wall, s.statGame = game;
            ++s.statFrames;
            const double seconds = std::chrono::duration<double>(wall - s.statStart).count();
            if (seconds >= 1.0 && s.log) {
                std::fprintf(s.log, "stat wall=%.3f frames=%ld fps=%.1f game=%.3f rate=%.3f\n", seconds, s.statFrames - 1,
                             double(s.statFrames - 1) / seconds, game - s.statGame, (game - s.statGame) / seconds);
                s.statFrames = 0;
            }
            if (s.every > 0 && now >= s.nextShot) {
                shot = true;
                while (s.nextShot <= now) s.nextShot += s.every;
            }
        } else shot = s.every > 0 && s.frame % s.every == 0;
        for (long f : s.shots) shot = shot || f == now;
        if (shot) Shot(s, window);
        if (s.frames >= 0 && now >= s.frames) {
            if (s.log) std::fprintf(s.log, "exit %ld\n", s.gameClock ? s.frames : s.frame), std::fclose(s.log);
            std::fflush(stdout);
            _exit(0);
        }
        // (the next swap's mouse: motion, placement, wheel)
        for (const Range& r : s.move)
            if (Within(r, now)) s.x += r.a, s.y += r.b;
        for (const Range& r : s.cursor)
            if (Within(r, now)) s.x = r.a, s.y = r.b;
        for (const Range& r : s.wheel)
            if (r.first == now && s.scroll) s.scroll(window, 0.0, double(r.a));
    }
}
}

extern "C" GLFWwindow* __wrap_glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share)
{
    Smoke& s = State();
    if (s.on) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    if (s.on && s.width > 0 && s.height > 0) width = s.width, height = s.height;
    s_window = __real_glfwCreateWindow(width, height, title, s.on ? nullptr : monitor, share);
    return s_window;
}

extern "C" void __wrap_glfwSwapBuffers(GLFWwindow* window)
{
    BeforePresent(window);
    __real_glfwSwapBuffers(window);
}

#if CANE_VULKAN
// (Vulkan: the frames are presented by CaneVk::Present, platform/vulkan/gl_vulkan.h; the screenshot reads the layer's
// window picture through the same GL calls)
extern "C" void __real__ZN6CaneVk7PresentEv();
extern "C" void __wrap__ZN6CaneVk7PresentEv()
{
    if (s_window) BeforePresent(s_window);
    __real__ZN6CaneVk7PresentEv();
}
#endif

extern "C" int __wrap_glfwGetKey(GLFWwindow* window, int key)
{
    Smoke& s = State();
    if (s.on) {
        for (const Range& r : s.keys)
            if (r.a == key && Within(r, s.now())) return GLFW_PRESS;
        if (ShSmokeAutoKey && ShSmokeAutoKey(key)) return GLFW_PRESS;   // (the game's automatic rhythm player)
        return GLFW_RELEASE;   // (the real keyboard never reaches a smoke run)
    }
    return __real_glfwGetKey(window, key);
}

extern "C" int __wrap_glfwGetWindowAttrib(GLFWwindow* window, int attrib)
{
    if (State().on && attrib == GLFW_FOCUSED) return GLFW_TRUE;
    return __real_glfwGetWindowAttrib(window, attrib);
}

extern "C" int __wrap_glfwGetMouseButton(GLFWwindow* window, int button)
{
    Smoke& s = State();
    if (s.on) {
        for (const Range& r : s.mouse)
            if (r.a == button && Within(r, s.now())) return GLFW_PRESS;
        return GLFW_RELEASE;
    }
    return __real_glfwGetMouseButton(window, button);
}

extern "C" void __wrap_glfwGetCursorPos(GLFWwindow* window, double* x, double* y)
{
    Smoke& s = State();
    if (s.on) {
        if (x) *x = s.x;
        if (y) *y = s.y;
        return;
    }
    __real_glfwGetCursorPos(window, x, y);
}

extern "C" void __wrap_glfwSetInputMode(GLFWwindow* window, int mode, int value)
{
    Smoke& s = State();
    if (s.on && (mode == GLFW_CURSOR || mode == GLFW_RAW_MOUSE_MOTION)) {
        if (mode == GLFW_CURSOR && value != s.cursorMode) {
            s.cursorMode = value;
            if (s.log) std::fprintf(s.log, "cursor %ld %s\n", s.frame, value == GLFW_CURSOR_DISABLED ? "captured" : "free");
        }
        return;
    }
    __real_glfwSetInputMode(window, mode, value);
}

extern "C" int __wrap_glfwGetInputMode(GLFWwindow* window, int mode)
{
    if (State().on && mode == GLFW_CURSOR) return State().cursorMode;
    return __real_glfwGetInputMode(window, mode);
}

extern "C" GLFWscrollfun __wrap_glfwSetScrollCallback(GLFWwindow* window, GLFWscrollfun callback)
{
    if (State().on) State().scroll = callback;
    return __real_glfwSetScrollCallback(window, callback);
}

// ---- the virtual controller (joystick slot 1); no other controller exists in a smoke run
extern "C" int __wrap_glfwJoystickPresent(int jid)
{
    if (State().on) return FakePad(jid) ? GLFW_TRUE : GLFW_FALSE;
    return __real_glfwJoystickPresent(jid);
}

extern "C" int __wrap_glfwJoystickIsGamepad(int jid)
{
    if (State().on) return FakePad(jid) ? GLFW_TRUE : GLFW_FALSE;
    return __real_glfwJoystickIsGamepad(jid);
}

extern "C" int __wrap_glfwGetGamepadState(int jid, GLFWgamepadstate* state)
{
    Smoke& s = State();
    if (!s.on) return __real_glfwGetGamepadState(jid, state);
    if (!FakePad(jid) || !state) return GLFW_FALSE;
    *state = {};
    state->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = state->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
    for (const Range& r : s.padbtn)
        if (Within(r, s.now())) {
            if (r.a >= 0 && r.a <= GLFW_GAMEPAD_BUTTON_LAST) state->buttons[r.a] = GLFW_PRESS;
            else if (r.a == 1000) state->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = 1.0f;
            else if (r.a == 1001) state->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = 1.0f;
        }
    for (const Range& r : s.padaxis)
        if (Within(r, s.now()) && r.a >= 0 && r.a <= GLFW_GAMEPAD_AXIS_LAST) state->axes[r.a] = float(r.b) / 100.0f;
    return GLFW_TRUE;
}

extern "C" const char* __wrap_glfwGetJoystickGUID(int jid)
{
    if (State().on) return FakePad(jid) ? State().pad->guid : nullptr;
    return __real_glfwGetJoystickGUID(jid);
}

extern "C" const char* __wrap_glfwGetJoystickName(int jid)
{
    if (State().on) return FakePad(jid) ? State().pad->name : nullptr;
    return __real_glfwGetJoystickName(jid);
}

extern "C" const char* __wrap_glfwGetGamepadName(int jid)
{
    if (State().on) return FakePad(jid) ? State().pad->name : nullptr;
    return __real_glfwGetGamepadName(jid);
}

extern "C" const float* __wrap_glfwGetJoystickAxes(int jid, int* count)
{
    if (State().on) {
        static const float axes[6] = { 0, 0, -1, 0, 0, -1 };
        if (count) *count = FakePad(jid) ? 6 : 0;
        return FakePad(jid) ? axes : nullptr;
    }
    return __real_glfwGetJoystickAxes(jid, count);
}

extern "C" const unsigned char* __wrap_glfwGetJoystickButtons(int jid, int* count)
{
    if (State().on) {
        static const unsigned char buttons[15] = {};
        if (count) *count = FakePad(jid) ? 15 : 0;
        return FakePad(jid) ? buttons : nullptr;
    }
    return __real_glfwGetJoystickButtons(jid, count);
}

extern "C" const unsigned char* __wrap_glfwGetJoystickHats(int jid, int* count)
{
    if (State().on) {
        static const unsigned char hats[1] = {};
        if (count) *count = FakePad(jid) ? 1 : 0;
        return FakePad(jid) ? hats : nullptr;
    }
    return __real_glfwGetJoystickHats(jid, count);
}
