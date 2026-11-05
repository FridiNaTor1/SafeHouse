#pragma once

// ASEG (animation segment) data as loaded from world files: LoadAsegFromBrx 0x12F2A8 with its channels'
// key-frame tracks (module 0x12B3B0-0x12E4A8) and its three event lists (0x12F590). These loaders read most of
// a world file's bytes outside geometry (Paris: 1009 ASEGs, ~80k reads).
//
// The object model's class ASEG (an ALO) embeds ASEGD and calls LoadAsegFromBrx from its load slot.

#include <array>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

class CBinaryInputStream;
struct XF;

// A position track (0x12B3B0 by kind; the vtable's Load is slot 1). Evaluation (asega.cpp, the animation code):
// slot 2 Setup gives a transform record the segment of keys around its parameter (XF::pg, an ASEGA's time), slot 3
// Eval the track's value / velocity at a time.
struct TRKP
{
    int kind = -1;
    virtual ~TRKP() = default;
    virtual void Load(CBinaryInputStream* pbis) = 0;
    virtual void Setup(XF* pxf) const;
    virtual void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const;
};

// kind 0: a constant position (0x12B4F8)
struct TRKPC : TRKP
{
    glm::vec3 pos{0.0f};
    void Load(CBinaryInputStream* pbis) override;
    void Setup(XF* pxf) const override;   // orig 0x12B520
    void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const override;  // orig 0x12B590
};

// kind 1: keys {pos, t, tangent in, tangent out} (0x12B5E0); kind 2: keys {pos, t, tangent} (0x12BA98)
struct KEYP
{
    glm::vec3 pos;
    float t;
    glm::vec3 vIn;
    glm::vec3 vOut;
};
struct TRKPK : TRKP
{
    bool fTwoTangents = false;
    std::vector<KEYP> akey;
    void Load(CBinaryInputStream* pbis) override;
    int IkeyFind(float g) const;          // orig 0x12B6D8 / 0x12BB70
    void Setup(XF* pxf) const override;   // orig 0x12B738 / 0x12BBD0
    void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const override;  // orig 0x12B9D0 / 0x12BE68
};

// kinds 3 / 4: per-axis keys (0x12BF30: {g, dgIn, dgOut}, 0x12C570: {g, dg}) at shared times; axes without
// keys stay at v
struct TRKPA : TRKP
{
    int cfKey = 2;
    glm::vec3 v{0.0f};
    std::vector<float> at;
    std::array<std::vector<float>, 3> aag;  // per axis: c * cfKey floats, empty when the axis has no keys
    void Load(CBinaryInputStream* pbis) override;
    int IkeyFind(float g) const;          // orig 0x12C0E0 / 0x12C708
    void Setup(XF* pxf) const override;   // orig 0x12C140 / 0x12C768
    void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const override;  // orig 0x12C440 / 0x12CA68
};

// kind 5: nothing to load (0x12B4F0)
struct TRKPN : TRKP
{
    void Load(CBinaryInputStream*) override {}
};

// A scalar track (0x12CB98 by kind); slot 2 Eval (asega.cpp).
struct TRKG
{
    int kind = -1;
    virtual ~TRKG() = default;
    virtual void Load(CBinaryInputStream* pbis) = 0;
    virtual void Eval(float* pg, float* pdg, float t, float dtScale) const;
};

// kind 0: a constant (0x12CC68)
struct TRKGC : TRKG
{
    float g = 0.0f;
    void Load(CBinaryInputStream* pbis) override;
    void Eval(float* pg, float* pdg, float t, float dtScale) const override;  // orig 0x12CC98
};

// kind 1: keys {t, g, dgIn, dgOut} (0x12CCB8)
struct KEYG
{
    float t;
    float g;
    float dgIn;
    float dgOut;
};
struct TRKGK : TRKG
{
    std::vector<KEYG> akey;
    void Load(CBinaryInputStream* pbis) override;
    void Eval(float* pg, float* pdg, float t, float dtScale) const override;  // orig 0x12CDA0
};

// kind 2: nothing to load (0x12CC60)
struct TRKGN : TRKG
{
    void Load(CBinaryInputStream*) override {}
};

std::unique_ptr<TRKP> PtrkpNew(int kind);  // 0x12B3B0
std::unique_ptr<TRKG> PtrkgNew(int kind);  // 0x12CB98

// A channel (8 bytes {word {oid:20, isub:10, fDisabled:1 (bit 30), fScalar:1 (bit 31)}, track}).
struct ASEGCHN
{
    int oid = 0;
    int isub = 0;
    int fDisabled = 0;
    int fScalar = 0;
    std::unique_ptr<TRKP> ptrkp;  // when !fScalar
    std::unique_ptr<TRKG> ptrkg;  // when fScalar (the second track read wins, as in the original)
};

// The second channel list (12 bytes {kind, n, scalar track}).
struct ASEGCHG
{
    int kind = 0;
    int n = 0;
    std::unique_ptr<TRKG> ptrkg;
};

// An event (16 bytes {t, kind, 2 words}), kinds' payloads per 0x12F590.
struct ASEGEV
{
    float t = 0.0f;
    int kind = 0;
    std::array<int, 4> an{};
    std::array<float, 3> ag{};
    std::array<uint8_t, 24> ab{};  // kind 24: three 8-byte blobs
};

struct ASEGD
{
    float tMax = 0.0f;                  // +0x74
    std::vector<ASEGCHN> achn;          // +0x5C / +0x60
    std::vector<ASEGCHG> achg;          // +0x64 / +0x68
    std::array<std::vector<ASEGEV>, 3> aaev;  // +0x98, +0xA0 (timed), +0xA8
};

// orig: 0x0012F2A8: the data, then the object's options (g_pfnLoadOptionsFromBrx(pvOptions, pbis): the
// object model's LoadOptionsFromBrx 0x13F810; when unset, the load stops before the options)
void LoadAsegFromBrx(ASEGD* pasegd, void* pvOptions, CBinaryInputStream* pbis);
// orig: 0x0012F590
void LoadAsegEventsFromBrx(std::vector<ASEGEV>* paev, int fTime, CBinaryInputStream* pbis);

extern void (*g_pfnLoadOptionsFromBrx)(void* pv, CBinaryInputStream* pbis);
