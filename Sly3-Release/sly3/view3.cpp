// --view-world3 W: the standalone scene of a Sly 3 world (scene3.h) drawn by the copied GL renderer, with a free
// camera, or one frame to a PNG (--screenshot). --ram3 eeMemory.bin: the condition set, the world filter and the
// camera of a PCSX2 RAM dump (a savestate's).

#include "scene3.h"

#include "addr.h"
#include "condset3.h"
#include "hud3.h"
#include "light3.h"
#include "load3.h"
#include "text3.h"

#include "../bis.h"
#include "../cls.h"
#include "../sw.h"
#include "../disc.h"
#include "../display.h"
#include "../gl.h"
#include "../emitter.h"
#include "../light.h"
#include "../render.h"
#include "../shd.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool FParseCamera3(const std::string& s, CAMERA& cam)
{
    float a[5];
    if (std::sscanf(s.c_str(), "%f,%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3], &a[4]) != 5) return false;
    cam.pos = glm::vec3(a[0], a[1], a[2]);
    cam.yaw = a[3];
    cam.pitch = a[4];
    cam.fMat = false;
    return true;
}

// The camera of a PCSX2 RAM dump: the CM object (class 0xAC; found by its vtable word at +0xC...)
bool FCameraFromRam3(const std::vector<uint8_t>& ram, CAMERA& cam, RENDEROPTS& opts);

double s_xLast = 0, s_yLast = 0;
bool s_fMouse = false;
}  // namespace

int ViewWorld3(const VIEW3OPTS& vo)
{
    const WORLD3* pw = nullptr;
    const std::vector<WORLD3> aw = g_disc.Worlds();
    for (const WORLD3& w : aw)
        if (w.name == vo.world || std::to_string(w.iworld) == vo.world) pw = &w;
    if (!pw || pw->fk.cb == 0) { std::fprintf(stderr, "no world %s (--list-files)\n", vo.world.c_str()); return 1; }
    std::vector<uint8_t> ram;
    std::string error;
    if (!vo.ram.empty())
    {
        if (!FLoadCondSet3FromRam(vo.ram, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        ram = ReadAll(vo.ram);
        std::printf("RAM %s: filter %d, condition %d (+%zu), GAME +0xC 0x%X\n", vo.ram.c_str(), g_nFilterCur3, g_condset3.cond,
                    g_condset3.acond.size(), g_grfGame3);
    }
    else g_nFilterCur3 = pw->iworld;
    const std::vector<uint8_t> ab = g_disc.ReadWorld(pw->fk);
    WORLDWALK3 walk;
    if (!FWalkWorld3(ab, walk, error)) { std::fprintf(stderr, "walk: %s\n", error.c_str()); return 1; }
    // --ram3-hud: the world's objects loaded (the Sly 3 class loaders' loader) for their screen objects, before the scene
    std::unique_ptr<SW> pswHud;
    static TEXT3 s_textHud;
    if (vo.nRamHud && !ram.empty())
    {
        InitClasses();
        pswHud = std::make_unique<SW>();
        LOAD3OPTS lo;
        lo.psw = pswHud.get();
        lo.iworld = pw->iworld;
        WORLDWALK3 walkLoad;
        if (!FWalkWorld3(ab, walkLoad, error, &lo)) { std::fprintf(stderr, "load: %s\n", error.c_str()); return 1; }
        CBinaryInputStream bis;
        bis.OpenMemory(int(ab.size()), ab.data());
        bis.m_ib = walk.ibText;
        LoadTextFromBrx3(&s_textHud, 0, &bis);
        if (!FInitHud3FromSw(ab, pswHud.get(), &s_textHud, error)) { std::fprintf(stderr, "HUD: %s\n", error.c_str()); return 1; }
    }
    SCENE scene;
    SCENE3STATS st;
    if (vo.fRamPools && !ram.empty()) g_pramPools3 = &ram;
    if (!FBuildScene3(ab, walk, scene, st, error)) { std::fprintf(stderr, "scene: %s\n", error.c_str()); return 1; }
    std::printf("world %s: %d groups (%d with shaders), %d objects: %d ALO heads read (%d filtered out, %d with another "
                "loader), %d glob sets (+%d empty), %d failed\n", pw->name.c_str(), st.cgroup, st.cgroupShd, st.cobj, st.cobjAlo,
                st.cobjFiltered, st.cnotFirst, st.cglobset, st.cglobsetEmpty, st.cfail);
    std::printf("placement: %d group instances by proxies (%d proxies without a group), %d scene entries; %d groups without "
                "instances (option set %d), %d unplaced pools not drawn (%d of their instances placed from RAM)\n", st.cproxy,
                st.cproxyNoGroup, st.cplaced, st.cgroupNoInst, g_condset3.cond, st.cgroupPool, st.cpoolRam);
    for (const std::string& s : st.aerr) std::printf("  %s\n", s.c_str());
    std::printf("textures: %d CLUTs (%d transformed), %d BMPs, %d filtered, %d shared, %d shaders, %d SAAs, %d bulk blocks, "
                "%d copies of the other pass skipped\n", g_shdstats.cclut, g_shdstats.ctransformed, g_shdstats.cbmp,
                g_shdstats.cfiltered, g_shdstats.cshared, g_shdstats.cshd, g_shdstats.csaa, g_shdstats.cbulk, g_shdstats.cskippedPass);
    std::printf("globs: %d sets, %d globs (%d %d %d %d by type), %d instances, %d vertices, %d triangles, %d filtered, %d "
                "without a mesh, %d boxes, %d entries without a shader\n", g_globstats.cglobset, g_globstats.cglob,
                g_globstats.cmeshk[0], g_globstats.cmeshk[1], g_globstats.cmeshk[2], g_globstats.cmeshk[3], g_globstats.cinstance,
                g_globstats.cvtx, g_globstats.ctri, g_globstats.cfiltered, g_globstats.cnomesh, g_globstats.cbox, g_globstats.cnoshd);
    if (std::getenv("SLY3_SCENE_ONLY")) return st.cfail ? 1 : 0;
    if (vo.fRamPose && !ram.empty()) std::printf("RAM pose: %d entries posed\n", CApplyRamPose3(scene, st, ram));
    // the lights: a RAM dump's (light3.h FLightsFromRam3), else the stand-in (SLY3_APPROX_LIGHT=1: always it)
    {
        LIGHTSET lights;
        if (!std::getenv("SLY3_APPROX_LIGHT") && !ram.empty() && FLightsFromRam3(ram, lights))
        {
            std::printf("lights (RAM): %zu baked, %zu context candidates, world shader %.3f %.3f %.3f %.3f\n", lights.alight.size(),
                        lights.actx.size(), lights.worldShader.r, lights.worldShader.g, lights.worldShader.b, lights.worldShader.a);
            ApplyLighting(scene, lights);
            SetWorldLightsFromRam3(ram);   // (the frame's lights: the cast shadows' direction)
        }
        else ApproximateBakeLighting(scene);
    }

    RENDEROPTS opts;
    CAMERA cam;
    cam.pos = glm::vec3(0, 0, 2000);
    opts.rgbaFog = g_condset3.rgbaFog;
    opts.sFogNear = g_condset3.fogNear;
    opts.sFogFar = g_condset3.fogFar;
    opts.uFogMax = g_condset3.fogMax;
    opts.fFog = !vo.fNoFog;
    if (!ram.empty() && FCameraFromRam3(ram, cam, opts))
        std::printf("PCSX2 camera: %.1f,%.1f,%.1f (fov %.1f, fade distance x %.3f)\n", cam.pos.x, cam.pos.y, cam.pos.z, cam.fovY,
                    1.0f / cam.rMRDAdjust);
    // debugging: SLY3_NEAR_RAY=deg lists the glob sets within deg of the view direction; SLY3_HIDEOIDS=a,b,c hides them
    if (const char* sz = std::getenv("SLY3_NEAR_RAY"))
    {
        const float cosMax = std::cos(glm::radians(float(std::atof(sz))));
        for (const SCENEGLOBSET& sgs : scene.aglobset)
        {
            const glm::vec3 d = sgs.posBounds - cam.pos;
            const float l = glm::length(d);
            if (l < 1 || glm::dot(d / l, cam.Dir()) < cosMax) continue;
            std::printf("near ray: oid %d at %.0f %.0f %.0f r %.0f d %.0f globs %zu:", sgs.oid, sgs.posBounds.x, sgs.posBounds.y,
                        sgs.posBounds.z, sgs.sBoundsRadius, l, sgs.pglobset->aglob.size());
            for (const GLOB& g : sgs.pglobset->aglob)
                std::printf(" [rp %d k%d %s]", g.rp, g.meshk, g.asubglob.empty() || !g.asubglob[0].pshd ? "-" :
                            (std::to_string(g.asubglob[0].pshd->shdk) + "/" + std::to_string(g.asubglob[0].pshd->grfshd)).c_str());
            std::printf("\n");
        }
    }
    if (std::getenv("SLY3_RAMPROBE") && !ram.empty())
    {
        auto u32 = [&](uint32_t va) { uint32_t u; std::memcpy(&u, ram.data() + (va & 0x1FFFFFF), 4); return u; };
        auto f32 = [&](uint32_t va) { float f; std::memcpy(&f, ram.data() + (va & 0x1FFFFFF), 4); return f; };
        int cprobe = 0;
        for (const SCENEGLOBSET& sgs : scene.aglobset)
        {
            const GLOBSET& gs = *sgs.pglobset;
            uint8_t key[16];
            std::memcpy(key, &gs.posCenter, 12);
            std::memcpy(key + 12, &gs.sRadius, 4);
            int cfound = 0;
            for (uint32_t va = 0x100000; va + 0x40 < ram.size(); va += 0x10)
            {
                if (std::memcmp(ram.data() + va + 0x10, key, 12) || std::memcmp(ram.data() + va + 0x20, key + 12, 4)) continue;
                if (u32(va + 0x38) != gs.aglob.size()) continue;
                ++cfound;
                if (std::getenv("SLY3_RAMPROBE_GRF"))   // the ALO's +0x28 / +0x2C words (flags?)
                    std::printf("grf oid %d alo 0x%X: %08X %08X\n", sgs.oid, u32(va), u32(u32(va) + 0x28), u32(u32(va) + 0x2C));
                if (std::getenv("SLY3_RAMPROBE_OID") && std::atoi(std::getenv("SLY3_RAMPROBE_OID")) == sgs.oid)
                {
                    const uint32_t paloP = u32(va), pxfP = u32(paloP + 0x44);
                    std::printf("groups of oid %d at 0x%X (alo 0x%X at %.0f %.0f %.0f; native %.0f %.0f %.0f):", sgs.oid, va, paloP,
                                f32(pxfP + 0x30), f32(pxfP + 0x34), f32(pxfP + 0x38), sgs.amatGlob.empty() ? 0 : sgs.amatGlob[0][3].x,
                                sgs.amatGlob.empty() ? 0 : sgs.amatGlob[0][3].y, sgs.amatGlob.empty() ? 0 : sgs.amatGlob[0][3].z);
                    for (size_t i = 0; i < gs.aglob.size(); ++i)
                    {
                        const uint32_t grp = u32(va + 0x40) + 0x28 * uint32_t(i);
                        std::printf(" [%zu n0 %d flags 0x%X]", i, int(u32(grp)), u32(grp + 0xC));
                    }
                    std::printf("\n");
                }
                if (cprobe++ < 6)
                {
                    const uint32_t palo = u32(va);
                    std::printf("probe oid %d glob 0x%X alo 0x%X:", sgs.oid, va, palo);
                    for (uint32_t o = 0; o < 0x40; o += 4) std::printf(" %X", u32(palo + o));
                    const uint32_t grp = u32(va + 0x40);
                    std::printf("\n  group0 %X %X %X %X %X; xf0 0x%X\n", u32(grp), u32(grp + 4), u32(grp + 8), u32(grp + 0xC), u32(grp + 0x10), u32(palo + 0x44));
                    const uint32_t xf = u32(palo + 0x44);
                    for (uint32_t o = 0; o < 0x100; o += 0x10)
                        std::printf("   xf+%02X %10.3f %10.3f %10.3f %10.3f\n", o, f32(xf + o), f32(xf + o + 4), f32(xf + o + 8), f32(xf + o + 12));
                    std::printf("   native world pos %.3f %.3f %.3f\n", sgs.amatGlob.empty() ? 0 : sgs.amatGlob[0][3].x, sgs.amatGlob.empty() ? 0 : sgs.amatGlob[0][3].y, sgs.amatGlob.empty() ? 0 : sgs.amatGlob[0][3].z);
                }
            }
            if (!cfound) std::printf("probe oid %d: not in RAM\n", sgs.oid);
        }
    }
    // debugging: SLY3_PICK=x,y (pixels of the --size frame): the entries whose glob bounds cover that pixel
    if (const char* sz = std::getenv("SLY3_PICK"))
    {
        float px = 0, py = 0;
        std::sscanf(sz, "%f,%f", &px, &py);
        const glm::mat4 m = cam.MatWorldToClip(float(vo.width) / float(vo.height));
        for (const SCENEGLOBSET& sgs : scene.aglobset)
            for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
            {
                const GLOB& g = sgs.pglobset->aglob[i];
                const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                const glm::vec4 h = m * glm::vec4(c, 1.0f);
                if (h.w <= 0) continue;
                const float sx = (h.x / h.w * 0.5f + 0.5f) * float(vo.width), sy = (0.5f - h.y / h.w * 0.5f) * float(vo.height);
                const float sr = g.sRadius / h.w * float(vo.height) / (2.0f * std::tan(glm::radians(cam.fovY) * 0.5f));
                if ((sx - px) * (sx - px) + (sy - py) * (sy - py) > sr * sr) continue;
                std::printf("pick: oid %d glob %zu rp %d meshk %d n0 %d hidden %d r %.0f d %.0f shd %d/%d/0x%X\n", sgs.oid, i, g.rp, int(g.meshk), g.n0,
                            int(g.fHidden), g.sRadius, h.w, g.asubglob.empty() || !g.asubglob[0].pshd ? -1 : g.asubglob[0].pshd->id,
                            g.asubglob.empty() || !g.asubglob[0].pshd ? -1 : g.asubglob[0].pshd->shdk, g.asubglob.empty() || !g.asubglob[0].pshd ? 0 : g.asubglob[0].pshd->grfshd);
            }
    }
    if (const char* sz = std::getenv("SLY3_HIDEOIDS"))
        for (SCENEGLOBSET& sgs : scene.aglobset)
            if (std::strstr((std::string(",") + sz + ",").c_str(), ("," + std::to_string(sgs.oid) + ",").c_str()))
                std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), 0);
    if (!vo.camera.empty() && !FParseCamera3(vo.camera, cam)) { std::fprintf(stderr, "bad --camera\n"); return 2; }
    // the particles (emitter.cpp with Sly 3's emitters 0x1B; the loaded world of --ram3-hud): run 3 s before the picture
    // (SLY3_PARTICLE_T=seconds), SLY3_EMITDUMP=1 lists the emitters
    if (pswHud && !std::getenv("SLY3_NO_PARTICLES"))
    {
        UpdateLoWorlds3(pswHud.get());
        SetParticleCamera(cam.pos, cam.Dir(), glm::normalize(glm::cross(glm::vec3(0, 0, 1), cam.Dir())));
        const float t = std::getenv("SLY3_PARTICLE_T") ? float(std::atof(std::getenv("SLY3_PARTICLE_T"))) : 3.0f;
        StepParticles(pswHud.get(), int(t * 60.0f), 1.0f / 60.0f);
        std::printf("particles: %d emitters (%d groups), %d particle systems, %d particles\n", g_particlestats.cemitter,
                    g_particlestats.cemitterGroup, g_particlestats.cpsys, g_particlestats.cparticle);
        if (std::getenv("SLY3_EMITDUMP")) DumpEmitters();
    }
    opts.fCelBorders = !vo.fNoCel;

    const bool fShot = !vo.screenshot.empty();
    if (!g_gl.FInit(vo.width, vo.height, !fShot, "Sly3-Release", error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    BuildSceneGpu(scene);
    if (fShot)
    {
        if (vo.nRamHud && !ram.empty())
        {
            std::string log;
            const int c = CApplyHudFromRam3(ram, vo.nRamHud == 1, log);
            std::printf("HUD (%s): %d screen objects shown:%s\n", vo.nRamHud == 1 ? "RAM slots" : "native keys", c, log.c_str());
            BindHeadTargets3(scene);
            // the heads are lit by their cameras' lights (light3.h; hud3 AlightsHeadTargets3)
            if (!std::getenv("SLY3_APPROX_LIGHT"))
            {
                LIGHTSET lights;
                if (FLightsFromRam3(ram, lights)) RelightForward(scene, lights, true, AlightsHeadTargets3(lights.worldShader));
            }
        }
        RenderScene(scene, cam, opts);
        if (vo.nRamHud && !ram.empty()) DrawHud3(g_gl.renderWidth, g_gl.renderHeight);
        glFinish();
        const bool fOk = g_gl.FWriteScreenshot(vo.screenshot);
        std::printf("%s %s: %d draws, %d passes, %d cel borders, %d textures (%d failed)\n", fOk ? "wrote" : "FAILED to write",
                    vo.screenshot.c_str(), g_renderstats.cdraw, g_renderstats.cpass, g_renderstats.ccel, g_renderstats.ctexture,
                    g_renderstats.cfailTexture);
        g_gl.Terminate();
        return fOk ? 0 : 1;
    }
    std::printf("free camera: WASD move, Q/E down/up, shift fast, right mouse look, C cel borders, P print camera, F12 "
                "screenshot (view3.png)\n");
    double tLast = glfwGetTime();
    bool fCLast = false, fPLast = false, fF12Last = false;
    while (!glfwWindowShouldClose(g_gl.window))
    {
        glfwPollEvents();
        const double t = glfwGetTime();
        const float dt = float(t - tLast);
        tLast = t;
        GLFWwindow* w = g_gl.window;
        const float speed = (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) ? 4000.0f : 800.0f) * dt;
        if (cam.fMat)
        {
            // leave the game's matrix for yaw / pitch on the first move
            const glm::vec3 d = cam.Dir();
            cam.yaw = glm::degrees(std::atan2(d.y, d.x));
            cam.pitch = glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
        }
        const glm::vec3 fwd = cam.Dir();
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
        bool fMove = false;
        if (glfwGetKey(w, GLFW_KEY_W)) cam.pos += fwd * speed, fMove = true;
        if (glfwGetKey(w, GLFW_KEY_S)) cam.pos -= fwd * speed, fMove = true;
        if (glfwGetKey(w, GLFW_KEY_D)) cam.pos += right * speed, fMove = true;
        if (glfwGetKey(w, GLFW_KEY_A)) cam.pos -= right * speed, fMove = true;
        if (glfwGetKey(w, GLFW_KEY_E)) cam.pos.z += speed, fMove = true;
        if (glfwGetKey(w, GLFW_KEY_Q)) cam.pos.z -= speed, fMove = true;
        if (glfwGetKey(w, GLFW_KEY_ESCAPE)) glfwSetWindowShouldClose(w, 1);
        const bool fC = glfwGetKey(w, GLFW_KEY_C), fP = glfwGetKey(w, GLFW_KEY_P), fF12 = glfwGetKey(w, GLFW_KEY_F12);
        if (fC && !fCLast) opts.fCelBorders = !opts.fCelBorders;
        if (fP && !fPLast) std::printf("--camera %.1f,%.1f,%.1f,%.2f,%.2f\n", cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw, cam.pitch);
        fCLast = fC; fPLast = fP;
        double x, y;
        glfwGetCursorPos(w, &x, &y);
        if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT))
        {
            if (s_fMouse)
            {
                cam.yaw -= float(x - s_xLast) * 0.2f;
                cam.pitch = std::clamp(cam.pitch - float(y - s_yLast) * 0.2f, -89.0f, 89.0f);
                fMove = true;
            }
            s_fMouse = true;
        }
        else s_fMouse = false;
        if (fMove) cam.fMat = false;
        s_xLast = x; s_yLast = y;
        int fbw, fbh;
        glfwGetFramebufferSize(w, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != g_display.outputWidth || fbh != g_display.outputHeight)) LayoutDisplay(fbw, fbh);
        UpdateShaderAnimations(float(t), dt);
        RenderScene(scene, cam, opts);
        if (fF12 && !fF12Last) g_gl.FWriteScreenshot("view3.png");
        fF12Last = fF12;
        g_gl.Present();
        glfwSwapBuffers(w);
    }
    g_gl.Terminate();
    return 0;
}

namespace
{
// The world's cameras: SW (0x475480) +0x3EB8 count, +0x3EBC pointers (orig3 0x27DE38 updates each with 0x1662C0 /
// UpdateCmMat4 0x166A40); the camera is an object of class 0x05 (vt 0x3557E8, 0x450 bytes). Fields seen in the
// PCSX2 dumps: +0x50 / +0x60 / +0x70 the rows (view direction, left, up), +0x80 the position, +0x138 / +0x13C the
// frustum tangents (x / y; guess), +0x144 near (50; guess), +0x148 far, +0x1A0 the projection.
bool FCameraFromRam3(const std::vector<uint8_t>& ram, CAMERA& cam, RENDEROPTS& opts)
{
    (void)opts;
    if (ram.size() < 0x2000000) return false;
    auto u32 = [&](uint32_t va) { uint32_t u; std::memcpy(&u, ram.data() + (va & 0x1FFFFFF), 4); return u; };
    auto f32 = [&](uint32_t va) { float f; std::memcpy(&f, ram.data() + (va & 0x1FFFFFF), 4); return f; };
    const uint32_t sw = orig3::VA_SW;
    if (u32(sw + 0x3EB8) == 0) return false;
    const uint32_t pcm = u32(sw + 0x3EBC);
    if (pcm < 0x100000 || pcm >= 0x2000000) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) cam.mat[r][c] = f32(pcm + 0x50 + 0x10 * uint32_t(r) + 4 * uint32_t(c));
    cam.pos = glm::vec3(f32(pcm + 0x80), f32(pcm + 0x84), f32(pcm + 0x88));
    cam.fMat = true;
    const float ty = f32(pcm + 0x13C), sNear = f32(pcm + 0x144);
    if (ty > 0.01f && ty < 10.0f) cam.fovY = glm::degrees(2.0f * std::atan(ty));
    if (sNear > 0.1f && sNear < 1000.0f) cam.sNear = sNear;
    // +0x114: the distance scale of the fades (RenderGlob 0x18E9B8; {1, 0.30, 0.30, 0.30} at +0x110 at the title)
    const float sDist = f32(pcm + 0x114);
    if (sDist > 0.01f && sDist < 100.0f) cam.rMRDAdjust = 1.0f / sDist;
    const glm::vec3 d = cam.mat[0];
    cam.yaw = glm::degrees(std::atan2(d.y, d.x));
    cam.pitch = glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
    return true;
}
}  // namespace
