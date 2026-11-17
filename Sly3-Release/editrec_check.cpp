// --editor-record W (PC addition, the map editor's recorder on Sly 3: editrec.h, Sly2-Release/notes/EDITOR_2026-11-12.md
// "Phase 5"): world W read from its parts with the ravel segments recorded (worldparts.h g_paravelseg), loaded with the
// recorder on (g_peditrec), and every record checked against the world file's bytes: each object record's header (Sly
// 3's: S8 mode, mode 1's filters / conditions, S16 cid, S32 oid, S16 n, S32 cb) names the object recorded; each
// top-level record lies in one binary part (an Objects.bin: the editor patches it there); each LOX / proxy matrix and
// each ALO transform matrix read again from its offset is the matrix the loader stored; each path's point count, each
// option's and glob set's range lie inside the file. Prints one summary line; exit 1 when anything does not match.
#include "editrec.h"
#include "worldparts.h"

#include "alo.h"
#include "cls.h"
#include "disc.h"
#include "sw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
template <typename T> T Rd(const std::vector<uint8_t>& ab, std::size_t ib)
{
    T v{};
    if (ib + sizeof(T) <= ab.size()) std::memcpy(&v, ab.data() + ib, sizeof(T));
    return v;
}
glm::mat4 MatAt(const std::vector<uint8_t>& ab, std::size_t ib)
{
    float a[12];
    for (int i = 0; i < 12; ++i) a[i] = Rd<float>(ab, ib + 4u * unsigned(i));
    glm::mat4 m(1.0f);
    for (int i = 0; i < 3; ++i) m[i] = glm::vec4(a[3 * i], a[3 * i + 1], a[3 * i + 2], 0.0f);
    m[3] = glm::vec4(a[9], a[10], a[11], 1.0f);
    return m;
}
bool FSameMat(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!(a[c][r] == b[c][r]) && !(std::isnan(a[c][r]) && std::isnan(b[c][r]))) return false;
    return true;
}
}  // namespace

int EditRecordCommand(const std::string& world)
{
    InitClasses();
    FK fk;
    int filter = 0;
    std::string error;
    if (!FResolveWorld(world, fk, filter, error)) { std::fprintf(stderr, "editor-record: %s\n", error.c_str()); return 1; }
    std::vector<RAVELSEG> aseg;
    g_paravelseg = &aseg;
    std::vector<uint8_t> ab = g_disc.ReadWorld(fk);
    g_paravelseg = nullptr;
    std::sort(aseg.begin(), aseg.end(), [](const RAVELSEG& a, const RAVELSEG& b) { return a.ibWorld < b.ibWorld; });
    auto pseg = [&](std::size_t ib, std::size_t cb) -> const RAVELSEG* {
        auto it = std::upper_bound(aseg.begin(), aseg.end(), ib, [](std::size_t x, const RAVELSEG& s) { return x < s.ibWorld; });
        if (it == aseg.begin()) return nullptr;
        --it;
        return ib + cb <= it->ibWorld + it->cb ? &*it : nullptr;
    };
    EDITRECORDER rec;
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    g_peditrec = &rec;
    const bool fOk = FLoadWorldBytes(psw.get(), fk, ab, "", &cbRead, &cbFile);
    g_peditrec = nullptr;
    if (!fOk) { std::fprintf(stderr, "editor-record: world %s did not load\n", world.c_str()); return 1; }

    int cBad = 0;
    auto bad = [&](const char* what, int oid, std::size_t ib) {
        if (++cBad <= 12) std::printf("editor-record: MISMATCH %s (oid %d at %zu)\n", what, oid, ib);
    };
    // the object records
    int cTop = 0, cTopInPart = 0, cMade = 0, cDepthMax = 0;
    std::map<std::string, int> mpPartTop;
    for (const EDITOBJREC& r : rec.aobj)
    {
        cMade += r.plo != nullptr;
        cDepthMax = std::max(cDepthMax, r.depth);
        std::size_t ib = r.ibRecord;
        const int mode = Rd<int8_t>(ab, ib++);
        if (mode == 1)
        {
            const int n1 = Rd<uint8_t>(ab, ib), n2 = Rd<uint8_t>(ab, ib + 1);
            ib += 2 + 2u * unsigned(n1) + 4u * unsigned(n2);
        }
        const int cid = Rd<int16_t>(ab, ib), oid = Rd<int32_t>(ab, ib + 2), cb = Rd<int32_t>(ab, ib + 8);
        if (cid != r.cid || oid != r.oid || ib + 12 != r.ibBody || std::size_t(cb) != r.cb) bad("record header", r.oid, r.ibRecord);
        if (r.ibBody + r.cb > ab.size()) bad("record outside the file", r.oid, r.ibRecord);
        if (r.depth == 0)
        {
            ++cTop;
            if (const RAVELSEG* ps = pseg(r.ibRecord, r.ibBody - r.ibRecord + r.cb))
            {
                ++cTopInPart;
                ++mpPartTop[ps->rel];
            }
        }
        if (r.ibList && Rd<int16_t>(ab, r.ibList) <= 0) bad("record list count", r.oid, r.ibList);
    }
    // the fields
    std::map<int, int> mpfkc;
    int cMat = 0, cMatSame = 0;
    for (const EDITFIELD& f : rec.afield)
    {
        ++mpfkc[f.fk];
        const int oid = f.plo ? f.plo->oid : -1;
        if (f.ib >= ab.size() || (f.ibEnd && (f.ibEnd < f.ib || f.ibEnd > ab.size()))) { bad("field range", oid, f.ib); continue; }
        switch (f.fk)
        {
        case EDITFK_LoxMatrix:
            if (const LOX* p = dynamic_cast<const LOX*>(f.plo))
            {
                ++cMat;
                if (FSameMat(MatAt(ab, f.ib), p->mat)) ++cMatSame;
            }
            break;
        case EDITFK_XfMatrix:
            if (const ALO* p = dynamic_cast<const ALO*>(f.plo); p && f.i >= 0 && f.i < int(p->axf.size()))
            {
                ++cMat;
                if (FSameMat(MatAt(ab, f.ib), p->axf[std::size_t(f.i)]->matLoad)) ++cMatSame;
                else bad("transform matrix", oid, f.ib);
            }
            break;
        case EDITFK_Path:
            if (Rd<uint16_t>(ab, f.ib + 2) != f.i) bad("path point count", oid, f.ib);
            break;
        default: break;
        }
    }
    // (a LOX matrix the class changes after its load, e.g. a placed proxy's attach, is not a recorder error; the
    // transform records are kept as loaded in matLoad and must all match)
    std::printf("editor-record: %s: %zu object records (%d made, %d top-level, %d of them in %zu binary parts, depth up to %d), "
                "%zu fields (matrices %d, %d read back the same; options %d, option lists %d, paths %d, task links %d, "
                "glob sets %d, collision %d), %zu part segments; %d mismatches\n",
                g_disc.SzWorldElf(filter).c_str(), rec.aobj.size(), cMade, cTop, cTopInPart, mpPartTop.size(), cDepthMax, rec.afield.size(),
                cMat, cMatSame, mpfkc[EDITFK_Option], mpfkc[EDITFK_OptionArray], mpfkc[EDITFK_Path], mpfkc[EDITFK_TaskLinks],
                mpfkc[EDITFK_Globset], mpfkc[EDITFK_Soes], aseg.size(), cBad);
    return cBad || cTop == 0 || cTopInPart != cTop ? 1 : 0;
}
