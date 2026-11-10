// See scene.h.

#include "scene.h"

#include "passhelpers.h"
#include "render.h"
#include "simdmat.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
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
#ifdef __SWITCH__
        sgs.fVisibleCleared = false;   // (the groups' flags set below)
#endif
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
                    if (!FGlobLit(glob)) continue;
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
    // The entries' checks on the pass helpers (passhelpers.h), in blocks of entries: they are memory bound (each group
    // reads its entry's records of what it was last lit with, scattered across the scene). They read the world's lights
    // and write only their entry's records; what they find to light (the bakes and the forward lightings, which write
    // the groups' vertices, shared by the entries of a glob set, and upload them) is kept per block and done below.
    struct RELIGHT
    {
        SCENEGLOBSET* psgs;
        uint32_t iglob;
        bool fBake;
        glm::vec3 pos;      // bake: the sphere's centre; forward: the light context's position
        float sRadius;      // (bake)
        const LIGHTSET* plights;
        const std::vector<glm::mat4>* pamatBone;  // (forward)
        const GLGLOB* pglInst;
    };
    constexpr size_t CENTRY_BLOCK = 16;
    const size_t centryAll = scene.aglobset.size();
    const size_t cblock = (centryAll + CENTRY_BLOCK - 1) / CENTRY_BLOCK;
    static std::vector<std::vector<RELIGHT>> s_aarelight;
    if (s_aarelight.size() < cblock) s_aarelight.resize(cblock);
    static std::mutex s_mutexLitList;   // (FGlobLit's cache is the shared glob's: its first answers, one thread at a time)
    PassHelpers().ForBlocks(centryAll, CENTRY_BLOCK, [&](size_t ientryFirst, size_t ientryLim) {
    const size_t iblock = ientryFirst / CENTRY_BLOCK;
    std::vector<RELIGHT>& arelight = s_aarelight[iblock];
    arelight.clear();
    // FBakeLightsUnchangedForAll per key version, asked once a block (most groups' keys are a frame old)
    uint32_t avForAll[4] = {};
    bool afForAll[4] = {};
    int cForAll = 0;
    auto fUnchangedForAll = [&](uint32_t v) {
        for (int k = 0; k < cForAll; ++k)
            if (avForAll[k] == v) return afForAll[k];
        const bool f = FBakeLightsUnchangedForAll(v, lightsFrame);
        if (cForAll < 4) { avForAll[cForAll] = v; afForAll[cForAll] = f; ++cForAll; }
        return f;
    };
    for (size_t ientry = ientryFirst; ientry < ientryLim; ++ientry)
    {
#ifdef __SWITCH__
        // (the Switch: the entry two ahead fetched, its glob set and the list of its lit groups, scattered over the heap)
        if (ientry + 2 < ientryLim)
        {
            const SCENEGLOBSET& sn = scene.aglobset[ientry + 2];
            __builtin_prefetch(sn.pglobset.get());
            __builtin_prefetch(sn.aiglobLit.data());
        }
#endif
        SCENEGLOBSET& sgs = scene.aglobset[ientry];
        if (!sgs.fTransform) continue;
        GLOBSET& gs = *sgs.pglobset;
        // only the groups with run-time lit vertices (SCENEGLOBSET::aiglobLit): both paths below end at that test for
        // the others; an entry without any is passed over whole (its fields below are only read for its lit groups)
        if (sgs.pglobLitKey != gs.aglob.data() || sgs.cglobLitKey != gs.aglob.size())
        {
            std::lock_guard<std::mutex> lock(s_mutexLitList);
            sgs.aiglobLit.clear();
            for (size_t i = 0; i < gs.aglob.size(); ++i)
            {
                const GLOB& g = gs.aglob[i];
                if (g.meshk == MESHK_Strips && !g.avtx.empty() && FGlobLit(g)) sgs.aiglobLit.push_back(uint32_t(i));
            }
            sgs.pglobLitKey = gs.aglob.data();
            sgs.cglobLitKey = gs.aglob.size();
        }
        if (sgs.aiglobLit.empty()) continue;
        const int irtgt = alightsRtgt && sgs.irtgt >= 0 && !alightsRtgt[sgs.irtgt].actx.empty() ? sgs.irtgt : -1;
        const LIGHTSET& lights = irtgt >= 0 ? alightsRtgt[irtgt] : lightsFrame;
        const bool fNewSet = sgs.irtgtLit != irtgt;
        sgs.irtgtLit = int8_t(irtgt);
        std::array<LIGHTREC, 4> alSel;
        bool fAlSel = false;
        glm::vec3 posAlSel(0.0f);
        for (const uint32_t iglobLit : sgs.aiglobLit)
        {
#ifdef __SWITCH__
            // (the Switch: the lit group two ahead fetched, the lines of it read below and its matrices: the lit groups
            // are a scattered few of a glob set's large groups)
            if (const size_t k = size_t(&iglobLit - sgs.aiglobLit.data()); k + 2 < sgs.aiglobLit.size())
            {
                const size_t in = sgs.aiglobLit[k + 2];
                __builtin_prefetch(&gs.aglob[in]);
                __builtin_prefetch(&gs.aglob[in].fHistoryWorld);
                if (in < sgs.amatGlob.size()) __builtin_prefetch(&sgs.amatGlob[in]);
                if (in < sgs.amatLit.size()) __builtin_prefetch(&sgs.amatLit[in]);
            }
#endif
            const size_t i = iglobLit;
            GLOB& glob = gs.aglob[i];
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
                // (the same matrix bytes as at the last key: the key from then, unless a light it takes changed)
                const bool fSameMat = std::memcmp(&sgs.amatLit[i], &m, sizeof(glm::mat4)) == 0;
                sgs.amatLit[i] = m;
                // (the sphere only when a test or the bake needs it: a key that stands (below) goes without)
                glm::vec3 posSphere(0.0f);
                float sRadius = 0.0f;
                auto sphere = [&] {
                    posSphere = glm::vec3(MulMat4Vec4(m, glm::vec4(glob.posCenter, 1.0f)));
                    sRadius = glob.sRadius * std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])),
                                                                 glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                                                 glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
                };
                if (sgs.akeyBake.size() != gs.aglob.size()) sgs.akeyBake.assign(gs.aglob.size(), 0);
                if (sgs.aversionKey.size() != gs.aglob.size())
                {
                    sgs.aversionKey.assign(gs.aglob.size(), 0);
                    sgs.akeyBakeLights.assign(gs.aglob.size(), 0);
                }
                static const bool s_fKeyAlways = std::getenv("SLY2N_BAKEKEYALWAYS") != nullptr;   // (debug: the old way, every key hashed)
                const uint32_t version = WorldLightsVersion();
                // (the same matrix and no change since its key that any bake takes (FBakeLightsUnchangedForAll, once
                // per key version a call): the key stands, without the sphere or the per-group test)
                const bool fKeyStands = !s_fKeyAlways && fSameMat && sgs.aversionKey[i] != 0 && fUnchangedForAll(sgs.aversionKey[i]);
                if (!fKeyStands)
                {
                    sphere();
                    if (s_fKeyAlways || !fSameMat || sgs.aversionKey[i] == 0 ||
                        !FBakeLightsUnchangedSince(sgs.aversionKey[i], posSphere, sRadius, lights))
                        sgs.akeyBakeLights[i] = BakeKey(m, posSphere, sRadius, lights);
                    else if (static const bool s_fKeyCheck = std::getenv("SLY2N_BAKEKEYCHECK") != nullptr;   // (debug: the kept key checked)
                             s_fKeyCheck && BakeKey(m, posSphere, sRadius, lights) != sgs.akeyBakeLights[i])
                        std::printf("bakekey: kept key differs: oid %d glob %zu, kept at version %u, now %u\n", sgs.oid, i, sgs.aversionKey[i], version);
                }
                sgs.aversionKey[i] = version;
                const uint64_t key = sgs.akeyBakeLights[i] ^ uint64_t(uintptr_t(pglInst));
                if (key == sgs.akeyBake[i]) continue;
                if (fKeyStands) sphere();
                // (a shared glob without an own copy is lit by the last entry: only the entries with copies differ)
                sgs.akeyBake[i] = key;
                arelight.push_back({&sgs, uint32_t(i), true, posSphere, sRadius, &lights, nullptr, pglInst});   // (the bake: below)
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
            const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : glm::vec3(MulMat4Vec4(m, glm::vec4(glob.posCenter, 1.0f)));
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
            arelight.push_back({&sgs, uint32_t(i), false, posCtx, 0.0f, &lights, pamatBone, pglInst});   // (the lighting: below)
        }
        sgs.versionLit = WorldLightsVersion();
        sgs.worldShaderLit = lightsFrame.worldShader;   // (the targets' sets copy the frame's)
    }
    });
    // what the checks found to light, in the entries' order (the blocks hold consecutive entries)
    static std::vector<const RELIGHT*> s_apr;
    s_apr.clear();
    for (size_t iblock = 0; iblock < cblock; ++iblock)
        for (const RELIGHT& r : s_aarelight[iblock]) s_apr.push_back(&r);
    // The lightings of the groups lit once in this call (nearly all) on the pass helpers: each writes only its group's
    // vertices. A group two entries light (a shared glob set's, lit at each entry's place into the same vertices) stays
    // on the game thread below, in the entries' order with its upload after each; so does every group with
    // SLY2N_BAKEDIFF (its debug bakes). The uploads follow in the entries' order.
    static std::vector<uint8_t> s_afDone;
    s_afDone.assign(s_apr.size(), 0);
#ifdef __SWITCH__
    static std::vector<GLOBCOLORS> s_acols;   // (the Switch: the colours packed on the helpers, by lighting)
#endif
    static const bool s_fBakeDiff = std::getenv("SLY2N_BAKEDIFF") != nullptr;   // debugging: vs a bake at the origin
    if (!s_fBakeDiff && s_apr.size() > 1)
    {
        static std::vector<uint8_t> s_afAlone;
        s_afAlone.resize(s_apr.size());
#ifdef __SWITCH__
        // (the Switch: the groups lit more than once found by sorting, not by a map with a node allocated per group every
        // frame)
        static std::vector<std::pair<const GLOB*, uint32_t>> s_aglobk;
        s_aglobk.clear();
        for (size_t k = 0; k < s_apr.size(); ++k) s_aglobk.emplace_back(&s_apr[k]->psgs->pglobset->aglob[s_apr[k]->iglob], uint32_t(k));
        std::sort(s_aglobk.begin(), s_aglobk.end(), [](const auto& a, const auto& b) { return std::less<const GLOB*>()(a.first, b.first); });
        for (size_t i = 0; i < s_aglobk.size();)
        {
            size_t j = i + 1;
            while (j < s_aglobk.size() && s_aglobk[j].first == s_aglobk[i].first) ++j;
            for (size_t m = i; m < j; ++m) s_afAlone[s_aglobk[m].second] = j - i == 1;
            i = j;
        }
#else
        static std::unordered_map<const GLOB*, int> s_mpcglob;
        s_mpcglob.clear();
        for (const RELIGHT* pr : s_apr) ++s_mpcglob[&pr->psgs->pglobset->aglob[pr->iglob]];
        for (size_t k = 0; k < s_apr.size(); ++k) s_afAlone[k] = s_mpcglob[&s_apr[k]->psgs->pglobset->aglob[s_apr[k]->iglob]] == 1;
#endif
#ifdef __SWITCH__
        // (the Switch: a group lit there has its colours packed there too, for its upload below; nothing lights it again
        // in between, it is lit once in this call)
        if (s_acols.size() < s_apr.size()) s_acols.resize(s_apr.size());
#endif
        PassHelpers().ForBlocks(s_apr.size(), 2, [&](size_t kFirst, size_t kLim) {
            for (size_t k = kFirst; k < kLim; ++k)
            {
                if (!s_afAlone[k]) continue;
                const RELIGHT& r = *s_apr[k];
                SCENEGLOBSET& sgs = *r.psgs;
                GLOB& glob = sgs.pglobset->aglob[r.iglob];
                if (r.fBake) BakeGlobLighting(glob, sgs.amatGlob[r.iglob], r.pos, r.sRadius, *r.plights);
                else ForwardGlobLighting(glob, sgs.amatGlob[r.iglob], r.pos, *r.plights, r.pamatBone, sgs.aalLit[r.iglob].data());
#ifdef __SWITCH__
                if (fUpload) PackGlobColors(glob, s_acols[k]);
#endif
                s_afDone[k] = 1;
            }
        });
    }
    for (size_t k = 0; k < s_apr.size(); ++k)
    {
        const RELIGHT& r = *s_apr[k];
        const bool fDone = s_afDone[k] != 0;
        SCENEGLOBSET& sgs = *r.psgs;
        const size_t i = r.iglob;
        GLOB& glob = sgs.pglobset->aglob[i];
        const glm::mat4& m = sgs.amatGlob[i];
        const LIGHTSET& lights = *r.plights;
        if (r.fBake && !fDone)
        {
            const glm::vec3& posSphere = r.pos;
            const float sRadius = r.sRadius;
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
                            posSphere.z, r.pglInst != nullptr, gOrigin, bright());
        }
        else if (!r.fBake)
        {
            const glm::vec3& posCtx = r.pos;
            if (!fDone) ForwardGlobLighting(glob, m, posCtx, lights, r.pamatBone, sgs.aalLit[i].data());   // (the lights chosen at posCtx)
            if (static const bool s_f = std::getenv("SLY2N_FWDDBG") != nullptr; s_f)   // debugging: the forward-lit groups lit
                std::printf("fwd oid %d glob %zu ctx %.0f %.0f %.0f r %.0f A0 %08X B0 %08X\n", sgs.oid, i, posCtx.x, posCtx.y, posCtx.z, glob.sRadius,
                            glob.avtx[0].rgbaA, glob.avtx[0].rgbaB);
            static const bool s_fDbg = std::getenv("SLY2_LIGHTDBG") != nullptr;   // debugging: the skinned groups' lights
            if (s_fDbg && !glob.abone.empty())
            {
                const glm::vec3 pos = sgs.fPosLight ? sgs.posLight : glm::vec3(MulMat4Vec4(m, glm::vec4(glob.posCenter, 1.0f)));
                LIGHTREC al[4];
                SelectLights(lights, pos, al);
                std::printf("relight oid %d ib %zu glob %zu pos %.0f %.0f %.0f A0 %08X B0 %08X\n", sgs.oid, sgs.ib, i, pos.x, pos.y, pos.z, glob.avtx[0].rgbaA, glob.avtx[0].rgbaB);
                for (const LIGHTREC& l : al)
                    std::printf("   kind %d off %.3f scale %.3f doff %.3f dscale %.5f rgba %.3f %.3f %.3f %.3f dir %.1f %.1f %.1f\n", l.kind,
                                l.angularOffset, l.angularScale, l.distanceOffset, l.distanceScale, l.rgba.r, l.rgba.g, l.rgba.b, l.rgba.a,
                                l.dirOrPos.x, l.dirOrPos.y, l.dirOrPos.z);
            }
        }
        if (fUpload)
        {
#ifdef __SWITCH__
            if (fDone) UploadGlobColorsGpu(glob, r.pglInst ? r.pglInst : glob.pgl, s_acols[k]);   // (packed on the helpers)
            else UpdateGlobColorsGpu(glob, r.pglInst ? r.pglInst : glob.pgl);
#else
            UpdateGlobColorsGpu(glob, r.pglInst ? r.pglInst : glob.pgl);
#endif
        }
        ++c;
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
            const glm::vec3 pos = glm::vec3(MulMat4Vec4(m, glm::vec4(glob.posCenter, 1.0f)));
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
    // (the groups it gave vertices are lit now: every entry's list found again, a shared GLOBSET's entries too)
    for (SCENEGLOBSET& sgs : scene.aglobset) sgs.pglobLitKey = nullptr;
}
