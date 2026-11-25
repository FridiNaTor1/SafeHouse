// Dear ImGui's draw lists through glad: see imgui_gl.h. Follows imgui_impl_opengl3.cpp's RenderDrawData.
#include "imgui_gl.h"

#include <glad/glad.h>

#include "imgui/imgui.h"

#include <cstddef>
#include <cstdint>

namespace cane::imgui_gl {
namespace {
GLuint s_program = 0, s_vao = 0, s_vbo = 0, s_ebo = 0, s_font = 0;
GLint s_locProj = -1, s_locTex = -1;

void MakeFont()
{
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    GLint last = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
    glGenTextures(1, &s_font);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, GLuint(last));
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(s_font)));
}
}  // namespace

void Init(unsigned program)
{
    s_program = program;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "cane_imgui_gl";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    if (!s_program) return;
    s_locProj = glGetUniformLocation(s_program, "ProjMtx");
    s_locTex = glGetUniformLocation(s_program, "Texture");
}

void NewFrame()
{
    if (s_program && !s_font) MakeFont();
}

void Render(ImDrawData* data)
{
    if (!s_program || !data || data->CmdListsCount == 0) return;
    const int fbW = int(data->DisplaySize.x * data->FramebufferScale.x), fbH = int(data->DisplaySize.y * data->FramebufferScale.y);
    if (fbW <= 0 || fbH <= 0) return;
    if (!s_vao)
    {
        glGenVertexArrays(1, &s_vao);
        glGenBuffers(1, &s_vbo);
        glGenBuffers(1, &s_ebo);
    }
    // (the state put back afterwards)
    GLint lastProgram, lastTexture, lastActive, lastArray, lastVao, lastViewport[4], lastScissor[4], lastSrcRgb, lastDstRgb,
        lastSrcA, lastDstA, lastEqRgb, lastEqA, lastFramebuffer;
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &lastActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArray);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVao);
    glGetIntegerv(GL_VIEWPORT, lastViewport);
    glGetIntegerv(GL_SCISSOR_BOX, lastScissor);
    glGetIntegerv(GL_BLEND_SRC_RGB, &lastSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &lastDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &lastSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &lastDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &lastEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &lastEqA);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &lastFramebuffer);
    const GLboolean lastBlend = glIsEnabled(GL_BLEND), lastCull = glIsEnabled(GL_CULL_FACE), lastDepth = glIsEnabled(GL_DEPTH_TEST),
                    lastStencil = glIsEnabled(GL_STENCIL_TEST), lastScissorTest = glIsEnabled(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, fbW, fbH);
    const float l = data->DisplayPos.x, r = data->DisplayPos.x + data->DisplaySize.x;
    const float t = data->DisplayPos.y, b = data->DisplayPos.y + data->DisplaySize.y;
    const float ortho[4][4] = {{2.0f / (r - l), 0, 0, 0}, {0, 2.0f / (t - b), 0, 0}, {0, 0, -1.0f, 0},
                               {(r + l) / (l - r), (t + b) / (b - t), 0, 1.0f}};
    glUseProgram(s_program);
    glUniform1i(s_locTex, 0);
    glUniformMatrix4fv(s_locProj, 1, GL_FALSE, &ortho[0][0]);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, pos)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, uv)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, col)));

    const ImVec2 clipOff = data->DisplayPos, clipScale = data->FramebufferScale;
    for (int n = 0; n < data->CmdListsCount; ++n)
    {
        const ImDrawList* list = data->CmdLists[n];
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(list->VtxBuffer.Size) * GLsizeiptr(sizeof(ImDrawVert)), list->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(list->IdxBuffer.Size) * GLsizeiptr(sizeof(ImDrawIdx)), list->IdxBuffer.Data, GL_STREAM_DRAW);
        for (int i = 0; i < list->CmdBuffer.Size; ++i)
        {
            const ImDrawCmd& cmd = list->CmdBuffer[i];
            if (cmd.UserCallback) continue;   // (none in the games' windows)
            const float x0 = (cmd.ClipRect.x - clipOff.x) * clipScale.x, y0 = (cmd.ClipRect.y - clipOff.y) * clipScale.y;
            const float x1 = (cmd.ClipRect.z - clipOff.x) * clipScale.x, y1 = (cmd.ClipRect.w - clipOff.y) * clipScale.y;
            if (x1 <= x0 || y1 <= y0) continue;
            glScissor(int(x0), int(float(fbH) - y1), int(x1 - x0), int(y1 - y0));
            glBindTexture(GL_TEXTURE_2D, GLuint(reinterpret_cast<intptr_t>(cmd.GetTexID())));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(cmd.ElemCount), sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                     reinterpret_cast<void*>(intptr_t(cmd.IdxOffset * sizeof(ImDrawIdx))), GLint(cmd.VtxOffset));
        }
    }

    glUseProgram(GLuint(lastProgram));
    glBindTexture(GL_TEXTURE_2D, GLuint(lastTexture));
    glActiveTexture(GLenum(lastActive));
    glBindVertexArray(GLuint(lastVao));
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(lastArray));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(lastFramebuffer));
    glBlendEquationSeparate(GLenum(lastEqRgb), GLenum(lastEqA));
    glBlendFuncSeparate(GLenum(lastSrcRgb), GLenum(lastDstRgb), GLenum(lastSrcA), GLenum(lastDstA));
    (lastBlend ? glEnable : glDisable)(GL_BLEND);
    (lastCull ? glEnable : glDisable)(GL_CULL_FACE);
    (lastDepth ? glEnable : glDisable)(GL_DEPTH_TEST);
    (lastStencil ? glEnable : glDisable)(GL_STENCIL_TEST);
    (lastScissorTest ? glEnable : glDisable)(GL_SCISSOR_TEST);
    glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    glScissor(lastScissor[0], lastScissor[1], lastScissor[2], lastScissor[3]);
}

void Shutdown()
{
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    if (s_vbo) glDeleteBuffers(1, &s_vbo);
    if (s_ebo) glDeleteBuffers(1, &s_ebo);
    if (s_font) glDeleteTextures(1, &s_font);
    s_vao = s_vbo = s_ebo = s_font = 0;
    s_program = 0;
}
}  // namespace cane::imgui_gl
