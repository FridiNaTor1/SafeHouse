#pragma once

// The GL layer (adapted from Sly1-Release gl.h / glshaders.h): the window and context, the off-screen scene
// target the frame renders into, embedded shaders, and the screenshot read-back.

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <string>

class GLSHADER
{
public:
    GLuint ID = 0;
    void Init(const char* vertexFile, const char* fragmentFile);
    void Use() const { glUseProgram(ID); }
    void Delete() { glDeleteProgram(ID); ID = 0; }
    GLint Loc(const char* name) const { return glGetUniformLocation(ID, name); }

private:
    static bool CompileErrors(GLuint shader, const char* type, const char* file);
};

class GL
{
public:
    GLFWwindow* window = nullptr;
    // Scene target (colour RGBA8 + 32-bit float depth, reversed Z: greater is nearer, like the GS).
    GLuint fbo = 0, fbc = 0, rbo = 0;
    GLuint fboMsaa = 0, rboColorMsaa = 0, rboDepthMsaa = 0;
    int msaa = 0;
    int width = 1280, height = 720;          // window
    int renderWidth = 1280, renderHeight = 720;

    // Original 0x1475C0: previous/current ordinary pictures, then an immutable two-sheet atlas.
    GLuint texOverlay = 0;
    void RememberOverlaySourceFrame();
    bool CaptureOverlayFrame();
    void ReleaseOverlayFrame();

    // fVisible false: an invisible window (headless screenshots).
    bool FInit(int width, int height, bool fVisible, const char* title, std::string& error);
    void CreateFramebuffers(int w, int h);
    // The scene target to draw into this frame.
    GLuint FboScene() const { return fboMsaa ? fboMsaa : fbo; }
    // Resolves MSAA and copies the scene to the window.
    void Present();
    // Reads the resolved scene as RGBA rows (top row first) and writes a PNG.
    bool FWriteScreenshot(const std::string& path);
    void Terminate();

private:
    GLuint fboOverlay = 0, fboOverlaySource[2]{}, texOverlaySource[2]{};
    int iOverlaySource = 0, cOverlaySources = 0;
    void ReleaseOverlaySources();
};

extern GL g_gl;
extern GLSHADER glGlobShader;
extern GLSHADER glCelBorderShader;
extern GLSHADER glHudShader;
extern GLSHADER glOverlayShader;
