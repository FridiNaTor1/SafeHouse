// Dear ImGui's OpenGL 3 renderer (imgui_impl_opengl3.h, the same functions) on the Switch's GL layer: the installer, the
// launcher and the debug menus of Sly 2 and Sly 3 draw through it (Sly 1 has its own, sly1/deko/imgui.cpp). The standard backend compiles
// GLSL at run time, which deko3d cannot: this one uses the precompiled imgui program (deko/shaders/imgui.*,
// cmake/switch_shaders.py) and otherwise draws as the standard one does, restoring the caller's GL state after it.
// After ProjectCane's platform/switch/deko/imgui.cpp.
#include "gl_deko.h"

#include <imgui/imgui.h>
#include <imgui/imgui_impl_opengl3.h>

#include <cstddef>
#include <cstdint>

namespace
{
struct Renderer
{
    GLuint program = 0, font = 0, vertices = 0, indices = 0, array = 0;
    GLint projection = -1, texture = -1;
};
Renderer* Get() { return ImGui::GetCurrentContext() ? static_cast<Renderer*>(ImGui::GetIO().BackendRendererUserData) : nullptr; }

void SetupState(ImDrawData* data, int width, int height)
{
    Renderer* r = Get();
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width, height);
    const float l = data->DisplayPos.x, rgt = l + data->DisplaySize.x, t = data->DisplayPos.y, b = t + data->DisplaySize.y;
    const float m[4][4] = {{2.0f / (rgt - l), 0, 0, 0}, {0, 2.0f / (t - b), 0, 0}, {0, 0, -1, 0}, {(rgt + l) / (l - rgt), (t + b) / (b - t), 0, 1}};
    glUseProgram(r->program);
    glUniform1i(r->texture, 0);
    glUniformMatrix4fv(r->projection, 1, GL_FALSE, &m[0][0]);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindVertexArray(r->array);
    glBindBuffer(GL_ARRAY_BUFFER, r->vertices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->indices);
}
}  // namespace

bool ImGui_ImplOpenGL3_Init(const char*)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(!io.BackendRendererUserData);
    io.BackendRendererUserData = new Renderer;
    io.BackendRendererName = "SafeHouse_deko3d";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    return ImGui_ImplOpenGL3_CreateDeviceObjects();
}

void ImGui_ImplOpenGL3_Shutdown()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplOpenGL3_DestroyDeviceObjects();
    delete Get();
    io.BackendRendererUserData = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
}

void ImGui_ImplOpenGL3_NewFrame()
{
    Renderer* r = Get();
    if (r && !r->font) ImGui_ImplOpenGL3_CreateFontsTexture();
}

bool ImGui_ImplOpenGL3_CreateFontsTexture()
{
    Renderer* r = Get();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    GLint oldTexture = 0, oldAlignment = 4, oldRowLength = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRowLength);
    if (!r->font) glGenTextures(1, &r->font);
    glBindTexture(GL_TEXTURE_2D, r->font);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRowLength);
    glBindTexture(GL_TEXTURE_2D, GLuint(oldTexture));
    ImGui::GetIO().Fonts->SetTexID(reinterpret_cast<ImTextureID>(uintptr_t(r->font)));
    return true;
}

void ImGui_ImplOpenGL3_DestroyFontsTexture()
{
    Renderer* r = Get();
    if (!r || !r->font) return;
    glDeleteTextures(1, &r->font);
    r->font = 0;
    ImGui::GetIO().Fonts->SetTexID(nullptr);
}

bool ImGui_ImplOpenGL3_CreateDeviceObjects()
{
    Renderer* r = Get();
    GLint oldArray = 0, oldBuffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    r->program = CaneDeko::CreateProgram("imgui.vert", nullptr, "imgui.frag", "");
    if (!r->program) return false;
    r->projection = glGetUniformLocation(r->program, "ProjMtx");
    r->texture = glGetUniformLocation(r->program, "Texture");
    glGenBuffers(1, &r->vertices);
    glGenBuffers(1, &r->indices);
    glGenVertexArrays(1, &r->array);
    glBindVertexArray(r->array);
    glBindBuffer(GL_ARRAY_BUFFER, r->vertices);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, pos)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, uv)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, col)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->indices);
    glBindVertexArray(GLuint(oldArray));
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(oldBuffer));
    return ImGui_ImplOpenGL3_CreateFontsTexture();
}

void ImGui_ImplOpenGL3_DestroyDeviceObjects()
{
    Renderer* r = Get();
    if (!r) return;
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    if (r->vertices) glDeleteBuffers(1, &r->vertices);
    if (r->indices) glDeleteBuffers(1, &r->indices);
    if (r->array) glDeleteVertexArrays(1, &r->array);
    if (r->program) glDeleteProgram(r->program);
    r->vertices = r->indices = r->array = r->program = 0;
}

void ImGui_ImplOpenGL3_RenderDrawData(ImDrawData* data)
{
    if (!data) return;
    const int width = int(data->DisplaySize.x * data->FramebufferScale.x), height = int(data->DisplaySize.y * data->FramebufferScale.y);
    Renderer* r = Get();
    if (!r || !r->program || width <= 0 || height <= 0 || !data->TotalVtxCount) return;

    // the caller's state, restored after
    GLint program, array, buffer, active, texture, viewport[4], scissor[4], eqRgb, eqAlpha, srcRgb, dstRgb, srcAlpha, dstAlpha;
    GLboolean mask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    const bool blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), depth = glIsEnabled(GL_DEPTH_TEST),
               stencil = glIsEnabled(GL_STENCIL_TEST), scissorTest = glIsEnabled(GL_SCISSOR_TEST);

    SetupState(data, width, height);
    const ImVec2 origin = data->DisplayPos, scale = data->FramebufferScale;
    for (int n = 0; n < data->CmdListsCount; ++n)
    {
        const ImDrawList* list = data->CmdLists[n];
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(list->VtxBuffer.Size) * GLsizeiptr(sizeof(ImDrawVert)), list->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(list->IdxBuffer.Size) * GLsizeiptr(sizeof(ImDrawIdx)), list->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback)
            {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) SetupState(data, width, height);
                else cmd.UserCallback(list, &cmd);
                continue;
            }
            const float x0 = (cmd.ClipRect.x - origin.x) * scale.x, y0 = (cmd.ClipRect.y - origin.y) * scale.y;
            const float x1 = (cmd.ClipRect.z - origin.x) * scale.x, y1 = (cmd.ClipRect.w - origin.y) * scale.y;
            if (x1 <= x0 || y1 <= y0) continue;
            // (GL scissor rectangles are bottom-up)
            glScissor(int(x0), int(float(height) - y1), int(x1 - x0), int(y1 - y0));
            glBindTexture(GL_TEXTURE_2D, GLuint(reinterpret_cast<uintptr_t>(cmd.GetTexID())));
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(cmd.ElemCount), sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                     reinterpret_cast<void*>(uintptr_t(cmd.IdxOffset * sizeof(ImDrawIdx))), GLint(cmd.VtxOffset));
        }
    }

    glUseProgram(GLuint(program));
    glBindVertexArray(GLuint(array));
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(buffer));
    glBindTexture(GL_TEXTURE_2D, GLuint(texture));
    glActiveTexture(GLenum(active));
    glBlendEquationSeparate(GLenum(eqRgb), GLenum(eqAlpha));
    glBlendFuncSeparate(GLenum(srcRgb), GLenum(dstRgb), GLenum(srcAlpha), GLenum(dstAlpha));
    (blend ? glEnable : glDisable)(GL_BLEND);
    (cull ? glEnable : glDisable)(GL_CULL_FACE);
    (depth ? glEnable : glDisable)(GL_DEPTH_TEST);
    (stencil ? glEnable : glDisable)(GL_STENCIL_TEST);
    (scissorTest ? glEnable : glDisable)(GL_SCISSOR_TEST);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
}
