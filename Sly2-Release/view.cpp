// --view-world W: the static scene of world record W (its textures and meshes at their load-time transforms)
// with a free camera, or one frame to a PNG (--screenshot file.png [--camera x,y,z,yaw,pitch]).
// Until the world loader lands, the scene comes from the parse maps and reference RAM (scene.h).

#include "view.h"

#include "disc.h"
#include "display.h"
#include "gl.h"
#include "hud.h"
#include "emitter.h"
#include "lightsw.h"
#include "splayer.h"
#include "sm.h"
#include "worldaccess.h"
#include "region.h"
#include "render.h"
#include "rtgt.h"
#include "cls.h"
#include "scene.h"
#include "scenesw.h"
#include "sw.h"
#include "shd.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>

namespace
{
std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The 44 world records (0x20F318: two words, then per world a 64-byte key and 12 bytes) in the boot world file.
bool FWorldKey(int iworld, const std::string& loadmaps, FK& fk, std::string& error)
{
    const std::vector<uint8_t> boot = g_disc.ReadWorld(FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40)));
    std::ifstream map(loadmaps + "/w2/map/file-0.map");
    std::string line;
    size_t ib = 0;
    while (std::getline(map, line))
        if (line.find("0x20F318") != std::string::npos) { ib = std::strtoull(line.c_str(), nullptr, 10); break; }
    if (!ib) { error = "no 0x20F318 run in the boot world map"; return false; }
    const size_t ibRec = ib + 8 + size_t(iworld) * 76;
    if (ibRec + 64 > boot.size()) { error = "world record outside the boot world"; return false; }
    fk = FkFromRecord(boot.data() + ibRec);
    return true;
}

uint32_t U32Ram(const std::vector<uint8_t>& ram, uint32_t va)
{
    uint32_t v = 0;
    if (va + 4 <= ram.size()) std::memcpy(&v, ram.data() + va, 4);
    return v;
}

bool FParseCamera(const std::string& s, CAMERA& cam)
{
    float a[5];
    if (std::sscanf(s.c_str(), "%f,%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3], &a[4]) != 5) return false;
    cam.pos = glm::vec3(a[0], a[1], a[2]);
    cam.yaw = a[3];
    cam.pitch = a[4];
    return true;
}

double s_xLast = 0, s_yLast = 0;
bool s_fMouse = false;
}  // namespace

int ViewWorld(const VIEWOPTS& vo)
{
    std::string error;
    const int iworld = vo.iworld;
    std::vector<uint8_t> world;
    std::string mapPath, ramPath;
    if (iworld == 0 || iworld == 1)
    {
        // the boot world / front end themselves
        world = g_disc.ReadWorld(FkFromRecord(g_disc.PbElf(iworld == 0 ? VA_FK_BOOT_WORLD : VA_FK_FRONTEND, 0x40)));
        mapPath = vo.loadmaps + (iworld == 0 ? "/w2/map/file-0.map" : "/w2/map/file-2.map");
    }
    else
    {
        FK fk;
        if (!FWorldKey(iworld, vo.loadmaps, fk, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        world = g_disc.ReadWorld(fk);
        mapPath = vo.loadmaps + "/w" + std::to_string(iworld) + "/map/file-3.map";
    }
    ramPath = vo.loadmaps + "/w" + std::to_string(iworld) + "/final.ram";
    g_nFilterCur = iworld;   // 0x3E1110: the world record being loaded
    SCENE scene;
    int ctransform = 0;
    std::unique_ptr<SW> psw;
    if (vo.fFromMap)
    {
        if (!FLoadSceneFromMap(world, mapPath, scene, false, error)) { std::fprintf(stderr, "load: %s\n", error.c_str()); return 1; }
        ctransform = CApplyRamTransforms(scene, ramPath);
    }
    else
    {
        // the world loader (sw.cpp): objects, their transforms, their glob sets
        InitClasses();
        FK fk;
        int filter = 0;
        if (!FResolveWorld(std::to_string(iworld), fk, filter, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        psw = std::make_unique<SW>();
        psw->filter = filter;
        std::size_t cbRead = 0, cbFile = 0;
        if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) return 1;
        std::vector<ALO*> apaloDbg;
        ctransform = CBuildSceneFromSw(psw.get(), scene, &apaloDbg);
        if (std::getenv("SLY2_DUMPFX2"))   // debugging: the type-2 meshes
        {
            int n = 0;
            for (size_t k = 0; k < scene.aglobset.size() && n < 3; ++k)
                for (const GLOB& g : scene.aglobset[k].pglobset->aglob)
                    if (g.meshk == MESHK_Effect && n < 3)
                    {
                        ++n;
                        std::printf("type2 entry %zu owner %d cbone %d strips %zu rp %d\n", k, k < apaloDbg.size() && apaloDbg[k] ? apaloDbg[k]->oid : -1,
                                    g.cbone, g.asubfx.size(), g.rp);
                        for (const SUBFX& fx : g.asubfx)
                        {
                            std::printf("  strip cv %zu cw %d recs %zu shd 0x%X:", fx.apos.size(), fx.cw, fx.arec.size() / 4, fx.pshd ? fx.pshd->id : 0);
                            for (size_t r = 0; r < fx.arec.size() && r < 48; r += 4)
                                std::printf(" [%d %d %d %d]", fx.arec[r], fx.arec[r + 1], fx.arec[r + 2], fx.arec[r + 3]);
                            std::printf("\n   pos:");
                            for (size_t v = 0; v < fx.apos.size() && v < 8; ++v) std::printf(" (%.1f %.1f %.1f)", fx.apos[v].x, fx.apos[v].y, fx.apos[v].z);
                            std::printf("\n");
                        }
                    }
        }
        if (std::getenv("SLY2_DUMPLIGHTS"))   // debugging: the lights' loaded fields and options
        {
            for (const OPT& o : psw->aoptSw)
            {
                const uint32_t va = 0x2AC880 + uint32_t(o.eopid) * 0x28;
                std::printf("  sw opt %d grf 0x%X k %u w8 0x%X w14 0x%X w18 0x%X:", o.eopid, o.grf, g_disc.U32Elf(va), g_disc.U32Elf(va + 8),
                            g_disc.U32Elf(va + 0x14), g_disc.U32Elf(va + 0x18));
                for (const OPTVAL& v : o.aval) std::printf(" [n %d g %g %g %g %g]", v.n, v.ag[0], v.ag[1], v.ag[2], v.ag[3]);
                std::printf("\n");
            }
            if (std::getenv("SLY2_DUMPEMITTERS"))
                for (LO* plo : AploOfClass(psw.get(), std::getenv("SLY2_DUMPCID") ? std::atoi(std::getenv("SLY2_DUMPCID")) : 0x1D))
                {
                    std::printf("emitter oid %d cid %d (%s) parent %d\n", plo->oid, plo->Cid(), g_acls[plo->Cid()].name, plo->paloParent ? plo->paloParent->oid : -1);
                    for (const auto& [k, u] : plo->afield)
                    {
                        float g; std::memcpy(&g, &u, 4);
                        std::printf("  field getter 0x%X +0x%X = 0x%08X (%g)\n", uint32_t(k >> 32), uint32_t(k), u, g);
                    }
                    for (const OPT& o : plo->aopt)
                    {
                        const uint32_t va = 0x2AC880 + uint32_t(o.eopid) * 0x28;
                        std::printf("  opt %d grf 0x%X k %u w8 0x%X w14 0x%X w18 0x%X w24 0x%X:", o.eopid, o.grf, g_disc.U32Elf(va),
                                    g_disc.U32Elf(va + 8), g_disc.U32Elf(va + 0x14), g_disc.U32Elf(va + 0x18), g_disc.U32Elf(va + 0x24));
                        for (const OPTVAL& v : o.aval) std::printf(" [k %d n %d g %g %g %g %g c%zu]", v.k, v.n, v.ag[0], v.ag[1], v.ag[2], v.ag[3], v.alist.size());
                        std::printf("\n");
                    }
                }
            for (LO* plo : AploLights(psw.get()))
            {
                std::printf("light oid %d cid %d (%s) alo %d xf %zu\n", plo->oid, plo->Cid(), g_acls[plo->Cid()].name,
                            int(FCidDerivedFrom(plo->Cid(), 0x30)), FCidDerivedFrom(plo->Cid(), 0x30) ? static_cast<ALO*>(plo)->axf.size() : 0);
                if (const LOX* px = dynamic_cast<const LOX*>(plo))
                {
                    const glm::mat4 w = px->pxf ? px->pxf->matWorld : glm::mat4(1.0f), m = px->mat, wm = w * m;
                    std::printf("  lox ixf %d pxf %d mat pos %g %g %g r0 %g %g %g | xf pos %g %g %g | w*m pos %g %g %g r0 %g %g %g\n", px->ixfRef, px->pxf != nullptr,
                                m[3][0], m[3][1], m[3][2], m[0][0], m[0][1], m[0][2], w[3][0], w[3][1], w[3][2], wm[3][0], wm[3][1], wm[3][2], wm[0][0], wm[0][1], wm[0][2]);
                }
                if (FCidDerivedFrom(plo->Cid(), 0x30) && !static_cast<ALO*>(plo)->axf.empty())
                {
                    const glm::mat4 m = MatWorldOfAlo(static_cast<ALO*>(plo));
                    std::printf("  world r0 %g %g %g pos %g %g %g\n", m[0][0], m[0][1], m[0][2], m[3][0], m[3][1], m[3][2]);
                }
                for (const auto& [k, u] : plo->afield)
                {
                    float g; std::memcpy(&g, &u, 4);
                    std::printf("  field getter 0x%X +0x%X = 0x%08X (%g)\n", uint32_t(k >> 32), uint32_t(k), u, g);
                }
                for (const OPT& o : plo->aopt)
                {
                    const uint32_t va = 0x2AC880 + uint32_t(o.eopid) * 0x28;
                    std::printf("  opt %d grf 0x%X k %u w8 0x%X wC 0x%X w10 0x%X w14 0x%X w18 0x%X:", o.eopid, o.grf, g_disc.U32Elf(va),
                                g_disc.U32Elf(va + 8), g_disc.U32Elf(va + 0xC), g_disc.U32Elf(va + 0x10), g_disc.U32Elf(va + 0x14), g_disc.U32Elf(va + 0x18));
                    for (const OPTVAL& v : o.aval) std::printf(" [n %d g %g %g %g %g]", v.n, v.ag[0], v.ag[1], v.ag[2], v.ag[3]);
                    std::printf("\n");
                }
            }
        }
        if (const char* pszF = std::getenv("SLY2_ONLYOIDS"))   // debugging: only the objects listed (a file of oids)
        {
            std::ifstream in(pszF);
            std::string line;
            std::getline(in, line);
            std::set<int> setoid;
            std::stringstream ss(line);
            for (std::string t; std::getline(ss, t, ',');) setoid.insert(std::atoi(t.c_str()));
            int cdrop = 0;
            for (size_t k = 0; k < scene.aglobset.size() && k < apaloDbg.size(); ++k)
                if (apaloDbg[k] && !setoid.count(apaloDbg[k]->oid))
                {
                    std::fill(scene.aglobset[k].afVisible.begin(), scene.aglobset[k].afVisible.end(), 0);
                    ++cdrop;
                    if (std::getenv("SLY2_ONLYOIDS_V"))
                        std::printf("drop entry %zu oid %d cid %d (%s) parent %d\n", k, apaloDbg[k]->oid, apaloDbg[k]->Cid(),
                                    apaloDbg[k]->Cid() >= 0 ? g_acls[apaloDbg[k]->Cid()].name : "?",
                                    apaloDbg[k]->paloParent ? apaloDbg[k]->paloParent->oid : -1);
                }
            std::printf("SLY2_ONLYOIDS: %zu objects, %d scene entries hidden\n", setoid.size(), cdrop);
        }
        if (const char* pszR = std::getenv("SLY2_OWNERS"))   // debugging: the objects whose globs have this radius
        {
            const float r = float(std::atof(pszR));
            for (size_t k = 0; k < scene.aglobset.size() && k < apaloDbg.size(); ++k)
                for (const GLOB& g : scene.aglobset[k].pglobset->aglob)
                    if (std::fabs(g.sRadius - r) < 1.0f && apaloDbg[k])
                    {
                        const ALO* pa = apaloDbg[k];
                        std::printf("entry %zu: oid %d cid %d (%s) parent %d (%s) instance %d\n", k, pa->oid, pa->Cid(),
                                    pa->Cid() >= 0 ? g_acls[pa->Cid()].name : "?", pa->paloParent ? pa->paloParent->oid : -1,
                                    pa->paloParent && pa->paloParent->Cid() >= 0 ? g_acls[pa->paloParent->Cid()].name : "-", pa->oidInstance);
                        break;
                    }
        }
        // the screen objects (sprite sets, cid 0x7E family) for the 2D
        for (LO* plo : AploOfClass(psw.get(), 0x7E))
            if (auto pso = static_cast<SPRITESET*>(plo)->pso)
            {
                if (pso->fExternal && pso->aelem.empty())
                {
                    std::string err;
                    if (!FLoadScreenFile(pso.get(), g_disc.ReadWorld(pso->fk), err))
                        std::printf("screen 0x%X: file %u: %s\n", pso->oid, pso->fk.id, err.c_str());
                }
                scene.aso.push_back(pso);
            }
    }
    if (std::getenv("SLY2_DUMPTEXT") && psw)   // debugging: the world's localized strings
    {
        for (const auto& [id, str] : psw->atext) std::printf("text 0x%X: %s\n", id, str.c_str());
        return 0;
    }
    if (std::getenv("SLY2_DUMPSCREENS"))
    {
        for (const auto& pso : scene.aso)
        {
            int ck[9] = {};
            for (const SELEM& e : pso->aelem) if (e.kind < 9) ++ck[e.kind];
            if (pso->fExternal) std::printf("  file sector 0x%X cb 0x%X id %u\n", pso->fk.isector, pso->fk.cb, pso->fk.id);
            std::printf("screen oid 0x%X ext %d elems %zu (1:%d 2:%d 3:%d 4:%d 6:%d 7:%d 8:%d) groups %zu\n", pso->oid, pso->fExternal,
                        pso->aelem.size(), ck[1], ck[2], ck[3], ck[4], ck[6], ck[7], ck[8], pso->agroup.size());
            const char* szAll = std::getenv("SLY2_DUMPSCREENOID");
            const bool fAll = szAll && std::strtol(szAll, nullptr, 0) == pso->oid;
            for (size_t g = 0; g < pso->agroup.size(); ++g)
            {
                const SGROUP& grp = pso->agroup[g];
                std::printf("  group %zu entries %d slots %d parts:", g, grp.cframeSet, grp.cframe);
                for (const SPART& part : grp.apart)
                {
                    std::printf(" [id 0x%X len %.2f keys %zu", part.id, part.dt, part.akey.size());
                    if (fAll)
                        for (const SKEY& k : part.akey)
                        {
                            std::printf(" t%.2f", k.t);
                            for (uint8_t b : k.ab) std::printf(" b%d", b);
                            for (const auto& t : k.atag) std::printf(" e%d:%d", t.first, t.second);
                            for (const SITEM& it : k.aitem)
                            {
                                const uint32_t ie = it.aw[0];
                                const SELEM* pe = nullptr;
                                for (const SELEM& e : pso->aelem) if (e.idx == ie) pe = &e;
                                std::printf(" {slot %u elem %u k%d", it.aw[1], ie, pe ? int(pe->kind) : -1);
                                if (pe && pe->kind == 4) std::printf(" g%u", pe->ref);
                                if (pe && pe->kind == 3) std::printf(" txt0x%X", pe->n3);
                                std::printf(" c%d}", it.s6);
                            }
                        }
                    if (!fAll)
                        for (const SKEY& k : part.akey)
                            for (const auto& t : k.atag) std::printf(" e%d:%d@%.2f", t.first, t.second, k.t);
                    std::printf("]");
                }
                std::printf("\n");
            }
            if (std::getenv("SLY2_DUMPSCREENHASH"))   // (debug: every element's and group's content as a hash: two discs compared)
            {
                auto mix = [](uint64_t& h, const void* pv, size_t cb) { const uint8_t* pb = static_cast<const uint8_t*>(pv); for (size_t i = 0; i < cb; ++i) h = (h ^ pb[i]) * 1099511628211ull; };
                std::function<uint64_t(const SELEM&)> hashElem = [&](const SELEM& e) {
                    uint64_t h = 1469598103934665603ull;
                    mix(h, &e.kind, 4);
                    for (const SPOLY& poly : e.apoly) { mix(h, &poly.primitive, 1); mix(h, &poly.control, 1); mix(h, &poly.rgba, 4); mix(h, poly.axy.data(), poly.axy.size() * 4); }
                    if (e.kind == 2) mix(h, &e.mat, sizeof(e.mat));
                    for (const auto& r : e.arec) mix(h, r.data(), r.size());
                    if (e.kind == 3) { mix(h, &e.aul3[1], 4); mix(h, e.ag3, 12); mix(h, e.ab3, 2); const int id = e.ab3[1] == 0 ? IdTextUsa(e.n3) : e.n3; mix(h, &id, 4); }
                    if (e.kind == 4) mix(h, &e.ref, 2);
                    if (e.kind == 6 || e.kind == 7) { mix(h, &e.x0, 2); mix(h, &e.y0, 2); mix(h, &e.x1, 2); mix(h, &e.y1, 2); mix(h, &e.idTexture, 4); }
                    return h;
                };
                for (const SELEM& e : pso->aelem)
                    if (e.kind == 8) std::printf("    H elem %u kind 8: %zu glyphs\n", e.idx, e.aul.size());
                    else if (e.kind) std::printf("    H elem %u kind %u: %016llx\n", e.idx, e.kind, static_cast<unsigned long long>(hashElem(e)));
                for (size_t g = 0; g < pso->agroup.size(); ++g)
                {
                    uint64_t h = 1469598103934665603ull;
                    const SGROUP& grp = pso->agroup[g];
                    mix(h, &grp.cframeSet, 2); mix(h, &grp.cframe, 2);
                    for (const SPART& part : grp.apart)
                    {
                        mix(h, &part.dt, 4); mix(h, &part.id, 4);
                        for (const SKEY& k : part.akey)
                        {
                            mix(h, &k.t, 4);
                            for (const SITEM& it : k.aitem) { mix(h, it.aw, 6); mix(h, &it.s6, 2); mix(h, &it.b30, 1); mix(h, &it.b31, 1); mix(h, &it.n34, 4); mix(h, &it.mat, sizeof(it.mat)); }
                            for (const auto& c : k.acolor) mix(h, c.data(), 8);
                            mix(h, k.aw.data(), k.aw.size() * 2); mix(h, k.ab.data(), k.ab.size());
                            for (const auto& t : k.atag) { mix(h, &t.first, 1); mix(h, &t.second, 4); }
                        }
                    }
                    std::printf("    H group %zu: %016llx\n", g, static_cast<unsigned long long>(h));
                }
            }
            for (const SELEM& e : pso->aelem)
            {
                if (e.kind == 6) std::printf("    image elem %u: id 0x%X\n", e.idx, e.idTexture);
                if (e.kind == 3) std::printf("    text elem %u: src %d id 0x%X size %.1f w %.1f h %.1f align %d colour %08X\n", e.idx, e.ab3[1], e.n3, e.ag3[0], e.ag3[1], e.ag3[2], e.ab3[0], e.aul3[1]);
            }
        }
        return 0;
    }
    // SLY2_RTGTTEST=x,y,z,yaw,pitch: a stand-in owner for every kind-6 id (the game's 0x1BEED8), its render target
    // seeing the whole world from that camera (so the frame shows only the screens; checks rtgt.h end to end).
    static float s_artgtCam[5] = {};
    static const bool s_fRtgtTest = std::getenv("SLY2_RTGTTEST") &&
        std::sscanf(std::getenv("SLY2_RTGTTEST"), "%f,%f,%f,%f,%f", &s_artgtCam[0], &s_artgtCam[1], &s_artgtCam[2], &s_artgtCam[3], &s_artgtCam[4]) == 5;
    static int s_ownerTest = 0;
    if (s_fRtgtTest) g_pfnRtgtOwnerOfId = [](int) -> const void* { return &s_ownerTest; };
    LIGHTSET lights;
    // The world's lights natively (lightsw.h) unless --ram-lights: the records from the options and transforms.
    if (psw && !vo.fApproxLight && !vo.fRamLights)
    {
        UpdateWorldLightsFromSw(psw.get());
        if (std::getenv("SLY2_LIGHTCHECK")) CheckLightsAgainstRam(psw.get(), ramPath.c_str(), true);
    }
    if (!vo.fApproxLight && !vo.fRamLights && FHaveWorldLights())
    {
        ApplyLighting(scene, WorldLights());
        std::printf("lights: %zu from the world\n", WorldLights().alight.size());
    }
    else if (!vo.fApproxLight && FLightsFromRam(ramPath, lights))
    {
        ApplyLighting(scene, lights);
        std::printf("lights: %zu active, world shader %.3f %.3f %.3f %.3f\n", lights.alight.size(), lights.worldShader.x,
                    lights.worldShader.y, lights.worldShader.z, lights.worldShader.w);
    }
    else ApproximateBakeLighting(scene);
    std::printf("world %d: %zu globsets (%d placed%s), %d shaders, %d bitmaps\n", iworld,
                scene.aglobset.size(), ctransform, vo.fFromMap ? " from the reference RAM" : " by the object model",
                g_shdstats.cshd, g_shdstats.cbmp);

    if (std::getenv("SLY2_LISTRP"))
    {
        const int rp = std::atoi(std::getenv("SLY2_LISTRP"));
        for (const SCENEGLOBSET& sgs : scene.aglobset)
            for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
            {
                const GLOB& g = sgs.pglobset->aglob[i];
                if (g.rp != rp) continue;
                glm::vec3 c = sgs.fTransform ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : g.posCenter;
                if (g.meshk >= 2) { size_t cv = 0; for (const SUBFX& fx : g.asubfx) cv += fx.apos.size(); std::printf("  meshk %d strips %zu positions %zu first rec %02X%02X%02X%02X\n", g.meshk, g.asubfx.size(), cv,
                    g.asubfx.empty() || g.asubfx[0].arec.size() < 4 ? 0 : g.asubfx[0].arec[0], g.asubfx.empty() || g.asubfx[0].arec.size() < 4 ? 0 : g.asubfx[0].arec[1], g.asubfx.empty() || g.asubfx[0].arec.size() < 4 ? 0 : g.asubfx[0].arec[2], g.asubfx.empty() || g.asubfx[0].arec.size() < 4 ? 0 : g.asubfx[0].arec[3]); }
                std::printf("rp %d glob@%zu #%zu placed %d hidden %d visible %d at %.0f %.0f %.0f r %.0f MRD %.0g\n", rp, sgs.ib, i, int(sgs.fTransform),
                            int(g.fHidden), sgs.fTransform ? int(sgs.afVisible[i]) : -1, c.x, c.y, c.z, g.sRadius, g.sMRD);
            }
    }
    RENDEROPTS opts;
    CAMERA cam;
    const std::vector<uint8_t> ram = ReadAll(ramPath);
    const std::vector<uint8_t> ramCam = vo.cameraRam.empty() ? ram : ReadAll(vo.cameraRam);
    if (ram.size() >= 0x2000000 && ramCam.size() >= 0x2000000)
    {
        opts.rgbaCel = U32Ram(ram, 0x2DEB50);
        opts.rgbaFog = U32Ram(ram, 0x2DE18C);
        // The reference's camera (CM 0x2DDF40: +0x40/+0x44 frustum tangents, +0x4C near, +0x60 the camera matrix,
        // row 0 the view direction, row 3 the position).
        const uint32_t cm = 0x2DDF40;
        float m[16], tx, ty, sNear;
        std::memcpy(m, ramCam.data() + cm + 0x60, 64);
        std::memcpy(&tx, ramCam.data() + cm + 0x40, 4);
        std::memcpy(&ty, ramCam.data() + cm + 0x44, 4);
        std::memcpy(&sNear, ramCam.data() + cm + 0x4C, 4);
        std::memcpy(&cam.rMRDAdjust, ramCam.data() + cm + 0x1C, 4);
        std::memcpy(&cam.grfChannels, ramCam.data() + cm + 0x14, 4);
        cam.pos = glm::vec3(m[12], m[13], m[14]);
        // Fog (CM +0x240 near, +0x244 far, +0x248 max) and FOGCOL 0x2DE18C, from the same RAM as the camera.
        std::memcpy(&opts.sFogNear, ramCam.data() + cm + 0x240, 4);
        std::memcpy(&opts.sFogFar, ramCam.data() + cm + 0x244, 4);
        std::memcpy(&opts.uFogMax, ramCam.data() + cm + 0x248, 4);
        opts.rgbaFog = U32Ram(ramCam, 0x2DE18C);
        opts.fFog = !vo.fNoFog && opts.sFogNear > 0 && opts.sFogFar > opts.sFogNear;
        cam.yaw = glm::degrees(std::atan2(m[1], m[0]));
        cam.pitch = glm::degrees(std::asin(std::clamp(m[2], -1.0f, 1.0f)));
        if (ty > 0.01f && ty < 10.0f) cam.fovY = glm::degrees(2.0f * std::atan(ty));
        if (sNear > 0.1f && sNear < 1000.0f) cam.sNear = sNear;
        std::printf("reference camera: %.1f,%.1f,%.1f,%.2f,%.2f (fov %.1f, near %.1f)\n", cam.pos.x, cam.pos.y, cam.pos.z,
                    cam.yaw, cam.pitch, cam.fovY, cam.sNear);
        (void)tx;
    }
    if (!vo.camera.empty() && !FParseCamera(vo.camera, cam)) { std::fprintf(stderr, "bad --camera\n"); return 2; }
    opts.fCelBorders = !vo.fNoCel;
    opts.fShadows = !vo.fNoShadow;

    const bool fShot = !vo.screenshot.empty();
    g_gl.msaa = vo.msaa;
    g_display.msaaSamples = vo.msaa;
    if (!g_gl.FInit(vo.width, vo.height, !fShot, "Sly2-Release", error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    BuildSceneGpu(scene);

    // --screens: native playback of screen objects
    struct PLAYING { CScreenPlayer player; int idNext = -1; };
    std::vector<std::unique_ptr<PLAYING>> aplaying;
    {
        size_t i = 0;
        const std::string& s = vo.screens;
        while (i < s.size())
        {
            size_t j = s.find(',', i);
            if (j == std::string::npos) j = s.size();
            const std::string tok = s.substr(i, j - i);
            i = j + 1;
            const int oid = int(std::strtol(tok.c_str(), nullptr, 0));
            const size_t colon = tok.find(':');
            const int idPart = colon != std::string::npos ? int(std::strtol(tok.c_str() + colon + 1, nullptr, 0)) : -1;
            for (const auto& pso : scene.aso)
                if (pso->oid == oid && !pso->agroup.empty())
                {
                    auto pp = std::make_unique<PLAYING>();
                    pp->player.Init(pso.get());
                    // by default the show animation (the root group's part 1: 0x30F / 0x316 / ...), then its idle loop
                    // (part 3: 0x310 / 0x318 / ...), as 0x2128E8 / 0x212A70 do with the object's ids
                    if (idPart < 0) { pp->player.FStartPartIndex(1); pp->idNext = -3; }
                    else if (!pp->player.FStartPart(idPart)) std::printf("screen 0x%X: no part 0x%X\n", oid, idPart);
                    aplaying.push_back(std::move(pp));
                    break;
                }
        }
    }
    auto updateScreens = [&](float dt) {
        if (s_fRtgtTest)
        {
            const int i = IrtgtOfOwner(&s_ownerTest);
            for (SCENEGLOBSET& sgs : scene.aglobset) sgs.irtgt = int8_t(i);
            if (i >= 0)
            {
                CAMERA c;
                c.yaw = s_artgtCam[3];
                c.pitch = s_artgtCam[4];
                const glm::vec3 fwd = c.Dir(), left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), fwd)), up = glm::cross(fwd, left);
                SetRtgtCamera(i, glm::vec3(s_artgtCam[0], s_artgtCam[1], s_artgtCam[2]), up, fwd, left, glm::radians(50.0f), 10.0f);
            }
        }
        ++g_cframeScreen;
        for (auto& pp : aplaying)
        {
            pp->player.Update(dt);
            if (pp->idNext != -1 && pp->player.FPartDone())
            {
                if (pp->idNext <= -2) pp->player.FStartPartIndex(-pp->idNext);
                else pp->player.FStartPart(pp->idNext);
                pp->idNext = -1;
            }
        }
    };
    const SW* pswText = psw.get();
    auto drawHud = [&]() {
        g_hud.Begin();
        if (!aplaying.empty())
        {
            const std::vector<FONTBRX> afont = FindFonts(scene.aso);
            const FONTBRX* pfont = afont.empty() ? nullptr : &afont[0];
            const FONTBRX* picons = afont.size() > 1 && afont[1].pso == afont[0].pso ? &afont[1] : nullptr;
            for (auto& pp : aplaying) pp->player.Draw(pfont, picons, pswText);
        }
        if (!vo.textTest.empty())
        {
            const std::vector<FONTBRX> afont = FindFonts(scene.aso);
            if (!afont.empty())
            {
                // The glyphs are black shapes (alpha 0x80) with alpha-0 holes; the text colour is the reader's
                // colour offset (0x2160D8): white; &x: the icon font in its own colours.
                std::string text;
                for (size_t i = 0; i < vo.textTest.size(); ++i)
                    text += (vo.textTest[i] == '\\' && i + 1 < vo.textTest.size() && vo.textTest[i + 1] == 'n') ? (++i, '\n') : vo.textTest[i];
                g_hud.DrawText(afont[0], afont.size() > 1 && afont[1].pso == afont[0].pso ? &afont[1] : nullptr, text, 0.0f, -vo.textSize, vo.textSize, 1.0f, 1.0f, 1, glm::vec4(255, 255, 255, 0),
                               glm::vec4(1.0f), glm::vec4(0.0f));
            }
        }
        if (!vo.hudRam.empty())
        {
            std::string log;
            CDrawHudFromRam(vo.hudRam, scene.aso, log);
            static bool s_fLogged = false;
            if (!s_fLogged) { std::printf("HUD from %s: %s\n", vo.hudRam.c_str(), log.c_str()); s_fLogged = true; }
        }
        // --fade A / --letterbox U / --wipe U: the screen primitives (for checking them)
        if (vo.fade > 0) g_hud.Fade(glm::vec4(0, 0, 0, vo.fade * 128.0f));
        if (vo.letterbox > 0) g_hud.Letterbox(vo.letterbox);
        if (vo.wipe > 0) g_hud.Wipe(vo.wipe, glm::vec4(0, 0, 0, 128));
        g_hud.Flush(g_gl.renderWidth, g_gl.renderHeight);
    };
    if (!vo.textTest.empty()) { opts.fMeshes = opts.fCelBorders = opts.fShadows = false; opts.rgbaClear = 0x00402020; }

    if (vo.cframeBench > 0)
    {
        cane::gl::SwapInterval(0);
        RenderScene(scene, cam, opts);   // warm-up (textures)
        glFinish();
        double tCpu = 0;
        const double t0 = glfwGetTime();
        for (int i = 0; i < vo.cframeBench; ++i)
        {
            const double t1 = glfwGetTime();
            cam.yaw += 360.0f / float(vo.cframeBench);   // look around
            RenderScene(scene, cam, opts);
            tCpu += glfwGetTime() - t1;
            g_gl.Present();
            cane::gl::SwapBuffers(g_gl.window);
        }
        glFinish();
        const double t = glfwGetTime() - t0;
        std::printf("bench: %d frames, %.2f ms/frame (%.0f fps), RenderScene CPU %.2f ms; last frame %d draws %d passes "
                    "in %d GL draw calls, %d records culled by the frustum, %d by MRD\n",
                    vo.cframeBench, 1000.0 * t / vo.cframeBench, vo.cframeBench / t, 1000.0 * tCpu / vo.cframeBench,
                    g_renderstats.cdraw, g_renderstats.cpass, g_renderstats.cbatch, g_renderstats.cculled, g_renderstats.cmrd);
        g_gl.Terminate();
        return 0;
    }
    if (fShot)
    {
        // --time T: the animations' state after T seconds at 60 Hz.
        for (int i = 0; i < int(vo.tAnim * 60.0f); ++i)
        {
            UpdateShaderAnimations(float(i + 1) / 60.0f, 1.0f / 60.0f);
            updateScreens(1.0f / 60.0f);
        }
        // --particles T: the emitters and particle systems run T seconds (emitter.h)
        if (vo.tParticles > 0 && psw)
        {
            const glm::vec3 dir = cam.Dir(), left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), dir));
            SetParticleCamera(cam.pos, dir, left);
            InitEmittersOfSw(psw.get());
            if (std::getenv("SLY2_DUMPEMITTERS")) DumpEmitters();
            if (vo.oidBurst != -1)
            {
                // --burst OID: the explosions with the oid burst (at their own place), then the particles run
                StepParticles(psw.get(), 1, 1.0f / 60.0f);
                ExplodeOid(psw.get(), vo.oidBurst);
            }
            StepParticles(psw.get(), int(vo.tParticles * 60.0f), 1.0f / 60.0f);
            if (std::getenv("SLY2_DUMPEMITTERS")) DumpEmitters();
        }
        RenderScene(scene, cam, opts);
        drawHud();
        glFinish();
        const bool fOk = g_gl.FWriteScreenshot(vo.screenshot);
        if (vo.tParticles > 0)
            std::printf("particles: %d emitters (%d groups), %d explosions, %d systems, %d drawn, %d RIPs drawn, %d bursts\n",
                        g_particlestats.cemitter, g_particlestats.cemitterGroup, g_particlestats.cexpl, g_particlestats.cpsys,
                        g_particlestats.cdrawn, g_particlestats.crip, g_particlestats.cburst);
        std::printf("%s %s: %d draws, %d passes, %d cel borders, %d textures (%d failed)\n", fOk ? "wrote" : "FAILED to write",
                    vo.screenshot.c_str(), g_renderstats.cdraw, g_renderstats.cpass, g_renderstats.ccel,
                    g_renderstats.ctexture, g_renderstats.cfailTexture);
        g_gl.Terminate();
        return fOk ? 0 : 1;
    }

    std::printf("free camera: WASD move, Q/E down/up, shift fast, right mouse look, C cel borders, P print camera, "
                "F12 screenshot (view.png)\n");
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
        const glm::vec3 fwd = cam.Dir();
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
        if (glfwGetKey(w, GLFW_KEY_W)) cam.pos += fwd * speed;
        if (glfwGetKey(w, GLFW_KEY_S)) cam.pos -= fwd * speed;
        if (glfwGetKey(w, GLFW_KEY_D)) cam.pos += right * speed;
        if (glfwGetKey(w, GLFW_KEY_A)) cam.pos -= right * speed;
        if (glfwGetKey(w, GLFW_KEY_E)) cam.pos.z += speed;
        if (glfwGetKey(w, GLFW_KEY_Q)) cam.pos.z -= speed;
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
            }
            s_fMouse = true;
        }
        else s_fMouse = false;
        s_xLast = x; s_yLast = y;
        int fbw, fbh;
        glfwGetFramebufferSize(w, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != g_display.outputWidth || fbh != g_display.outputHeight)) LayoutDisplay(fbw, fbh);
        UpdateShaderAnimations(float(t), dt);
        updateScreens(dt);
        RenderScene(scene, cam, opts);
        drawHud();
        if (fF12 && !fF12Last) g_gl.FWriteScreenshot("view.png");
        fF12Last = fF12;
        g_gl.Present();
        cane::gl::SwapBuffers(w);
    }
    g_gl.Terminate();
    return 0;
}
