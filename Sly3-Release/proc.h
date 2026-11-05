#pragma once

// CProc: a lambda closure (tag 0x20, the original's "O" {cref; ?; SPX env @8; c @0xC; fRest @0x10;
// SPY formals @0x14; SPY body @0x18}).

#include "ref.h"

class CProc
{
public:
    std::shared_ptr<CFrame> m_pframe;         // the environment
    std::shared_ptr<CPair> m_ppairFormals;    // the parameter symbols (a Dot cell before the rest parameter)
    int m_crefReq = 0;                        // parameters before the Dot
    int m_fVarArg = 0;                        // a rest parameter follows
    std::shared_ptr<CPair> m_ppairCodeExpr;   // the body forms

    // 0x128F70: a copy for a new environment (pframeClone, or this one's when null)
    void CloneTo(CProc* pprocClone, CFrame* pframeClone) const;
};

std::shared_ptr<CProc> PprocNew();
