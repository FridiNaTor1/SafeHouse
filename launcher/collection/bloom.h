#pragma once
// The glow of the Collection Style Launcher's scene, after the PS3 menu's own bloom (Levels.xmb: Bloom enable,
// Intensity, threshold; the passes and their taps are the disc's shader files'): what is brighter than the
// threshold is copied at a quarter of the size, blurred with a three-tap Gaussian (0.25, 0.5, 0.25; one tap is 1/320
// of the width, 1/180 of the height) and added back. How strongly the PS3 adds it is not known: see kStrength.
//
// The scene is drawn through Dear ImGui's draw list; two draw callbacks redirect it into a texture and put the result
// back into the framebuffer that was bound. Without shader support (or with SAFEHOUSE_COLLECTION_BLOOM=0) the scene is
// drawn as it is.
#include <cstdlib>
#include <string>

#include <glad/glad.h>
#include <imgui/imgui.h>

#include "install.h"

namespace cane::collection {
class Bloom {
public:
    Bloom() = default;
    Bloom(const Bloom&) = delete;
    ~Bloom() { release(); }
    // Between begin and end the draw list's commands are the scene.
    void begin(ImDrawList* dl, const json& settings, bool) {
        m_active = false;
        if (m_failed || !settings.is_object() || !settings.value("enable", false)) return;
        float scale = 1.0f;
        if (const char* e = std::getenv("SAFEHOUSE_COLLECTION_BLOOM")) scale = float(std::atof(e));
        m_strength = settings.value("intensity", 4.0f) * kStrength * scale;
        m_threshold = settings.value("threshold", 0.25f);
        if (m_strength <= 0) return;
        m_active = true;
        dl->AddCallback(&Bloom::beginCallback, this);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
    void end(ImDrawList* dl) {
        if (!m_active) return;
        dl->AddCallback(&Bloom::endCallback, this);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

private:
    // The part of the level's Intensity that is added (4 on the menu: 0.6 of the blurred highlights).
    static constexpr float kStrength = 0.15f;
    struct Target {
        GLuint fbo = 0, tex = 0;
        int w = 0, h = 0;
    };
    static void beginCallback(const ImDrawList*, const ImDrawCmd* cmd) { static_cast<Bloom*>(cmd->UserCallbackData)->beginScene(); }
    static void endCallback(const ImDrawList*, const ImDrawCmd* cmd) { static_cast<Bloom*>(cmd->UserCallbackData)->endScene(); }
    static bool make(Target& t, int w, int h) {
        if (t.fbo && t.w == w && t.h == h) return true;
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo), glDeleteTextures(1, &t.tex);
        t = {};
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        t.w = w, t.h = h;
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    static GLuint program(const char* fragment) {
#if defined(CANE_ANDROID) || defined(__SWITCH__)
        static const char* head = "#version 300 es\nprecision highp float;\n";
#else
        static const char* head = "#version 330 core\n";
#endif
        // one triangle over the whole target
        static const char* vertex = "out vec2 uv;\nvoid main() {\n  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                    "  uv = p;\n  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        auto compile = [&](GLenum kind, const char* body) {
            const GLuint s = glCreateShader(kind);
            const char* parts[2] = {head, body};
            glShaderSource(s, 2, parts, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                glDeleteShader(s);
                return GLuint(0);
            }
            return s;
        };
        const GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
        GLuint p = 0;
        if (vs && fs) {
            p = glCreateProgram();
            glAttachShader(p, vs), glAttachShader(p, fs);
            glLinkProgram(p);
            GLint ok = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) glDeleteProgram(p), p = 0;
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return p;
    }
    bool prepare() {
        if (m_bright) return true;
        m_bright = program("in vec2 uv;\nout vec4 colour;\nuniform sampler2D source;\nuniform float threshold;\n"
                           "void main() { colour = vec4(max(texture(source, uv).rgb - vec3(threshold), vec3(0.0)), 1.0); }\n");
        m_blur = program("in vec2 uv;\nout vec4 colour;\nuniform sampler2D source;\nuniform vec2 tap;\n"
                         "void main() { colour = vec4(texture(source, uv).rgb * 0.5 + texture(source, uv - tap).rgb * 0.25 + texture(source, uv + tap).rgb * 0.25, 1.0); }\n");
        m_add = program("in vec2 uv;\nout vec4 colour;\nuniform sampler2D source;\nuniform sampler2D glow;\nuniform float strength;\n"
                        "void main() { colour = vec4(texture(source, uv).rgb + texture(glow, uv).rgb * strength, 1.0); }\n");
        glGenVertexArrays(1, &m_vao);
        return m_bright && m_blur && m_add && m_vao;
    }
    void beginScene() {
        m_redirected = false;
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_previous);
        const int w = viewport[2], h = viewport[3];
        if (w < 16 || h < 16) return;  // (a window being minimized: this frame as it is)
        if (!prepare() || !make(m_scene, w, h) || !make(m_small[0], std::max(1, w / 4), std::max(1, h / 4)) ||
            !make(m_small[1], std::max(1, w / 4), std::max(1, h / 4))) {
            m_failed = true;  // (no shaders or no render targets here: the scene without the glow from now on)
            glBindFramebuffer(GL_FRAMEBUFFER, GLuint(m_previous));
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_scene.fbo);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        m_redirected = true;
    }
    void pass(GLuint prog, const Target* to, GLuint source, int w, int h) {
        glBindFramebuffer(GL_FRAMEBUFFER, to ? to->fbo : GLuint(m_previous));
        glViewport(0, 0, to ? to->w : w, to ? to->h : h);
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        glUniform1i(glGetUniformLocation(prog, "source"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    void endScene() {
        if (!m_redirected) return;
        m_redirected = false;
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glBindVertexArray(m_vao);
        glUseProgram(m_bright);
        glUniform1f(glGetUniformLocation(m_bright, "threshold"), m_threshold);
        pass(m_bright, &m_small[0], m_scene.tex, 0, 0);
        for (int i = 0; i < 2; ++i) {
            glUseProgram(m_blur);
            glUniform2f(glGetUniformLocation(m_blur, "tap"), 1.0f / 320.0f, 0.0f);
            pass(m_blur, &m_small[1], m_small[0].tex, 0, 0);
            glUniform2f(glGetUniformLocation(m_blur, "tap"), 0.0f, 1.0f / 180.0f);
            pass(m_blur, &m_small[0], m_small[1].tex, 0, 0);
        }
        glUseProgram(m_add);
        glUniform1f(glGetUniformLocation(m_add, "strength"), m_strength);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_small[0].tex);
        glUniform1i(glGetUniformLocation(m_add, "glow"), 1);
        pass(m_add, nullptr, m_scene.tex, m_scene.w, m_scene.h);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
    }
    void release() {
        for (Target* t : {&m_scene, &m_small[0], &m_small[1]})
            if (t->fbo) glDeleteFramebuffers(1, &t->fbo), glDeleteTextures(1, &t->tex);
        for (GLuint p : {m_bright, m_blur, m_add})
            if (p) glDeleteProgram(p);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
    }
    Target m_scene, m_small[2];
    GLuint m_bright = 0, m_blur = 0, m_add = 0, m_vao = 0;
    GLint m_previous = 0;
    float m_strength = 0, m_threshold = 0.25f;
    bool m_active = false, m_failed = false, m_redirected = false;
};
}  // namespace cane::collection
