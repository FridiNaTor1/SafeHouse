#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>   // (glfwCreateWindowSurface: Sly 2's Vulkan renderer on the SDL surface)
#include <GLFW/glfw3.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <SDL_syswm.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "android_app.h"
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

struct GLFWwindow {
    SDL_Window* native = nullptr;
    SDL_GLContext context = nullptr;
    // Character previews run while the launcher activity owns the display.
    // Their EGL pbuffer must survive the SDL activity surface going away.
    bool offscreen = false;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLSurface parkingSurface = EGL_NO_SURFACE;
    int width = 0, height = 0;
    // Only the launcher uses density-scaled logical coordinates. Native game
    // cameras and offscreen preview/readback retain their physical pixels.
    float uiScale = 1;
    bool close = false, focused = true, visible = true;
    bool vulkan = false;   // (made with GLFW_NO_API: SDL_WINDOW_VULKAN, no GL context)
    int cursorMode = GLFW_CURSOR_NORMAL;
    std::array<unsigned char, GLFW_KEY_LAST + 1> keys{};
    std::array<unsigned char, 8> mouse{};
    double x = 0, y = 0;
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
struct GLFWcursor { SDL_Cursor* native = nullptr; };
namespace {
GLFWwindow* current = nullptr;
GLFWmonitor monitor;
GLFWmonitor* monitors[] = {&monitor};
GLFWerrorfun errorCallback = nullptr;
GLFWjoystickfun joystickCallback = nullptr;
GLFWmonitorfun monitorCallback = nullptr;
std::string lastError, clipboard;
int lastCode = 0;
bool visibleHint = true;
bool noApiHint = false;   // GLFW_CLIENT_API = GLFW_NO_API: a Vulkan window (no EGL context)
bool lostGraphics = false;
bool appForeground = true;
struct AndroidIme {
    ImGuiContext* context = nullptr;
    ImGuiID owner = 0, dismissedOwner = 0, deactivateOwner = 0;
    SDL_Rect rect{};
    bool wanted = false, wasActive = false, reactivate = false;
    bool consumeBackRelease = false;
} ime;
std::uint64_t surfaceGeneration = 1;   // (cane::android::windowSurfaceSerial)
bool touchControlsVisible = true;
bool touchControlsKnown = false;
struct TouchDrag {
    SDL_FingerID finger = 0;
    bool active = false, blocked = false, scrolling = false;
    ImVec2 start{}, position{}, delta{};
    ImGuiWindow* window = nullptr;
} touchDrag;
double timeOffset = 0;
auto origin = std::chrono::steady_clock::now();
struct Pad {
    SDL_GameController* controller = nullptr;
    std::string guid;
    std::array<float,6> axes{};
    std::array<unsigned char,15> buttons{};
};
std::array<Pad, GLFW_JOYSTICK_LAST + 1> pads;
bool previewProcess() {
    const char* role=std::getenv("SAFEHOUSE_ANDROID_ROLE");
    return role && std::strcmp(role,"preview")==0;
}
bool launcherProcess() {
    const char* role=std::getenv("SAFEHOUSE_ANDROID_ROLE");
    return role && std::strcmp(role,"launcher")==0;
}
void updateUiScale(GLFWwindow* window) {
    if(!window || window->offscreen || !launcherProcess())return;
    SDL_GetWindowSize(window->native,&window->width,&window->height);
    float dpi=160;
    if(SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window->native),&dpi,nullptr,nullptr)!=0 || !std::isfinite(dpi))dpi=160;
    // Full Android density scaling alone can reduce a high-resolution phone
    // to an unusably short 400px viewport. Keep the desktop layout at least
    // 1280x720 where the display permits it, and use the display's actual DPI
    // on external monitors/DeX instead of inheriting the phone's density.
    const float fit=std::max(1.f,std::min(window->width/1280.f,window->height/720.f));
    window->uiScale=std::clamp(dpi/160.f,1.f,fit);
}
ImVec2 coordinateScale(GLFWwindow* window) {
    if(!window || window->offscreen || window->uiScale<=1)return {1,1};
    // Match the rounded logical size exactly, including fractional DPI.
    return {window->width/float(std::max(1,int(std::round(window->width/window->uiScale)))),
        window->height/float(std::max(1,int(std::round(window->height/window->uiScale))))};
}
ImVec2 logicalPosition(GLFWwindow* window,float x,float y) {
    const auto scale=coordinateScale(window);return {x/scale.x,y/scale.y};
}
void error(int code, const char* detail) {
    lastCode=code; lastError=detail ? detail : "Android display error";
    if (errorCallback) errorCallback(code,lastError.c_str());
}
void useTouchControls(bool visible) {
    // (the pad's own state, which Java also changes when a device is plugged in or removed, decides: a stale
    // copy here once left the pad hidden for good)
    if(touchControlsKnown && touchControlsVisible==visible && cane::android::touchState().active==visible)return;
    touchControlsKnown=true;touchControlsVisible=visible;
    try{cane::android::setTouchControlsVisible(visible);}
    catch(const std::exception& failure){error(GLFW_PLATFORM_ERROR,failure.what());}
}
void graphicsLost(GLFWwindow* window,const char* detail) {
    if(!lostGraphics){error(GLFW_PLATFORM_ERROR,detail);std::cerr<<lastError<<std::endl;}
    lostGraphics=true;
    if(window)window->close=true;
}
void createParkingSurface(GLFWwindow* window) {
    window->display=eglGetCurrentDisplay();
    EGLint id=0,count=0,type=0;EGLConfig config{};
    const auto context=static_cast<EGLContext>(window->context);
    if(window->display==EGL_NO_DISPLAY || !eglQueryContext(window->display,context,EGL_CONFIG_ID,&id))return;
    const EGLint attributes[]={EGL_CONFIG_ID,id,EGL_NONE};
    if(!eglChooseConfig(window->display,attributes,&config,1,&count) || count!=1 ||
       !eglGetConfigAttrib(window->display,config,EGL_SURFACE_TYPE,&type) || !(type&EGL_PBUFFER_BIT))return;
    const EGLint size[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    window->parkingSurface=eglCreatePbufferSurface(window->display,config,size);
}
void retainGraphicsContext(GLFWwindow* window) {
    if(!window || window->offscreen || window->vulkan || lostGraphics)return;
    const auto expected=static_cast<EGLContext>(window->context);
    const EGLContext actual=eglGetCurrentContext();
    if(actual==expected)return;
    if(actual!=EGL_NO_CONTEXT){
        graphicsLost(window,"The Android graphics context was replaced. Returning to the launcher; start the session again.");return;
    }
    // The nonblocking SDL event pump unbinds its window context on pause. Keep
    // that exact context current on a compatible tiny surface so simulation,
    // late joins and GPU resource uploads remain valid while rendering is
    // suspended. Direct EGL binding leaves SDL's original backup/TLS intact;
    // its normal foreground path restores the context to the window surface.
    if(window->parkingSurface==EGL_NO_SURFACE ||
       !eglMakeCurrent(window->display,window->parkingSurface,window->parkingSurface,expected))
        graphicsLost(window,"Android could not preserve the graphics context in the background. Returning to the launcher; start the session again.");
}
void detectImeDismissal() {
    // SDL's Android DummyEdit handles Back in onKeyPreIme(), without sending
    // a key event to the game. Its native callback stops text input instead.
    // Respect that dismissal until the user selects an input field again.
    if(ime.wasActive && !SDL_IsTextInputActive() && ime.wanted) {
        ime.dismissedOwner=ime.owner;
        ime.deactivateOwner=ime.owner;
        ime.wasActive=false;
    }
}
void stopIme() {
    ime.wanted=false;ime.wasActive=false;ime.reactivate=false;
    if(SDL_IsTextInputActive())SDL_StopTextInput();
}
void applyIme(ImGuiViewport* viewport,const ImGuiPlatformImeData& data) {
    if(!current || current->offscreen || !appForeground)return;
    detectImeDismissal();
    const ImGuiID owner=ImGui::GetActiveID();
    if(!data.WantVisible || owner==0) {
        stopIme();ime.owner=0;ime.dismissedOwner=0;
        return;
    }
    const bool newOwner=owner!=ime.owner;
    if(newOwner || ime.reactivate)ime.dismissedOwner=0;
    ime.reactivate=false;ime.owner=owner;ime.wanted=true;
    if(ime.dismissedOwner==owner)return;
    // Android pans its surface using the focused DummyEdit rectangle. The
    // default (0,0) rectangle leaves low fields hidden behind the keyboard.
    const ImVec2 offset=viewport?viewport->Pos:ImVec2(0,0);
    const auto scale=coordinateScale(current);
    SDL_Rect rect{int(std::floor((data.InputPos.x-offset.x)*scale.x)),
        int(std::floor((data.InputPos.y-offset.y)*scale.y)),std::max(1,int(std::ceil(scale.x))),
        std::max(1,int(std::ceil(data.InputLineHeight*scale.y)))};
    const bool moved=rect.x!=ime.rect.x || rect.y!=ime.rect.y || rect.w!=ime.rect.w || rect.h!=ime.rect.h;
    if(moved || newOwner)SDL_SetTextInputRect(&rect);
    // SDL_SetTextInputRect stores the rectangle; StartTextInput applies it to
    // the Android widget. Reposition when necessary, never reopen every poll.
    if(!SDL_IsTextInputActive() || moved || newOwner)SDL_StartTextInput();
    ime.rect=rect;ime.wasActive=SDL_IsTextInputActive();
}
void platformIme(ImGuiViewport* viewport,ImGuiPlatformImeData* data) {
    applyIme(viewport,*data);
}
void scrollTouchFrame(ImGuiContext* context) {
    auto& drag=touchDrag;
    // Buttons, sliders, text fields and scrollbar handles retain their native
    // drags. ImGui's no-move window background owns a MoveId rather than a
    // widget; that background is an eligible place to start scrolling.
    const bool widget=context->ActiveId && (!context->ActiveIdWindow ||
        context->ActiveId!=context->ActiveIdWindow->MoveId);
    if(widget || drag.blocked){drag.delta={};return;}
    if(drag.active && !drag.window) {
        auto* window=context->HoveredWindow;
        while(window && (window->ScrollMax.y<=0 || (window->Flags&ImGuiWindowFlags_NoScrollWithMouse)))
            window=window->ParentWindow;
        if(window && window->InnerRect.Contains(drag.position))drag.window=window;
    }
    if(drag.window) {
        const ImVec2 distance=drag.position-drag.start;
        if(std::abs(distance.y)>6 && std::abs(distance.y)>=std::abs(distance.x))drag.scrolling=true;
        if(drag.scrolling && drag.delta.y!=0)
            ImGui::SetScrollY(drag.window,std::clamp(drag.window->Scroll.y-drag.delta.y,0.f,drag.window->ScrollMax.y));
    }
    drag.delta={};
    if(!drag.active)drag={};
}
void imeFrameHook(ImGuiContext* context,ImGuiContextHook* hook) {
    if(hook->Type==ImGuiContextHookType_Shutdown) {
        stopIme();ime={};touchDrag={};return;
    }
    if(hook->Type==ImGuiContextHookType_NewFramePost) {
        // Preserve ImGui's previous-active/edited state until NewFrame has
        // recorded it, then deactivate before widgets are submitted. Their
        // IsItemDeactivatedAfterEdit handlers can persist the edited value.
        if(ime.deactivateOwner && context->ActiveId==ime.deactivateOwner)ImGui::ClearActiveID();
        ime.deactivateOwner=0;return;
    }
    // ImGui calls SetPlatformImeDataFn only when the cursor rectangle changes.
    // Also observe Back/new field taps when that rectangle remains identical.
    applyIme(ImGui::GetMainViewport(),context->PlatformImeData);
    scrollTouchFrame(context);
}
void installIme() {
    auto* context=ImGui::GetCurrentContext();
    if(!context || context==ime.context || !current || current->offscreen)return;
    stopIme();ime={};ime.context=context;
    ImGui::GetIO().SetPlatformImeDataFn=platformIme;
    ImGuiContextHook hook{};hook.Callback=imeFrameHook;
    hook.Type=ImGuiContextHookType_NewFramePost;ImGui::AddContextHook(context,&hook);
    hook.Type=ImGuiContextHookType_EndFramePost;ImGui::AddContextHook(context,&hook);
    hook.Type=ImGuiContextHookType_Shutdown;ImGui::AddContextHook(context,&hook);
}
int keyCode(SDL_Keycode key) {
    if (key >= SDLK_a && key <= SDLK_z) return GLFW_KEY_A + key-SDLK_a;
    if (key >= SDLK_0 && key <= SDLK_9) return GLFW_KEY_0 + key-SDLK_0;
    if (key >= SDLK_F1 && key <= SDLK_F24) return GLFW_KEY_F1 + key-SDLK_F1;
    switch(key) {
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
    case SDLK_NUMLOCKCLEAR: return GLFW_KEY_NUM_LOCK;
    case SDLK_KP_ENTER: return GLFW_KEY_KP_ENTER;
    case SDLK_KP_DECIMAL: return GLFW_KEY_KP_DECIMAL;
    case SDLK_KP_DIVIDE: return GLFW_KEY_KP_DIVIDE;
    case SDLK_KP_MULTIPLY: return GLFW_KEY_KP_MULTIPLY;
    case SDLK_KP_MINUS: return GLFW_KEY_KP_SUBTRACT;
    case SDLK_KP_PLUS: return GLFW_KEY_KP_ADD;
    default: return key >= 32 && key < 127 ? int(key) : GLFW_KEY_UNKNOWN;
    }
}
int modifiers() {
    const auto m=SDL_GetModState();
    return ((m&KMOD_SHIFT)?GLFW_MOD_SHIFT:0) | ((m&KMOD_CTRL)?GLFW_MOD_CONTROL:0) |
        ((m&KMOD_ALT)?GLFW_MOD_ALT:0) | ((m&KMOD_GUI)?GLFW_MOD_SUPER:0);
}
void refreshPads() {
    // The last slot is reserved for the optional touch overlay.
    for (int slot=0;slot<GLFW_JOYSTICK_LAST;++slot) {
        auto& pad=pads[slot];
        if (pad.controller && !SDL_GameControllerGetAttached(pad.controller)) {
            SDL_GameControllerClose(pad.controller);pad={};
            if (joystickCallback) joystickCallback(slot,GLFW_DISCONNECTED);
        }
    }
    for (int i=0;i<SDL_NumJoysticks();++i) {
        if (!SDL_IsGameController(i)) continue;
        const auto id=SDL_JoystickGetDeviceInstanceID(i);
        if (std::any_of(pads.begin(),pads.end(),[id](const Pad& p){return p.controller && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p.controller))==id;})) continue;
        for(int slot=0;slot<GLFW_JOYSTICK_LAST;++slot) if(!pads[slot].controller) {
            if(auto* controller=SDL_GameControllerOpen(i)) {
                auto& pad=pads[slot];pad.controller=controller;
                char guid[33]{};SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)),guid,sizeof(guid));pad.guid=guid;
                if(joystickCallback) joystickCallback(slot,GLFW_CONNECTED);
            }
            break;
        }
    }
}
GLFWvidmode mode() {
    if(previewProcess())return {1280,720,8,8,8,60};
    SDL_DisplayMode dm{};SDL_GetCurrentDisplayMode(0,&dm);
    return {dm.w,dm.h,8,8,8,dm.refresh_rate>0?dm.refresh_rate:60};
}
void event(const SDL_Event& ev) {
    if(ev.type==SDL_CONTROLLERDEVICEADDED || ev.type==SDL_CONTROLLERDEVICEREMOVED) refreshPads();
    auto* w=current;if(!w)return;
    if(w->offscreen) {
        // SDL window and activity lifecycle events belong to a visible Android
        // surface, not the offscreen preview or its continuously animated pose.
        if(ev.type==SDL_DROPFILE)SDL_free(ev.drop.file);
        if(ev.type==SDL_QUIT)w->close=true;
        return;
    }
    // Resize/focus callbacks can issue GPU work, so restore the parking
    // context before dispatching any event queued by SDL's pause pump.
    retainGraphicsContext(w);
    if(lostGraphics){if(ev.type==SDL_DROPFILE)SDL_free(ev.drop.file);return;}
    switch(ev.type) {
    case SDL_QUIT: w->close=true;break;
    case SDL_RENDER_DEVICE_RESET:
        // (a Vulkan window has no EGL context to lose: SDL's resume posts this when its EGL restore finds none; the
        // renderer makes its surface again itself, cane::android::windowSurfaceSerial)
        if(w->vulkan)break;
        // SDL can replace a lost EGL context on resume. Every engine GPU name
        // and state cache then belongs to the former context. End the session
        // through its normal save/exit path rather than use those stale names.
        graphicsLost(w,"The Android graphics context was lost. Returning to the launcher; start the session again.");break;
    case SDL_APP_WILLENTERBACKGROUND:
        appForeground=false;stopIme();touchDrag={};
        w->keys.fill(0);w->mouse.fill(0);w->focused=false;
        if(w->focus)w->focus(w,0);break;
    case SDL_APP_DIDENTERFOREGROUND:
        appForeground=true;++surfaceGeneration;
        w->focused=true;if(w->focus)w->focus(w,1);break;
    case SDL_WINDOWEVENT:
        if(ev.window.event==SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event==SDL_WINDOWEVENT_RESIZED) {
            updateUiScale(w);
            int fw=0,fh=0;SDL_GL_GetDrawableSize(w->native,&fw,&fh);
            if(w->framebuffer)w->framebuffer(w,fw,fh);
            const auto logical=logicalPosition(w,float(ev.window.data1),float(ev.window.data2));
            if(w->size)w->size(w,int(std::round(logical.x)),int(std::round(logical.y)));
        } else if(ev.window.event==SDL_WINDOWEVENT_DISPLAY_CHANGED) {
            updateUiScale(w);
        } else if(ev.window.event==SDL_WINDOWEVENT_FOCUS_GAINED || ev.window.event==SDL_WINDOWEVENT_FOCUS_LOST) {
            // An IME window can take Android window focus without backgrounding
            // this activity. Forwarding that to ImGui clears the active text
            // field, so subsequent keyboard text has nowhere to go.
            if(ev.window.event==SDL_WINDOWEVENT_FOCUS_LOST && appForeground && SDL_IsTextInputActive())break;
            w->focused=ev.window.event==SDL_WINDOWEVENT_FOCUS_GAINED;
            if(!w->focused){w->keys.fill(0);w->mouse.fill(0);}
            if(w->focus)w->focus(w,w->focused);
        } else if(ev.window.event==SDL_WINDOWEVENT_CLOSE) w->close=true;
        break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
        if(ev.key.keysym.sym==SDLK_AC_BACK) {
            if(ev.type==SDL_KEYDOWN && (ime.wanted || SDL_IsTextInputActive())) {
                ime.dismissedOwner=ime.owner;ime.consumeBackRelease=true;
                ime.deactivateOwner=ime.owner;
                ime.wasActive=false;
                if(SDL_IsTextInputActive())SDL_StopTextInput();
                break;
            }
            if(ev.type==SDL_KEYUP && ime.consumeBackRelease){ime.consumeBackRelease=false;break;}
        }
        // Android IME keystrokes and the touch CHAT button are not a switch to
        // desktop input. Meaningful physical gameplay keys hide the overlay.
        const int key=keyCode(ev.key.keysym.sym);
        if(ev.type==SDL_KEYDOWN && key!=GLFW_KEY_UNKNOWN && ev.key.keysym.sym!=SDLK_AC_BACK &&
           !SDL_IsTextInputActive() && !ime.wanted && !cane::android::touchInputRecently())useTouchControls(false);
        const int action=ev.type==SDL_KEYUP?GLFW_RELEASE:ev.key.repeat?GLFW_REPEAT:GLFW_PRESS;
        if(key>=0 && key<=GLFW_KEY_LAST)w->keys[key]=action!=GLFW_RELEASE;
        if(w->key)w->key(w,key,ev.key.keysym.scancode,action,modifiers());break;
    }
    case SDL_TEXTINPUT: {
        const auto* s=reinterpret_cast<const unsigned char*>(ev.text.text);
        while(*s) {
            unsigned cp=*s++;int count=0;
            if(cp>=0xf0){cp&=7;count=3;}else if(cp>=0xe0){cp&=15;count=2;}else if(cp>=0xc0){cp&=31;count=1;}
            while(count-- && *s)cp=(cp<<6)|(*s++&63);
            if(w->character)w->character(w,cp);
        }
        break;
    }
    case SDL_MOUSEMOTION:
        if(ImGui::GetCurrentContext())ImGui::GetIO().AddMouseSourceEvent(ev.motion.which==SDL_TOUCH_MOUSEID?ImGuiMouseSource_TouchScreen:ImGuiMouseSource_Mouse);
        if(ev.motion.which!=SDL_TOUCH_MOUSEID && (ev.motion.xrel || ev.motion.yrel))useTouchControls(false);
        {const auto position=logicalPosition(w,float(ev.motion.x),float(ev.motion.y));w->x=position.x;w->y=position.y;}
        if(w->cursor)w->cursor(w,w->x,w->y);break;
    case SDL_MOUSEBUTTONDOWN:case SDL_MOUSEBUTTONUP: {
        const int button=ev.button.button==SDL_BUTTON_LEFT?GLFW_MOUSE_BUTTON_LEFT:ev.button.button==SDL_BUTTON_RIGHT?GLFW_MOUSE_BUTTON_RIGHT:ev.button.button==SDL_BUTTON_MIDDLE?GLFW_MOUSE_BUTTON_MIDDLE:int(ev.button.button)-1;
        const bool down=ev.type==SDL_MOUSEBUTTONDOWN;
        if(ImGui::GetCurrentContext())ImGui::GetIO().AddMouseSourceEvent(ev.button.which==SDL_TOUCH_MOUSEID?ImGuiMouseSource_TouchScreen:ImGuiMouseSource_Mouse);
        if(down && ev.button.which!=SDL_TOUCH_MOUSEID)useTouchControls(false);
        if(down && button==GLFW_MOUSE_BUTTON_LEFT && ime.dismissedOwner)ime.reactivate=true;
        if(button>=0 && button<int(w->mouse.size()))w->mouse[button]=down;
        if(w->button)w->button(w,button,down?GLFW_PRESS:GLFW_RELEASE,modifiers());break;
    }
    case SDL_MOUSEWHEEL:
        if(ev.wheel.which!=SDL_TOUCH_MOUSEID && (ev.wheel.preciseX || ev.wheel.preciseY))useTouchControls(false);
        if(w->scroll)w->scroll(w,ev.wheel.preciseX,ev.wheel.preciseY);break;
    case SDL_CONTROLLERBUTTONDOWN: useTouchControls(false);break;
    case SDL_CONTROLLERAXISMOTION:
        if((ev.caxis.axis<=SDL_CONTROLLER_AXIS_RIGHTY && std::abs(int(ev.caxis.value))>7000) ||
           (ev.caxis.axis>=SDL_CONTROLLER_AXIS_TRIGGERLEFT && ev.caxis.value>7000))useTouchControls(false);
        break;
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
        if(ev.type==SDL_FINGERDOWN)useTouchControls(true);
        int width=0,height=0;SDL_GetWindowSize(w->native,&width,&height);
        const auto position=logicalPosition(w,ev.tfinger.x*width,ev.tfinger.y*height);
        if(ev.type==SDL_FINGERDOWN) {
            if(touchDrag.active)touchDrag.blocked=true;
            else{touchDrag={};touchDrag.active=true;touchDrag.finger=ev.tfinger.fingerId;touchDrag.start=touchDrag.position=position;}
        }else if(touchDrag.active && ev.tfinger.fingerId==touchDrag.finger) {
            touchDrag.delta+=position-touchDrag.position;touchDrag.position=position;
            if(ev.type==SDL_FINGERUP)touchDrag.active=false;
        }
        break;
    }
    case SDL_DROPFILE: if(w->drop){const char* p=ev.drop.file;w->drop(w,1,&p);}SDL_free(ev.drop.file);break;
    default:break;
    }
}
}
namespace {
SDL_Window* createSurfaceWindow(const char* title,const GLFWvidmode& dm,Uint32 flags) {
    // The activity can be between surfaces when the game makes its window (a portrait phone turning to landscape at
    // start destroys the first surface): SDL then fails with "Could not fetch native window". Wait for the new one.
    for(int attempt=0;;++attempt){
        SDL_Window* window=SDL_CreateWindow(title,0,0,dm.width>0?dm.width:1280,dm.height>0?dm.height:720,flags);
        if(window || attempt>=100)return window;
        if(attempt==0)std::cerr<<"Android: no surface for the window yet ("<<SDL_GetError()<<"): waiting"<<std::endl;
        SDL_PumpEvents();SDL_Delay(30);
    }
}
}
namespace cane::android {
bool graphicsContextLost(){return lostGraphics;}
std::uint64_t windowSurfaceSerial() {
    // SDL replaces data->native_window on the Java thread (surfaceDestroyed: NULL, surfaceCreated: the new one); a
    // change seen here, or a return to the foreground, means a new surface for the renderer.
    static ANativeWindow* seen=nullptr;
    if(!current || !current->vulkan || !current->native)return 0;
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    ANativeWindow* window=SDL_GetWindowWMInfo(current->native,&info) && info.subsystem==SDL_SYSWM_ANDROID?info.info.android.window:nullptr;
    if(window!=seen){seen=window;++surfaceGeneration;}
    return window && appForeground?surfaceGeneration:0;
}
void setPresentationRate(int framesPerSecond) {
    // ANativeWindow_setFrameRate (API 30) lets a 90/120 Hz panel drop to the
    // game's rate: the display and compositor stop waking twice per frame and
    // each presented frame lands on a vblank instead of alternating 1-2
    // refreshes. Re-applied when Android gives the activity a new surface.
    static ANativeWindow* applied=nullptr;static int appliedRate=-1;
    if(!current || current->offscreen || !current->native)return;
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(current->native,&info) || info.subsystem!=SDL_SYSWM_ANDROID)return;
    ANativeWindow* window=info.info.android.window;
    if(!window || (window==applied && framesPerSecond==appliedRate))return;
    applied=window;appliedRate=framesPerSecond;
    using SetFrameRate=int32_t(*)(ANativeWindow*,float,int8_t);
    static const auto setFrameRate=reinterpret_cast<SetFrameRate>(dlsym(RTLD_DEFAULT,"ANativeWindow_setFrameRate"));
    if(!setFrameRate)return; // Android 8-10 keep the panel's own rate
    // 0 = ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT (a game, not video).
    const int32_t result=setFrameRate(window,float(std::max(framesPerSecond,1)),0);
    std::cerr<<"Android presentation rate hint: "<<framesPerSecond<<" FPS"<<(result==0?"":" (rejected)")<<std::endl;
}
}
extern "C" {
int glfwInit() {
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE,"0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS,"1");
    const Uint32 systems=SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS|(previewProcess()?0:SDL_INIT_VIDEO);
    if(SDL_InitSubSystem(systems)!=0){error(GLFW_PLATFORM_ERROR,SDL_GetError());return 0;}
    refreshPads();return 1;
}
void glfwTerminate(){for(auto& p:pads){if(p.controller)SDL_GameControllerClose(p.controller);p={};}joystickCallback=nullptr;SDL_QuitSubSystem(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER);}
void glfwWindowHint(int hint,int value){if(hint==GLFW_VISIBLE)visibleHint=value!=0;else if(hint==GLFW_CLIENT_API)noApiHint=value==GLFW_NO_API;}
void glfwDefaultWindowHints(){visibleHint=true;noApiHint=false;}
void glfwWindowHintString(int,const char*){}
int glfwGetPlatform(){return GLFW_PLATFORM_NULL;}
GLFWwindow* glfwCreateWindow(int width,int height,const char* title,GLFWmonitor*,GLFWwindow*) {
    if(current){error(GLFW_PLATFORM_ERROR,"Android uses one SDL surface per activity");return nullptr;}
    if(previewProcess()) {
        if(width<=0 || height<=0){error(GLFW_PLATFORM_ERROR,"Android preview dimensions must be positive");return nullptr;}
        auto* w=new GLFWwindow;w->offscreen=true;w->width=width;w->height=height;w->visible=visibleHint;
        w->display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        const EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        const EGLint contextAttributes[]={EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,1,EGL_NONE};
        const EGLint surfaceAttributes[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
        EGLConfig config{};EGLint count=0;
        if(w->display!=EGL_NO_DISPLAY && eglInitialize(w->display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_ES_API) &&
           eglChooseConfig(w->display,attributes,&config,1,&count) && count==1) {
            w->eglContext=eglCreateContext(w->display,config,EGL_NO_CONTEXT,contextAttributes);
            w->surface=eglCreatePbufferSurface(w->display,config,surfaceAttributes);
        }
        if(w->eglContext==EGL_NO_CONTEXT || w->surface==EGL_NO_SURFACE || !eglMakeCurrent(w->display,w->surface,w->surface,w->eglContext)) {
            const EGLint code=eglGetError();
            if(w->surface!=EGL_NO_SURFACE)eglDestroySurface(w->display,w->surface);
            if(w->eglContext!=EGL_NO_CONTEXT)eglDestroyContext(w->display,w->eglContext);
            if(w->display!=EGL_NO_DISPLAY)eglTerminate(w->display);
            delete w;const auto detail="Cannot create Android ES 3.1 preview pbuffer (EGL error "+std::to_string(code)+")";
            error(GLFW_VERSION_UNAVAILABLE,detail.c_str());return nullptr;
        }
        current=w;return w;
    }
    if(const char* role=std::getenv("SAFEHOUSE_ANDROID_ROLE");role && std::strcmp(role,"game")==0)
        cane::android::showGame();
    if(noApiHint){
        // (Sly 2's Vulkan renderer: the activity's surface for VK_KHR_android_surface; no EGL context)
        auto* w=new GLFWwindow;w->visible=visibleHint;w->vulkan=true;
        auto dm=mode();
        w->native=createSurfaceWindow(title,dm,SDL_WINDOW_VULKAN|SDL_WINDOW_FULLSCREEN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
        if(!w->native){error(GLFW_PLATFORM_ERROR,SDL_GetError());delete w;return nullptr;}
        updateUiScale(w);
        if(!launcherProcess() && SDL_IsTextInputActive())SDL_StopTextInput();
        current=w;return w;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    // ES 3.2: Sly 2's glob program has a geometry stage and vertex-stage storage buffers (gles_sly2.cpp).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,0);SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,0);
    // An opaque RGBX8888 surface: SurfaceFlinger can scan it out as a
    // hardware overlay instead of blending an alpha layer, and the present
    // quad never dithers through a 16-bit format.
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,8);SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,8);SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,0);
    auto* w=new GLFWwindow;w->visible=visibleHint;
    auto dm=mode();
    w->native=createSurfaceWindow(title,dm,SDL_WINDOW_OPENGL|SDL_WINDOW_FULLSCREEN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(w->native)w->context=SDL_GL_CreateContext(w->native);
    // (an ES 3.1 / 3.0 GPU, or the emulator's software renderer, still runs the launcher; Sly 2 reports what it lacks)
    for(int minor=1;w->native && !w->context && minor>=0;--minor){
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,minor);w->context=SDL_GL_CreateContext(w->native);
    }
    if(!w->native || !w->context){error(GLFW_VERSION_UNAVAILABLE,SDL_GetError());if(w->native)SDL_DestroyWindow(w->native);delete w;return nullptr;}
    createParkingSurface(w);
    updateUiScale(w);
    // SDL starts text input with its video (SDL_VideoInit); the game has no text fields, and with text input active a
    // physical key would not count as a switch away from the touch pad (event(): SDL_KEYDOWN). The launcher manages
    // it per ImGui field (applyIme).
    if(!launcherProcess() && SDL_IsTextInputActive())SDL_StopTextInput();
    current=w;return w;
}
void glfwDestroyWindow(GLFWwindow* w){
    if(!w)return;
    if(w->offscreen){
        if(eglGetCurrentContext()==w->eglContext)eglMakeCurrent(w->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroySurface(w->display,w->surface);eglDestroyContext(w->display,w->eglContext);eglTerminate(w->display);
    }else{
        if(w->parkingSurface!=EGL_NO_SURFACE){
            if(eglGetCurrentSurface(EGL_DRAW)==w->parkingSurface)eglMakeCurrent(w->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            eglDestroySurface(w->display,w->parkingSurface);
        }
        if(w->context)SDL_GL_DeleteContext(w->context);
        SDL_DestroyWindow(w->native);
    }
    if(current==w)current=nullptr;delete w;
}
void glfwMakeContextCurrent(GLFWwindow* w){
    if(w){
        if(w->offscreen){if(!eglMakeCurrent(w->display,w->surface,w->surface,w->eglContext)){error(GLFW_PLATFORM_ERROR,"Cannot activate Android preview pbuffer");return;}}
        else if(w->vulkan){}
        else if(SDL_GL_MakeCurrent(w->native,w->context)!=0){error(GLFW_PLATFORM_ERROR,SDL_GetError());return;}
    }else if(current){
        if(current->offscreen)eglMakeCurrent(current->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        else if(!current->vulkan)SDL_GL_MakeCurrent(current->native,nullptr);
    }
    current=w;
}
GLFWwindow* glfwGetCurrentContext(){
    if(!current)return nullptr;
    const EGLContext expected=current->offscreen?current->eglContext:static_cast<EGLContext>(current->context);
    // SDL deliberately unbinds the context while its activity is backgrounded.
    // Keep the logical window, but never tell the renderer it can issue draws.
    return expected!=EGL_NO_CONTEXT && eglGetCurrentContext()==expected?current:nullptr;
}
GLFWglproc glfwGetProcAddress(const char* name){return current && current->offscreen?reinterpret_cast<GLFWglproc>(eglGetProcAddress(name)):reinterpret_cast<GLFWglproc>(SDL_GL_GetProcAddress(name));}
int glfwExtensionSupported(const char* extension){
    if(current && current->offscreen){
        if(!extension || !*extension || std::strchr(extension,' '))return 0;
        GLint count=0;glGetIntegerv(GL_NUM_EXTENSIONS,&count);
        for(GLint i=0;i<count;++i)if(const auto* value=glGetStringi(GL_EXTENSIONS,GLuint(i));value && std::strcmp(reinterpret_cast<const char*>(value),extension)==0)return 1;
        return 0;
    }
    return SDL_GL_ExtensionSupported(extension);
}
void glfwSwapBuffers(GLFWwindow* w){if(w && !w->vulkan){if(w->offscreen)glFlush();else if(w->native && w->focused && !lostGraphics)SDL_GL_SwapWindow(w->native);}}
const char** glfwGetRequiredInstanceExtensions(uint32_t* count){
    static std::vector<const char*> names;
    unsigned n=0;
    if(!current || !current->vulkan || !SDL_Vulkan_GetInstanceExtensions(current->native,&n,nullptr)){if(count)*count=0;return nullptr;}
    names.resize(n);SDL_Vulkan_GetInstanceExtensions(current->native,&n,names.data());
    if(count)*count=n;return names.data();
}
VkResult glfwCreateWindowSurface(VkInstance instance,GLFWwindow* w,const VkAllocationCallbacks*,VkSurfaceKHR* surface){
    if(!w || !w->vulkan || !SDL_Vulkan_CreateSurface(w->native,instance,surface))return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}
void glfwSwapInterval(int value){if(!current || !current->offscreen)SDL_GL_SetSwapInterval(value);}
void glfwPollEvents(){
    installIme();detectImeDismissal();
    SDL_Event ev;while(SDL_PollEvent(&ev))event(ev);
    retainGraphicsContext(current);
}
void glfwWaitEventsTimeout(double seconds){SDL_Event ev;if(SDL_WaitEventTimeout(&ev,int(std::clamp(seconds,0.0,1.0)*1000)))event(ev);glfwPollEvents();}
int glfwWindowShouldClose(GLFWwindow* w){return w?w->close:0;}
void glfwSetWindowShouldClose(GLFWwindow* w,int value){if(w)w->close=value!=0;}
void glfwGetFramebufferSize(GLFWwindow* w,int* width,int* height){int a=0,b=0;if(w){if(w->offscreen){a=w->width;b=w->height;}else if(w->vulkan)SDL_Vulkan_GetDrawableSize(w->native,&a,&b);else SDL_GL_GetDrawableSize(w->native,&a,&b);}if(width)*width=a;if(height)*height=b;}
void glfwGetWindowSize(GLFWwindow* w,int* width,int* height){int a=0,b=0;if(w){if(w->offscreen){a=w->width;b=w->height;}else{updateUiScale(w);SDL_GetWindowSize(w->native,&a,&b);a=int(std::round(a/w->uiScale));b=int(std::round(b/w->uiScale));}}if(width)*width=a;if(height)*height=b;}
void glfwGetWindowContentScale(GLFWwindow* w,float* x,float* y){updateUiScale(w);const auto scale=coordinateScale(w);if(x)*x=scale.x;if(y)*y=scale.y;}
void glfwGetWindowPos(GLFWwindow*,int* x,int* y){if(x)*x=0;if(y)*y=0;}
void glfwSetWindowSize(GLFWwindow*,int,int){}
void glfwSetWindowSizeLimits(GLFWwindow*,int,int,int,int){}
void glfwSetWindowPos(GLFWwindow*,int,int){}
GLFWmonitor* glfwGetWindowMonitor(GLFWwindow*){return &monitor;}
GLFWmonitor* glfwGetPrimaryMonitor(){return &monitor;}
GLFWmonitor** glfwGetMonitors(int* count){if(count)*count=1;return monitors;}
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*){static GLFWvidmode result;result=mode();return &result;}
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* m,int* count){if(count)*count=1;return glfwGetVideoMode(m);}
void glfwGetMonitorPos(GLFWmonitor*,int* x,int* y){if(x)*x=0;if(y)*y=0;}
void glfwGetMonitorWorkarea(GLFWmonitor*,int* x,int* y,int* width,int* height){glfwGetMonitorPos(nullptr,x,y);auto m=mode();if(width)*width=m.width;if(height)*height=m.height;}
void glfwSetWindowMonitor(GLFWwindow*,GLFWmonitor*,int,int,int,int,int){}
void glfwSetWindowIcon(GLFWwindow*,int,const GLFWimage*){}
void glfwSetWindowAttrib(GLFWwindow*,int,int){}
int glfwGetWindowAttrib(GLFWwindow* w,int attr){if(!w)return 0;if(attr==GLFW_FOCUSED)return w->focused;if(attr==GLFW_ICONIFIED)return !w->focused;if(attr==GLFW_VISIBLE)return w->visible;return 0;}
// (the game's process: its window shown = the game's activity, hidden = the launcher's. The launcher's process hides
// its window while a game runs, which the game's activity covers by itself, and shows it when the game has ended: its
// own activity back to the front. It never starts the game's activity: one started without its session ends at once.)
void glfwShowWindow(GLFWwindow* w){
    if(!w)return;
    w->visible=true;
    if(w->offscreen)return;
    if(launcherProcess())cane::android::showLauncherWindow();
    else cane::android::showGame();
}
void glfwHideWindow(GLFWwindow* w){if(w){w->visible=false;if(!w->offscreen && !launcherProcess())cane::android::showLauncher();}}
void glfwIconifyWindow(GLFWwindow*){}
void glfwRestoreWindow(GLFWwindow*){}
void glfwFocusWindow(GLFWwindow*){}
double glfwGetTime(){return std::chrono::duration<double>(std::chrono::steady_clock::now()-origin).count()+timeOffset;}
void glfwSetTime(double value){origin=std::chrono::steady_clock::now();timeOffset=value;}
int glfwGetError(const char** detail){if(detail)*detail=lastError.empty()?nullptr:lastError.c_str();return std::exchange(lastCode,0);}
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun f){return std::exchange(errorCallback,f);}
GLFWmonitorfun glfwSetMonitorCallback(GLFWmonitorfun f){return std::exchange(monitorCallback,f);}
GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun f){return std::exchange(joystickCallback,f);}
int glfwGetKey(GLFWwindow* w,int key){return w&&key>=0&&key<=GLFW_KEY_LAST&&w->focused?w->keys[key]:0;}
const char* glfwGetKeyName(int key,int){static char result[2]{};if(key>=32&&key<127){result[0]=char(key);return result;}return nullptr;}
int glfwGetMouseButton(GLFWwindow* w,int button){return w&&button>=0&&button<int(w->mouse.size())?w->mouse[button]:0;}
void glfwGetCursorPos(GLFWwindow* w,double* x,double* y){if(x)*x=w?w->x:0;if(y)*y=w?w->y:0;}
void glfwSetCursorPos(GLFWwindow* w,double x,double y){if(w){w->x=x;w->y=y;const auto scale=coordinateScale(w);if(!w->offscreen)SDL_WarpMouseInWindow(w->native,int(x*scale.x),int(y*scale.y));}}
int glfwGetInputMode(GLFWwindow* w,int mode){return w&&mode==GLFW_CURSOR?w->cursorMode:0;}
void glfwSetInputMode(GLFWwindow* w,int mode,int value){if(w&&mode==GLFW_CURSOR){w->cursorMode=value;if(!w->offscreen){SDL_SetRelativeMouseMode(value==GLFW_CURSOR_DISABLED?SDL_TRUE:SDL_FALSE);SDL_ShowCursor(value==GLFW_CURSOR_NORMAL);}}}
GLFWcursor* glfwCreateStandardCursor(int shape){auto* c=new GLFWcursor;c->native=SDL_CreateSystemCursor(shape==GLFW_HAND_CURSOR?SDL_SYSTEM_CURSOR_HAND:shape==GLFW_IBEAM_CURSOR?SDL_SYSTEM_CURSOR_IBEAM:SDL_SYSTEM_CURSOR_ARROW);return c;}
void glfwDestroyCursor(GLFWcursor* c){if(c){SDL_FreeCursor(c->native);delete c;}}
void glfwSetCursor(GLFWwindow*,GLFWcursor* c){SDL_SetCursor(c?c->native:SDL_GetDefaultCursor());}
void glfwSetClipboardString(GLFWwindow*,const char* text){SDL_SetClipboardText(text);}
const char* glfwGetClipboardString(GLFWwindow*){char* text=SDL_GetClipboardText();clipboard=text?text:"";SDL_free(text);return clipboard.c_str();}
int glfwJoystickPresent(int jid){if(jid==GLFW_JOYSTICK_LAST)return cane::android::touchState().active;return jid>=0&&jid<GLFW_JOYSTICK_LAST&&pads[jid].controller&&SDL_GameControllerGetAttached(pads[jid].controller);}
int glfwJoystickIsGamepad(int jid){return glfwJoystickPresent(jid);}
const char* glfwGetJoystickName(int jid){if(jid==GLFW_JOYSTICK_LAST)return cane::input::kTouchPadName.data();return glfwJoystickPresent(jid)?SDL_GameControllerName(pads[jid].controller):nullptr;}
const char* glfwGetGamepadName(int jid){return glfwGetJoystickName(jid);}
const char* glfwGetJoystickGUID(int jid){if(jid==GLFW_JOYSTICK_LAST)return "00000000000000000000000000000000";return glfwJoystickPresent(jid)?pads[jid].guid.c_str():nullptr;}
int glfwUpdateGamepadMappings(const char* text){SDL_RWops* data=SDL_RWFromConstMem(text,int(std::strlen(text)));return data && SDL_GameControllerAddMappingsFromRW(data,1)>=0;}
int glfwGetGamepadState(int jid,GLFWgamepadstate* state){
    if(!state||!glfwJoystickPresent(jid))return 0;
    if(jid==GLFW_JOYSTICK_LAST){auto touch=cane::android::touchState();std::copy(touch.axes.begin(),touch.axes.end(),state->axes);std::copy(touch.buttons.begin(),touch.buttons.end(),state->buttons);return 1;}
    auto& p=pads[jid];
    constexpr SDL_GameControllerButton buttons[]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_BACK,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_GUIDE,SDL_CONTROLLER_BUTTON_LEFTSTICK,SDL_CONTROLLER_BUTTON_RIGHTSTICK,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT};
    for(int i=0;i<15;++i)state->buttons[i]=SDL_GameControllerGetButton(p.controller,buttons[i]);
    for(int i=0;i<6;++i){float v=SDL_GameControllerGetAxis(p.controller,SDL_GameControllerAxis(i));state->axes[i]=i<4?std::clamp(v/32767.f,-1.f,1.f):std::clamp(v/16383.5f-1.f,-1.f,1.f);}
    return 1;
}
const float* glfwGetJoystickAxes(int jid,int* count){static GLFWgamepadstate s{};bool ok=glfwGetGamepadState(jid,&s);if(count)*count=ok?6:0;return ok?s.axes:nullptr;}
const unsigned char* glfwGetJoystickButtons(int jid,int* count){static GLFWgamepadstate s{};bool ok=glfwGetGamepadState(jid,&s);if(count)*count=ok?15:0;return ok?s.buttons:nullptr;}
int glfwRawMouseMotionSupported(){return 1;} // (SDL's relative mouse mode, GLFW_CURSOR_DISABLED)
const unsigned char* glfwGetJoystickHats(int,int* count){if(count)*count=0;return nullptr;} // (SDL's mapping reports the D-pad as buttons)
#define CALLBACK(Type,Name,Field) Type Name(GLFWwindow* w,Type f){return w?std::exchange(w->Field,f):nullptr;}
CALLBACK(GLFWframebuffersizefun,glfwSetFramebufferSizeCallback,framebuffer)
CALLBACK(GLFWwindowsizefun,glfwSetWindowSizeCallback,size)
CALLBACK(GLFWwindowposfun,glfwSetWindowPosCallback,position)
CALLBACK(GLFWwindowfocusfun,glfwSetWindowFocusCallback,focus)
CALLBACK(GLFWcursorposfun,glfwSetCursorPosCallback,cursor)
CALLBACK(GLFWcursorenterfun,glfwSetCursorEnterCallback,enter)
CALLBACK(GLFWmousebuttonfun,glfwSetMouseButtonCallback,button)
CALLBACK(GLFWscrollfun,glfwSetScrollCallback,scroll)
CALLBACK(GLFWkeyfun,glfwSetKeyCallback,key)
CALLBACK(GLFWcharfun,glfwSetCharCallback,character)
CALLBACK(GLFWdropfun,glfwSetDropCallback,drop)
#undef CALLBACK
}
