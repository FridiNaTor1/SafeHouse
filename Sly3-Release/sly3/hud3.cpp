// See hud3.h.

#include "hud3.h"

#include "addr.h"
#include "load3.h"
#include "text3.h"

#include "../alo.h"
#include "../sw.h"

#include "../bis.h"
#include "../disc.h"
#include "../hud.h"
#include "../rtgt.h"
#include "../scene.h"
#include "../sound.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>

HUD3 g_hud3;

namespace
{
void FinishHud3(const TEXT3* ptext);
}

namespace
{
// Windows-1252's 0x80..0x9F
const std::pair<char16_t, uint8_t> s_acp1252[] = {
    {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
    {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91},
    {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98},
    {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F}};

int CodeFromCh(char16_t ch)
{
    if (ch < 0x80 || (ch >= 0xA0 && ch < 0x100)) return ch;
    for (const auto& [u, c] : s_acp1252)
        if (u == ch) return c;
    return -1;
}

SELEM ElemFromElem3(const SELEM3& e3, uint32_t idx)
{
    SELEM e;
    e.idx = idx;
    e.kind = e3.kind;
    switch (e3.kind)
    {
    case 1:
        e.apoly.resize(e3.apoly.size());
        for (size_t i = 0; i < e3.apoly.size(); ++i)
        {
            const SPOLY3& p3 = e3.apoly[i];
            SPOLY& p = e.apoly[i];
            p.primitive = p3.primitive;
            p.unused = p3.unused;
            p.control = p3.control;
            p.fVertexBytes = p3.fVertexBytes;
            for (const SVTX3& v : p3.avtx)
            {
                p.rgba = v.rgba;   // (Sly 2's loader keeps the last vertex's word too)
                p.axy.push_back({v.x, v.y});
                p.ab.push_back(v.b);
            }
        }
        break;
    case 3:
        e.aul3[1] = e3.rgba3;
        e.ag3[0] = e3.ag3[0];   // size +0x18
        e.ag3[2] = e3.ag3[1];   // height +0x20
        e.ag3[1] = e3.ag3[2];   // width +0x1C
        e.ab3[0] = e3.ab3[0];   // halign +0x24
        e.ab3[1] = e3.ab3[1];   // kind +0xC
        e.n3 = e3.n3;
        break;
    case 4:
        e.ref = e3.ref;
        break;
    case 6:
    case 7:
        e.x0 = e3.x0; e.y0 = e3.y0; e.x1 = e3.x1; e.y1 = e3.y1;
        e.idTexture = e3.idTexture;
        break;
    case 8:
    {
        // a font: its glyphs by the 8-bit code, sorted (FONTBRX: asub 1-based, aul / ag parallel)
        std::vector<std::pair<int, size_t>> acode;
        for (size_t i = 0; i < e3.ach.size(); ++i)
        {
            const int c = CodeFromCh(e3.ach[i]);
            if (c >= 0) acode.push_back({c, i});
        }
        std::sort(acode.begin(), acode.end());
        acode.erase(std::unique(acode.begin(), acode.end(), [](auto& a, auto& b) { return a.first == b.first; }), acode.end());
        e.asub.resize(acode.size() + 1);
        for (size_t k = 0; k < acode.size(); ++k)
        {
            e.aul.push_back(uint32_t(acode[k].first));
            e.ag.push_back(e3.adx[acode[k].second]);
            e.asub[k + 1] = ElemFromElem3(e3.aglyph[acode[k].second], uint32_t(k + 1));
        }
        break;
    }
    default: break;
    }
    return e;
}
}  // namespace

std::string StrFontFromUtf16(const std::u16string& str)
{
    std::string out;
    for (char16_t ch : str)
    {
        const int c = CodeFromCh(ch);
        if (c > 0) out += char(c);
    }
    return out;
}

std::shared_ptr<SCREENOBJ> PsoFromScreenObj3(const SCREENOBJ3& so3)
{
    auto pso = std::make_shared<SCREENOBJ>();
    pso->oid = so3.oid;
    pso->fExternal = so3.fExternal;
    pso->g90 = so3.g80;
    pso->ul94 = so3.ul84;
    pso->ul98 = so3.ul88;
    pso->ulBC = so3.ulAC;
    pso->aelem.resize(so3.aelem.size());
    for (size_t i = 0; i < so3.aelem.size(); ++i) pso->aelem[i] = ElemFromElem3(so3.aelem[i], uint32_t(i));
    pso->agroup.resize(so3.agroup.size());
    for (size_t g = 0; g < so3.agroup.size(); ++g)
    {
        const SGROUP3& g3 = so3.agroup[g];
        SGROUP& gr = pso->agroup[g];
        gr.cframeSet = g3.cframeSet;
        gr.cframe = g3.cframe;
        gr.apart.resize(g3.apart.size());
        for (size_t p = 0; p < g3.apart.size(); ++p)
        {
            const SPART3& p3 = g3.apart[p];
            SPART& part = gr.apart[p];
            part.dt = p3.dt;
            part.id = p3.id;
            part.akey.resize(p3.akey.size());
            for (size_t k = 0; k < p3.akey.size(); ++k)
            {
                const SKEY3& k3 = p3.akey[k];
                SKEY& key = part.akey[k];
                key.t = k3.t;
                for (const SITEM3& i3 : k3.aitem)
                {
                    SITEM it;
                    it.aw[0] = i3.w;          // the element
                    it.aw[1] = i3.ab[0];      // the slot
                    it.aw[2] = i3.ab[1];      // the masked section's link
                    it.s6 = i3.ab[3] == 0x7F ? -1 : int16_t(i3.ab[3]);
                    it.b30 = i3.ab[2];
                    it.n34 = i3.n;
                    for (int q = 0; q < 3; ++q) it.mat[q] = glm::vec2(i3.mat[q][0], i3.mat[q][1]);
                    key.aitem.push_back(it);
                }
                for (const auto& c : k3.acolor) key.acolor.push_back({c[1], c[0]});   // (Sly 2: [1] offset, [0] scale)
                for (uint8_t b : k3.ab) key.aw.push_back(b);
                if (k3.bFlag) key.ab.push_back(7);
                if (k3.b3 >= 1 && k3.b3 <= 4) key.atag.push_back({uint8_t(k3.b3 - 1), k3.n});
            }
        }
    }
    return pso;
}

namespace
{
const SCREEN3* s_pscreenDrawing = nullptr;

std::string StrHudText3(int id)
{
    if (!g_hud3.ptext) return "";
    const std::u16string* pstr = PstrText3(*g_hud3.ptext, id);
    return pstr ? StrFontFromUtf16(*pstr) : std::string();
}

// orig3: the script strings of 0x47A4B8 the HUD needs (0x265E38 calls the entry's fn(arg))
std::string StrScriptString3(int id)
{
    const auto it = g_hud3.mpidstr.find(id);
    if (it != g_hud3.mpidstr.end()) return it->second;
    switch (id)
    {
    case 1700: return StrHudText3(13) + "\n" + StrHudText3(17);   // 0x222140 (a new game; 14 / 16 otherwise)
    case 846: return g_hud3.idPrompt >= 0 ? StrHudText3(g_hud3.idPrompt) : std::string();   // 0x25F5E8
    case 834: return s_pscreenDrawing && s_pscreenDrawing->idTextBC >= 0 ? StrHudText3(s_pscreenDrawing->idTextBC) : std::string();
    default: return std::string();
    }
}
}  // namespace

void SetHudString3(int id, const std::string& str) { g_hud3.mpidstr[id] = str; }

namespace
{
std::vector<HEADCAM3>& Aheadcam()
{
    static std::vector<HEADCAM3> s_a;
    s_a.reserve(8);   // (the entries are the targets' owners: their addresses stay)
    return s_a;
}

const void* PvHeadOwnerOfId3(int id)
{
    for (const HEADCAM3& hc : Aheadcam())
        if (hc.id == id) return &hc;
    return nullptr;
}
}  // namespace

void SetHeadCamera3(const HEADCAM3& hc)
{
    for (HEADCAM3& h : Aheadcam())
        if (h.id == hc.id) { h = hc; return; }
    if (Aheadcam().size() < 8) Aheadcam().push_back(hc);
    g_pfnRtgtOwnerOfId = PvHeadOwnerOfId3;
}

void ClearHeadCameras3()
{
    for (const HEADCAM3& h : Aheadcam()) ReleaseRtgt(&h);
    Aheadcam().clear();
}

void BindHeadTargets3(SCENE& scene)
{
    for (SCENEGLOBSET& sgs : scene.aglobset) sgs.irtgt = -1;
    for (const HEADCAM3& hc : Aheadcam())
    {
        const int irtgt = IrtgtOfOwner(&hc);
        if (irtgt < 0) continue;
        // (0x1BE6F0's basis: up b0, forward b1, left b2)
        SetRtgtCamera(irtgt, hc.pos, hc.up, hc.dir, hc.left, 2.0f * std::atan(hc.tanY), 4.0f);
        for (SCENEGLOBSET& sgs : scene.aglobset)
            if (sgs.oid == hc.oidActor) sgs.irtgt = int8_t(irtgt);
    }
}

const LIGHTSET* AlightsHeadTargets3(const glm::vec4& worldShader)
{
    static LIGHTSET s_als[CRTGT];
    for (LIGHTSET& l : s_als) l = LIGHTSET{};
    for (const HEADCAM3& hc : Aheadcam())
    {
        const int irtgt = IrtgtOfOwner(&hc);
        if (irtgt < 0 || irtgt >= CRTGT || hc.alight.empty()) continue;
        LIGHTSET& l = s_als[irtgt];
        l.fFixed = true;
        l.actx = hc.alight;
        l.worldShader = worldShader;
    }
    return s_als;
}

void SetScreenOccluding3(SCREEN3* ps, bool f)
{
    auto& a = g_hud3.apsOccluding;
    a.erase(std::remove(a.begin(), a.end(), ps), a.end());
    if (f && ps) a.push_back(ps);
}

bool FInitHud3(const std::vector<uint8_t>& ab, const WORLDWALK3& walk, const TEXT3* ptext, std::string& error)
{
    g_hud3 = HUD3{};
    int cmis = 0;
    if (!FLoadScreenObjs3(ab, walk, g_hud3.aso3, cmis, error)) return false;
    for (SCREENOBJ3& so3 : g_hud3.aso3)
        if (so3.fExternal)
        {
            std::string errorExt;
            if (!FLoadScreenFile3(so3, errorExt)) std::fprintf(stderr, "hud3: external screen object %d: %s\n", so3.oid, errorExt.c_str());
        }
    std::map<int, bool> mpoid;
    for (const SCREENOBJ3& so3 : g_hud3.aso3)
    {
        g_hud3.aso.push_back(PsoFromScreenObj3(so3));
        if (mpoid[so3.oid]) continue;
        mpoid[so3.oid] = true;
        auto ps = std::make_unique<SCREEN3>();
        ps->pso3 = &so3;
        ps->pso = g_hud3.aso.back();
        ps->oid = so3.oid;
        ps->player.Init(ps->pso.get());
        g_hud3.ascreen.push_back(std::move(ps));
    }
    FinishHud3(ptext);
    return true;
}

namespace
{
void FinishHud3(const TEXT3* ptext)
{
    g_hud3.ptext = ptext;
    g_hud3.afont = FindFonts(g_hud3.aso);
    // the font object (orig3 SW +0x4334: m_ext's oid 1061): a text element draws with its element +0x2C (0: the text
    // font, 92 characters); "&2" switches to element 2 (the button icons, 81 characters), "&." back (0x264B70)
    if (!g_hud3.afont.empty())
    {
        const SCREENOBJ* psoFont = g_hud3.afont[0].pso;
        for (const FONTBRX& f : g_hud3.afont)
        {
            if (f.pso != psoFont) continue;
            if (f.ifont == 0 || !g_hud3.pfontText) g_hud3.pfontText = &f;
            if (f.ifont == 2) g_hud3.pfontIcons = &f;
        }
    }
    // the copied renderer in Sly 3's units (hud3.h)
    g_uiKy = 7.0f / 15.0f;
    // (x: 512 / 640 as the asm reads it. The "7% wider than PCSX2" of earlier comparisons was theirs: a 640 x 448 picture
    // puts the 4:3 canvas pillarboxed in it (display.h uiScale), while PCSX2 shows the 640 x 448 frame at 4:3. Compare
    // at 640 x 480 with --aspect ps2-4:3 (the camera's 640 / 448 aspect shown at 4:3): the 2D and the 3D both match.)
    g_uiKx = 0.8f;
    g_sScreenColorOffset = 2.0f;
    g_fScreenShdAnyFlags = true;
    g_fScreenDepthGreater = true;
    g_pfnLocalizedString = StrHudText3;
    g_pfnScriptString = StrScriptString3;
    g_idScreenSubtitle = 844;   // 0x25F598 (FDialogSubtitleText on 0x4BC220)
    g_idScreenTicker = -1;
    if (!g_sndhooks.pfnText) g_sndhooks.pfnText = StrHudText3;
}
}  // namespace

bool FInitHud3FromSw(const std::vector<uint8_t>& ab, SW* psw, const TEXT3* ptext, std::string& error)
{
    g_hud3 = HUD3{};
    CBinaryInputStream bis;
    bis.OpenMemory(int(ab.size()), ab.data());
    std::vector<const LOSCREEN3*> aplo;
    for (const auto& plo : psw->aploAll)
        if (plo->Cid() >= 0x9E && plo->Cid() <= 0xA7) aplo.push_back(static_cast<const LOSCREEN3*>(plo.get()));
    g_hud3.aso3.reserve(aplo.size());
    for (const LOSCREEN3* plo : aplo)
    {
        SCREENOBJ3 so3;
        so3.cid = plo->Cid();
        so3.oid = plo->oid;
        if (!plo->so.aelem.empty() || !plo->so.agroup.empty())
        {
            so3 = plo->so;   // (the Sly 3 class loaders read them: LOSCREEN3::so)
            so3.cid = plo->Cid();
            so3.oid = plo->oid;
        }
        else if (!plo->fExternal && plo->ibElems)
        {
            try
            {
                bis.m_ib = plo->ibElems - 17;   // (S8, F32, 3 x U32: the head the loader read)
                LoadScreenObj3(so3, &bis);
            }
            catch (const std::exception& e)
            {
                error = "screen object " + std::to_string(plo->oid) + ": " + e.what();
                return false;
            }
        }
        else
        {
            // an external screen object (screen3.h FLoadScreenFile3: its own file, read here instead of when shown)
            so3.fExternal = 1;
            so3.abKey.assign(plo->abKey.begin(), plo->abKey.end());
            std::string errorExt;
            if (!FLoadScreenFile3(so3, errorExt))
                std::fprintf(stderr, "hud3: external screen object %d: %s\n", plo->oid, errorExt.c_str());
        }
        g_hud3.aso3.push_back(std::move(so3));
        g_hud3.aso.push_back(PsoFromScreenObj3(g_hud3.aso3.back()));
        auto ps = std::make_unique<SCREEN3>();
        ps->pso3 = &g_hud3.aso3.back();
        ps->pso = g_hud3.aso.back();
        ps->oid = plo->oid;
        ps->plo = plo;
        ps->oidParent = plo->paloParent ? plo->paloParent->oid : -1;
        // the fields its options set (orig3: +0x94 the priority, +0x64 / +0x68 the show / hide parts)
        ps->priority = int32_t(plo->U32Field(0, 0x94, 0));
        ps->idShow = int32_t(plo->U32Field(0, 0x64, 886));
        ps->idHide = int32_t(plo->U32Field(0, 0x68, 888));
        ps->player.Init(ps->pso.get());
        g_hud3.ascreen.push_back(std::move(ps));
    }
    FinishHud3(ptext);
    // (testing: SLY3_HUD_SHOW=oid[:part],oid,... shows those objects from the start; SLY3_HUD_LIST=1 lists the objects)
    if (const char* sz = std::getenv("SLY3_HUD_SHOW"))
        for (const char* p = sz; *p;)
        {
            const int oid = std::atoi(p);
            const char* pc = p;   // (oid:part shows that part, e.g. an external menu's 893)
            while (*pc && *pc != ',' && *pc != ':') ++pc;
            const int idPart = *pc == ':' ? std::atoi(pc + 1) : -1;
            for (auto& ps : g_hud3.ascreen)
                if (ps->oid == oid) ShowScreen3(ps.get(), idPart);
            while (*p && *p != ',') ++p;
            if (*p) ++p;
        }
    if (std::getenv("SLY3_HUD_LIST"))
        for (const auto& ps : g_hud3.ascreen)
            std::printf("hud3: oid %d (parent %d) cid 0x%X priority %d parts %d / %d, %zu elements, %zu groups\n", ps->oid,
                        ps->oidParent, ps->pso3->cid, ps->priority, ps->idShow, ps->idHide, ps->pso->aelem.size(),
                        ps->pso->agroup.size());
    if (const char* sz = std::getenv("SLY3_HUD_LIST"); sz && *sz == '2')  // (the text elements' ids too)
        for (const auto& ps : g_hud3.ascreen)
            for (std::size_t k = 0; k < ps->pso3->aelem.size(); ++k)
                if (const SELEM3& e = ps->pso3->aelem[k]; e.kind == 3)
                    std::printf("hud3:   %d text elem %zu: kind %d id %d\n", ps->oid, k, e.ab3[1], e.n3);
    return true;
}

std::string StrTextOfId3(int id) { return StrHudText3(id); }

SCREEN3* PscreenFind3(int oid, int oidParent)
{
    for (auto& ps : g_hud3.ascreen)
        if (ps->oid == oid && (oidParent == -1 || ps->oidParent == oidParent)) return ps.get();
    return nullptr;
}

SCREEN3* PscreenFromLo3(const LO* plo)
{
    for (auto& ps : g_hud3.ascreen)
        if (ps->plo == plo) return ps.get();
    return nullptr;
}

namespace
{
bool FHasPart(const SCREEN3* ps, int id)
{
    if (id < 0 || ps->pso->agroup.empty()) return false;
    for (const SPART& p : ps->pso->agroup[0].apart)
        if (p.id == id) return true;
    return false;
}
}  // namespace

void ShowScreen3(SCREEN3* ps, int idPart)
{
    if (!ps) return;
    if (idPart >= 0) { ps->player.FStartPart(idPart); ps->state = 2; return; }
    if (FHasPart(ps, ps->idShow)) { ps->player.FStartPart(ps->idShow); ps->state = 3; }
    else { ps->player.FStartPart(ps->idIdle); ps->state = 2; }
}

void HideScreen3(SCREEN3* ps)
{
    if (!ps || ps->state < 2) return;
    if (FHasPart(ps, ps->idHide)) { ps->player.FStartPart(ps->idHide); ps->state = 4; }
    else { ps->player.StopRoot(); ps->state = 1; }
}

void UpdateHud3(float dt)
{
    ++g_cframeScreen;
    for (auto& ps : g_hud3.ascreen)
    {
        if (ps->state < 2) continue;
        // orig3: 0x261250: the show part ends in the idle part, the hide part in stopped
        if (ps->player.FPartDone())
        {
            if (ps->state == 3) { ps->player.FStartPart(ps->idIdle); ps->state = 2; continue; }
            if (ps->state == 4) { ps->player.StopRoot(); ps->state = 1; continue; }
        }
        ps->player.Update(dt);
    }
}

void DrawHud3(int width, int height)
{
    g_hud.Begin();
    std::vector<SCREEN3*> ashown;
    // orig3: 0x261BD0 / 0x262128: not drawn below an occluding object's priority
    int priorityMin = -0x7FFFFFFF;
    for (const SCREEN3* ps : g_hud3.apsOccluding) priorityMin = std::max(priorityMin, ps->priority);
    for (auto& ps : g_hud3.ascreen)
        if (ps->fVisible() && ps->priority >= priorityMin) ashown.push_back(ps.get());
    std::stable_sort(ashown.begin(), ashown.end(), [](const SCREEN3* a, const SCREEN3* b) { return a->priority < b->priority; });
    for (SCREEN3* ps : ashown)
    {
        g_zScreenBase = float(ps->priority << 18);
        s_pscreenDrawing = ps;
        ps->player.Draw(g_hud3.pfontText, g_hud3.pfontIcons, nullptr);
    }
    g_zScreenBase = 0.0f;
    s_pscreenDrawing = nullptr;
    g_hud.Flush(width, height);
}

int CApplyHudFromRam3(const std::vector<uint8_t>& ram, bool fRamSlots, std::string& log)
{
    if (ram.size() < 0x2000000) return 0;
    auto u32 = [&](uint32_t va) { uint32_t u; std::memcpy(&u, ram.data() + (va & 0x1FFFFFF), 4); return u; };
    auto s32 = [&](uint32_t va) { return int32_t(u32(va)); };
    auto s16 = [&](uint32_t va) { int16_t s; std::memcpy(&s, ram.data() + (va & 0x1FFFFFF), 2); return s; };
    auto u16 = [&](uint32_t va) { uint16_t s; std::memcpy(&s, ram.data() + (va & 0x1FFFFFF), 2); return s; };
    auto f32 = [&](uint32_t va) { float f; std::memcpy(&f, ram.data() + (va & 0x1FFFFFF), 4); return f; };
    int cshown = 0;
    g_hud3.idPrompt = s32(0x46F788);
    g_dialogsub.id = s32(0x4BC220);          // (Sly 2's 0x4435F0)
    g_dialogsub.rgba = u32(0x4BC224);
    // the active render-target cameras (class 0x06, oid 772 / 773 = ids 0x304 / 0x305)
    ClearHeadCameras3();
    {
        const uint32_t l = orig3::VA_SW + 0x3020 + 12 * 6;
        const uint32_t off = u32(l + 8);
        for (uint32_t p = u32(l), n = 0; p && n < 10000; p = u32(p + off), ++n)
        {
            const int oid = s32(p + 0x18);
            if ((oid != 772 && oid != 773) || s32(p + 0x10C) != 1 || !u32(p + 0x10)) continue;
            HEADCAM3 hc;
            hc.id = oid == 772 ? 0x304 : 0x305;
            hc.oidActor = s32(u32(p + 0x10) + 0x18);
            auto v3 = [&](uint32_t a) { return glm::vec3(f32(a), f32(a + 4), f32(a + 8)); };
            hc.dir = v3(p + 0x90);
            hc.left = v3(p + 0xA0);
            hc.up = v3(p + 0xB0);
            hc.pos = v3(p + 0xC0);
            hc.tanY = f32(p + 0x13C);
            // its lights (inline at +0x328, +0x324 of them): each light's record +0x110 and selection +0xF0 / +0x100
            for (uint32_t i = 0, c = std::min(u32(p + 0x324), 4u); i < c; ++i)
            {
                const uint32_t pl = u32(p + 0x328 + 4 * i), q = pl + 0x110;
                if (!pl) continue;
                LIGHTREC rec;
                LIGHTSEL sel;
                rec.kind = u32(q);
                rec.angularOffset = f32(q + 4);
                rec.angularScale = f32(q + 8);
                rec.distanceOffset = f32(q + 0xC);
                rec.rgba = glm::vec4(v3(q + 0x10), f32(q + 0x1C));
                rec.dirOrPos = v3(q + 0x20);
                rec.distanceScale = f32(q + 0x2C);
                sel.pos = v3(pl + 0xF0);
                sel.gConst = f32(pl + 0x100);
                sel.gSlope = f32(pl + 0x104);
                hc.alight.emplace_back(rec, sel);
            }
            SetHeadCamera3(hc);
            log += " [head " + std::to_string(hc.id) + " actor " + std::to_string(hc.oidActor) + "]";
        }
    }
    std::vector<std::pair<int, int>> aoccl;    // SW +0x431C's list (oid, parent oid)
    for (uint32_t p = u32(orig3::VA_SW + 0x431C), n = 0; p && n < 64; p = u32(p + 0xA4), ++n)
        aoccl.push_back({s32(p + 0x18), u32(p + 0x10) ? s32(u32(p + 0x10) + 0x18) : -1});
    for (int cid = 0x9E; cid <= 0xA7; ++cid)
    {
        const uint32_t l = orig3::VA_SW + 0x3020 + 12 * uint32_t(cid);
        uint32_t pso = u32(l);
        const uint32_t off = u32(l + 8);
        for (int n = 0; pso && n < 10000; ++n, pso = u32(pso + off))
        {
            const int oid = s32(pso + 0x18), state = s32(pso + 0x48);
            if (state < 2) continue;
            const uint32_t ppar = u32(pso + 0x10);
            SCREEN3* ps = PscreenFind3(oid, ppar ? s32(ppar + 0x18) : -1);
            if (!ps) ps = PscreenFind3(oid);
            if (!ps) { log += " (oid " + std::to_string(oid) + " not loaded)"; continue; }
            ps->state = state;
            ps->priority = s32(pso + 0x94);
            ps->idShow = s32(pso + 0x64);
            ps->idHide = s32(pso + 0x68);
            ps->idTextBC = s32(pso + 0xBC);
            ps->player.Init(ps->pso.get());
            const uint32_t cgroup = u32(pso + 0x70), pg = u32(pso + 0x74);
            if (cgroup != ps->pso->agroup.size()) { log += " (oid " + std::to_string(oid) + ": group count differs)"; continue; }
            ++cshown;
            log += " " + std::to_string(oid) + "(cid " + std::to_string(cid) + " state " + std::to_string(state) + " part " +
                   std::to_string(s32(pso + 0x6C)) + " pri " + std::to_string(ps->priority) + ")";
            // each group's runtime (0x30): +2 U16 entries, +8 entries of 0x18 {S32 slots, slots of 0x70, S16 part, S16
            // key, F32 time}
            if (fRamSlots)
            {
                std::vector<std::vector<bool>> afTaken(cgroup);
                for (uint32_t g = 0; g < cgroup; ++g) afTaken[g].assign(u16(pg + 0x30 * g + 2), false);
                for (uint32_t g = 0; g < cgroup; ++g)
                {
                    const uint32_t pe = u32(pg + 0x30 * g + 8);
                    auto& agr = ps->player.agroup[g];
                    for (uint32_t e = 0; e < agr.aentry.size(); ++e)
                    {
                        const uint32_t ea = pe + 0x18 * e;
                        SENTRYRT& en = agr.aentry[e];
                        en.ipart = s16(ea + 8);
                        en.ikey = s16(ea + 10);
                        en.t = f32(ea + 0xC);
                        en.fUsed = en.ipart >= 0;
                        const uint32_t pslots = u32(ea + 4);
                        for (uint32_t s = 0; s < en.aslot.size() && s < uint32_t(s32(ea)); ++s)
                        {
                            const uint32_t sa = pslots + 0x70 * s;
                            SSLOT& y = en.aslot[s];
                            y = SSLOT{};
                            y.mask = s32(sa + 0x34);
                            const uint32_t pel = u32(sa + 0x30);
                            if (!pel) continue;
                            const uint32_t idx = u32(pel);
                            if (idx >= ps->pso->aelem.size()) continue;
                            y.pelem = &ps->pso->aelem[idx];
                            for (int i = 0; i < 6; ++i) y.m[size_t(i)] = f32(sa + 0x10 + 4 * uint32_t(i));
                            y.c0 = u32(sa);
                            y.c1 = u32(sa + 4);
                            // (CrossKey's 0x264478: a kind-6 quad takes the target of the object its id names)
                            if (y.pelem->kind == SELEMK_Image && g_pfnRtgtOwnerOfId)
                                if (const void* powner = g_pfnRtgtOwnerOfId(y.pelem->idTexture); powner && IrtgtAcquire(powner) >= 0)
                                    y.prtgtOwner = powner;
                        }
                    }
                }
                // a kind-4 slot plays its group's first playing entry not yet linked (the RAM keeps no link)
                for (uint32_t g = 0; g < cgroup; ++g)
                    for (SENTRYRT& en : ps->player.agroup[g].aentry)
                        for (SSLOT& y : en.aslot)
                            if (y.pelem && y.pelem->kind == SELEMK_Ref && y.pelem->ref < cgroup)
                            {
                                auto& sub = ps->player.agroup[y.pelem->ref];
                                for (size_t k = 0; k < sub.aentry.size(); ++k)
                                    if (sub.aentry[k].ipart >= 0 && !afTaken[y.pelem->ref][k])
                                    {
                                        afTaken[y.pelem->ref][k] = true;
                                        y.ientrySub = int(k);
                                        break;
                                    }
                            }
            }
            else
            {
                // the native keys: the root's part and time, then each nested group's playing entries at theirs
                for (uint32_t g = 0; g < cgroup; ++g)
                {
                    const uint32_t pe = u32(pg + 0x30 * g + 8);
                    for (uint32_t e = 0; e < u16(pg + 0x30 * g + 2); ++e)
                    {
                        const uint32_t ea = pe + 0x18 * e;
                        const int ipart = s16(ea + 8);
                        if (ipart >= 0) ps->player.SeedEntry(int(g), int(e), ipart, f32(ea + 0xC));
                    }
                }
            }
        }
    }
    g_hud3.apsOccluding.clear();
    for (const auto& [oid, oidPar] : aoccl)
        if (SCREEN3* ps = PscreenFind3(oid, oidPar)) SetScreenOccluding3(ps, true);
    return cshown;
}

int HudDump3Command(const std::string& world)
{
    const std::vector<WORLD3> aw = g_disc.Worlds();
    for (const WORLD3& wd : aw)
    {
        if (wd.fk.cb == 0 || (wd.name != world && std::to_string(wd.iworld) != world)) continue;
        const std::vector<uint8_t> ab = g_disc.ReadWorld(wd.fk);
        WORLDWALK3 walk;
        std::string error;
        if (!FWalkWorld3(ab, walk, error)) { std::printf("walk: %s\n", error.c_str()); return 1; }
        static TEXT3 s_text;
        CBinaryInputStream bis;
        bis.OpenMemory(int(ab.size()), ab.data());
        bis.m_ib = walk.ibText;
        LoadTextFromBrx3(&s_text, 0, &bis);
        if (!FInitHud3(ab, walk, &s_text, error)) { std::printf("%s\n", error.c_str()); return 1; }
        for (size_t i = 0; i < g_hud3.aso3.size(); ++i)
        {
            const SCREENOBJ3& so3 = g_hud3.aso3[i];
            const SCREENOBJ& so = *g_hud3.aso[i];
            int ck[9] = {};
            for (const SELEM3& e : so3.aelem) ++ck[std::min<uint32_t>(e.kind, 8)];
            std::printf("oid %d cid 0x%X ext %d canvas %u x %u: %zu elements (sprites %d, text %d, refs %d, quads %d/%d, sets %d), "
                        "%zu groups; root parts:", so3.oid, so3.cid, so3.fExternal, so3.ul84, so3.ul88, so3.aelem.size(), ck[1],
                        ck[3], ck[4], ck[6], ck[7], ck[8], so3.agroup.size());
            if (!so.agroup.empty())
                for (const SPART& p : so.agroup[0].apart) std::printf(" %d(%.2fs)", p.id, p.dt);
            int cpoly = 0, cvary = 0;
            for (const SELEM3& e : so3.aelem)
                for (const SPOLY3& p : e.apoly)
                {
                    ++cpoly;
                    for (const SVTX3& v : p.avtx)
                        if (v.rgba != p.avtx[0].rgba) { ++cvary; break; }
                }
            std::printf("; %d polygons, %d with vertex colours that differ\n", cpoly, cvary);
            if (const char* sz = std::getenv("SLY3_HUD_POLYS"); sz && std::atoi(sz) == so3.oid)
                for (size_t k = 0; k < so3.aelem.size(); ++k)
                    for (const SPOLY3& p : so3.aelem[k].apoly)
                    {
                        std::printf("   elem %zu poly prim %d ctl %d vb %d cv %zu rgba %08X:", k, p.primitive, p.control,
                                    p.fVertexBytes, p.avtx.size(), p.avtx.empty() ? 0u : p.avtx[0].rgba);
                        for (const SVTX3& v : p.avtx) std::printf(" (%d %d b%02X)", v.x, v.y, v.b);
                        std::printf("\n");
                    }
            for (size_t k = 0; k < so3.aelem.size(); ++k)
            {
                const SELEM3& e = so3.aelem[k];
                if (e.kind == 3)
                    std::printf("   text elem %zu: kind %d id %d (0x%X) size %.1f w %.1f h %.1f align %d/%d rgba %08X '%s'\n", k,
                                e.ab3[1], e.n3, unsigned(e.n3), e.ag3[0], e.ag3[2], e.ag3[1], e.ab3[0], e.ab3[2], e.rgba3,
                                e.ab3[1] == 0 ? StrHudText3(e.n3).c_str() : "");
                else if (e.kind == 6 || e.kind == 7)
                    std::printf("   quad elem %zu: kind %u (%d %d)-(%d %d) id %d\n", k, e.kind, e.x0, e.y0, e.x1, e.y1, e.idTexture);
                else if (e.kind == 8)
                {
                    std::printf("   font elem %zu: mode %d, %zu characters:", k, e.mode, e.ach.size());
                    for (uint16_t ch : e.ach) std::printf(ch < 0x80 && ch > 0x20 ? " %c" : " U+%04X", ch);
                    std::printf("\n");
                }
            }
        }
        return 0;
    }
    std::printf("no world %s\n", world.c_str());
    return 1;
}
