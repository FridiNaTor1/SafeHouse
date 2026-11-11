// The GLFW API on SDL2 for the iOS app (cmake/ios.cmake): the launcher, the extractor's host and Sly 2 were written
// for GLFW; iOS windows, input and controllers come from SDL's UIKit backend. Each program framework links its own
// copy (its windows and callbacks are its own); SDL itself is shared (SafeHouseCore). Adapted from the Android shim
// (platform/android/glfw_sdl.cpp) without its EGL / Vulkan / activity parts.
//
// - The launcher's window: OpenGL ES 3.0 (SDL's EAGL view), drawn in logical units of at least 1280x720 like on
//   Android (the launcher's layout needs that much room); glBindFramebuffer(.., 0) means SDL's view framebuffer.
// - The game's window (GLFW_CLIENT_API = GLFW_NO_API): a Metal view; platform/metal/context.mm takes its layer
//   (cane_glfw_metal_layer) and draws in pixels. The touch pad overlays it (ios_app.mm attachTouchOverlay).
#include <GLFW/glfw3.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_metal.h>
#include <OpenGLES/ES3/gl.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "ios_app.h"
#include "../controller_family.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

// (program_entry.cpp) "launcher", "game", "extractor" or "tool": the program this copy of the shim belongs to.
extern "C" const char* cane_ios_program_role(void);

struct GLFWwindow {
    SDL_Window* native = nullptr;
    SDL_GLContext context = nullptr;
    SDL_MetalView metalView = nullptr;
    GLuint viewFramebuffer = 0;  // (SDL's UIKit GL view draws into its own framebuffer object, not 0)
    bool metal = false;          // (made with GLFW_NO_API: the game's Metal window)
    float uiScale = 1;           // logical units per pixel (the launcher: >= 1280x720 logical)
    int width = 0, height = 0;   // logical size
    bool close = false, focused = true, visible = true;
    int cursorMode = GLFW_CURSOR_NORMAL;
    std::array<unsigned char, GLFW_KEY_LAST + 1> keys{};
    std::array<unsigned char, 8> mouse{};
    double x = 0, y = 0;
    void* user = nullptr;
    GLFWframebuffersizefun framebuffer = nullptr;
    GLFWwindowsizefun size = nullptr;
    GLFWwindowposfun position = nullptr;
    GLFWwindowfocusfun focus = nullptr;
    GLFWcursorposfun cursor = nullptr;
    GLFWcursorenterfun enter = nullptr;
    GLFWmousebuttonfun button = nullptr;
    GLFWscrollfun scroll = nullptr;
    GLFWkeyfun key = nullptr;
    GLFWcharfun character = nullptr;
    GLFWdropfun drop = nullptr;
};
struct GLFWmonitor {};
struct GLFWcursor { int shape = 0; };
namespace {
GLFWwindow* current = nullptr;
GLFWwindow* mainWindow = nullptr;  // (the one window a program has; events go to it)
GLFWmonitor monitor;
GLFWmonitor* monitors[] = {&monitor};
GLFWerrorfun errorCallback = nullptr;
GLFWjoystickfun joystickCallback = nullptr;
GLFWmonitorfun monitorCallback = nullptr;
std::string lastError, clipboard;
int lastCode = 0;
bool visibleHint = true;
bool noApiHint = false;
bool appForeground = true;
bool touchControlsVisible = true, touchControlsKnown = false;
int swaps = 0;
double timeOffset = 0;
auto origin = std::chrono::steady_clock::now();
struct Ime {
    ImGuiContext* context = nullptr;
    ImGuiID owner = 0;
    SDL_Rect rect{};
} ime;
struct TouchDrag {
    SDL_FingerID finger = 0;
    bool active = false, blocked = false, scrolling = false;
    ImVec2 start{}, position{}, delta{};
    ImGuiWindow* window = nullptr;
} touchDrag;
struct Pad {
    SDL_GameController* controller = nullptr;
    std::string guid;
};
std::array<Pad, GLFW_JOYSTICK_LAST + 1> pads;

bool launcherRole() { return std::strcmp(cane_ios_program_role(), "launcher") == 0; }
void error(int code, const char* detail) {
    lastCode = code;
    lastError = detail ? detail : "iOS display error";
    if (errorCallback) errorCallback(code, lastError.c_str());
}
void drawableSize(GLFWwindow* w, int* width, int* height) {
    int a = 0, b = 0;
    if (w && w->native) {
        if (w->metal) SDL_Metal_GetDrawableSize(w->native, &a, &b);
        else SDL_GL_GetDrawableSize(w->native, &a, &b);
    }
    if (width) *width = a;
    if (height) *height = b;
}
// The logical size: pixels for the game, >= 1280x720 units for the launcher (as Android's density scaling).
void updateScale(GLFWwindow* w) {
    if (!w || !w->native) return;
    int pw = 0, ph = 0;
    drawableSize(w, &pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    if (launcherRole()) {
        int points = 0, unused = 0;
        SDL_GetWindowSize(w->native, &points, &unused);
        const float density = points > 0 ? float(pw) / float(points) : 1.f;  // pixels per point (2 or 3)
        const float fit = std::max(1.f, std::min(pw / 1280.f, ph / 720.f));
        w->uiScale = std::clamp(density, 1.f, fit);
    } else {
        w->uiScale = 1;
    }
    w->width = std::max(1, int(std::round(pw / w->uiScale)));
    w->height = std::max(1, int(std::round(ph / w->uiScale)));
}
// SDL's window coordinates (points) -> logical units
ImVec2 logicalPosition(GLFWwindow* w, float x, float y) {
    int points = 0, pointsH = 0;
    SDL_GetWindowSize(w->native, &points, &pointsH);
    if (points <= 0 || pointsH <= 0) return {x, y};
    return {x * float(w->width) / float(points), y * float(w->height) / float(pointsH)};
}
void useTouchControls(bool visible) {
    if (touchControlsKnown && touchControlsVisible == visible) return;
    touchControlsKnown = true;
    touchControlsVisible = visible;
    cane::ios::setTouchControlsVisible(visible);
}

// ---- ImGui text fields (the launcher): the on-screen keyboard follows the focused field
void applyIme(ImGuiViewport* viewport, const ImGuiPlatformImeData& data) {
    if (!current || !appForeground) return;
    const ImGuiID owner = ImGui::GetActiveID();
    if (!data.WantVisible || owner == 0) {
        if (SDL_IsTextInputActive()) SDL_StopTextInput();
        ime.owner = 0;
        return;
    }
    const ImVec2 offset = viewport ? viewport->Pos : ImVec2(0, 0);
    int points = 0, pointsH = 0;
    SDL_GetWindowSize(current->native, &points, &pointsH);
    const float sx = current->width > 0 ? float(points) / current->width : 1, sy = current->height > 0 ? float(pointsH) / current->height : 1;
    SDL_Rect rect{int((data.InputPos.x - offset.x) * sx), int((data.InputPos.y - offset.y) * sy), 1,
                  std::max(1, int(std::ceil(data.InputLineHeight * sy)))};
    const bool moved = rect.x != ime.rect.x || rect.y != ime.rect.y || rect.h != ime.rect.h;
    if (moved || owner != ime.owner) SDL_SetTextInputRect(&rect);
    if (!SDL_IsTextInputActive()) SDL_StartTextInput();
    ime.rect = rect;
    ime.owner = owner;
}
void platformIme(ImGuiViewport* viewport, ImGuiPlatformImeData* data) { applyIme(viewport, *data); }
// One finger on a scrollable window's background scrolls it (as on Android).
void scrollTouchFrame(ImGuiContext* context) {
    auto& drag = touchDrag;
    const bool widget = context->ActiveId && (!context->ActiveIdWindow || context->ActiveId != context->ActiveIdWindow->MoveId);
    if (widget || drag.blocked) {
        drag.delta = {};
        return;
    }
    if (drag.active && !drag.window) {
        auto* window = context->HoveredWindow;
        while (window && (window->ScrollMax.y <= 0 || (window->Flags & ImGuiWindowFlags_NoScrollWithMouse))) window = window->ParentWindow;
        if (window && window->InnerRect.Contains(drag.position)) drag.window = window;
    }
    if (drag.window) {
        const ImVec2 distance = drag.position - drag.start;
        if (std::abs(distance.y) > 6 && std::abs(distance.y) >= std::abs(distance.x)) drag.scrolling = true;
        if (drag.scrolling && drag.delta.y != 0)
            ImGui::SetScrollY(drag.window, std::clamp(drag.window->Scroll.y - drag.delta.y, 0.f, drag.window->ScrollMax.y));
    }
    drag.delta = {};
    if (!drag.active) drag = {};
}
void imeFrameHook(ImGuiContext* context, ImGuiContextHook* hook) {
    if (hook->Type == ImGuiContextHookType_Shutdown) {
        if (SDL_IsTextInputActive()) SDL_StopTextInput();
        ime = {};
        touchDrag = {};
        return;
    }
    applyIme(ImGui::GetMainViewport(), context->PlatformImeData);
    scrollTouchFrame(context);
}
void installIme() {
    auto* context = ImGui::GetCurrentContext();
    if (!context || context == ime.context || !current || !launcherRole()) return;
    ime = {};
    ime.context = context;
    ImGui::GetIO().SetPlatformImeDataFn = platformIme;
    ImGuiContextHook hook{};
    hook.Callback = imeFrameHook;
    hook.Type = ImGuiContextHookType_EndFramePost;
    ImGui::AddContextHook(context, &hook);
    hook.Type = ImGuiContextHookType_Shutdown;
    ImGui::AddContextHook(context, &hook);
}

int keyCode(SDL_Keycode key) {
    if (key >= SDLK_a && key <= SDLK_z) return GLFW_KEY_A + key - SDLK_a;
    if (key >= SDLK_0 && key <= SDLK_9) return GLFW_KEY_0 + key - SDLK_0;
    if (key >= SDLK_F1 && key <= SDLK_F12) return GLFW_KEY_F1 + key - SDLK_F1;
    switch (key) {
    case SDLK_ESCAPE: case SDLK_AC_BACK: return GLFW_KEY_ESCAPE;
    case SDLK_RETURN: return GLFW_KEY_ENTER;
    case SDLK_SPACE: return GLFW_KEY_SPACE;
    case SDLK_TAB: return GLFW_KEY_TAB;
    case SDLK_BACKSPACE: return GLFW_KEY_BACKSPACE;
    case SDLK_DELETE: return GLFW_KEY_DELETE;
    case SDLK_INSERT: return GLFW_KEY_INSERT;
    case SDLK_HOME: return GLFW_KEY_HOME;
    case SDLK_END: return GLFW_KEY_END;
    case SDLK_PAGEUP: return GLFW_KEY_PAGE_UP;
    case SDLK_PAGEDOWN: return GLFW_KEY_PAGE_DOWN;
    case SDLK_LEFT: return GLFW_KEY_LEFT;
    case SDLK_RIGHT: return GLFW_KEY_RIGHT;
    case SDLK_UP: return GLFW_KEY_UP;
    case SDLK_DOWN: return GLFW_KEY_DOWN;
    case SDLK_LSHIFT: return GLFW_KEY_LEFT_SHIFT;
    case SDLK_RSHIFT: return GLFW_KEY_RIGHT_SHIFT;
    case SDLK_LCTRL: return GLFW_KEY_LEFT_CONTROL;
    case SDLK_RCTRL: return GLFW_KEY_RIGHT_CONTROL;
    case SDLK_LALT: return GLFW_KEY_LEFT_ALT;
    case SDLK_RALT: return GLFW_KEY_RIGHT_ALT;
    case SDLK_LGUI: return GLFW_KEY_LEFT_SUPER;
    case SDLK_RGUI: return GLFW_KEY_RIGHT_SUPER;
    case SDLK_CAPSLOCK: return GLFW_KEY_CAPS_LOCK;
    default: return key >= 32 && key < 127 ? int(key) : GLFW_KEY_UNKNOWN;
    }
}
int modifiers() {
    const auto m = SDL_GetModState();
    return ((m & KMOD_SHIFT) ? GLFW_MOD_SHIFT : 0) | ((m & KMOD_CTRL) ? GLFW_MOD_CONTROL : 0) |
           ((m & KMOD_ALT) ? GLFW_MOD_ALT : 0) | ((m & KMOD_GUI) ? GLFW_MOD_SUPER : 0);
}
void refreshPads() {
    // The last slot is the touch pad.
    for (int slot = 0; slot < GLFW_JOYSTICK_LAST; ++slot) {
        auto& pad = pads[slot];
        if (pad.controller && !SDL_GameControllerGetAttached(pad.controller)) {
            SDL_GameControllerClose(pad.controller);
            pad = {};
            if (joystickCallback) joystickCallback(slot, GLFW_DISCONNECTED);
        }
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        const auto id = SDL_JoystickGetDeviceInstanceID(i);
        if (std::any_of(pads.begin(), pads.end(), [id](const Pad& p) {
                return p.controller && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p.controller)) == id;
            }))
            continue;
        for (int slot = 0; slot < GLFW_JOYSTICK_LAST; ++slot)
            if (!pads[slot].controller) {
                if (auto* controller = SDL_GameControllerOpen(i)) {
                    auto& pad = pads[slot];
                    pad.controller = controller;
                    char guid[33]{};
                    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)), guid, sizeof(guid));
                    pad.guid = guid;
                    std::cerr << "iOS: controller " << (SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "?")
                              << " in slot " << slot << std::endl;
                    if (joystickCallback) joystickCallback(slot, GLFW_CONNECTED);
                }
                break;
            }
    }
}
GLFWvidmode mode() {
    SDL_DisplayMode dm{};
    SDL_GetCurrentDisplayMode(0, &dm);
    return {dm.w, dm.h, 8, 8, 8, dm.refresh_rate > 0 ? dm.refresh_rate : 60};
}
void notifySize(GLFWwindow* w) {
    updateScale(w);
    int fw = 0, fh = 0;
    drawableSize(w, &fw, &fh);
    if (w->framebuffer) w->framebuffer(w, fw, fh);
    if (w->size) w->size(w, w->width, w->height);
}
void event(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) refreshPads();
    auto* w = mainWindow;
    if (!w) {
        if (ev.type == SDL_DROPFILE || ev.type == SDL_DROPTEXT) SDL_free(ev.drop.file);
        return;
    }
    switch (ev.type) {
    case SDL_QUIT: w->close = true; break;
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
        // (the home button / app switcher: the game sees its window lose focus and pauses itself)
        if (!appForeground) break;
        appForeground = false;
        touchDrag = {};
        w->keys.fill(0);
        w->mouse.fill(0);
        w->focused = false;
        if (w->focus) w->focus(w, 0);
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        if (appForeground) break;
        appForeground = true;
        w->focused = true;
        if (w->focus) w->focus(w, 1);
        break;
    case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) notifySize(w);
        else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            // (the on-screen keyboard takes no focus from the window; ignore a loss while it is up)
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST && appForeground && SDL_IsTextInputActive()) break;
            w->focused = ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
            if (!w->focused) w->keys.fill(0), w->mouse.fill(0);
            if (w->focus) w->focus(w, w->focused);
        } else if (ev.window.event == SDL_WINDOWEVENT_CLOSE) w->close = true;
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const int key = keyCode(ev.key.keysym.sym);
        // a hardware keyboard replaces the touch pad (not the on-screen keyboard's text)
        if (ev.type == SDL_KEYDOWN && key != GLFW_KEY_UNKNOWN && !SDL_IsTextInputActive() && !cane::ios::touchInputRecently())
            useTouchControls(false);
        const int action = ev.type == SDL_KEYUP ? GLFW_RELEASE : ev.key.repeat ? GLFW_REPEAT : GLFW_PRESS;
        if (key >= 0 && key <= GLFW_KEY_LAST) w->keys[key] = action != GLFW_RELEASE;
        if (w->key) w->key(w, key, ev.key.keysym.scancode, action, modifiers());
        break;
    }
    case SDL_TEXTINPUT: {
        const auto* s = reinterpret_cast<const unsigned char*>(ev.text.text);
        while (*s) {
            unsigned cp = *s++;
            int count = 0;
            if (cp >= 0xf0) cp &= 7, count = 3;
            else if (cp >= 0xe0) cp &= 15, count = 2;
            else if (cp >= 0xc0) cp &= 31, count = 1;
            while (count-- && *s) cp = (cp << 6) | (*s++ & 63);
            if (w->character) w->character(w, cp);
        }
        break;
    }
    case SDL_MOUSEMOTION:
        if (ImGui::GetCurrentContext())
            ImGui::GetIO().AddMouseSourceEvent(ev.motion.which == SDL_TOUCH_MOUSEID ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
        if (ev.motion.which != SDL_TOUCH_MOUSEID && (ev.motion.xrel || ev.motion.yrel)) useTouchControls(false);
        {
            const auto p = logicalPosition(w, float(ev.motion.x), float(ev.motion.y));
            w->x = p.x, w->y = p.y;
        }
        if (w->cursor) w->cursor(w, w->x, w->y);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        const int button = ev.button.button == SDL_BUTTON_LEFT ? GLFW_MOUSE_BUTTON_LEFT
                           : ev.button.button == SDL_BUTTON_RIGHT ? GLFW_MOUSE_BUTTON_RIGHT
                           : ev.button.button == SDL_BUTTON_MIDDLE ? GLFW_MOUSE_BUTTON_MIDDLE
                                                                   : int(ev.button.button) - 1;
        const bool down = ev.type == SDL_MOUSEBUTTONDOWN;
        if (ImGui::GetCurrentContext())
            ImGui::GetIO().AddMouseSourceEvent(ev.button.which == SDL_TOUCH_MOUSEID ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
        if (down && ev.button.which != SDL_TOUCH_MOUSEID) useTouchControls(false);
        {
            const auto p = logicalPosition(w, float(ev.button.x), float(ev.button.y));
            w->x = p.x, w->y = p.y;
            if (w->cursor) w->cursor(w, w->x, w->y);
        }
        if (button >= 0 && button < int(w->mouse.size())) w->mouse[button] = down;
        if (w->button) w->button(w, button, down ? GLFW_PRESS : GLFW_RELEASE, modifiers());
        break;
    }
    case SDL_MOUSEWHEEL:
        if (ev.wheel.which != SDL_TOUCH_MOUSEID && (ev.wheel.preciseX || ev.wheel.preciseY)) useTouchControls(false);
        if (w->scroll) w->scroll(w, ev.wheel.preciseX, ev.wheel.preciseY);
        break;
    case SDL_CONTROLLERBUTTONDOWN: useTouchControls(false); break;
    case SDL_CONTROLLERAXISMOTION:
        if ((ev.caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY && std::abs(int(ev.caxis.value)) > 7000) ||
            (ev.caxis.axis >= SDL_CONTROLLER_AXIS_TRIGGERLEFT && ev.caxis.value > 7000))
            useTouchControls(false);
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP: {
        // (the game: fingers reach SDL only while the touch pad is hidden, and a touch brings it back)
        if (ev.type == SDL_FINGERDOWN && !launcherRole()) useTouchControls(true);
        const auto position = ImVec2(ev.tfinger.x * w->width, ev.tfinger.y * w->height);
        if (ev.type == SDL_FINGERDOWN) {
            if (touchDrag.active) touchDrag.blocked = true;
            else {
                touchDrag = {};
                touchDrag.active = true;
                touchDrag.finger = ev.tfinger.fingerId;
                touchDrag.start = touchDrag.position = position;
            }
        } else if (touchDrag.active && ev.tfinger.fingerId == touchDrag.finger) {
            touchDrag.delta += position - touchDrag.position;
            touchDrag.position = position;
            if (ev.type == SDL_FINGERUP) touchDrag.active = false;
        }
        break;
    }
    case SDL_DROPFILE:
        if (w->drop) {
            const char* p = ev.drop.file;
            w->drop(w, 1, &p);
        }
        SDL_free(ev.drop.file);
        break;
    default: break;
    }
}
void (*realBindFramebuffer)(GLenum, GLuint) = nullptr;
// (glad's glBindFramebuffer in the launcher) framebuffer 0 = the window's view
void bindFramebuffer(GLenum target, GLuint framebuffer) {
    if (framebuffer == 0 && current) framebuffer = current->viewFramebuffer;
    if (realBindFramebuffer) realBindFramebuffer(target, framebuffer);
}
void readViewFramebuffer(GLFWwindow* w) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(w->native, &info) && info.subsystem == SDL_SYSWM_UIKIT) w->viewFramebuffer = info.info.uikit.framebuffer;
}
// --ios-smoke: the first launcher frame drawn 5 s or more after its first one, as Documents/smoke-launcher.png; then
// it closes (CI, ios.yml). (Time, not a frame count: an idle launcher draws only a few frames a second.)
double smokeStart = -1;
bool smokeDone = false;
void smokeFrame(GLFWwindow* w) {
    if (!cane::ios::smokeTest() || !launcherRole() || smokeDone) return;
    ++swaps;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (smokeStart < 0) smokeStart = now;
    if (now - smokeStart < 5.0 || swaps < 10) return;
    smokeDone = true;
    int pw = 0, ph = 0;
    drawableSize(w, &pw, &ph);
    std::vector<unsigned char> pixels(size_t(std::max(pw, 1)) * size_t(std::max(ph, 1)) * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, w->viewFramebuffer);
    glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    const std::string file = cane::ios::documentsDirectory() + "/smoke-launcher.png";
    const bool ok = pw > 0 && ph > 0 && stbi_write_png(file.c_str(), pw, ph, 4, pixels.data(), pw * 4);
    cane::ios::log(std::string("smoke: launcher frame ") + std::to_string(swaps) + ", " + std::to_string(pw) + "x" + std::to_string(ph) + (ok ? " saved to " : " NOT saved to ") + file);
    w->close = true;
}
// (--ios-smoke) the launcher stops after 30 s even when it drew no frame to save
void smokeDeadline() {
    if (!cane::ios::smokeTest() || !launcherRole() || !mainWindow || smokeStart < 0) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!smokeDone && now - smokeStart > 30.0) {
        smokeDone = true;
        cane::ios::log("smoke: no launcher frame after 30 s (" + std::to_string(swaps) + " frames drawn): closing");
        mainWindow->close = true;
    }
}
}  // namespace

// The game window's CAMetalLayer (platform/metal/context.mm on iOS); the view is made on first use, over the window.
extern "C" void* cane_glfw_metal_layer(GLFWwindow* w) {
    if (!w || !w->native || !w->metal) return nullptr;
    if (!w->metalView) {
        w->metalView = SDL_Metal_CreateView(w->native);
        if (!w->metalView) {
            error(GLFW_PLATFORM_ERROR, SDL_GetError());
            return nullptr;
        }
        // (the touch pad goes above the new Metal view)
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(w->native, &info) && info.subsystem == SDL_SYSWM_UIKIT) cane::ios::attachTouchOverlay(info.info.uikit.window);
    }
    return SDL_Metal_GetLayer(w->metalView);
}

extern "C" {
int glfwInit() {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, launcherRole() ? "0" : "2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        error(GLFW_PLATFORM_ERROR, SDL_GetError());
        return 0;
    }
    refreshPads();
    return 1;
}
void glfwTerminate() {
    for (auto& p : pads) {
        if (p.controller) SDL_GameControllerClose(p.controller);
        p = {};
    }
    joystickCallback = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}
void glfwInitHint(int, int) {}
void glfwWindowHint(int hint, int value) {
    if (hint == GLFW_VISIBLE) visibleHint = value != 0;
    else if (hint == GLFW_CLIENT_API) noApiHint = value == GLFW_NO_API;
}
void glfwDefaultWindowHints() { visibleHint = true, noApiHint = false; }
void glfwWindowHintString(int, const char*) {}
int glfwGetPlatform() { return GLFW_PLATFORM_NULL; }
GLFWwindow* glfwCreateWindow(int, int, const char* title, GLFWmonitor*, GLFWwindow*) {
    if (mainWindow) {
        error(GLFW_PLATFORM_ERROR, "iOS: one window per program");
        return nullptr;
    }
    auto* w = new GLFWwindow;
    w->visible = visibleHint;
    const auto dm = mode();
    Uint32 flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS;
    if (!visibleHint) flags |= SDL_WINDOW_HIDDEN;
    if (noApiHint) {
        w->metal = true;
        w->native = SDL_CreateWindow(title, 0, 0, dm.width, dm.height, flags | SDL_WINDOW_METAL);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        w->native = SDL_CreateWindow(title, 0, 0, dm.width, dm.height, flags | SDL_WINDOW_OPENGL);
        if (w->native) w->context = SDL_GL_CreateContext(w->native);
        if (w->native && !w->context) {
            cane::ios::log(std::string("iOS: no OpenGL ES 3.0 context: ") + SDL_GetError());
        }
    }
    if (!w->native || (!w->metal && !w->context)) {
        error(GLFW_VERSION_UNAVAILABLE, SDL_GetError());
        if (w->native) SDL_DestroyWindow(w->native);
        delete w;
        return nullptr;
    }
    if (!w->metal) readViewFramebuffer(w);
    updateScale(w);
    // (SDL starts text input with its video; the launcher manages it per ImGui field, the game has none)
    if (SDL_IsTextInputActive()) SDL_StopTextInput();
    {
        int pw = 0, ph = 0;
        drawableSize(w, &pw, &ph);
        cane::ios::log(std::string("iOS: ") + cane_ios_program_role() + " window " + std::to_string(pw) + "x" + std::to_string(ph) +
                       " px, " + std::to_string(w->width) + "x" + std::to_string(w->height) + (w->metal ? " (Metal)" : " (OpenGL ES)"));
    }
    mainWindow = w;
    current = w;
    return w;
}
void glfwDestroyWindow(GLFWwindow* w) {
    if (!w) return;
    if (w->metal) cane::ios::attachTouchOverlay(nullptr);
    if (w->metalView) SDL_Metal_DestroyView(w->metalView);
    if (w->context) SDL_GL_DeleteContext(w->context);
    SDL_DestroyWindow(w->native);
    if (current == w) current = nullptr;
    if (mainWindow == w) mainWindow = nullptr;
    delete w;
}
void glfwMakeContextCurrent(GLFWwindow* w) {
    if (w && !w->metal && SDL_GL_MakeCurrent(w->native, w->context) != 0) {
        error(GLFW_PLATFORM_ERROR, SDL_GetError());
        return;
    }
    if (!w && current && !current->metal) SDL_GL_MakeCurrent(current->native, nullptr);
    current = w;
}
GLFWwindow* glfwGetCurrentContext() { return current; }
GLFWglproc glfwGetProcAddress(const char* name) {
    if (!name) return nullptr;
    if (!std::strcmp(name, "glBindFramebuffer")) {
        if (!realBindFramebuffer) realBindFramebuffer = reinterpret_cast<void (*)(GLenum, GLuint)>(SDL_GL_GetProcAddress(name));
        return reinterpret_cast<GLFWglproc>(&bindFramebuffer);
    }
    return reinterpret_cast<GLFWglproc>(SDL_GL_GetProcAddress(name));
}
int glfwExtensionSupported(const char* extension) { return current && !current->metal ? SDL_GL_ExtensionSupported(extension) : 0; }
void glfwSwapBuffers(GLFWwindow* w) {
    if (!w || w->metal || !w->native || !appForeground) return;
    smokeFrame(w);
    SDL_GL_SwapWindow(w->native);
}
void glfwSwapInterval(int value) {
    if (current && !current->metal) SDL_GL_SetSwapInterval(value);
}
void glfwPollEvents() {
    smokeDeadline();
    // (the launcher) a game waiting for the main thread runs here, in place of the launcher's frames
    if (launcherRole() && cane::ios::programWaiting()) {
        GLFWwindow* w = mainWindow;
        if (w && w->native) {
            if (w->context) SDL_GL_MakeCurrent(w->native, nullptr);
            SDL_HideWindow(w->native);
        }
        cane::ios::runPendingProgram();
        // (a program that left without closing its window: the window and the touch pad go away with it)
        cane::ios::attachTouchOverlay(nullptr);
        for (Uint32 id = 1; id < 256; ++id)
            if (SDL_Window* other = SDL_GetWindowFromID(id); other && (!w || other != w->native)) SDL_HideWindow(other);
        if (w && w->native) {
            if (w->visible) SDL_ShowWindow(w->native);
            if (w->context) SDL_GL_MakeCurrent(w->native, w->context);
            readViewFramebuffer(w);
            current = w;
            w->keys.fill(0);
            w->mouse.fill(0);
            notifySize(w);
        }
        refreshPads();
    }
    installIme();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) event(ev);
}
void glfwWaitEventsTimeout(double seconds) {
    smokeDeadline();
    if (launcherRole() && cane::ios::programWaiting()) return glfwPollEvents();
    SDL_Event ev;
    if (SDL_WaitEventTimeout(&ev, int(std::clamp(seconds, 0.0, 1.0) * 1000))) event(ev);
    glfwPollEvents();
}
void glfwWaitEvents() { glfwWaitEventsTimeout(0.1); }
void glfwPostEmptyEvent() {
    SDL_Event ev{};
    ev.type = SDL_USEREVENT;
    SDL_PushEvent(&ev);
}
int glfwWindowShouldClose(GLFWwindow* w) { return w ? w->close : 0; }
void glfwSetWindowShouldClose(GLFWwindow* w, int value) {
    if (w) w->close = value != 0;
}
void glfwGetFramebufferSize(GLFWwindow* w, int* width, int* height) { drawableSize(w, width, height); }
void glfwGetWindowSize(GLFWwindow* w, int* width, int* height) {
    if (w) updateScale(w);
    if (width) *width = w ? w->width : 0;
    if (height) *height = w ? w->height : 0;
}
void glfwGetWindowContentScale(GLFWwindow* w, float* x, float* y) {
    // (pixels per logical unit)
    if (w) updateScale(w);
    if (x) *x = w ? w->uiScale : 1.f;
    if (y) *y = w ? w->uiScale : 1.f;
}
void glfwGetWindowPos(GLFWwindow*, int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
}
void glfwSetWindowSize(GLFWwindow*, int, int) {}
void glfwSetWindowSizeLimits(GLFWwindow*, int, int, int, int) {}
void glfwSetWindowAspectRatio(GLFWwindow*, int, int) {}
void glfwSetWindowPos(GLFWwindow*, int, int) {}
void glfwSetWindowTitle(GLFWwindow*, const char*) {}
GLFWmonitor* glfwGetWindowMonitor(GLFWwindow*) { return &monitor; }
GLFWmonitor* glfwGetPrimaryMonitor() { return &monitor; }
GLFWmonitor** glfwGetMonitors(int* count) {
    if (count) *count = 1;
    return monitors;
}
const char* glfwGetMonitorName(GLFWmonitor*) { return "iOS display"; }
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) {
    static GLFWvidmode result;
    result = mode();
    return &result;
}
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* m, int* count) {
    if (count) *count = 1;
    return glfwGetVideoMode(m);
}
void glfwGetMonitorPos(GLFWmonitor*, int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
}
void glfwGetMonitorWorkarea(GLFWmonitor*, int* x, int* y, int* width, int* height) {
    glfwGetMonitorPos(nullptr, x, y);
    const auto m = mode();
    if (width) *width = m.width;
    if (height) *height = m.height;
}
void glfwGetMonitorContentScale(GLFWmonitor*, float* x, float* y) {
    if (x) *x = 1;
    if (y) *y = 1;
}
void glfwSetWindowMonitor(GLFWwindow*, GLFWmonitor*, int, int, int, int, int) {}
void glfwSetWindowIcon(GLFWwindow*, int, const GLFWimage*) {}
void glfwSetWindowAttrib(GLFWwindow*, int, int) {}
int glfwGetWindowAttrib(GLFWwindow* w, int attr) {
    if (!w) return 0;
    if (attr == GLFW_FOCUSED) return w->focused && appForeground;
    if (attr == GLFW_ICONIFIED) return !appForeground;
    if (attr == GLFW_VISIBLE) return w->visible;
    return 0;
}
void glfwShowWindow(GLFWwindow* w) {
    if (!w) return;
    w->visible = true;
    SDL_ShowWindow(w->native);
}
void glfwHideWindow(GLFWwindow* w) {
    if (!w) return;
    w->visible = false;
    SDL_HideWindow(w->native);
}
void glfwIconifyWindow(GLFWwindow*) {}
void glfwRestoreWindow(GLFWwindow*) {}
void glfwMaximizeWindow(GLFWwindow*) {}
void glfwFocusWindow(GLFWwindow* w) {
    if (w) SDL_RaiseWindow(w->native);
}
void glfwRequestWindowAttention(GLFWwindow*) {}
double glfwGetTime() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count() + timeOffset; }
void glfwSetTime(double value) {
    origin = std::chrono::steady_clock::now();
    timeOffset = value;
}
uint64_t glfwGetTimerValue() { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }
uint64_t glfwGetTimerFrequency() { return uint64_t(std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num); }
int glfwGetError(const char** detail) {
    if (detail) *detail = lastError.empty() ? nullptr : lastError.c_str();
    return std::exchange(lastCode, 0);
}
void glfwGetVersion(int* major, int* minor, int* revision) {
    if (major) *major = 3;
    if (minor) *minor = 4;
    if (revision) *revision = 0;
}
const char* glfwGetVersionString() { return "3.4.0 SDL2 (SafeHouse iOS)"; }
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun f) { return std::exchange(errorCallback, f); }
GLFWmonitorfun glfwSetMonitorCallback(GLFWmonitorfun f) { return std::exchange(monitorCallback, f); }
GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun f) { return std::exchange(joystickCallback, f); }
int glfwGetKey(GLFWwindow* w, int key) { return w && key >= 0 && key <= GLFW_KEY_LAST && w->focused ? w->keys[key] : 0; }
const char* glfwGetKeyName(int key, int) {
    static char result[2]{};
    if (key >= 32 && key < 127) {
        result[0] = char(key);
        return result;
    }
    return nullptr;
}
int glfwGetKeyScancode(int key) { return key; }
int glfwGetMouseButton(GLFWwindow* w, int button) { return w && button >= 0 && button < int(w->mouse.size()) ? w->mouse[button] : 0; }
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
    if (x) *x = w ? w->x : 0;
    if (y) *y = w ? w->y : 0;
}
void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
    if (w) w->x = x, w->y = y;
}
int glfwGetInputMode(GLFWwindow* w, int mode) { return w && mode == GLFW_CURSOR ? w->cursorMode : 0; }
void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
    if (w && mode == GLFW_CURSOR) {
        w->cursorMode = value;
        SDL_SetRelativeMouseMode(value == GLFW_CURSOR_DISABLED ? SDL_TRUE : SDL_FALSE);
    }
}
GLFWcursor* glfwCreateStandardCursor(int shape) {
    auto* c = new GLFWcursor;
    c->shape = shape;
    return c;
}
GLFWcursor* glfwCreateCursor(const GLFWimage*, int, int) { return new GLFWcursor; }
void glfwDestroyCursor(GLFWcursor* c) { delete c; }
void glfwSetCursor(GLFWwindow*, GLFWcursor*) {}
void glfwSetClipboardString(GLFWwindow*, const char* text) { SDL_SetClipboardText(text); }
const char* glfwGetClipboardString(GLFWwindow*) {
    char* text = SDL_GetClipboardText();
    clipboard = text ? text : "";
    SDL_free(text);
    return clipboard.c_str();
}
int glfwJoystickPresent(int jid) {
    if (jid == GLFW_JOYSTICK_LAST) return cane::ios::touchState().active;
    return jid >= 0 && jid < GLFW_JOYSTICK_LAST && pads[jid].controller && SDL_GameControllerGetAttached(pads[jid].controller);
}
int glfwJoystickIsGamepad(int jid) { return glfwJoystickPresent(jid); }
const char* glfwGetJoystickName(int jid) {
    if (jid == GLFW_JOYSTICK_LAST) return glfwJoystickPresent(jid) ? cane::input::kTouchPadName.data() : nullptr;
    return glfwJoystickPresent(jid) ? SDL_GameControllerName(pads[jid].controller) : nullptr;
}
const char* glfwGetGamepadName(int jid) { return glfwGetJoystickName(jid); }
const char* glfwGetJoystickGUID(int jid) {
    if (jid == GLFW_JOYSTICK_LAST) return "00000000000000000000000000000000";
    return glfwJoystickPresent(jid) ? pads[jid].guid.c_str() : nullptr;
}
void glfwSetJoystickUserPointer(int, void*) {}
void* glfwGetJoystickUserPointer(int) { return nullptr; }
int glfwUpdateGamepadMappings(const char* text) {
    SDL_RWops* data = SDL_RWFromConstMem(text, int(std::strlen(text)));
    return data && SDL_GameControllerAddMappingsFromRW(data, 1) >= 0;
}
int glfwGetGamepadState(int jid, GLFWgamepadstate* state) {
    if (!state || !glfwJoystickPresent(jid)) return 0;
    if (jid == GLFW_JOYSTICK_LAST) {
        const auto touch = cane::ios::touchState();
        std::copy(touch.axes.begin(), touch.axes.end(), state->axes);
        std::copy(touch.buttons.begin(), touch.buttons.end(), state->buttons);
        return 1;
    }
    auto& p = pads[jid];
    constexpr SDL_GameControllerButton buttons[] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
    for (int i = 0; i < 15; ++i) state->buttons[i] = SDL_GameControllerGetButton(p.controller, buttons[i]);
    for (int i = 0; i < 6; ++i) {
        const float v = SDL_GameControllerGetAxis(p.controller, SDL_GameControllerAxis(i));
        state->axes[i] = i < 4 ? std::clamp(v / 32767.f, -1.f, 1.f) : std::clamp(v / 16383.5f - 1.f, -1.f, 1.f);
    }
    return 1;
}
const float* glfwGetJoystickAxes(int jid, int* count) {
    static GLFWgamepadstate s{};
    const bool ok = glfwGetGamepadState(jid, &s);
    if (count) *count = ok ? 6 : 0;
    return ok ? s.axes : nullptr;
}
const unsigned char* glfwGetJoystickButtons(int jid, int* count) {
    static GLFWgamepadstate s{};
    const bool ok = glfwGetGamepadState(jid, &s);
    if (count) *count = ok ? 15 : 0;
    return ok ? s.buttons : nullptr;
}
const unsigned char* glfwGetJoystickHats(int, int* count) {
    if (count) *count = 0;
    return nullptr;
}
int glfwRawMouseMotionSupported() { return 1; }
#define CALLBACK(Type, Name, Field) \
    Type Name(GLFWwindow* w, Type f) { return w ? std::exchange(w->Field, f) : nullptr; }
CALLBACK(GLFWframebuffersizefun, glfwSetFramebufferSizeCallback, framebuffer)
CALLBACK(GLFWwindowsizefun, glfwSetWindowSizeCallback, size)
CALLBACK(GLFWwindowposfun, glfwSetWindowPosCallback, position)
CALLBACK(GLFWwindowfocusfun, glfwSetWindowFocusCallback, focus)
CALLBACK(GLFWcursorposfun, glfwSetCursorPosCallback, cursor)
CALLBACK(GLFWcursorenterfun, glfwSetCursorEnterCallback, enter)
CALLBACK(GLFWmousebuttonfun, glfwSetMouseButtonCallback, button)
CALLBACK(GLFWscrollfun, glfwSetScrollCallback, scroll)
CALLBACK(GLFWkeyfun, glfwSetKeyCallback, key)
CALLBACK(GLFWcharfun, glfwSetCharCallback, character)
CALLBACK(GLFWdropfun, glfwSetDropCallback, drop)
#undef CALLBACK
// (glfw3native.h's Cocoa calls, which ImGui's GLFW backend makes on Apple; nothing to give on iOS)
void* glfwGetCocoaWindow(GLFWwindow*) { return nullptr; }
void* glfwGetCocoaView(GLFWwindow*) { return nullptr; }
uint32_t glfwGetCocoaMonitor(GLFWmonitor*) { return 0; }
// (callbacks the programs set but this platform never calls)
GLFWwindowclosefun glfwSetWindowCloseCallback(GLFWwindow*, GLFWwindowclosefun) { return nullptr; }
GLFWwindowrefreshfun glfwSetWindowRefreshCallback(GLFWwindow*, GLFWwindowrefreshfun) { return nullptr; }
GLFWwindowiconifyfun glfwSetWindowIconifyCallback(GLFWwindow*, GLFWwindowiconifyfun) { return nullptr; }
GLFWwindowmaximizefun glfwSetWindowMaximizeCallback(GLFWwindow*, GLFWwindowmaximizefun) { return nullptr; }
GLFWwindowcontentscalefun glfwSetWindowContentScaleCallback(GLFWwindow*, GLFWwindowcontentscalefun) { return nullptr; }
GLFWcharmodsfun glfwSetCharModsCallback(GLFWwindow*, GLFWcharmodsfun) { return nullptr; }
void glfwSetWindowUserPointer(GLFWwindow* w, void* pointer) {
    if (w) w->user = pointer;
}
void* glfwGetWindowUserPointer(GLFWwindow* w) { return w ? w->user : nullptr; }
}
