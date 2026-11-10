// See moviegl.h.

#include "moviegl.h"

#include "display.h"
#include "gl.h"
#include "hud.h"
#include "skipcut.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>

#include <cstdio>
#include <cstdlib>

namespace
{
GLuint s_vao = 0;
GLSHADER s_shader;

double MovieInputTime()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

void BeginMoviePresentation(MOVIEGL& m)
{
    ResetMovieSkip();
    m.fAwaitShortcutRelease = true;
    m.tInput = MovieInputTime();
}

bool FMovieShortcutSkip(MOVIEGL& m, bool fHeld)
{
    if (!fHeld) m.fAwaitShortcutRelease = false;
    return fHeld && !m.fAwaitShortcutRelease;
}

void DrawMoviePicture(MOVIEGL& m, const uint8_t* prgba, int cx, int cy)
{
    if (!s_vao)
    {
        glGenVertexArrays(1, &s_vao);
        s_shader.Init("screen.vert", "screen.frag");
    }
    if (!m.tex || m.cx != cx || m.cy != cy)
    {
        if (m.tex) glDeleteTextures(1, &m.tex);
        glGenTextures(1, &m.tex);
        glBindTexture(GL_TEXTURE_2D, m.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cx, cy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m.cx = cx;
        m.cy = cy;
    }
    glBindTexture(GL_TEXTURE_2D, m.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cx, cy, GL_RGBA, GL_UNSIGNED_BYTE, prgba);
    // letterboxed to the PS2's 4:3 display
    const int w = g_gl.renderWidth, h = g_gl.renderHeight;
    const double aspect = 4.0 / 3.0;
    int vx = 0, vy = 0, vw = w, vh = h;
    if (w > h * aspect) { vw = int(h * aspect); vx = (w - vw) / 2; }
    else { vh = int(w / aspect); vy = (h - vh) / 2; }
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.FboScene());
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vx, vy, vw, vh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    s_shader.Use();
    glUniform1i(s_shader.Loc("tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glViewport(0, 0, w, h);
}

bool PresentMovieGl(const uint8_t* prgba, int cx, int cy, void* pv)
{
    MOVIEGL& m = *static_cast<MOVIEGL*>(pv);
    if (m.fWindowed)
    {
        glfwPollEvents();
        GLFWwindow* w = g_gl.window;
        if (glfwWindowShouldClose(w)) return false;
        GLFWgamepadstate pad{};
        const bool fShortcut = glfwGetKey(w, GLFW_KEY_ESCAPE) || glfwGetKey(w, GLFW_KEY_ENTER) ||
                               glfwGetKey(w, GLFW_KEY_SPACE) ||
                               (glfwGetGamepadState(GLFW_JOYSTICK_1, &pad) && pad.buttons[GLFW_GAMEPAD_BUTTON_START]);
        if (FMovieShortcutSkip(m, fShortcut)) return false;
        // (skipcut.h) any button shows "Hold (O) to skip"; Circle held 2 s (or pressed, with Instant Skip) skips
        const double tNow = MovieInputTime();
        const float dt = float(std::clamp(tNow - m.tInput, 0.0, 0.1));
        m.tInput = tNow;
        // (tests: SLY2N_TESTKEYS during a movie: frame 1000000 + the movie's picture, the game's steps being stopped)
        extern int g_frameJoy;
        const int frameJoySaved = g_frameJoy;
        static const bool s_fTestKeys = std::getenv("SLY2N_TESTKEYS") != nullptr;
        if (s_fTestKeys) g_frameJoy = 1000000 + m.cframe;
        const bool fSkip = FMovieSkip(w, dt);
        g_frameJoy = frameJoySaved;
        if (fSkip) return false;
        // (test: SLY2N_MOVIESKIPAT=s: the hold-to-skip taken s seconds into each movie, as FMovieSkip does)
        if (static const char* s_sz = std::getenv("SLY2N_MOVIESKIPAT"); s_sz && m.cframe / m.fps >= std::atof(s_sz))
        {
            std::printf("movie: skipped by SLY2N_MOVIESKIPAT at picture %d\n", m.cframe);
            FMovieSkipTest();
            return false;
        }
    }
    if (!prgba) return true;
    DrawMoviePicture(m, prgba, cx, cy);
    if (m.fWindowed)
    {
        g_hud.Begin();
        DrawMovieSkip();
        g_hud.Flush(g_gl.renderWidth, g_gl.renderHeight);
    }
    ++m.cframe;
    if (!m.screenshot.empty() && m.cframe / m.fps >= m.tScreenshot)
    {
        g_gl.FWriteScreenshot(m.screenshot);
        std::printf("wrote %s (picture %d, %dx%d)\n", m.screenshot.c_str(), m.cframe, cx, cy);
        return false;
    }
    if (m.fWindowed)
    {
        g_gl.Present();
        cane::gl::SwapBuffers(g_gl.window);
    }
    return true;
}

int ViewMovieCommand(const std::string& arg, const std::string& screenshot, double t, int width, int height)
{
    MOVIEINFO info;
    if (!FFindMovie(arg, &info)) { std::fprintf(stderr, "no movie %s (--list-movies)\n", arg.c_str()); return 2; }
    std::string error;
    g_gl.msaa = 0;
    g_display.msaaSamples = 0;
    if (!g_gl.FInit(width, height, screenshot.empty(), "Sly2-Release movie", error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    cane::gl::SwapInterval(0);   // the movie's sound is the clock
    MOVIEGL m;
    m.screenshot = screenshot;
    m.tScreenshot = t;
    m.fWindowed = screenshot.empty();
    BeginMoviePresentation(m);
    MOVIESTATS stats;
    const bool fOk = FPlayMovie(info.fk, PresentMovieGl, &m, &stats, screenshot.empty() ? 1e9 : t + 2.0);
    std::printf("movie %u: %dx%d %.2f fps, %d pictures%s\n", info.fk.id, stats.cx, stats.cy, stats.fps, stats.cframe,
                stats.fSkipped ? " (skipped)" : "");
    g_gl.Terminate();
    return fOk ? 0 : 1;
}
