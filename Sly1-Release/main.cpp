#include "mods.h"
#include "musmod.h"
#include "texmod.h"
#include "textmod.h"
#include "prompt_glyphs.h"
#include "skipcut.h"
#include <chrono>
#include <tuple>
#include "main.h"
#include "hide.h"
#include "update.h"
#include "jump.h"
#include "pipe.h"
#include "target.h"
#include "sensor.h"
#include "save.h"
#include "fmv.h"
#include "sound.h"
#include "tv.h"
#include <algorithm>
#include "resource1.h"
#include "runtime_error.h"
#include "emitter.h"
#include "blip.h"
#include "content.h"
#include "../platform/platform.h"
#include "../platform/game_sched.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iostream>
#include <thread>
#include <stb/stb_image_write.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <mmsystem.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

bool g_fDeveloperDebugMode = false;
extern bool g_fImGuiFrameActive;
void PrintSoundDiagnostics();
void TestSaveCompatibility(const std::filesystem::path& fixtures);

namespace
{
void LoadDebugModeSetting()
{
    g_fDeveloperDebugMode = cane::debugMode();
    g_fDebugKeys = g_fDeveloperDebugMode;   // (F3: one switch, the launcher's Settings > Advanced > Debug mode)
    std::printf("debug mode: %s\n", g_fDeveloperDebugMode ? "on" : "off");
}

class FramePacer
{
public:
    using Clock = std::chrono::steady_clock;

    FramePacer()
        : m_deadline(Clock::now())
    {
#ifdef _WIN32
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is 0x2. Use the value directly
        // so this still builds with Windows SDKs that predate the named flag.
        m_timer = CreateWaitableTimerExW(
            nullptr, nullptr, 0x2, TIMER_MODIFY_STATE | SYNCHRONIZE);

        if (m_timer == nullptr)
        {
            // Older Windows lacks high-resolution timers. Without a 1 ms
            // system timer period, waits round up to 15.6 ms ticks.
            m_timer = CreateWaitableTimerExW(
                nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
            m_fTimerPeriodRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
            m_minCoarseMargin = std::chrono::milliseconds(2);
        }
#elif defined(__linux__)
        // The default 50 us timer slack would delay the short final sleep
        // past the deadline.
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
#endif
    }

    ~FramePacer()
    {
#ifdef _WIN32
        if (m_timer != nullptr)
            CloseHandle(m_timer);
        if (m_fTimerPeriodRaised)
            timeEndPeriod(1);
#endif
    }

    // fSwapPacesFrames: VSync should block each swap at the target rate.
    // swapGeneration changes whenever the swap configuration changes.
    void Wait(bool fSwapPacesFrames, unsigned swapGeneration)
    {
        if (m_frameRate != g_targetFrameRate)
        {
            m_frameRate = g_targetFrameRate;
            m_deadline = Clock::now();
        }

        if (m_swapGeneration != swapGeneration)
        {
            m_swapGeneration = swapGeneration;
            m_fSwapNotPacing = false;
            m_waitStreak = 0;
        }

        // Two gates at nearly equal rates drift against each other and
        // periodically drop a vblank. When the swap should pace, run the timer
        // 1% fast so only the swap gates. If the timer still has to wait on
        // every frame, the swap is not blocking at the target rate (clamped
        // interval, driver override, minimized window), so pace exactly until
        // the swap configuration changes.
        const bool fLenient = fSwapPacesFrames && !m_fSwapNotPacing;

        double frameSeconds = 1.0 / static_cast<double>(m_frameRate);
        if (fLenient)
            frameSeconds *= 0.99;

        const auto frameDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(frameSeconds));

        m_deadline += frameDuration;
        auto now = Clock::now();

        if (now >= m_deadline)
        {
            // Drop missed presentation deadlines. The next call schedules one
            // complete interval from this presentation rather than catching up.
            m_deadline = now;
            m_waitStreak = 0;
            return;
        }

        if (fLenient && ++m_waitStreak >= 30)
            m_fSwapNotPacing = true;

        // Waking from a deep idle state can take over a millisecond on low-end
        // CPUs, so widen the margin by the recently observed wake lateness.
        const auto coarseDeadline =
            m_deadline - m_minCoarseMargin - (std::min)(m_wakeLateness, kMaxWakeLateness);
        if (now < coarseDeadline)
        {
#ifdef _WIN32
            if (m_timer != nullptr)
            {
                const auto wait100ns = std::chrono::duration_cast<
                    std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>>(
                        coarseDeadline - now).count();

                LARGE_INTEGER dueTime{};
                dueTime.QuadPart = -(std::max<LONGLONG>)(1, wait100ns);

                if (SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(m_timer, INFINITE);
                else
                    std::this_thread::sleep_until(coarseDeadline);
            }
            else
            {
                std::this_thread::sleep_until(coarseDeadline);
            }
#else
            std::this_thread::sleep_until(coarseDeadline);
#endif
            // Decays by 1/64 per frame, about one second at 60 FPS.
            const auto lateness = Clock::now() - coarseDeadline;
            m_wakeLateness = (std::max)(lateness, m_wakeLateness - m_wakeLateness / 64);
        }

#ifdef _WIN32
        // SwitchToThread returns at once when nothing else is ready, but lets
        // driver and audio threads run instead of being starved by this
        // above-normal-priority thread.
        while (Clock::now() < m_deadline - kSpinMargin)
            SwitchToThread();
#else
        // The coarse margin covers a deep idle-state exit. This short sleep
        // is too brief for the idle governor to pick a deep state, so it wakes
        // within a few microseconds.
        if (Clock::now() < m_deadline - kSpinMargin)
            std::this_thread::sleep_until(m_deadline - kSpinMargin);
#endif

        // YieldProcessor is a CPU pause hint, not a scheduler yield. It keeps
        // the final boundary precise without giving up a scheduling quantum.
        while (Clock::now() < m_deadline)
        {
#ifdef _WIN32
            YieldProcessor();
#else
            __builtin_ia32_pause();
#endif
        }
    }

private:
    static constexpr Clock::duration kSpinMargin = std::chrono::microseconds(50);
    static constexpr Clock::duration kMaxWakeLateness = std::chrono::microseconds(1500);

#ifdef _WIN32
    HANDLE m_timer = nullptr;
    bool m_fTimerPeriodRaised = false;
    Clock::duration m_minCoarseMargin = std::chrono::microseconds(250);
#else
    Clock::duration m_minCoarseMargin = std::chrono::microseconds(200);
#endif
    Clock::duration m_wakeLateness = Clock::duration::zero();
    Clock::time_point m_deadline;
    int m_frameRate = 60;
    int m_waitStreak = 0;
    unsigned m_swapGeneration = 0;
    bool m_fSwapNotPacing = false;
};

bool s_runningFrame = false;
// Hidden test hook: --test-prompt-strip draws every controller glyph.
bool s_fPromptStripFixture = false;
// Benchmark-only: --smoke-press BTN@FRAME holds a button for a few frames
// after the smoke level is ready (e.g. L1 opens the binoculars).
// Several presses: --smoke-press BTN@FRAME[+COUNT][,...] (held COUNT frames, default 4; frames counted
// from when the smoke level is ready).
// BUTTON is a JOY_BUTTON index, or F3 (the debug window key, kSmokePressF3).
std::vector<std::tuple<int, long, long>> s_smokePresses; // button, frame, count
constexpr int kSmokePressF3 = -3;
long s_smokeLevelFrame = -1;
// Benchmark-only: capture the presented window image (not just the scene).
std::string s_smokeWindowShot;
bool s_smokeWindowShotNow = false;
// --smoke-require-picture: the last smoke frame's scene and composited window
// image (mean of R+G+B, 0..765), read before the swap.
bool s_presentProbeNow = false;
double s_presentSceneMean = -1.0;
double s_presentWindowMean = -1.0;

double MeanOfReadPixels(int w, int h)
{
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    double sum = 0.0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
    return pixels.empty() ? 0.0 : sum / (pixels.size() / 4);
}

void ProbePresent()
{
    s_presentProbeNow = false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    s_presentSceneMean = MeanOfReadPixels(g_gl.renderWidth, g_gl.renderHeight);
    int w = 0, h = 0;
    glfwGetFramebufferSize(g_gl.window, &w, &h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    s_presentWindowMean = MeanOfReadPixels(w, h);
}

void FlushTransitionFrame()
{
    // The previous game frame has already been presented. Drain outstanding
    // GPU work and leave that front-buffer image visible while the synchronous
    // world transition runs. This is the original lightweight desktop path:
    // no extra framebuffer allocation, copy, redraw, or swap.
    glFinish();
}

void RunGameFrame()
{
    if (s_runningFrame)
        return;

    s_runningFrame = true;

    // Retail leaves the movie queued while the outgoing wipe is visible.
    // Once the screen reaches black, play the movie before executing the
    // transition that replaces the current world.
    if (FCutscenePending() && g_wipe.wipes != WIPES_WipingOut)
    {
        ExecutePendingCutscenes();
    }

    if (g_transition.m_fPending != 0)
    {
		FlushTransitionFrame();
		g_transition.Execute(file);
    }

    if (FCutscenePending() && g_wipe.wipes != WIPES_WipingOut)
    {
        ExecutePendingCutscenes();
    }

    static bool s_fF3WasDown = false;
	// (F3, the debug window: only with the launcher's Settings > Advanced > Debug mode on, Debug Mode.ini)
	// (--smoke-press F3@FRAME holds it in a test run, still only with Debug mode on)
	bool fF3Test = false;
	for (const auto& [button, frame, count] : s_smokePresses)
		if (button == kSmokePressF3 && s_smokeLevelFrame >= frame && s_smokeLevelFrame < frame + count) fF3Test = true;
	const bool fF3Down = g_fDebugKeys && g_gl.window != nullptr &&
        (fF3Test || glfwGetKey(g_gl.window, GLFW_KEY_F3) == GLFW_PRESS);
    if (fF3Down && !s_fF3WasDown)
    {
        g_fDebugGuiOpen = !g_fDebugGuiOpen;

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(g_gl.window, &framebufferWidth, &framebufferHeight);
        FrameBufferSizeCallBack(g_gl.window, framebufferWidth, framebufferHeight);
    }
    s_fF3WasDown = fF3Down;

    g_sceneFbo = g_fMsaa ? g_gl.fboMSAA : g_gl.fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);

    glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
    glClearColor(rgbaSky.r, rgbaSky.g, rgbaSky.b, rgbaSky.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    g_joy.Update(g_gl.window);
    for (const auto& [button, frame, count] : s_smokePresses)
        if (button >= 0 && s_smokeLevelFrame >= frame && s_smokeLevelFrame < frame + count)
            g_joy.current[static_cast<JOY_BUTTON>(button)] = true;
    // Skip cutscenes (skipcut.h): the hold / hint, and the fast-forward that
    // runs the scene to its natural end.
    UpdateSkipCut(g_clock.dtReal);
    const bool fSkipFast = g_psw != nullptr && FSkipCutFastForward();
    RenderMenuGui(g_psw);

    if (fSkipFast)
    {
        // Many game steps this frame (each a normal step at the minimum step
        // time), no pad input, nothing drawn: the screen stays black.
        const JOY joySaved = g_joy;
        const auto tStart = std::chrono::steady_clock::now();
        do
        {
            for (int button = 0; button < BTN_MAX; ++button)
                g_joy.current[static_cast<JOY_BUTTON>(button)] = g_joy.previous[static_cast<JOY_BUTTON>(button)] = false;
            g_joy.x = g_joy.y = g_joy.x2 = g_joy.y2 = g_joy.uDeflect = g_joy.uDeflect2 = 0.0f;
            UpdateUi(&g_ui);
            UpdateGameState(g_clock.dt);
            SetupCm(g_pcm);
            OpenFrame();
            MarkClockTick(&g_clock);
            UpdateCodes();
            UpdateSw(g_psw, g_clock.dt);
        } while (FSkipCutStep() && g_psw != nullptr &&
            std::chrono::steady_clock::now() - tStart < std::chrono::milliseconds(25));
        g_joy = joySaved;
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    else if (g_psw != nullptr)
    {
        UpdateUi(&g_ui);
        UpdateGameState(g_clock.dt);

        SetupCm(g_pcm);
        OpenFrame();
        MarkClockTick(&g_clock);
        UpdateCodes();
        UpdateSw(g_psw, g_clock.dt);

        if (g_fRenderModels == true)
        {
            RenderSw(g_psw, g_pcm);
            RenderUi(&g_ui);
            DrawSw(g_psw, g_pcm);
        }

        if (g_fRenderCollision == true)
		{
            DrawSwCollisionAll(g_pcm);
		}

		if (g_fRenderDialogTriggers == true)
			DrawSwDialogTriggers(g_pcm);

        DrawUi(&g_ui);
        DrawSkipCut();
        if (s_fPromptStripFixture)
            DrawPromptStripFixture();
    }

    if (g_fMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fboMSAA);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.fbo);
        glBlitFramebuffer(0, 0, g_gl.renderWidth, g_gl.renderHeight,
            0, 0, g_gl.renderWidth, g_gl.renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(g_gl.presentX, g_gl.presentY, g_gl.presentWidth, g_gl.presentHeight);

	// Presentation is a hard OpenGL state boundary. World, shadow, particle,
	// and UI passes are allowed to use these states internally, but none of
	// them may influence the default-framebuffer clear or fullscreen composite.
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // The fullscreen composite: the finished scene (g_gl.fbc) onto the window.
    // (The 2026-11-03 upstream merge dropped these lines: the game ran and the
    // movies, which draw themselves, played, but every game frame was black.)
    glScreenShader.Use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_gl.fbc);
    glBindVertexArray(g_gl.sao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (s_presentProbeNow)
        ProbePresent();

    if (g_fImGuiFrameActive)
    {
        ImGui::Render();
        // With nothing to draw the backend would still query and restore
        // about 25 GL states, each forcing a glthread sync.
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData != nullptr && drawData->CmdListsCount > 0)
            ImGui_ImplOpenGL3_RenderDrawData(drawData);
        g_fImGuiFrameActive = false;
    }
    if (s_smokeWindowShotNow)
    {
        s_smokeWindowShotNow = false;
        int w = 0, h = 0;
        glfwGetFramebufferSize(g_gl.window, &w, &h);
        std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        stbi_flip_vertically_on_write(true);
        stbi_write_png(s_smokeWindowShot.c_str(), w, h, 4, pixels.data(), w * 4);
    }
    cane::gl::SwapBuffers(g_gl.window);

    ++g_cframe;
    s_runningFrame = false;
}

#ifdef _WIN32
WNDPROC s_glfwWindowProc = nullptr;
constexpr UINT kRunTrackedWindowFrame = WM_APP + 0x504;
std::atomic_bool s_windowTracking = false;
std::atomic_bool s_windowFrameThreadRunning = false;
HWND s_gameWindow = nullptr;
HANDLE s_windowTrackingEvent = nullptr;
std::thread s_windowFrameThread;
int s_cutsceneNcDrag = HTNOWHERE;
POINT s_cutsceneNcDragStart{};
RECT s_cutsceneNcWindowStart{};

bool BeginCutsceneNonClientDrag(HWND hwnd, int hitTest)
{
	switch (hitTest)
	{
	case HTCAPTION:
	case HTLEFT:
	case HTRIGHT:
	case HTTOP:
	case HTTOPLEFT:
	case HTTOPRIGHT:
	case HTBOTTOM:
	case HTBOTTOMLEFT:
	case HTBOTTOMRIGHT:
		break;
	default:
		return false;
	}

	s_cutsceneNcDrag = hitTest;
	GetCursorPos(&s_cutsceneNcDragStart);
	GetWindowRect(hwnd, &s_cutsceneNcWindowStart);
	SetCapture(hwnd);
	return true;
}

void UpdateCutsceneNonClientDrag(HWND hwnd)
{
	if (s_cutsceneNcDrag == HTNOWHERE)
		return;

	POINT cursor{};
	GetCursorPos(&cursor);
	const int dx = cursor.x - s_cutsceneNcDragStart.x;
	const int dy = cursor.y - s_cutsceneNcDragStart.y;
	RECT rect = s_cutsceneNcWindowStart;

	if (s_cutsceneNcDrag == HTCAPTION)
	{
		OffsetRect(&rect, dx, dy);
	}
	else
	{
		if (s_cutsceneNcDrag == HTLEFT || s_cutsceneNcDrag == HTTOPLEFT ||
			s_cutsceneNcDrag == HTBOTTOMLEFT)
			rect.left += dx;
		if (s_cutsceneNcDrag == HTRIGHT || s_cutsceneNcDrag == HTTOPRIGHT ||
			s_cutsceneNcDrag == HTBOTTOMRIGHT)
			rect.right += dx;
		if (s_cutsceneNcDrag == HTTOP || s_cutsceneNcDrag == HTTOPLEFT ||
			s_cutsceneNcDrag == HTTOPRIGHT)
			rect.top += dy;
		if (s_cutsceneNcDrag == HTBOTTOM || s_cutsceneNcDrag == HTBOTTOMLEFT ||
			s_cutsceneNcDrag == HTBOTTOMRIGHT)
			rect.bottom += dy;

		constexpr int minimumWidth = 320;
		constexpr int minimumHeight = 240;
		if (rect.right - rect.left < minimumWidth)
		{
			if (s_cutsceneNcDrag == HTLEFT || s_cutsceneNcDrag == HTTOPLEFT ||
				s_cutsceneNcDrag == HTBOTTOMLEFT)
				rect.left = rect.right - minimumWidth;
			else
				rect.right = rect.left + minimumWidth;
		}
		if (rect.bottom - rect.top < minimumHeight)
		{
			if (s_cutsceneNcDrag == HTTOP || s_cutsceneNcDrag == HTTOPLEFT ||
				s_cutsceneNcDrag == HTTOPRIGHT)
				rect.top = rect.bottom - minimumHeight;
			else
				rect.bottom = rect.top + minimumHeight;
		}
	}

	SetWindowPos(hwnd, nullptr, rect.left, rect.top,
		rect.right - rect.left, rect.bottom - rect.top,
		SWP_NOACTIVATE | SWP_NOZORDER);
}

void EndCutsceneNonClientDrag(HWND hwnd)
{
	if (s_cutsceneNcDrag == HTNOWHERE)
		return;
	s_cutsceneNcDrag = HTNOWHERE;
	if (GetCapture() == hwnd)
		ReleaseCapture();
}

void WindowFrameThreadMain()
{
	while (s_windowFrameThreadRunning.load(std::memory_order_acquire))
	{
		if (s_windowTracking.load(std::memory_order_acquire) && s_gameWindow != nullptr)
		{
			DWORD_PTR result = 0;
			SendMessageTimeoutW(s_gameWindow, kRunTrackedWindowFrame, 0, 0,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result);
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
		// Sleep until a move, resize or caption-button press begins.
		else if (s_windowTrackingEvent == nullptr ||
			WaitForSingleObject(s_windowTrackingEvent, INFINITE) != WAIT_OBJECT_0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
	}
}

void BeginWindowTracking()
{
	s_windowTracking.store(true, std::memory_order_release);
	if (s_windowTrackingEvent != nullptr)
		SetEvent(s_windowTrackingEvent);
}

LRESULT CALLBACK LiveResizeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// DefWindowProc enters a modal tracking loop for caption dragging, resizing,
	// caption buttons, and the system menu. A movie is driven by the main thread,
	// so that loop freezes its video while XAudio continues independently. During
	// an FMV, handle these interactions without entering the modal loop.
	if (FCutscenePlaying())
	{
		if (message == WM_NCLBUTTONDOWN)
		{
			const int hitTest = static_cast<int>(wParam);
			if (BeginCutsceneNonClientDrag(hwnd, hitTest))
				return 0;
			if (hitTest == HTMINBUTTON)
			{
				ShowWindow(hwnd, SW_MINIMIZE);
				return 0;
			}
			if (hitTest == HTMAXBUTTON)
			{
				ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
				return 0;
			}
			if (hitTest == HTCLOSE)
			{
				PostMessageW(hwnd, WM_CLOSE, 0, 0);
				return 0;
			}
			// Do not pass any remaining caption presses (for example the window
			// icon) to DefWindowProc while an FMV is active; they can also start a
			// modal system-menu loop.
			return 0;
		}
		else if (message == WM_NCLBUTTONDBLCLK && wParam == HTCAPTION)
		{
			ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
			return 0;
		}
		else if (message == WM_NCRBUTTONDOWN || message == WM_NCRBUTTONUP ||
			message == WM_NCMBUTTONDOWN || message == WM_NCMBUTTONUP ||
			message == WM_NCXBUTTONDOWN || message == WM_NCXBUTTONUP ||
			message == WM_CONTEXTMENU)
		{
			// The native system menu is modal too. Suppress it only while a movie
			// is active so right-clicking or holding the caption cannot stop video.
			return 0;
		}
		else if (message == WM_MOUSEMOVE && s_cutsceneNcDrag != HTNOWHERE)
		{
			UpdateCutsceneNonClientDrag(hwnd);
			return 0;
		}
		else if (message == WM_LBUTTONUP && s_cutsceneNcDrag != HTNOWHERE)
		{
			EndCutsceneNonClientDrag(hwnd);
			return 0;
		}
		else if (message == WM_CAPTURECHANGED && s_cutsceneNcDrag != HTNOWHERE)
		{
			s_cutsceneNcDrag = HTNOWHERE;
		}
		else if (message == WM_CANCELMODE && s_cutsceneNcDrag != HTNOWHERE)
		{
			EndCutsceneNonClientDrag(hwnd);
			return 0;
		}
	}
	else if (s_cutsceneNcDrag != HTNOWHERE)
	{
		EndCutsceneNonClientDrag(hwnd);
	}

	// Any mouse hold in the non-client area can enter a private Windows tracking
	// loop before GLFW receives another event. Start pumping immediately instead
	// of waiting for WM_ENTERSIZEMOVE or WM_ENTERMENULOOP. This covers caption
	// buttons, title-bar dragging, and holding right-click before the system menu
	// is shown, while still letting GLFW's original procedure own the behavior.
	if (message == WM_NCLBUTTONDOWN ||
		message == WM_NCRBUTTONDOWN ||
		message == WM_NCMBUTTONDOWN)
	{
		BeginWindowTracking();
		const LRESULT result = CallWindowProcW(s_glfwWindowProc, hwnd, message, wParam, lParam);
		s_windowTracking.store(false, std::memory_order_release);
		return result;
	}

    switch (message)
    {
        case WM_ENTERSIZEMOVE:
		case WM_ENTERMENULOOP:
		BeginWindowTracking();
		break;

        case kRunTrackedWindowFrame:
        if (s_windowTracking.load(std::memory_order_acquire))
        {
            RunGameFrame();
            return 0;
        }
        break;

        case WM_EXITSIZEMOVE:
		case WM_EXITMENULOOP:
		s_windowTracking.store(false, std::memory_order_release);
		break;

        case WM_DESTROY:
		s_windowTracking.store(false, std::memory_order_release);
        break;
    }

    return CallWindowProcW(s_glfwWindowProc, hwnd, message, wParam, lParam);
}

void InstallLiveResizeWindowProc()
{
    HWND hwnd = glfwGetWin32Window(g_gl.window);
    if (hwnd == nullptr)
        return;

	HINSTANCE instance = GetModuleHandleW(nullptr);
	HICON largeIcon = static_cast<HICON>(LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
	HICON smallIcon = static_cast<HICON>(LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
	if (largeIcon != nullptr)
		SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
	if (smallIcon != nullptr)
		SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    s_glfwWindowProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(LiveResizeWindowProc));
	s_gameWindow = hwnd;
	s_windowTrackingEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	s_windowFrameThreadRunning.store(true, std::memory_order_release);
	s_windowFrameThread = std::thread(WindowFrameThreadMain);
}

void ShutdownLiveResizeWindowProc()
{
	s_windowTracking.store(false, std::memory_order_release);
	s_windowFrameThreadRunning.store(false, std::memory_order_release);
	if (s_windowTrackingEvent != nullptr)
		SetEvent(s_windowTrackingEvent);
	if (s_windowFrameThread.joinable())
		s_windowFrameThread.join();
	if (s_windowTrackingEvent != nullptr)
	{
		CloseHandle(s_windowTrackingEvent);
		s_windowTrackingEvent = nullptr;
	}
}
#endif

bool s_fSwapIntervalStale = false;

void WindowPosCallback(GLFWwindow*, int, int)
{
    // The window may now be on a monitor with another refresh rate.
    s_fSwapIntervalStale = true;
}
}

#if defined(NDEBUG) && defined(_WIN32) && !defined(CANE_PORTABLE_SHADERS)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
#else
int main(int cphzArgs, char* aphzArgs[])
#endif
{
    try {
    // The launcher reads output through a pipe, where stdio would otherwise
    // buffer whole blocks and lose the tail of the log if the game crashes.
#ifdef _WIN32
    std::setvbuf(stdout, nullptr, _IONBF, 0); // MSVC treats _IOLBF as full buffering.
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
#endif
    double smokeSeconds=0;std::string smokeLevel;std::filesystem::path screenshotPath;
    // Benchmark helpers: --smoke-fixed-step makes runs reproducible,
    // --smoke-gpu-timing adds timer queries, and --smoke-screenshot-frame
    // captures a given frame counted from when the requested level is ready.
    std::string smokeWindow;
    bool smokeGpuTiming=false,smokeRequirePicture=false;long smokeShotFrame=-1;int smokeFps=0;
#if defined(NDEBUG) && defined(_WIN32) && !defined(CANE_PORTABLE_SHADERS)
    auto args=cane::arguments(0, nullptr);
#else
    auto args=cane::arguments(cphzArgs, aphzArgs);
#endif
    if(args.size()==2 && args[0]=="--test-save-compatibility"){TestSaveCompatibility(cane::path(args[1]));TestPromptGlyphs();std::cout<<"Save compatibility fixtures passed"<<std::endl;return 0;}
    if(args.size()==1 && args[0]=="--test-prompt-glyphs"){TestPromptGlyphs();std::cout<<"Prompt glyph checks passed"<<std::endl;return 0;}
    // The music tools need no game data (musmod.h).
    if(const int toolResult=RunMusicTool(args);toolResult>=0)return toolResult;
    // Mods (mods.h, texmod.h, textmod.h): --mods DIR, $SLY1_MODS, else <user dir>/Mods; an empty value: none.
    std::string modsDir;bool modsGiven=false;std::filesystem::path dumpTexturesDir,dumpTextDir;
    std::vector<std::string> runtimeArgs;std::string ps3Source,ps3Out;
    for(size_t i=0;i<args.size();++i) {
        if(args[i]=="--smoke-fixed-step"){g_dtFixedStep=1.0f/60.0f;continue;}
        if(args[i]=="--mods"||args[i]=="--dump-text"||args[i]=="--dump-textures") {
            auto option=args[i];if(++i==args.size())throw std::runtime_error("Missing value for "+option);
            if(option=="--mods"){modsDir=args[i];modsGiven=true;}
            else if(option=="--dump-text")dumpTextDir=cane::path(args[i]);
            else {
                dumpTexturesDir=cane::path(args[i]);
                // --dump-textures DIR LEVEL: load that level, dump what it draws, and quit
                if(i+1<args.size()&&args[i+1].rfind("--",0)!=0){smokeLevel=args[++i];if(smokeSeconds<=0)smokeSeconds=120;}
            }
            continue;
        }
        if(args[i]=="--texture-pack"){if(++i==args.size())throw std::runtime_error("Missing value for --texture-pack");SetTexturePackDirectory(cane::path(args[i]));continue;}
        // --ps3-hd-textures SOURCE OUT: the PS3 HD texture pack from The Sly Collection (ps3hd.cpp)
        if(args[i]=="--ps3-hd-textures"){if(i+2>=args.size())throw std::runtime_error("Usage: --ps3-hd-textures SOURCE OUT");ps3Source=args[i+1];ps3Out=args[i+2];i+=2;continue;}
        if(args[i]=="--test-prompt-strip"){s_fPromptStripFixture=true;continue;}
        // the run fails unless its last frame shows a picture in the window (the scene composited: tools/sly1-boot-check.sh)
        if(args[i]=="--smoke-require-picture"){smokeRequirePicture=true;continue;}
        if(args[i]=="--smoke-gpu-timing"){smokeGpuTiming=true;continue;}
        if(args[i]=="--smoke-window-shot"||args[i]=="--smoke-window") {
            auto option=args[i];if(++i==args.size())throw std::runtime_error("Missing value for "+option);
            if(option=="--smoke-window-shot")s_smokeWindowShot=args[i];else smokeWindow=args[i];
            continue;
        }
        if(args[i]=="--smoke-press") {
            if(++i==args.size())throw std::runtime_error("Missing value for --smoke-press");
            for(size_t start=0;start<args[i].size();) {
                const size_t end=std::min(args[i].find(',',start),args[i].size());
                const std::string press=args[i].substr(start,end-start);
                auto at=press.find('@');if(at==std::string::npos)throw std::runtime_error("--smoke-press expects BUTTON@FRAME[+COUNT][,...]");
                const auto plus=press.find('+',at);
                s_smokePresses.emplace_back(press.substr(0,at)=="F3"?kSmokePressF3:std::stoi(press.substr(0,at)),std::stol(press.substr(at+1,plus-at-1)),plus==std::string::npos?4L:std::stol(press.substr(plus+1)));
                start=end+1;
            }
            continue;
        }
        if(args[i]=="--smoke-screenshot-frame"||args[i]=="--smoke-fps") {
            auto option=args[i];if(++i==args.size())throw std::runtime_error("Missing value for "+option);
            if(option=="--smoke-fps")smokeFps=std::stoi(args[i]);else smokeShotFrame=std::stol(args[i]);
            continue;
        }
        if(args[i]=="--smoke-test-seconds"||args[i]=="--smoke-level"||args[i]=="--screenshot") {
            auto option=args[i];if(++i==args.size())throw std::runtime_error("Missing value for "+option);
            if(option=="--smoke-test-seconds") {smokeSeconds=std::stod(args[i]);if(!std::isfinite(smokeSeconds)||smokeSeconds<=0||smokeSeconds>600)throw std::runtime_error("Smoke duration must be 1..600 seconds");}
            else if(option=="--smoke-level")smokeLevel=args[i];else screenshotPath=cane::path(args[i]);
        }else runtimeArgs.push_back(args[i]);
    }
    InstallRuntimeErrorHandlers();
    cane::configureRuntime(runtimeArgs);
    if(!modsGiven){if(const char* env=std::getenv("SLY1_MODS"))modsDir=env;else modsDir=cane::utf8(cane::userDirectory()/"Mods");}
    SetModsDirectory(cane::path(modsDir));
    RefreshContentPackage(cane::dataDirectory());
    if(!dumpTextDir.empty())return DumpTextCommand(dumpTextDir);
    if(!ps3Out.empty())return Ps3HdTexturesCommand(cane::dataDirectory(),ps3Source,ps3Out);
    if(!dumpTexturesDir.empty())SetTextureDumpDirectory(dumpTexturesDir);
    Startup();
    if(!smokeLevel.empty()) {
        if(!PlevelinfoFromLevelName(smokeLevel))throw std::runtime_error("Unknown smoke-test level: "+smokeLevel);

    }
    if(smokeFps>0)g_targetFrameRate=smokeFps;
    auto smokeStart=std::chrono::steady_clock::now();size_t smokeFrames=0;bool screenshotWritten=false,controllerSeen=false,smokeWarpRequested=false;size_t buttonPresses=0;std::vector<double> frameTimes;auto frameStart=smokeStart;
    std::vector<double> workTimes,gpuTimes;long levelFrames=-1;auto warpTime=smokeStart;
    GLuint gpuQueries[4]{};bool gpuQueryPending[4]{};if(smokeGpuTiming)glGenQueries(4,gpuQueries);
    auto writeScreenshot=[&](const std::filesystem::path& file){
        int w=g_gl.renderWidth,h=g_gl.renderHeight;
        std::vector<unsigned char> pixels(static_cast<size_t>(w)*h*4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,g_gl.fbo);glReadBuffer(GL_COLOR_ATTACHMENT0);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());glBindFramebuffer(GL_READ_FRAMEBUFFER,0);glReadBuffer(GL_BACK);
        stbi_flip_vertically_on_write(true);
        if(!stbi_write_png(cane::utf8(file).c_str(),w,h,4,pixels.data(),w*4))throw std::runtime_error("Cannot write screenshot");
    };
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    cane::sched::StartupGameScheduling();   // (Windows: 1 ms timer, no power throttling, MMCSS "Games": platform/game_sched.h)
    FramePacer framePacer;
    glfwSetWindowPosCallback(g_gl.window, WindowPosCallback);
#ifdef _WIN32
		InstallLiveResizeWindowProc();
#endif

    while (!glfwWindowShouldClose(g_gl.window) && fQuitGame != true)
    {
        const bool levelReady=smokeWarpRequested&&!g_transition.m_fPending&&g_wipe.wipes==WIPES_Idle&&g_psw!=nullptr&&g_transition.m_plevelCurrent==PlevelinfoFromLevelName(smokeLevel);
        if(levelReady&&levelFrames>=30&&!dumpTexturesDir.empty()){std::cout<<"textures: "<<smokeLevel<<" dumped"<<std::endl;break;}
        if(levelReady&&levelFrames<0)std::cout<<"LOAD: warp_to_ready_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warpTime).count()<<std::endl;
        if(levelReady)++levelFrames;
        // (without --smoke-level, presses and the window shot count frames from the start)
        s_smokeLevelFrame=smokeLevel.empty()?static_cast<long>(smokeFrames):levelFrames;
        if((levelReady||smokeLevel.empty())&&smokeShotFrame>=0&&s_smokeLevelFrame+1==smokeShotFrame&&!s_smokeWindowShot.empty())s_smokeWindowShotNow=true;
        if(!smokeWindow.empty()&&smokeFrames==30){auto x=smokeWindow.find('x');glfwSetWindowSize(g_gl.window,std::stoi(smokeWindow.substr(0,x)),std::stoi(smokeWindow.substr(x+1)));}
        const int gpuSlot=static_cast<int>(smokeFrames%4);
        if(smokeGpuTiming&&gpuQueryPending[gpuSlot]){GLuint64 ns=0;glGetQueryObjectui64v(gpuQueries[gpuSlot],GL_QUERY_RESULT,&ns);if(levelFrames>0)gpuTimes.push_back(ns/1.0e6);gpuQueryPending[gpuSlot]=false;}
        if(smokeGpuTiming)glBeginQuery(GL_TIME_ELAPSED,gpuQueries[gpuSlot]);
        const auto workStart=std::chrono::steady_clock::now();
        if(smokeRequirePicture&&smokeSeconds>0&&smokeFrames>=120&&std::chrono::duration<double>(workStart-smokeStart).count()>=smokeSeconds-0.5)s_presentProbeNow=true;
		RunGameFrame();
		glfwPollEvents();
        if(smokeGpuTiming){glEndQuery(GL_TIME_ELAPSED);gpuQueryPending[gpuSlot]=true;}
        if(levelFrames>0)workTimes.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-workStart).count());
        if(levelReady&&smokeShotFrame>=0&&levelFrames==smokeShotFrame&&!screenshotPath.empty()){writeScreenshot(screenshotPath);screenshotWritten=true;}
        // SLY1_SHOTEVERY=DIR:N (like Sly 2's SLY2N_SHOTEVERY): the scene every N frames as DIR/fFRAME.png, with a log line
        static const std::string shotEvery=[]{const char* v=std::getenv("SLY1_SHOTEVERY");return std::string(v?v:"");}();
        if(!shotEvery.empty()) {
            const auto colon=shotEvery.rfind(':');
            const long every=colon==std::string::npos?300:std::max(1L,std::atol(shotEvery.c_str()+colon+1));
            if(smokeFrames%every==0) {
                writeScreenshot(cane::path(shotEvery.substr(0,colon))/("f"+std::to_string(smokeFrames)+".png"));
                std::cout<<"SHOT: frame="<<smokeFrames<<" t="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-smokeStart).count()<<" world="<<g_transition.m_worldCurrent<<" uis="<<g_ui.uis<<" wipes="<<g_wipe.wipes<<std::endl;
            }
        }

        if (s_fSwapIntervalStale)
        {
            s_fSwapIntervalStale = false;
            if (g_fVsync)
                ApplySwapInterval();
        }

        // The pacer runs with VSync on or off, so the game's update rate stays
        // at the target even when the swap does not block.
        framePacer.Wait(g_fSwapPacesFrames, g_swapGeneration);
        ++smokeFrames;
        if(!smokeLevel.empty() && !smokeWarpRequested && smokeFrames>=120 && !g_transition.m_fPending && g_wipe.wipes==WIPES_Idle) {WipeToWorldWarp(smokeLevel,(OID)-1,WIPEK_Fade);smokeWarpRequested=true;warpTime=std::chrono::steady_clock::now();}
        auto now=std::chrono::steady_clock::now();
        if(smokeSeconds>0){controllerSeen=controllerSeen||g_joy.gamepadConnected;for(int b=0;b<BTN_MAX;++b)if(g_joy.current[static_cast<JOY_BUTTON>(b)]&&!g_joy.previous[static_cast<JOY_BUTTON>(b)])++buttonPresses;frameTimes.push_back(std::chrono::duration<double,std::milli>(now-frameStart).count());}frameStart=now;
        if(!screenshotPath.empty() && !screenshotWritten && smokeShotFrame<0 && smokeFrames>=120 && (smokeSeconds<=0 || std::chrono::duration<double>(now-smokeStart).count()>=smokeSeconds-1)) {
            writeScreenshot(screenshotPath);
            screenshotWritten=true;
        }
        if(smokeSeconds>0 && smokeFrames>=120 && std::chrono::duration<double>(std::chrono::steady_clock::now()-smokeStart).count()>=smokeSeconds) {
            std::cout<<"SMOKE: frames="<<smokeFrames<<" world="<<g_transition.m_worldCurrent<<" seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-smokeStart).count()<<std::endl;
            if(g_psw==nullptr || smokeFrames<2)throw std::runtime_error("Smoke test did not load a playable world");
            if(!smokeLevel.empty() && g_transition.m_plevelCurrent!=PlevelinfoFromLevelName(smokeLevel))throw std::runtime_error("Smoke test did not reach requested world");
            if(smokeRequirePicture) {
                std::cout<<"PRESENT: scene_mean="<<s_presentSceneMean<<" window_mean="<<s_presentWindowMean<<std::endl;
                // (mean of R+G+B: a dark night scene is about 100, black 0; the window adds its borders and gamma)
                if(s_presentSceneMean<15.0)throw std::runtime_error("Smoke test: the scene is black");
                if(s_presentWindowMean<s_presentSceneMean*0.25)throw std::runtime_error("Smoke test: the scene is drawn but the window stays black");
            }
            PrintSoundDiagnostics();
            std::sort(frameTimes.begin(),frameTimes.end());std::cout<<"INPUT: controller="<<controllerSeen<<" presses="<<buttonPresses<<" FRAME: median_ms="<<frameTimes[frameTimes.size()/2]<<" p95_ms="<<frameTimes[frameTimes.size()*95/100]<<std::endl;
            auto report=[](const char* label,std::vector<double>& v){if(v.empty())return;std::sort(v.begin(),v.end());double sum=0;for(double x:v)sum+=x;std::cout<<label<<": frames="<<v.size()<<" mean_ms="<<sum/v.size()<<" median_ms="<<v[v.size()/2]<<" p95_ms="<<v[v.size()*95/100]<<" p99_ms="<<v[v.size()*99/100]<<std::endl;};
            report("WORK",workTimes);report("GPU",gpuTimes);
            fQuitGame=true;
        }

		}

#ifdef _WIN32
		ShutdownLiveResizeWindowProc();
#endif

		if (g_psw != nullptr)
			DeleteWorld(g_psw);

    ShutdownSound();
    ShutdownControllerInput();
    FreeTvGL();
    g_gl.TerminateGL();
    return 0;
    } catch (const std::exception& e) { ReportFatalRuntimeError(e.what()); std::cerr << "SafeHouse: " << e.what() << std::endl; return 1; }
}


void Startup()
{
	// Debug mode is intentionally sampled once. Editing the INI while running
	// takes effect only after restarting the program.
	LoadDebugModeSetting();
    g_gl.InitGL();
    StartupControllerInput();
    ApplySwapInterval();

    std::cout << "Sly Cooper 2002 Sony Computer Entertainment America & Sucker Punch Productions\n";
    SetPhase(PHASE_Startup);

    StartupClock();
    StartupBrx();
    StartupScreen();
    StartupUi();
    StartupFrame();
    StartupTarget();
    StartupJmt();
    StartupPipe();
    StartupHide();
    StartupSaveData(&g_saveData);
    StartupGame();
    StartupSound();
    StartupCodes();
}

bool fQuitGame;
