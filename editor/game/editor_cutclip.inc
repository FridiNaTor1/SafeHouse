// The cutscene editor's animation clips (included by editor_cutscene.inc; Sly 2): an animation's (ASEG, cid 0x6D) keys
// edited as the level's own bytes (its record in the package: the game's loader reads the edited clip), and new clips
// made from poses. notes/CUTSCENES_2026-11-18.md "Round 3".
//
// The data (aseg.cpp LoadAsegFromBrx, recorded by editrec.h EDITFK_Aseg): F32 tMax, S16 channel count, each channel
// {S32 oid:20, S16 sub:10, S8 position track kind (-1 none) + its data, S8 scalar track kind + its data, U8 disabled},
// U8 second-channel count {U8 kind, S32 n, S8 scalar kind + data}, then three event lists (U8 count {S8 kind, the
// timed list's S16 frame, the kind's payload}). Key times are U16 frames (1/60 s). The model below reads exactly that
// and writes it back byte for byte (checked before any edit: a clip it does not read back the same is read only).

constexpr float c_dtKey = 1.0f / 60.0f;

struct AKTRACK
{
    int kind = -1;                                   // the S8 kind (-1: none)
    bool fScalar = false;
    std::vector<uint8_t> raw;                        // a constant track's bytes (TRKPC 12 / TRKGC 4)
    // TRKPK (position kinds 1 / 2): {U16 frame, pos, tangent in[, tangent out]}
    struct KP { uint16_t f = 0; glm::vec3 pos{0.0f}, vIn{0.0f}, vOut{0.0f}; };
    std::vector<KP> akp;
    // TRKPA (position kinds 3 / 4): v, the shared frames, the keyed axes and their {g, dgIn[, dgOut]}
    glm::vec3 v{0.0f};
    std::vector<uint16_t> af;
    uint8_t grf = 0;
    std::array<std::vector<float>, 3> aag;
    // TRKGK (scalar kind 1): {U16 frame, g, dgIn, dgOut}
    struct KG { uint16_t f = 0; float g = 0, dgIn = 0, dgOut = 0; };
    std::vector<KG> akg;

    bool FPosKeys() const { return !fScalar && (kind == 1 || kind == 2); }
    bool FAxisKeys() const { return !fScalar && (kind == 3 || kind == 4); }
    bool FScalarKeys() const { return fScalar && kind == 1; }
    bool FKeyed() const { return FPosKeys() || FAxisKeys() || FScalarKeys(); }
    int CfAxis() const { return kind == 3 ? 3 : 2; }
    std::vector<uint16_t> Frames() const
    {
        std::vector<uint16_t> a;
        if (FPosKeys()) for (const KP& k : akp) a.push_back(k.f);
        if (FAxisKeys()) a = af;
        if (FScalarKeys()) for (const KG& k : akg) a.push_back(k.f);
        return a;
    }
};

struct AKCHN
{
    int32_t word = 0;
    int16_t sub = 0;
    AKTRACK tp, tg;                                  // position / scalar
    uint8_t fDisabled = 0;
    int Oid() const { return (word << 12) >> 12; }
    int Isub() const { return ((sub & 0x3FF) << 22) >> 22; }
};

struct AKCHG
{
    uint8_t kind = 0;
    int32_t n = 0;
    AKTRACK t;
};

struct AKEV
{
    std::vector<uint8_t> raw;                        // the whole event (its S8 kind first)
    bool fTimed = false;                             // (the second list: its S16 frame at raw[1])
    int F() const { int16_t f = 0; if (fTimed && raw.size() >= 3) std::memcpy(&f, raw.data() + 1, 2); return f; }
    void SetF(int f) { const int16_t s = int16_t(std::clamp(f, -32768, 32767)); if (fTimed && raw.size() >= 3) std::memcpy(raw.data() + 1, &s, 2); }
};

struct AKDATA
{
    float tMax = 0.0f;
    std::vector<AKCHN> achn;
    std::vector<AKCHG> achg;
    std::array<std::vector<AKEV>, 3> aaev;
};

// (the readers / writers: little-endian as the world files)
struct AKRD
{
    const uint8_t* pb;
    std::size_t cb, i = 0;
    bool fOk = true;
    bool Need(std::size_t n) { if (i + n > cb) fOk = false; return fOk; }
    template <class T> T Get() { T v{}; if (Need(sizeof(T))) { std::memcpy(&v, pb + i, sizeof(T)); i += sizeof(T); } return v; }
    glm::vec3 Vec() { glm::vec3 v; v.x = Get<float>(); v.y = Get<float>(); v.z = Get<float>(); return v; }
};
struct AKWR
{
    std::vector<uint8_t>& ab;
    template <class T> void Put(T v) { const auto* p = reinterpret_cast<const uint8_t*>(&v); ab.insert(ab.end(), p, p + sizeof(T)); }
    void Vec(const glm::vec3& v) { Put(v.x); Put(v.y); Put(v.z); }
};

bool FReadAkTrack(AKRD& rd, AKTRACK& t)
{
    if (!t.fScalar)
        switch (t.kind)
        {
        case 0: t.raw.assign(rd.pb + rd.i, rd.pb + std::min(rd.cb, rd.i + 12)); rd.Need(12); rd.i += 12; break;
        case 1: case 2:
        {
            const int c = rd.Get<uint16_t>();
            t.akp.resize(std::size_t(c));
            for (AKTRACK::KP& k : t.akp)
            {
                k.f = rd.Get<uint16_t>();
                k.pos = rd.Vec();
                k.vIn = rd.Vec();
                k.vOut = t.kind == 1 ? rd.Vec() : k.vIn;
                if (!rd.fOk) return false;
            }
            break;
        }
        case 3: case 4:
        {
            t.v = rd.Vec();
            const int c = rd.Get<uint16_t>();
            t.af.resize(std::size_t(c));
            for (uint16_t& f : t.af) f = rd.Get<uint16_t>();
            t.grf = rd.Get<uint8_t>();
            for (int j = 0; j < 3 && rd.fOk; ++j)
                if (t.grf & (1 << j))
                {
                    t.aag[std::size_t(j)].resize(std::size_t(c) * std::size_t(t.CfAxis()));
                    for (float& g : t.aag[std::size_t(j)]) g = rd.Get<float>();
                }
            break;
        }
        default: break;   // (5 and the unknown kinds: nothing, as the loader)
        }
    else
        switch (t.kind)
        {
        case 0: t.raw.assign(rd.pb + rd.i, rd.pb + std::min(rd.cb, rd.i + 4)); rd.Need(4); rd.i += 4; break;
        case 1:
        {
            const int c = rd.Get<uint16_t>();
            t.akg.resize(std::size_t(c));
            for (AKTRACK::KG& k : t.akg)
            {
                k.f = rd.Get<uint16_t>();
                k.g = rd.Get<float>();
                k.dgIn = rd.Get<float>();
                k.dgOut = rd.Get<float>();
            }
            break;
        }
        default: break;
        }
    return rd.fOk;
}

void WriteAkTrack(AKWR& wr, const AKTRACK& t)
{
    if (t.kind < 0) return;
    if (!t.raw.empty()) { wr.ab.insert(wr.ab.end(), t.raw.begin(), t.raw.end()); return; }
    if (t.FPosKeys())
    {
        wr.Put(uint16_t(t.akp.size()));
        for (const AKTRACK::KP& k : t.akp)
        {
            wr.Put(k.f);
            wr.Vec(k.pos);
            wr.Vec(k.vIn);
            if (t.kind == 1) wr.Vec(k.vOut);
        }
    }
    else if (t.FAxisKeys())
    {
        wr.Vec(t.v);
        wr.Put(uint16_t(t.af.size()));
        for (uint16_t f : t.af) wr.Put(f);
        wr.Put(t.grf);
        for (int j = 0; j < 3; ++j)
            if (t.grf & (1 << j))
                for (float g : t.aag[std::size_t(j)]) wr.Put(g);
    }
    else if (t.FScalarKeys())
    {
        wr.Put(uint16_t(t.akg.size()));
        for (const AKTRACK::KG& k : t.akg) { wr.Put(k.f); wr.Put(k.g); wr.Put(k.dgIn); wr.Put(k.dgOut); }
    }
}

// an event's payload size after its kind (and frame) (aseg.cpp LoadAsegEventsFromBrx)
int CbAkEventPayload(int kind)
{
    switch (kind)
    {
    case 0: return 26;
    case 3: return 4;
    case 5: return 6;
    case 7: case 8: case 13: case 14: case 15: case 16: return 8;
    case 17: return 16;
    case 20: return 12;
    case 1: case 2: case 21: return 2;
    case 4: case 11: case 12: case 22: return 4;
    case 24: return 24;
    default: return 0;
    }
}

bool FParseAk(const uint8_t* pb, std::size_t cb, AKDATA& d)
{
    AKRD rd{pb, cb};
    d = AKDATA();
    d.tMax = rd.Get<float>();
    const int cchn = rd.Get<int16_t>();
    if (cchn < 0 || !rd.fOk) return false;
    d.achn.resize(std::size_t(cchn));
    for (AKCHN& c : d.achn)
    {
        c.word = rd.Get<int32_t>();
        c.sub = rd.Get<int16_t>();
        c.tp.kind = rd.Get<int8_t>();
        if (c.tp.kind != -1 && !FReadAkTrack(rd, c.tp)) return false;
        c.tg.fScalar = true;
        c.tg.kind = rd.Get<int8_t>();
        if (c.tg.kind != -1 && !FReadAkTrack(rd, c.tg)) return false;
        c.fDisabled = rd.Get<uint8_t>();
        if (!rd.fOk) return false;
    }
    const int cchg = rd.Get<uint8_t>();
    d.achg.resize(std::size_t(cchg));
    for (AKCHG& g : d.achg)
    {
        g.kind = rd.Get<uint8_t>();
        g.n = rd.Get<int32_t>();
        g.t.fScalar = true;
        g.t.kind = rd.Get<int8_t>();
        if (!FReadAkTrack(rd, g.t)) return false;
    }
    for (int l = 0; l < 3 && rd.fOk; ++l)
    {
        const int c = rd.Get<uint8_t>();
        for (int k = 0; k < c && rd.fOk; ++k)
        {
            const std::size_t i0 = rd.i;
            const int kind = rd.Get<int8_t>();
            if (l == 1) rd.Get<int16_t>();
            const int cbp = CbAkEventPayload(kind);
            if (!rd.Need(std::size_t(cbp))) break;
            rd.i += std::size_t(cbp);
            AKEV ev;
            ev.fTimed = l == 1;
            ev.raw.assign(pb + i0, pb + rd.i);
            d.aaev[std::size_t(l)].push_back(std::move(ev));
        }
    }
    return rd.fOk && rd.i == cb;
}

std::vector<uint8_t> AkBytes(const AKDATA& d)
{
    std::vector<uint8_t> ab;
    AKWR wr{ab};
    wr.Put(d.tMax);
    wr.Put(int16_t(d.achn.size()));
    for (const AKCHN& c : d.achn)
    {
        wr.Put(c.word);
        wr.Put(c.sub);
        wr.Put(int8_t(c.tp.kind));
        WriteAkTrack(wr, c.tp);
        wr.Put(int8_t(c.tg.kind));
        WriteAkTrack(wr, c.tg);
        wr.Put(c.fDisabled);
    }
    wr.Put(uint8_t(d.achg.size()));
    for (const AKCHG& g : d.achg)
    {
        wr.Put(g.kind);
        wr.Put(g.n);
        wr.Put(int8_t(g.t.kind));
        WriteAkTrack(wr, g.t);
    }
    for (const auto& a : d.aaev)
    {
        wr.Put(uint8_t(a.size()));
        for (const AKEV& ev : a) ab.insert(ab.end(), ev.raw.begin(), ev.raw.end());
    }
    return ab;
}

// (xfmath.cpp EvalHermite: the value and its rate at t of a segment dt long; tangents per second)
void AkHermite(float* pg, float* pdg, float dt, float t, float g0, float dg0, float g1, float dg1)
{
    if (dt <= 0.0f) { if (pg) *pg = g0; if (pdg) *pdg = 0.0f; return; }
    const float u = t / dt, v = 1.0f - u;
    if (pg) *pg = (v * v * v + u * v * v * 3.0f) * g0 + (u * v * v * dt) * dg0 - (u * u * v * dt) * dg1 + (u * u * u + u * u * v * 3.0f) * g1;
    if (pdg) *pdg = (u * (v * 6.0f)) / dt * (g1 - g0) + (1.0f - u * (4.0f - u * 3.0f)) * dg0 + (u * (u * 3.0f - 2.0f)) * dg1;
}

// a track's value / rate at t (as the game's TRKPK / TRKPA / TRKGK Eval: asega.cpp); false: no keys
bool FEvalAkTrack(const AKTRACK& tr, float t, glm::vec3& pos, glm::vec3& vel)
{
    pos = vel = glm::vec3(0.0f);
    if (tr.FPosKeys())
    {
        const int c = int(tr.akp.size());
        if (!c) return false;
        const int cm1 = c - 1;
        int i = 1;
        while (i < cm1 && !(t < tr.akp[std::size_t(i)].f * c_dtKey)) ++i;
        i = std::min(i, std::max(cm1, 0));
        const auto& k0 = tr.akp[std::size_t(std::max(i - 1, 0))];
        const auto& k1 = tr.akp[std::size_t(i)];
        const float t0 = k0.f * c_dtKey, dt = (k1.f - k0.f) * c_dtKey;
        for (int j = 0; j < 3; ++j) AkHermite(&pos[j], &vel[j], dt, t - t0, k0.pos[j], k0.vOut[j], k1.pos[j], k1.vIn[j]);
        return true;
    }
    if (tr.FAxisKeys())
    {
        pos = tr.v;
        const int c = int(tr.af.size());
        if (!c) return true;
        const int cm1 = c - 1;
        int i = 1;
        while (i < cm1 && !(t < tr.af[std::size_t(i)] * c_dtKey)) ++i;
        i = std::min(i, std::max(cm1, 0));
        const int i0 = std::max(i - 1, 0), cf = tr.CfAxis(), ofsOut = cf == 3 ? 2 : 1;
        const float t0 = tr.af[std::size_t(i0)] * c_dtKey, dt = (tr.af[std::size_t(i)] - tr.af[std::size_t(i0)]) * c_dtKey;
        for (int j = 0; j < 3; ++j)
        {
            const auto& a = tr.aag[std::size_t(j)];
            if (a.empty()) continue;
            const float* k0 = &a[std::size_t(i0 * cf)];
            const float* k1 = &a[std::size_t(i * cf)];
            AkHermite(&pos[j], &vel[j], dt, t - t0, k0[0], k0[ofsOut], k1[0], k1[1]);
        }
        return true;
    }
    if (tr.FScalarKeys())
    {
        const int c = int(tr.akg.size());
        if (!c) return false;
        int i = 1;
        while (i < c && !(t < tr.akg[std::size_t(i)].f * c_dtKey)) ++i;
        i = std::min(i, c - 1);
        const auto& k1 = tr.akg[std::size_t(i)];
        const auto& k0 = tr.akg[std::size_t(std::max(i - 1, 0))];
        AkHermite(&pos.x, &vel.x, (k1.f - k0.f) * c_dtKey, t - k0.f * c_dtKey, k0.g, k0.dgOut, k1.g, k1.dgIn);
        return true;
    }
    if (!tr.raw.empty())
    {
        if (tr.fScalar && tr.raw.size() >= 4) std::memcpy(&pos.x, tr.raw.data(), 4);
        else if (!tr.fScalar && tr.raw.size() >= 12) std::memcpy(&pos[0], tr.raw.data(), 12);
        return true;
    }
    return false;
}

// the keys of a track at frame f: moved to fTo (-1: removed); false: none there (or fTo taken)
bool FMoveAkKeys(AKTRACK& tr, int f, int fTo)
{
    auto near = [&](int g) { return std::abs(g - f) <= 0; };
    if (tr.FPosKeys())
    {
        auto it = std::find_if(tr.akp.begin(), tr.akp.end(), [&](const AKTRACK::KP& k) { return near(k.f); });
        if (it == tr.akp.end()) return false;
        if (fTo < 0) { if (tr.akp.size() <= 1) return false; tr.akp.erase(it); return true; }
        if (std::any_of(tr.akp.begin(), tr.akp.end(), [&](const AKTRACK::KP& k) { return k.f == fTo && &k != &*it; })) return false;
        it->f = uint16_t(fTo);
        std::stable_sort(tr.akp.begin(), tr.akp.end(), [](const AKTRACK::KP& a, const AKTRACK::KP& b) { return a.f < b.f; });
        return true;
    }
    if (tr.FScalarKeys())
    {
        auto it = std::find_if(tr.akg.begin(), tr.akg.end(), [&](const AKTRACK::KG& k) { return near(k.f); });
        if (it == tr.akg.end()) return false;
        if (fTo < 0) { if (tr.akg.size() <= 1) return false; tr.akg.erase(it); return true; }
        if (std::any_of(tr.akg.begin(), tr.akg.end(), [&](const AKTRACK::KG& k) { return k.f == fTo && &k != &*it; })) return false;
        it->f = uint16_t(fTo);
        std::stable_sort(tr.akg.begin(), tr.akg.end(), [](const AKTRACK::KG& a, const AKTRACK::KG& b) { return a.f < b.f; });
        return true;
    }
    if (tr.FAxisKeys())
    {
        const auto it = std::find_if(tr.af.begin(), tr.af.end(), [&](uint16_t g) { return near(g); });
        if (it == tr.af.end()) return false;
        const std::size_t i = std::size_t(it - tr.af.begin()), cf = std::size_t(tr.CfAxis());
        if (fTo >= 0 && std::count(tr.af.begin(), tr.af.end(), uint16_t(fTo)) && tr.af[i] != fTo) return false;
        if (fTo < 0 && tr.af.size() <= 1) return false;
        // (the frames and each keyed axis' entries together)
        std::vector<std::pair<uint16_t, std::array<std::vector<float>, 3>>> a;
        for (std::size_t k = 0; k < tr.af.size(); ++k)
        {
            std::array<std::vector<float>, 3> v;
            for (int j = 0; j < 3; ++j)
                if (!tr.aag[std::size_t(j)].empty()) v[std::size_t(j)].assign(tr.aag[std::size_t(j)].begin() + std::ptrdiff_t(k * cf), tr.aag[std::size_t(j)].begin() + std::ptrdiff_t((k + 1) * cf));
            a.push_back({tr.af[k], std::move(v)});
        }
        if (fTo < 0) a.erase(a.begin() + std::ptrdiff_t(i));
        else a[i].first = uint16_t(fTo);
        std::stable_sort(a.begin(), a.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        tr.af.clear();
        for (auto& g : tr.aag) if (!g.empty()) g.clear();
        for (const auto& [ff, v] : a)
        {
            tr.af.push_back(ff);
            for (int j = 0; j < 3; ++j)
                if (tr.grf & (1 << j)) tr.aag[std::size_t(j)].insert(tr.aag[std::size_t(j)].end(), v[std::size_t(j)].begin(), v[std::size_t(j)].end());
        }
        return true;
    }
    return false;
}

// a key added at frame f on the curve as it is (its value and rate there: the clip plays the same until it is moved)
bool FAddAkKey(AKTRACK& tr, int f)
{
    if (!tr.FKeyed()) return false;
    const std::vector<uint16_t> af = tr.Frames();
    if (std::find(af.begin(), af.end(), uint16_t(f)) != af.end()) return false;
    glm::vec3 pos, vel;
    FEvalAkTrack(tr, f * c_dtKey, pos, vel);
    if (tr.FPosKeys())
    {
        AKTRACK::KP k;
        k.f = uint16_t(f), k.pos = pos, k.vIn = k.vOut = vel;
        tr.akp.insert(std::upper_bound(tr.akp.begin(), tr.akp.end(), k, [](const AKTRACK::KP& a, const AKTRACK::KP& b) { return a.f < b.f; }), k);
        return true;
    }
    if (tr.FScalarKeys())
    {
        AKTRACK::KG k;
        k.f = uint16_t(f), k.g = pos.x, k.dgIn = k.dgOut = vel.x;
        tr.akg.insert(std::upper_bound(tr.akg.begin(), tr.akg.end(), k, [](const AKTRACK::KG& a, const AKTRACK::KG& b) { return a.f < b.f; }), k);
        return true;
    }
    const std::size_t i = std::size_t(std::upper_bound(tr.af.begin(), tr.af.end(), uint16_t(f)) - tr.af.begin()), cf = std::size_t(tr.CfAxis());
    tr.af.insert(tr.af.begin() + std::ptrdiff_t(i), uint16_t(f));
    for (int j = 0; j < 3; ++j)
    {
        auto& a = tr.aag[std::size_t(j)];
        if (a.empty() && !(tr.grf & (1 << j))) continue;
        std::vector<float> k = cf == 3 ? std::vector<float>{pos[j], vel[j], vel[j]} : std::vector<float>{pos[j], vel[j]};
        a.insert(a.begin() + std::ptrdiff_t(i * cf), k.begin(), k.end());
    }
    return true;
}

// every key's frame (and the length) times k; the tangents (per second) over k: the same motion, slower / faster
void StretchAkTrack(AKTRACK& tr, float k)
{
    auto fr = [&](uint16_t f) { return uint16_t(std::clamp(std::lround(f * k), 0L, 65535L)); };
    for (auto& key : tr.akp) { key.f = fr(key.f); key.vIn /= k; key.vOut /= k; }
    for (auto& key : tr.akg) { key.f = fr(key.f); key.dgIn /= k; key.dgOut /= k; }
    for (auto& f : tr.af) f = fr(f);
    for (int j = 0; j < 3; ++j)
    {
        auto& a = tr.aag[std::size_t(j)];
        for (std::size_t i = 0; i < a.size(); ++i)
            if (i % std::size_t(tr.CfAxis()) != 0) a[i] /= k;
    }
}

// ---- the clip's bytes

// The ASEG's data in its part (parsed; false: not found, or not read back byte for byte: read only)
bool CCutsceneEditor::FClipData(ASEG* paseg, AKDATA& d, std::string& rel, std::size_t& ib, std::size_t& cb, std::string& error) const
{
    if (!paseg) { error = T("no animation"); return false; }
    const EDITFIELD* pf = nullptr;
    for (const EDITFIELD& f : e.m_rec.afield)
        if (f.fk == EDITFK_Aseg && f.plo == paseg) { pf = &f; break; }
    if (!pf)
        for (const EDITFIELD& f : e.m_rec.afield)
            if (f.fk == EDITFK_Aseg && f.plo && f.plo->oid == paseg->oid && FIsLoDerivedFrom(f.plo, EDCID(0x6D))) { pf = &f; break; }
    if (!pf) { error = T("this animation's keys are not in the level's bytes"); return false; }
    cb = pf->ibEnd - pf->ib;
    const std::vector<uint8_t>* pab = nullptr;
    if (!e.FLocate(pf->ib, cb, rel, ib) || !(pab = e.m_proj.partRef(rel)) || ib + cb > pab->size()) { error = T("this animation's keys are not in a binary part"); return false; }
    if (!FParseAk(pab->data() + ib, cb, d) || AkBytes(d) != std::vector<uint8_t>(pab->begin() + std::ptrdiff_t(ib), pab->begin() + std::ptrdiff_t(ib + cb)))
    {
        error = T("this animation's keys could not be read (read only)");
        return false;
    }
    return true;
}

bool CCutsceneEditor::FWriteClip(ASEG* paseg, const AKDATA& d, const std::string& label)
{
    AKDATA dOld;
    std::string rel, error;
    std::size_t ib = 0, cb = 0;
    if (!FClipData(paseg, dOld, rel, ib, cb, error)) { e.Status(error); return false; }
    e.m_proj.begin(label, true);
    const bool fOk = e.m_proj.spliceBytes(rel, ib, cb, AkBytes(d), e.EnclosingSizeFields(rel, ib));
    e.m_proj.end();
    if (!fOk) { e.Status(T("could not write")); return false; }
    e.Status(label);
    ForgetPoses();
    e.Reload(e.m_plSel ? e.m_plSel->oid : 0);
    return true;
}

// The keys of a sub-row (an object's channels, or the timed events) at clip time t: moved to tTo / removed (tTo < 0) /
// added (fAdd); the clip's length stretched (tStretch > 0: its new length)
bool CCutsceneEditor::FEditClipKeys(const CSCMD& c, const SUBROW& row, float t, float tTo, bool fAdd, float tStretch)
{
    ASEG* paseg = PasegOfClip(c);
    AKDATA d;
    std::string rel, error;
    std::size_t ib = 0, cb = 0;
    if (!FClipData(paseg, d, rel, ib, cb, error)) { e.Status(error); return false; }
    const int f = int(std::lround(t * 60.0f)), fTo = tTo < 0.0f ? -1 : int(std::lround(tTo * 60.0f));
    char sz[96];
    std::string what;
    int cDone = 0;
    if (tStretch > 0.0f)
    {
        const float k = tStretch / std::max(c_dtKey, d.tMax);
        if (std::fabs(k - 1.0f) < 1e-4f) return false;
        for (AKCHN& ch : d.achn) { StretchAkTrack(ch.tp, k); StretchAkTrack(ch.tg, k); }
        for (AKCHG& g : d.achg) StretchAkTrack(g.t, k);
        for (AKEV& ev : d.aaev[1]) ev.SetF(int(std::lround(ev.F() * k)));
        d.tMax = std::round(tStretch * 60.0f) / 60.0f;
        cDone = 1;
        std::snprintf(sz, sizeof sz, "%s %.2f s", T("lasts"), d.tMax);
        what = sz;
    }
    else if (row.fEvents)
    {
        for (AKEV& ev : d.aaev[1])
            if (ev.F() == f && fTo >= 0) { ev.SetF(fTo); ++cDone; }
        std::stable_sort(d.aaev[1].begin(), d.aaev[1].end(), [](const AKEV& a, const AKEV& b) { return a.F() < b.F(); });
        std::snprintf(sz, sizeof sz, "%s %.2f -> %.2f s", T("events moved"), t, tTo);
        what = sz;
    }
    else
    {
        for (AKCHN& ch : d.achn)
        {
            if (ch.Oid() != row.oid) continue;
            for (AKTRACK* ptr : {&ch.tp, &ch.tg})
            {
                if (!ptr->FKeyed()) continue;
                if (fAdd) cDone += FAddAkKey(*ptr, f);
                else cDone += FMoveAkKeys(*ptr, f, fTo);
            }
        }
        if (fAdd) std::snprintf(sz, sizeof sz, "%s %.2f s", T("a key added at"), t);
        else if (fTo < 0) std::snprintf(sz, sizeof sz, "%s %.2f s", T("the key removed at"), t);
        else std::snprintf(sz, sizeof sz, "%s %.2f -> %.2f s", T("the key moved"), t, tTo);
        what = sz;
        // (a key moved past the end: the clip longer)
        if (fTo >= 0) d.tMax = std::max(d.tMax, fTo * c_dtKey);
    }
    if (!cDone) { e.Status(T("no key there (or the time is taken by another key)")); return false; }
    return FWriteClip(paseg, d, std::string(T("Animation")) + " " + std::to_string(paseg->oid) + (row.name.empty() ? std::string() : " (" + row.name + ")") + ": " + what);
}

// ---- a new clip from poses: the character posed at each key as a clip the game already has poses it there (its
// channels' values at that clip's time), the keys eased between (Catmull-Rom); a sibling of the first pose's clip
// under the same character (its channels name the character's records), a new oid

int CCutsceneEditor::NewClipFromPoses(std::string& error)
{
    if (m_apose.size() < 2) { error = T("a clip needs two poses"); return 0; }
    std::vector<CLIPPOSE> ap = m_apose;
    std::stable_sort(ap.begin(), ap.end(), [](const CLIPPOSE& a, const CLIPPOSE& b) { return a.at < b.at; });
    for (std::size_t i = 1; i < ap.size(); ++i)
        if (std::lround(ap[i].at * 60.0f) == std::lround(ap[i - 1].at * 60.0f)) { error = T("two poses at the same time"); return 0; }
    // the clips posed from (their data), the template: the first pose's
    std::map<int, AKDATA> mpclip;
    ASEG* pasegT = nullptr;
    for (const CLIPPOSE& p : ap)
    {
        LO* pl = nullptr;
        std::function<void(LO*)> find = [&](LO* q) {
            if (pl) return;
            if (FIsLoDerivedFrom(q, EDCID(0x6D)) && q->oid == p.oidClip) { pl = q; return; }
            if (const auto it = e.m_mpchildren.find(q); it != e.m_mpchildren.end())
                for (LO* k : it->second) find(k);
        };
        if (LO* pa = PloOfOid(p.oidActor)) find(pa);
        if (!pl) { error = std::string(T("the pose's animation is not under the character:")) + " " + std::to_string(p.oidClip); return 0; }
        if (!pasegT) pasegT = static_cast<ASEG*>(pl);
        if (mpclip.count(p.oidClip)) continue;
        AKDATA d;
        std::string rel;
        std::size_t ib = 0, cb = 0;
        if (!FClipData(static_cast<ASEG*>(pl), d, rel, ib, cb, error)) return 0;
        mpclip[p.oidClip] = std::move(d);
    }
    const AKDATA& dT = mpclip[ap[0].oidClip];
    AKDATA dn;
    dn.tMax = std::round(ap.back().at * 60.0f) / 60.0f;
    dn.achg = dT.achg;
    dn.aaev[0] = dT.aaev[0];
    dn.aaev[2] = dT.aaev[2];
    for (const AKCHN& chT : dT.achn)
    {
        AKCHN ch;
        ch.word = chT.word, ch.sub = chT.sub, ch.fDisabled = chT.fDisabled;
        ch.tp = chT.tp;
        ch.tg = chT.tg;
        for (int s = 0; s < 2; ++s)
        {
            AKTRACK& tr = s ? ch.tg : ch.tp;
            const AKTRACK& trT = s ? chT.tg : chT.tp;
            if (!trT.FKeyed()) continue;   // (constant tracks kept)
            // the values at each pose: the pose's clip's channel of the same record, else the template's at its start
            std::vector<glm::vec3> av;
            for (const CLIPPOSE& p : ap)
            {
                glm::vec3 pos, vel;
                bool fFound = false;
                for (const AKCHN& cs : mpclip[p.oidClip].achn)
                    if (cs.Oid() == chT.Oid() && cs.Isub() == chT.Isub())
                    {
                        fFound = FEvalAkTrack(s ? cs.tg : cs.tp, p.tClip, pos, vel);
                        break;
                    }
                if (!fFound) FEvalAkTrack(trT, 0.0f, pos, vel);
                av.push_back(pos);
            }
            AKTRACK nt;
            nt.fScalar = s == 1;
            nt.kind = s ? 1 : 2;
            for (std::size_t i = 0; i < ap.size(); ++i)
            {
                // (Catmull-Rom tangents per second; held at the ends)
                glm::vec3 tan(0.0f);
                if (i > 0 && i + 1 < ap.size()) tan = (av[i + 1] - av[i - 1]) / std::max(c_dtKey, ap[i + 1].at - ap[i - 1].at);
                const uint16_t f = uint16_t(std::clamp(std::lround(ap[i].at * 60.0f), 0L, 65535L));
                if (s) nt.akg.push_back({f, av[i].x, tan.x, tan.x});
                else nt.akp.push_back({f, av[i], tan, tan});
            }
            tr = std::move(nt);
        }
        dn.achn.push_back(std::move(ch));
    }
    // the new record: a copy of the template's (a new oid), its data replaced, put before the template in the same list
    int iobjT = -1;
    for (std::size_t i = 0; i < e.m_rec.aobj.size(); ++i)
        if (e.m_rec.aobj[i].plo == pasegT) { iobjT = int(i); break; }
    if (iobjT < 0) { error = T("the animation's record was not found"); return 0; }
    const EDITOBJREC rT = e.m_rec.aobj[std::size_t(iobjT)];
    AKDATA dOld;
    std::string relD, relR, relL;
    std::size_t ibD = 0, cbD = 0, ibR = 0, cbR = 0, ibL = 0;
    if (!FClipData(pasegT, dOld, relD, ibD, cbD, error)) return 0;
    if (!e.FRecordRange(iobjT, relR, ibR, cbR) || relR != relD || ibD < ibR || ibD + cbD > ibR + cbR) { error = T("the animation's record was not found"); return 0; }
    if (!rT.ibList || !e.FLocate(rT.ibList, 2, relL, ibL) || relL != relR) { error = T("the animation's record list was not found"); return 0; }
    const int oidNew = e.OidNew();
    RECEDIT re;
    std::vector<uint8_t> rec;
    if (!e.FBuildCopy(iobjT, oidNew, re, rec, error)) return 0;
    if (rec.size() != cbR) { error = T("the animation's record could not be copied"); return 0; }
    const std::vector<uint8_t> abNew = AkBytes(dn);
    const std::size_t ibIn = ibD - ibR;
    rec.erase(rec.begin() + std::ptrdiff_t(ibIn), rec.begin() + std::ptrdiff_t(ibIn + cbD));
    rec.insert(rec.begin() + std::ptrdiff_t(ibIn), abNew.begin(), abNew.end());
    {
        const std::size_t ibOid = IbOidOfRecord(rec.data());
        int32_t cbRec;
        std::memcpy(&cbRec, rec.data() + ibOid + 6, 4);
        cbRec = int32_t(cbRec + int32_t(abNew.size()) - int32_t(cbD));
        std::memcpy(rec.data() + ibOid + 6, &cbRec, 4);
    }
    const std::vector<uint8_t>* pab = e.m_proj.partRef(relL);
    if (!pab || ibL + 2 > pab->size()) { error = T("could not write"); return 0; }
    int16_t cList;
    std::memcpy(&cList, pab->data() + ibL, 2);
    const std::vector<std::size_t> aibSize = e.EnclosingSizeFields(relR, ibR);
    const std::string label = std::string(T("New animation")) + " " + std::to_string(oidNew) + " " + T("from") + " " + std::to_string(ap.size()) + " " + T("poses");
    e.m_proj.begin(label, true);
    ++cList;
    bool fOk = e.m_proj.patch(relL, ibL, &cList, 2);   // (the list's count: before the record, not moved by it)
    fOk = fOk && e.m_proj.spliceBytes(relR, ibR, 0, rec, aibSize);
    e.m_proj.end();
    if (!fOk) { error = T("could not write"); if (e.m_proj.canUndo() && e.m_proj.history().back().label == label) e.m_proj.undo(); return 0; }
    e.Status(label);
    ForgetPoses();
    e.Reload(e.m_plSel ? e.m_plSel->oid : 0);
    return oidNew;
}

// The selected animation clip's keys (move by dragging a diamond; these buttons) and new clips from poses
void CCutsceneEditor::DrawClipPanel(const CSCMD& c)
{
    ASEG* paseg = PasegOfClip(c);
    if (!paseg) return;
    ImGui::SeparatorText(T("Animation keys"));
    AKDATA d;
    std::string rel, error;
    std::size_t ib = 0, cb = 0;
    const bool fOk = FClipData(paseg, d, rel, ib, cb, error);
    if (!fOk) { ImGui::TextDisabled("%s", error.c_str()); return; }
    const std::vector<SUBROW> asub = SubrowsOf(c);
    m_keyRow = std::clamp(m_keyRow, 0, std::max(0, int(asub.size()) - 1));
    if (!asub.empty())
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##keyrow", asub[std::size_t(m_keyRow)].name.c_str()))
        {
            for (std::size_t i = 0; i < asub.size(); ++i)
                if (ImGui::Selectable((asub[i].name + "##kr" + std::to_string(i)).c_str(), int(i) == m_keyRow)) m_keyRow = int(i), m_keyT = -1.0f;
            ImGui::EndCombo();
        }
        EdTip(T("The channel row the buttons below act on (its object's keys together; or the clip's timed events)"));
        const SUBROW& row = asub[std::size_t(m_keyRow)];
        const float tClip = std::clamp(m_t - c.t0, 0.0f, d.tMax);
        if (m_keyT >= 0.0f) ImGui::Text("%s %.2f s (%s)", T("selected key at"), m_keyT, T("drag it on its row to move it"));
        else ImGui::TextDisabled("%s", T("click a key (a diamond) on the rows below the clip to select it"));
        ImGui::BeginDisabled(row.fEvents);
        if (ImGui::Button(T("Add a key at the playhead"))) FEditClipKeys(c, row, tClip, 0.0f, true);
        EdTip(T("A key on each of the row's channels where the playhead is in the clip, on the curve as it is (nothing changes until it is moved)"));
        // (on the same line when it fits: a narrow inspector puts it under)
        const ImGuiStyle& sty = ImGui::GetStyle();
        if (ImGui::GetItemRectMax().x + sty.ItemSpacing.x + ImGui::CalcTextSize(T("Delete the key")).x + sty.FramePadding.x * 2 <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
            ImGui::SameLine();
        ImGui::BeginDisabled(m_keyT < 0.0f);
        if (ImGui::Button(T("Delete the key"))) { if (FEditClipKeys(c, row, m_keyT, -1.0f, false)) m_keyT = -1.0f; }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
    float len = d.tMax;
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputFloat(TL("clip length", "cliplen").c_str(), &len, 0, 0, "%.2f s", ImGuiInputTextFlags_EnterReturnsTrue) && len > c_dtKey)
        FEditClipKeys(c, SUBROW{}, 0.0f, 0.0f, false, len);
    EdTip(T("Retimes the whole clip: every key (and event) moved in proportion, the same motion slower or faster"));
    ImGui::TextDisabled("%s", T("Saved in the project as this level's own clip: the game plays the edited one."));
}

void CCutsceneEditor::DrawNewClipPanel()
{
    ImGui::SeparatorText(T("New animation from poses"));
    FCharacterCombo(TL("character", "nccha").c_str(), m_oidPathActor);
    if (ImGui::Button(T("Add the pose at the playhead")))
    {
        // (the chosen character, else the selected object: its clip at the playhead and that clip's time)
        int oid = m_oidPathActor >= 0 ? m_oidPathActor : e.m_plSel ? e.m_plSel->oid : -1;
        LO* plAseg = nullptr;
        float tClip = 0.0f;
        if (oid < 0 || !FAnimOn(oid, m_t, plAseg, tClip)) e.Status(T("choose a character that an animation poses at the playhead"));
        else
        {
            if (!m_apose.empty() && m_apose[0].oidActor != oid) m_apose.clear();
            const float at = m_apose.empty() ? 0.0f : m_apose.back().at + 1.0f;
            m_apose.push_back({oid, plAseg->oid, tClip, at});
        }
    }
    EdTip(T("Scrub to a moment where the character stands as it should: its pose there (the animation playing on it, at that time) becomes the next key"));
    int idel = -1;
    for (std::size_t i = 0; i < m_apose.size(); ++i)
    {
        CLIPPOSE& p = m_apose[i];
        ImGui::PushID(int(i));
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputFloat("##at", &p.at, 0, 0, "%.2f s");
        EdTip(T("The key's time in the new clip"));
        ImGui::SameLine();
        ImGui::Text("%s %d %s %.2f s", T("as animation"), p.oidClip, T("at"), p.tClip);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) idel = int(i);
        ImGui::PopID();
    }
    if (idel >= 0) m_apose.erase(m_apose.begin() + idel);
    if (!m_apose.empty())
    {
        ImGui::BeginDisabled(m_apose.size() < 2);
        if (ImGui::Button(T("Make the clip and play it at the playhead")))
        {
            std::string error;
            const int oidActor = m_apose[0].oidActor;
            if (const int oid = NewClipFromPoses(error))
            {
                Decode();   // (the world loaded again; the measurement kept: the commands are the same)
                char sz[32];
                std::snprintf(sz, sizeof sz, "%.2f", m_t);
                FAddAtTime(m_t, 0x0A, 0, {{0x10, oid}, {0x14, oidActor}, {0x18, 0}, {-2, 0}},
                           std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("Animation") + " " + std::to_string(oid) + " " + T("at") + " " + sz + " s");
                m_apose.clear();
            }
            else e.Status(error);
        }
        ImGui::EndDisabled();
        EdTip(T("A new animation of the character through these poses (eased between them), kept with the level; an Animation command plays it from the playhead on a track of its own"));
        ImGui::SameLine();
        if (ImGui::Button(T("Clear"))) m_apose.clear();
    }
}
