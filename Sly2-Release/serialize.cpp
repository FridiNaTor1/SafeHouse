#include "serialize.h"
#include "worldglobals.h"

#include "bis.h"
#include "loadtrace.h"

int g_fLoadDebugInfo = 0;
SERIALIZESTATS g_serializestats;
WORLD_VALUE(g_serializestats);   // (PC: kept across a kept world's load, worldglobals.h)

// orig: 0x0012A3D8 PpairSerializeIn (a cell per value: the tag byte, the debug header, the payload (tags 0-2,
// 5-18, 20: the tag alone; 21 int; 22 float; 23 bool; 24 symbol; 25 builtin; 26-30 vector, matrix, clq, lm, smp
// (3, 16, 3, 2, 3 floats); 31 a nested list; 33 an object; the others nothing), then the next cell. The
// original recurses for the next cell; this loops (same reads in the same order).
std::shared_ptr<CPair> PpairSerializeIn(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x12A3D8, pbis);
    std::shared_ptr<CPair> ppairHead;
    CPair* ppairPrev = nullptr;
    for (;;)
    {
        const uint8_t b = pbis->U8Read();
        if (b == 0xFF)
            break;
        auto ppair = PpairNew();
        if (g_fLoadDebugInfo)
        {
            pbis->S32Read();
            for (int i = 0; i < 6; i++)
                pbis->S16Read();
        }
        const int tagk = b & 0x7F;
        const bool fNext = (b & 0x80) != 0;
        g_serializestats.acpairTagk[tagk]++;
        CRef& ref = ppair->m_ref;
        auto readFloats = [&](int c) {
            glm::vec4 v(0.0f);
            for (int i = 0; i < c; i++)
                v[i] = pbis->F32Read();
            return v;
        };
        switch (tagk)
        {
        case 0: case 1: case 2: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13:
        case 14: case 15: case 16: case 17: case 18: case 20:
            ref.SetTag(static_cast<TAGK>(tagk));
            break;
        case TAGK_S32: ref.SetS32(pbis->S32Read()); break;
        case TAGK_F32: ref.SetF32(pbis->F32Read()); break;
        case TAGK_Bool: ref.SetBool(pbis->U32Read() != 0); break;
        case TAGK_Symid: ref.SetSymid(pbis->U32Read()); break;
        case TAGK_Bifk: ref.SetBifk(static_cast<BIFK>(pbis->U32Read())); break;
        case TAGK_Basic:
        {
            // The original assigns the word as an object pointer (AddRef through its vtable): only 0 is valid.
            const uint32_t id = pbis->U32Read();
            ref.SetBasic(nullptr);
            ref.m_n = static_cast<s32>(id);
            if (id != 0)
                g_serializestats.cbasicNonzero++;
            break;
        }
        case TAGK_Vector: ref.SetTag(TAGK_Vector); ref.m_vector = readFloats(3); break;
        case TAGK_Matrix:
        {
            glm::mat4 mat(0.0f);
            for (int i = 0; i < 16; i++)
                mat[i / 4][i % 4] = pbis->F32Read();
            ref.SetMatrix(mat);
            break;
        }
        case TAGK_Clq: ref.SetTag(TAGK_Clq); ref.m_vector = readFloats(3); break;
        case TAGK_Lm: ref.SetTag(TAGK_Lm); ref.m_vector = readFloats(2); break;
        case TAGK_Smp: ref.SetTag(TAGK_Smp); ref.m_vector = readFloats(3); break;
        case TAGK_Pair: ref.SetPair(PpairSerializeIn(pbis)); break;
        default: break;  // 3, 4, 19, 32, 34..: the cell's value stays Nil
        }
        if (ppairPrev != nullptr)
            ppairPrev->m_ppairNext = ppair;
        else
            ppairHead = ppair;
        ppairPrev = ppair.get();
        if (!fNext)
            break;
    }
    return ppairHead;
}
