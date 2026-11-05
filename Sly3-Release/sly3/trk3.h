#pragma once

// Sly 3's animation tracks (module 0x12CF70..0x12F240), re-derived from the Sly 3 asm. They are TRKP / TRKG
// (aseg.h) so the copied ASEGA / XF runtime (asega.cpp, xf.cpp) drives them like Sly 2's.
//
// Sly 3 shares the key times: an ASEG has time lists ({S8 fWide, U8 c, c x U8 / U16 frames}, aseg3.h) and a keyed
// track names one (its +4). The key of a time g is found in frames: IkeyFind3 0x12D0F0.
//
// Position tracks (the channel's S8 kind, table 0x31F880; vtables {Load slot 1, Setup slot 2, Eval slot 3}):
//   0 / 1   the static TRKPC objects 0x4799E8 (0, 0, 0) / 0x4799F8 (1, 1, 1)
//   2       TRKPC (vt 0x351340: 0x12D218 / 0x12D240 / 0x12D2B0), a vector
//   3       vt 0x351318 (0x12DED0 / 0x12DF30 / 0x12DFF0): S8 scale index, 3 x S16; the constant S16 x 0x36B168[S8]
//   4 / 5   vt 0x3512F0 / 0x3512C8 (0x12D330 / 0x12D438 / 0x12D660, 0x12D908 / 0x12DA00 / 0x12DC28; the segment
//           fetch 0x12D6F8 / 0x12DCC0): per key per keyed axis F32 {tanIn, tanOut, value} / {tan, value}
//   6 / 7   vt 0x3512A0 / 0x351278 (0x12E0B0 / 0x12E1D0 / 0x12E3F8, 0x12E7E0 / 0x12E8F0 / 0x12EB18; fetch 0x12E490 /
//           0x12EBB0): packed keys {U16 tanIn, U16 tanOut} / {U16 tan}, then an S16 per keyed axis; values are
//           S16 x 0x36B168[S8], tangents 5 bits per axis (bits 0..3 the index, bit 4 the sign) into 0x36B178[S8]
// Scalar tracks (0x12EF30): 0 TRKGC (0x12EFE0 / 0x12F010), 1 keyed (0x12F030 / Eval 0x12F120: F32 {value, tanIn,
// tanOut} per key), 2 none.

#include "../aseg.h"
#include "aseg3.h"

#include <array>
#include <vector>

namespace sly3
{
// orig3: 0x12CF90 (the tables 0x36B168 / 0x36B178, made at start-up)
float GScaleTrk3(int s8);               // 0x36B168: {1/16, 1/1024, 1/1024}
float GTanTrk3(int s8, int i);          // 0x36B178 + s8 * 0x40: tanf(i * pi / 32) x {60, 10, 10}[s8]
// orig3: 0x12D0F0: the key of time g (frames = (int)(g * 60); for i = 1; i < c - 1 && frames >= t(i); i++)
int IkeyFind3(const ASEGTIMES3& times, float g);

// kinds 4 / 5
struct TRKP3V : TRKP
{
    const ASEGTIMES3* ptimes = nullptr;  // +4
    int grf = 0;                         // +8 the keyed axes
    int cfKey = 3;                       // 3 {tanIn, tanOut, value} / 2 {tan, value}
    std::vector<float> ag;               // +0xC: the unkeyed axes' constants, then the keys
    void Load(CBinaryInputStream*) override {}
    void Fetch(float g, float* pg0, float* pg1, glm::vec4* pp0, glm::vec4* pp1, glm::vec4* pv0, glm::vec4* pv1) const;
    void Setup(XF* pxf) const override;  // orig3: 0x12D438 / 0x12DA00
    void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const override;  // orig3: 0x12D660 / 0x12DC28
};

// kinds 6 / 7
struct TRKP3P : TRKP
{
    const ASEGTIMES3* ptimes = nullptr;  // +4
    int grf = 0;                         // +8 (S8)
    int s8 = 0;                          // +9 the scale index
    int cu16 = 2;                        // the U16 per key: 2 {tanIn, tanOut} / 1 {tan}
    std::vector<uint16_t> an;            // +0xA: the unkeyed axes' S16, then the keys (cu16 U16, S16 per keyed axis)
    void Load(CBinaryInputStream*) override {}
    void Fetch(float g, float* pg0, float* pg1, glm::vec4* pp0, glm::vec4* pp1, glm::vec4* pv0, glm::vec4* pv1) const;
    void Setup(XF* pxf) const override;  // orig3: 0x12E1D0 / 0x12E8F0
    void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const override;  // orig3: 0x12E3F8 / 0x12EB18
};

// scalar kind 1
struct TRKG3K : TRKG
{
    const ASEGTIMES3* ptimes = nullptr;  // +4
    std::vector<float> ag;               // +8: {value, tanIn, tanOut} per key
    void Load(CBinaryInputStream*) override {}
    void Eval(float* pg, float* pdg, float t, float dtScale) const override;  // orig3: 0x12F120
};

// The runtime tracks of a loaded track (aseg3.h's ASEGTRK3, the time lists of its ASEG)
std::unique_ptr<TRKP> PtrkpFromAseg3(const ASEG3* paseg, const ASEGTRK3& trk);
std::unique_ptr<TRKG> PtrkgFromAseg3(const ASEG3* paseg, const ASEGTRK3& trk);
// Fills the ASEG's ASEGD (the copied runtime's view, asega.cpp) from the Sly 3 data
void BuildAsegd3(ASEG3* paseg);
}  // namespace sly3
