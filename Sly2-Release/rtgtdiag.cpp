// See rtgtdiag.h.

#include "rtgtdiag.h"

#include "clock.h"
#include "gl.h"
#include "platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

bool GlReadFramebufferRgba(GLuint fbo, int dx, int dy, std::vector<uint8_t>& out);   // (gl.cpp)
bool GlWritePngRgba(const std::string& path, int dx, int dy, const uint8_t* prgba);

namespace
{
struct DIAGSTATE
{
    bool fInit = false, fOn = false, fAll = false;
    std::filesystem::path dir;
    FILE* pf = nullptr;
    int cframe = 0;            // frames that drew a target
    bool fFrameOpen = false;   // (the current frame counted)
    bool fLog = false, fPictures = false;
    std::string pathComposite;
    std::chrono::steady_clock::time_point t0;
};
DIAGSTATE s_diag;

void Init()
{
    if (s_diag.fInit) return;
    s_diag.fInit = true;
    const char* sz = std::getenv("SLY2N_RTGTDUMP");
    if (!sz || !*sz || !std::strcmp(sz, "0")) return;
    s_diag.fOn = true;
    s_diag.fAll = std::getenv("SLY2N_RTGTDUMPALL") != nullptr;
    s_diag.dir = !std::strcmp(sz, "1") ? cane::userDirectory() / "rtgtdump" : std::filesystem::path(cane::path(sz));
    std::error_code ec;
    std::filesystem::create_directories(s_diag.dir, ec);
#ifdef _WIN32
    s_diag.pf = _wfopen((s_diag.dir / "rtgtdump.txt").wstring().c_str(), L"w");
#else
    s_diag.pf = std::fopen((s_diag.dir / "rtgtdump.txt").c_str(), "w");
#endif
    s_diag.t0 = std::chrono::steady_clock::now();
    const char* szRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* szVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    char ach[1024];
    std::snprintf(ach, sizeof ach, "rtgtdump: into %s; %s / %s; scene %dx%d, window %dx%d, MSAA %d\n",
                  cane::utf8(s_diag.dir).c_str(), szRenderer ? szRenderer : "?", szVersion ? szVersion : "?", g_gl.renderWidth,
                  g_gl.renderHeight, g_gl.width, g_gl.height, g_gl.msaa);
    std::fputs(ach, stderr);
    std::fflush(stderr);
    if (s_diag.pf) std::fputs(ach, s_diag.pf), std::fflush(s_diag.pf);
}

bool FPictureFrame(int n) { return n == 1 || n == 5 || n == 30 || n == 120 || n == 600 || n == 1800; }

void Line(const std::string& str)
{
    std::fputs(str.c_str(), stderr);
    std::fflush(stderr);
    if (s_diag.pf) std::fputs(str.c_str(), s_diag.pf), std::fflush(s_diag.pf);
}
}  // namespace

bool FRtgtDiagPass(int irtgt)
{
    (void)irtgt;
    Init();
    if (!s_diag.fOn) return false;
    if (!s_diag.fFrameOpen)
    {
        s_diag.fFrameOpen = true;
        const int n = ++s_diag.cframe;
        s_diag.fLog = s_diag.fAll || n <= 10 || n % 30 == 0;
        s_diag.fPictures = FPictureFrame(n);
    }
    return s_diag.fLog || s_diag.fPictures;
}

void RtgtDiagReport(const RTGTDIAG& d)
{
    if (!s_diag.fOn) return;
    // the pixels: those not the clear (RGBA 0), the alpha's mean, how many have alpha 0 / above 0x80 (GS units: 1.0)
    std::vector<uint8_t> apx;
    const bool fRead = GlReadFramebufferRgba(d.fbo, d.dx, d.dy, apx);
    size_t cDrawn = 0, cAlpha0 = 0, cAlphaFull = 0;
    double sumA = 0, sumR = 0, sumG = 0, sumB = 0;
    const size_t c = apx.size() / 4;
    for (size_t i = 0; i < c; ++i)
    {
        const uint8_t* p = &apx[i * 4];
        if (p[0] | p[1] | p[2] | p[3]) ++cDrawn;
        if (p[3] == 0) ++cAlpha0;
        if (p[3] >= 0x80) ++cAlphaFull;
        sumR += p[0], sumG += p[1], sumB += p[2], sumA += p[3];
    }
    const double rc = c ? 1.0 / double(c) : 0.0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_diag.t0).count();
    const glm::mat4& m = d.matWorldToClip;
    char ach[2048];
    std::snprintf(ach, sizeof ach,
                  "rtgt: frame %d (%.0f ms, clock t %.3f dt %.5f dtReal %.5f) target %d owner %p %dx%d fbo %u%s | camera pos %.2f %.2f %.2f "
                  "fwd %.3f %.3f %.3f up %.3f %.3f %.3f fovY %.2f deg near %.3f | clip rows "
                  "%.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g | records drawn %d, culled: frustum %d, "
                  "distance (MRD) %d | sub-draws %d, pass draws %d (%d without the near clip), batches %d, alpha-fail-all skipped %d, "
                  "triangles %lld | pixels %s: not clear %.1f %%, alpha 0 %.1f %%, alpha >= 0x80 %.1f %%, mean RGBA %.1f %.1f %.1f %.1f\n",
                  s_diag.cframe, ms, g_clock.t, g_clock.dt, g_clock.dtReal, d.irtgt, d.powner, d.dx, d.dy, d.fbo, d.fbo ? "" : " (NO FRAMEBUFFER)",
                  d.pos.x, d.pos.y, d.pos.z, d.dirForward.x, d.dirForward.y, d.dirForward.z, d.dirUp.x, d.dirUp.y, d.dirUp.z,
                  glm::degrees(d.radFovY), d.sNear, m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1], m[0][2], m[1][2],
                  m[2][2], m[3][2], m[0][3], m[1][3], m[2][3], m[3][3], d.crec, d.cculled, d.cmrd, d.cdraw, d.cpass, d.cnoclip, d.cbatch,
                  d.cfailAll, d.ctri, fRead ? "read" : "NOT READ", 100.0 * double(cDrawn) * rc, 100.0 * double(cAlpha0) * rc,
                  100.0 * double(cAlphaFull) * rc, sumR * rc, sumG * rc, sumB * rc, sumA * rc);
    if (s_diag.fLog || s_diag.fPictures) Line(ach);
    if (s_diag.fPictures && fRead)
    {
        std::vector<uint8_t> aflip(apx.size());
        const size_t row = size_t(d.dx) * 4;
        for (int y = 0; y < d.dy; ++y) std::memcpy(&aflip[size_t(y) * row], &apx[size_t(d.dy - 1 - y) * row], row);
        const std::string path = cane::utf8(s_diag.dir / ("rtgt" + std::to_string(d.irtgt) + "_" + std::to_string(s_diag.cframe) + ".png"));
        Line(std::string("rtgt: wrote ") + path + (GlWritePngRgba(path, d.dx, d.dy, aflip.data()) ? "\n" : " (FAILED)\n"));
    }
}

void RtgtDiagEndTargets()
{
    if (!s_diag.fOn || !s_diag.fFrameOpen) return;
    s_diag.fFrameOpen = false;
    if (s_diag.fPictures) s_diag.pathComposite = cane::utf8(s_diag.dir / ("frame_" + std::to_string(s_diag.cframe) + ".png"));
}

std::string RtgtDiagCompositePath()
{
    std::string path;
    path.swap(s_diag.pathComposite);
    return path;
}
