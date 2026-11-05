#pragma once

// Finding objects by oid around a context the way the animation code's callers do (orig 0x15E938 PloFindSwObject
// with the match kinds of 0x15E3E8 MatchSwObject): kind 1, an object whose nearest ancestor of the context's chain is
// the context itself; kind 4, the nearest (the object sharing the closest ancestor with the context, else any).
// The channel lists (k of LO +0x28 bits 48..51) and flag 0x100 (LO bit 60) are not modelled yet: TODO(world) when
// the object model has them. Part of the animation code (used by asega / bone / lookat).

#include "lo.h"

// fSameChannel: only the objects of the context's channel (its load group; see emitter.cpp PloFindOid). The original's
// full channel test (0x15E3E8: each object's k (+0x28 bits 48..51) and its +0x3C namespace list) is not modelled
LO* PloFindSwNearest(LO* ploCtx, OID oid, bool fSameChannel = false);
// orig 0x15E650 kind 0x204: the number of nearest matches (1 = unique), *pplo the first
int CploFindSwNearest(LO* ploCtx, OID oid, LO** pplo, bool fSameChannel = false);
LO* PloFindSwChild(LO* ploCtx, OID oid);
// orig 0x15E938 with kind 5 (any: the first object of the oid)
LO* PloFindSwAny(OID oid);
