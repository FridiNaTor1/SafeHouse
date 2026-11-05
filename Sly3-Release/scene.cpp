// See scene.h.

#include "scene.h"

#include "render.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace
{
struct MAPRUN
{
    size_t ibStart, ibEnd;
    uint32_t func;
};

std::vector<MAPRUN> ReadMap(const std::string& path)
{
    std::vector<MAPRUN> aruns;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        MAPRUN run{};
        std::string func;
        if (!(ss >> run.ibStart >> run.ibEnd >> func)) continue;
        run.func = static_cast<uint32_t>(std::strtoul(func.c_str(), nullptr, 0));
        aruns.push_back(run);
    }
    return aruns;
}

bool FCheck(const char* what, size_t ibStart, size_t ibGot, size_t ibWant, bool fVerbose, std::string& error)
{
    if (ibGot == ibWant)
    {
        if (fVerbose) std::printf("%-8s %9zu..%9zu ok\n", what, ibStart, ibWant);
        return true;
    }
    char sz[256];
    std::snprintf(sz, sizeof(sz), "%s at %zu: ended at %zu, the map says %zu (%+lld)", what, ibStart, ibGot, ibWant,
                  static_cast<long long>(ibGot) - static_cast<long long>(ibWant));
    error = sz;
    return false;
}
}  // namespace

bool FLoadSceneFromMap(const std::vector<uint8_t>& world, const std::string& mapPath, SCENE& scene, bool fVerbose,
                       std::string& error)
{
    const std::vector<MAPRUN> aruns = ReadMap(mapPath);
    if (aruns.empty()) { error = "no runs in " + mapPath; return false; }
    CBinaryInputStream bis;
    bis.OpenMemory(static_cast<int>(world.size()), world.data());
    ResetShaders();
    bool fGroup = false;
    for (size_t i = 0; i < aruns.size(); ++i)
    {
        const MAPRUN& run = aruns[i];
        try
        {
            if (run.func == 0x20F5D0)
            {
                if (fGroup) EndShaderGroup();
                fGroup = true;
                bis.m_ib = run.ibStart;
                LoadBulkHeaderFromBrx(nullptr, &bis);
                if (!FCheck("bulkhdr", run.ibStart, bis.m_ib, run.ibEnd, fVerbose, error)) return false;
            }
            else if (run.func == 0x1EAE40 && (i == 0 || aruns[i - 1].func != 0x1EAE40))
            {
                // 0x1EB748 starts with the CLUT count (0x1EAE40) and ends with the bulk block (0x20F640).
                size_t j = i;
                while (j < aruns.size() && aruns[j].func != 0x20F640) ++j;
                if (j == aruns.size()) { error = "texture block without its bulk read"; return false; }
                bis.m_ib = run.ibStart;
                LoadShadersFromBrx(nullptr, &bis);
                ++scene.cblockShd;
                if (!FCheck("textures", run.ibStart, bis.m_ib, aruns[j].ibEnd, fVerbose, error)) return false;
            }
            else if (run.func == 0x212298 && (i == 0 || aruns[i - 1].func != 0x212298))
            {
                size_t j = i + 1;
                while (j < aruns.size() && (aruns[j].func == 0x212298 || aruns[j].func == 0x214398 ||
                                            aruns[j].func == 0x2149D8 || aruns[j].func == 0x214460))
                    ++j;
                auto pso = std::make_shared<SCREENOBJ>();
                pso->ib = run.ibStart;
                // the object's id: LoadSwObjsFromBrx's record {..., S32 oid, S16 n, S32 cb} just before its options
                for (size_t k = i; k-- > 0;)
                    if (aruns[k].func == 0x13FB40)
                    {
                        int32_t oid;
                        std::memcpy(&oid, world.data() + aruns[k].ibEnd - 10, 4);
                        pso->oid = oid;
                        break;
                    }
                bis.m_ib = run.ibStart;
                LoadScreenObjFromBrx(pso.get(), &bis);
                if (!FCheck("screen", run.ibStart, bis.m_ib, aruns[j - 1].ibEnd, fVerbose, error)) return false;
                scene.aso.push_back(std::move(pso));
            }
            else if (run.func == 0x167320 && i > 0 && aruns[i - 1].func == 0x1F31E0)
            {
                size_t j = i;
                while (j < aruns.size() && aruns[j].func != 0x167870) ++j;
                if (j == aruns.size()) { error = "glob without its spheres"; return false; }
                bis.m_ib = run.ibStart;
                SCENEGLOBSET sgs;
                sgs.ib = run.ibStart;
                sgs.pglobset = std::make_unique<GLOBSET>();
                LoadGlobsetFromBrx(sgs.pglobset.get(), nullptr, &bis);
                if (!FCheck("glob", run.ibStart, bis.m_ib, aruns[j].ibEnd, fVerbose, error)) return false;
                scene.aglobset.push_back(std::move(sgs));
            }
        }
        catch (const std::exception& e)
        {
            char sz[256];
            std::snprintf(sz, sizeof(sz), "func 0x%X at %zu: %s", run.func, run.ibStart, e.what());
            error = sz;
            return false;
        }
    }
    scene.cblockBulk = g_shdstats.cbulk;
    return true;
}

// The reference keeps a glob at ALO+0x80: +0x10 its bounds centre and +0x20 radius as read, +0x38 group count,
// +0x40 the groups (0x28 each: +4 matrix owner ALO, +8 local MATRIX4*, +0xC flags, +0x10 chain; with flag 0x1000
// the world matrix is baked at chain+0x14). An ALO's world matrix is at +0x40.
int CApplyRamTransforms(SCENE& scene, const std::string& ramPath)
{
    std::ifstream in(ramPath, std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ram.size() < 0x2000000) return 0;
    auto u32 = [&](uint32_t va) -> uint32_t {
        va &= 0x1FFFFFF;
        uint32_t v; std::memcpy(&v, ram.data() + va, 4); return v;
    };
    auto mat = [&](uint32_t va) {
        float af[16];
        std::memcpy(af, ram.data() + (va & 0x1FFFFFF), 64);
        glm::mat4 m = glm::make_mat4(af);
        m[0][3] = m[1][3] = m[2][3] = 0.0f;
        m[3][3] = 1.0f;
        return m;
    };
    auto fValid = [&](uint32_t va) { return va >= 0x100000 && va < 0x2000000; };
    // Index the RAM by its 16-byte-aligned quadwords' first word, for the bounds search.
    int capplied = 0;
    for (SCENEGLOBSET& sgs : scene.aglobset)
    {
        GLOBSET& gs = *sgs.pglobset;
        uint8_t key[16];
        std::memcpy(key, &gs.posCenter, 12);
        std::memcpy(key + 12, &gs.sRadius, 4);
        // centre at +0x10 (a VECTOR: x y z, then the radius at +0x20, not +0x1C)
        std::vector<uint32_t> acand;
        for (uint32_t va = 0x100000 + 0x10; va + 0x20 < ram.size(); va += 0x10)
        {
            if (std::memcmp(ram.data() + va, key, 12) != 0) continue;
            if (std::memcmp(ram.data() + va + 0x10, key + 12, 4) != 0) continue;
            const uint32_t vaGlob = va - 0x10;
            if (static_cast<int>(u32(vaGlob + 0x38)) != static_cast<int>(gs.aglob.size())) continue;
            acand.push_back(vaGlob);
        }
        if (acand.empty()) continue;
        // Several identical globs (clones): take them in stream order.
        static std::vector<uint32_t> s_used;
        uint32_t vaGlob = 0;
        for (uint32_t va : acand)
            if (std::find(s_used.begin(), s_used.end(), va) == s_used.end()) { vaGlob = va; break; }
        if (!vaGlob) vaGlob = acand[0];
        s_used.push_back(vaGlob);
        sgs.vaGlob = vaGlob;
        const uint32_t agrp = u32(vaGlob + 0x40);
        if (!fValid(agrp)) continue;
        sgs.amatGlob.resize(gs.aglob.size(), glm::mat4(1.0f));
        sgs.afVisible.resize(gs.aglob.size(), 1);
        const uint32_t paloSelf = u32(vaGlob + 0);
        sgs.oid = static_cast<int32_t>(u32(paloSelf + 0x18));
        const uint32_t pxfRoot = u32(paloSelf + 0x60);
        if (fValid(pxfRoot))
        {
            sgs.fBounds = true;
            sgs.posBounds = glm::vec3(mat(pxfRoot + 0x40) * glm::vec4(gs.posCenter, 1.0f));
            sgs.sBoundsRadius = gs.sRadius;
            const uint32_t uMRD = u32(paloSelf + 0x38);
            std::memcpy(&sgs.sMRD, &uMRD, 4);
        }
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            const uint32_t pgrp = agrp + uint32_t(i) * 0x28;
            const uint32_t grf = u32(pgrp + 0xC);
            const uint32_t chain = u32(pgrp + 0x10);
            sgs.afVisible[i] = (grf & 0x800) ? 0 : 1;
            if ((grf & 0x1000) && fValid(chain))
            {
                sgs.amatGlob[i] = mat(chain + 0x14);
                continue;
            }
            uint32_t palo = u32(pgrp + 4);
            if (!fValid(palo)) palo = paloSelf;
            glm::mat4 m = fValid(palo) ? mat(palo + 0x40) : glm::mat4(1.0f);
            const uint32_t pmat = u32(pgrp + 8);
            if (fValid(pmat)) m = m * mat(pmat);
            sgs.amatGlob[i] = m;
        }
        sgs.fTransform = true;
        ++capplied;
    }
    return capplied;
}

void ApproximateBakeLighting(SCENE& scene)
{
    double sumA[3] = {}, sumB[4] = {};
    double n = 0;
    for (SCENEGLOBSET& sgs : scene.aglobset)
        for (GLOB& glob : sgs.pglobset->aglob)
            for (const SUBGLOB& sub : glob.asubglob)
            {
                if (!(sub.grf & 0x20) || !sub.pshd || (sub.pshd->shdk != 1 && sub.pshd->shdk != 7)) continue;
                for (uint32_t k = 0; k < sub.cvtx; ++k)
                {
                    const VTXGLOB& v = glob.avtx[sub.ivtxFirst + k];
                    for (int c = 0; c < 3; ++c) sumA[c] += (v.rgbaA >> (8 * c)) & 0xFF;
                    for (int c = 0; c < 4; ++c) sumB[c] += (v.rgbaB >> (8 * c)) & 0xFF;
                    n += 1;
                }
            }
    double amb[3] = {64, 64, 64}, lit[4] = {96, 96, 96, 64};
    if (n > 0)
    {
        for (int c = 0; c < 3; ++c) amb[c] = sumA[c] / n;
        for (int c = 0; c < 4; ++c) lit[c] = sumB[c] / n;
    }
    // The world's baked ambient is the dark outdoor one; objects lit at run time (characters, props) stand near
    // their lights. A neutral ambient and the mean light colour look closest to PCSX2 on Paris.
    const double ambLum = (amb[0] + amb[1] + amb[2]) / 3.0;
    for (double& c : amb) c = 0.5 * c + 0.5 * ambLum;
    const glm::vec3 dirLight = glm::normalize(glm::vec3(0.4f, -0.3f, 0.85f));
    for (SCENEGLOBSET& sgs : scene.aglobset)
        for (GLOB& glob : sgs.pglobset->aglob)
            for (const SUBGLOB& sub : glob.asubglob)
                for (uint32_t k = 0; k < sub.cvtx && sub.ivtxFirst + k < glob.avtx.size(); ++k)
            {
                VTXGLOB& v = glob.avtx[sub.ivtxFirst + k];
                if (v.fUnbaked != 1) continue;  // (a shared GLOBSET is baked once)
                // SHD+0x14 self-illumination (the bake reads it as / 4096) lifts the lit pass.
                const double si = sub.pshd ? sub.pshd->unSelfIllum / 4096.0 : 0.0;
                const double ndl = std::max(0.0f, glm::dot(glm::normalize(v.normal + glm::vec3(1e-6f)), dirLight));
                uint32_t a = 0, b = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const double vin = ((v.rgbaA >> (8 * c)) & 0xFF) / 255.0;
                    a |= uint32_t(std::clamp(amb[c] * vin * 1.8, 0.0, 255.0)) << (8 * c);
                    b |= uint32_t(std::clamp(lit[c] * 1.2 * vin, 0.0, 255.0)) << (8 * c);
                }
                a |= (v.rgbaA & 0xFF000000u);
                // the second pass carries the vertex's alpha too (an alpha-0 vertex stays invisible)
                const double aVtx = double(v.rgbaA >> 24) / 128.0;
                b |= uint32_t(std::clamp((lit[3] * (0.6 + 0.9 * ndl) + 128.0 * si) * aVtx, 0.0, 128.0)) << 24;
                v.rgbaA = a;
                v.rgbaB = b;
                v.fUnbaked = 2;
            }
}

bool FLightsFromRam(const std::string& ramPath, LIGHTSET& lights)
{
    std::ifstream in(ramPath, std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ram.size() < 0x2000000) return false;
    auto u32 = [&](uint32_t va) { uint32_t v; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; };
    auto f32 = [&](uint32_t va) { float v; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; };
    constexpr uint32_t SW = 0x3DD490, CLASSES = 0x2BAAE8;
    auto fDerived = [&](uint32_t cid, uint32_t cidBase) {
        for (uint32_t r = CLASSES + cid * 0x14; r; r = u32(r))
            if (r == CLASSES + cidBase * 0x14) return true;
        return false;
    };
    lights = LIGHTSET{};
    for (uint32_t cid = 4; cid < 199 && fDerived(cid, 4); ++cid)
        for (uint32_t p = u32(SW + 0x3038 + cid * 0xC); p; p = u32(p + 0x20))
        {
            uint64_t grf;
            std::memcpy(&grf, ram.data() + ((p + 0x28) & 0x1FFFFFF), 8);
            if (!((grf >> 60) & 1) || u32(p + 0x160) != 0) continue;
            LIGHTREC l;
            const uint32_t q = p + 0x120;
            l.kind = u32(q);
            l.angularOffset = f32(q + 4);
            l.angularScale = f32(q + 8);
            l.distanceOffset = f32(q + 0xC);
            l.rgba = glm::vec4(f32(q + 0x10), f32(q + 0x14), f32(q + 0x18), f32(q + 0x1C));
            l.dirOrPos = glm::vec3(f32(q + 0x20), f32(q + 0x24), f32(q + 0x28));
            l.distanceScale = f32(q + 0x2C);
            if (l.kind > 1) continue;
            lights.alight.push_back(l);
        }
    lights.worldShader = glm::vec4(f32(SW + 0x3E28), f32(SW + 0x3E2C), f32(SW + 0x3E30), f32(SW + 0x3E34));
    return true;
}

int RelightForward(SCENE& scene, const LIGHTSET& lightsFrame, bool fUpload, const LIGHTSET* alightsRtgt)
{
    int c = 0;
    // The instances of a shared GLOBSET with lit (unbaked) groups get their own vertex copies: each is lit at its
    // own place (the original bakes / lights every object with its own model matrix, 0x1CD9E0 / 0x1681F8 / 0x1A4478).
    if (fUpload)
    {
        // (entries are only added: count the GLOBSETs' entries when one has not been looked at yet)
        bool fUnchecked = false;
        for (const SCENEGLOBSET& sgs : scene.aglobset)
            if (sgs.fTransform && sgs.apglInst.size() != sgs.pglobset->aglob.size()) { fUnchecked = true; break; }
        bool fNew = false;
        if (fUnchecked)
        {
            std::unordered_map<const GLOBSET*, int> mpcentry;
            for (const SCENEGLOBSET& sgs : scene.aglobset) ++mpcentry[sgs.pglobset.get()];
            for (SCENEGLOBSET& sgs : scene.aglobset)
            {
                GLOBSET& gs = *sgs.pglobset;
                if (!sgs.fTransform || sgs.apglInst.size() == gs.aglob.size()) continue;
                sgs.apglInst.assign(gs.aglob.size(), nullptr);
                if (mpcentry[&gs] < 2) continue;
                for (size_t i = 0; i < gs.aglob.size(); ++i)
                {
                    const GLOB& glob = gs.aglob[i];
                    if (glob.meshk != MESHK_Strips || !glob.pgl || glob.avtx.empty()) continue;
                    if (!std::any_of(glob.avtx.begin(), glob.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; })) continue;
                    sgs.apglInst[i] = PglInstanceCopyGpu(glob);
                    fNew = true;
                    static const bool s_fDbg = std::getenv("SLY2N_INSTDBG") != nullptr;   // debugging
                    if (s_fDbg)
                    {
                        const glm::vec3 p = glm::vec3(sgs.amatGlob[i] * glm::vec4(glob.posCenter, 1.0f));
                        std::printf("instcopy oid %d glob %zu mesh 0x%X at %.0f %.0f %.0f r %.0f entries %d\n", sgs.oid, i, glob.grfMesh, p.x, p.y,
                                    p.z, glob.sRadius, mpcentry[&gs]);
                    }
                }
            }
        }
        if (fNew) FlushInstanceGlobsGpu();
    }
    for (SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.fTransform) continue;
        const int irtgt = alightsRtgt && sgs.irtgt >= 0 && !alightsRtgt[sgs.irtgt].actx.empty() ? sgs.irtgt : -1;
        const LIGHTSET& lights = irtgt >= 0 ? alightsRtgt[irtgt] : lightsFrame;
        const bool fNewSet = sgs.irtgtLit != irtgt;
        sgs.irtgtLit = int8_t(irtgt);
        std::array<LIGHTREC, 4> alSel;
        bool fAlSel = false;
        glm::vec3 posAlSel(0.0f);
        GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            GLOB& glob = gs.aglob[i];
            if (glob.meshk != MESHK_Strips || glob.avtx.empty()) continue;
            const GLGLOB* pglInst = i < sgs.apglInst.size() ? sgs.apglInst[i] : nullptr;
            if (!(glob.grfMesh & 1))
            {
                // orig: 0x001CD9E0 -> 0x001681F8: the light bake (P_M) of an object at its own model matrix (once a
                // tick for the flagged objects; the colours only change with the place, the lights reaching the group
                // or the world shader colour)
                if (irtgt >= 0) continue;
                const glm::mat4& m = sgs.amatGlob[i];
                if (sgs.amatLit.size() != gs.aglob.size()) sgs.amatLit.assign(gs.aglob.size(), glm::mat4(0.0f));
                if (sgs.amatLit[i] == m && sgs.versionLit == WorldLightsVersion() && !sgs.akeyBake.empty() && sgs.akeyBake[i] != 0)
                    continue;   // not moved, same lights
                sgs.amatLit[i] = m;
                const glm::vec3 posSphere = glm::vec3(m * glm::vec4(glob.posCenter, 1.0f));
                const float sRadius = glob.sRadius * std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])),
                                                                         glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                                                         glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
                if (sgs.akeyBake.size() != gs.aglob.size()) sgs.akeyBake.assign(gs.aglob.size(), 0);
                const uint64_t key = BakeKey(m, posSphere, sRadius, lights) ^ uint64_t(uintptr_t(pglInst));
                if (key == sgs.akeyBake[i]) continue;
                if (!std::any_of(glob.avtx.begin(), glob.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; }))
                {
                    sgs.akeyBake[i] = key;
                    continue;
                }
                // (a shared glob without an own copy is lit by the last entry: only the entries with copies differ)
                sgs.akeyBake[i] = key;
                static const bool s_fBakeDiff = std::getenv("SLY2N_BAKEDIFF") != nullptr;   // debugging: vs a bake at the origin
                float gOrigin = 0.0f;
                auto bright = [&]() {
                    double t = 0; size_t n = 0;
                    for (const VTXGLOB& v : glob.avtx)
                        if (v.fUnbaked)
                        {
                            for (int ch = 0; ch < 3; ++ch) t += double((v.rgbaA >> (8 * ch)) & 0xFF) + double((v.rgbaB >> (8 * ch)) & 0xFF) * double(v.rgbaB >> 24) / 128.0;
                            ++n;
                        }
                    return n ? float(t / (3.0 * double(n))) : 0.0f;
                };
                if (s_fBakeDiff)
                {
                    const glm::mat4 mId = glm::mat4(glm::mat3(m));
                    BakeGlobLighting(glob, mId, glm::vec3(mId * glm::vec4(glob.posCenter, 1.0f)), sRadius, lights);
                    gOrigin = bright();
                }
                BakeGlobLighting(glob, m, posSphere, sRadius, lights);
                if (s_fBakeDiff)
                    std::printf("bakediff oid %d glob %zu at %.0f %.0f %.0f inst %d origin %.1f here %.1f\n", sgs.oid, i, posSphere.x, posSphere.y,
                                posSphere.z, pglInst != nullptr, gOrigin, bright());
                if (fUpload) UpdateGlobColorsGpu(glob, pglInst ? pglInst : glob.pgl);
                ++c;
                continue;
            }
            const glm::mat4& m = sgs.amatGlob[i];
            if (sgs.amatLit.size() != gs.aglob.size()) sgs.amatLit.assign(gs.aglob.size(), glm::mat4(0.0f));
            // a skinned group is lit with its skinned normals (P_G 808 then 522): the same place needs the same bones too
            static const bool s_fBindPose = std::getenv("SLY2_BINDPOSELIGHT") != nullptr;   // (debug: the bind pose's normals)
            const std::vector<glm::mat4>* pamatBone = !s_fBindPose && i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty() ? &sgs.aamatBone[i] : nullptr;
            if (sgs.aamatBoneLit.size() != gs.aglob.size()) sgs.aamatBoneLit.assign(gs.aglob.size(), {});
            const bool fSameBones = !pamatBone || *pamatBone == sgs.aamatBoneLit[i];
            const bool fSamePlace = !glob.fHistoryWorld && sgs.amatLit[i] == m && fSameBones && !sgs.fPosLightMoved && !fNewSet;
            if (fSamePlace && sgs.versionLit == WorldLightsVersion() && irtgt < 0) continue;   // not moved, same lights
            sgs.amatLit[i] = m;
            if (!std::any_of(glob.avtx.begin(), glob.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; })) continue;
            const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : glm::vec3(m * glm::vec4(glob.posCenter, 1.0f));
            // the lighting depends only on the place and the four lights chosen there (and the world shader): when
            // another light changed, the same four chosen bit for bit give the same colours
            if (sgs.aalLit.size() != gs.aglob.size()) sgs.aalLit.assign(gs.aglob.size(), {});
            // (the globs of an entry share its light-context position: chosen once per entry and position)
            if (!fAlSel || posCtx != posAlSel)
            {
                SelectLights(lights, posCtx, alSel.data());
                fAlSel = true;
                posAlSel = posCtx;
            }
            const std::array<LIGHTREC, 4>& al = alSel;
            const bool fSameLights = std::memcmp(al.data(), sgs.aalLit[i].data(), sizeof(al)) == 0 &&
                                     std::memcmp(&lights.worldShader, &sgs.worldShaderLit, sizeof(glm::vec4)) == 0;
            if (fSamePlace && fSameLights) continue;
            sgs.aalLit[i] = al;
            if (pamatBone) sgs.aamatBoneLit[i] = *pamatBone;
            ForwardGlobLighting(glob, m, posCtx, lights, pamatBone);
            if (static const bool s_f = std::getenv("SLY2N_FWDDBG") != nullptr; s_f)   // debugging: the forward-lit groups lit
                std::printf("fwd oid %d glob %zu ctx %.0f %.0f %.0f r %.0f A0 %08X B0 %08X\n", sgs.oid, i, posCtx.x, posCtx.y, posCtx.z, glob.sRadius,
                            glob.avtx[0].rgbaA, glob.avtx[0].rgbaB);
            static const bool s_fDbg = std::getenv("SLY2_LIGHTDBG") != nullptr;   // debugging: the skinned groups' lights
            if (s_fDbg && !glob.abone.empty())
            {
                const glm::vec3 pos = sgs.fPosLight ? sgs.posLight : glm::vec3(m * glm::vec4(glob.posCenter, 1.0f));
                LIGHTREC al[4];
                SelectLights(lights, pos, al);
                std::printf("relight oid %d ib %zu glob %zu pos %.0f %.0f %.0f A0 %08X B0 %08X\n", sgs.oid, sgs.ib, i, pos.x, pos.y, pos.z, glob.avtx[0].rgbaA, glob.avtx[0].rgbaB);
                for (const LIGHTREC& l : al)
                    std::printf("   kind %d off %.3f scale %.3f doff %.3f dscale %.5f rgba %.3f %.3f %.3f %.3f dir %.1f %.1f %.1f\n", l.kind,
                                l.angularOffset, l.angularScale, l.distanceOffset, l.distanceScale, l.rgba.r, l.rgba.g, l.rgba.b, l.rgba.a,
                                l.dirOrPos.x, l.dirOrPos.y, l.dirOrPos.z);
            }
            if (fUpload) UpdateGlobColorsGpu(glob, pglInst ? pglInst : glob.pgl);
            ++c;
        }
        sgs.versionLit = WorldLightsVersion();
        sgs.worldShaderLit = lightsFrame.worldShader;   // (the targets' sets copy the frame's)
    }
    return c;
}

void ApplyLighting(SCENE& scene, const LIGHTSET& lights)
{
    for (SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.fTransform) continue;
        GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            GLOB& glob = gs.aglob[i];
            if (glob.meshk != MESHK_Strips) continue;
            if (glob.avtx.empty() && glob.iglobTemplate >= 0)
            {
                const GLOB& tmpl = gs.aglob[size_t(glob.iglobTemplate)];
                const bool fLit = std::any_of(tmpl.avtx.begin(), tmpl.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; });
                if (!fLit) continue;
                glob.avtx = tmpl.avtx;
                glob.aindex = tmpl.aindex;
                glob.asubglob = tmpl.asubglob;
                glob.grfMesh = tmpl.grfMesh;
            }
            if (!std::any_of(glob.avtx.begin(), glob.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; })) continue;
            const glm::mat4& m = sgs.amatGlob[i];
            const glm::vec3 pos = glm::vec3(m * glm::vec4(glob.posCenter, 1.0f));
            if (glob.grfMesh & 1) ForwardGlobLighting(glob, m, sgs.fPosLight ? sgs.posLight : pos, lights);
            else BakeGlobLighting(glob, m, pos, glob.sRadius, lights);
            static const int s_oidDbg = std::getenv("SLY2N_LIGHTOID") ? std::atoi(std::getenv("SLY2N_LIGHTOID")) : -2;   // debugging
            if (sgs.oid == s_oidDbg)
            {
                std::printf("lightoid %d glob %zu mesh 0x%X %s at %.0f %.0f %.0f r %.0f cvtx %zu:", sgs.oid, i, glob.grfMesh,
                            (glob.grfMesh & 1) ? "forward" : "bake", pos.x, pos.y, pos.z, glob.sRadius, glob.avtx.size());
                for (size_t k = 0; k < glob.avtx.size() && k < 8; ++k)
                    std::printf(" [src %08X A %08X B %08X u%d]", glob.avtx[k].rgbaSrc, glob.avtx[k].rgbaA, glob.avtx[k].rgbaB, glob.avtx[k].fUnbaked);
                std::printf("\n");
            }
        }
    }
}
