#include "proc.h"

#include "frame.h"

// orig: 0x00256F48 PprocNew
std::shared_ptr<CProc> PprocNew()
{
    return std::make_shared<CProc>();
}

// orig: 0x00128F70 CloneTo__5CProc
void CProc::CloneTo(CProc* pprocClone, CFrame* pframeClone) const
{
    pprocClone->m_pframe = pframeClone != nullptr ? pframeClone->shared_from_this() : m_pframe;
    pprocClone->m_ppairFormals = m_ppairFormals;
    pprocClone->m_crefReq = m_crefReq;
    pprocClone->m_fVarArg = m_fVarArg;
    pprocClone->m_ppairCodeExpr = m_ppairCodeExpr;
}
