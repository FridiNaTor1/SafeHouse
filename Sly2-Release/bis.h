#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "readtrace.h"

struct GEOM;
struct BSP;

// What a 64-byte file key record names (the converter's crawl, convert.cpp).
enum KEYK
{
    KEYK_Other,     // (any other key: a task list's)
    KEYK_World,     // a world record (the boot world's world table)
    KEYK_Screen,    // an external screen object's file
    KEYK_Exit,      // an exit's keys: the world it loads, then the movies it plays
    KEYK_Bank,      // the world's SFX bank
    KEYK_Ambience,  // the world's resident ambience stream
    KEYK_Dialog,    // a dialog line in one language (id = the line, lang = the game's language id)
    KEYK_Music,     // a music track (id = the world's music id)
};
// The converter's key collector (convert.cpp): called with every 64-byte file key record a loader reads; null in
// the game.
extern void (*g_pfnNoteKeyRecord)(const void* record64, KEYK keyk, int id, int lang);

using byte = std::uint8_t;

enum BISK
{
    BISK_Nil = -1,
    BISK_File = 0,
    BISK_Mem = 1
};

// (PC: set while the converter loads a world, worldparts.h) each Align's position and the bytes it skipped
extern std::vector<std::pair<std::size_t, std::size_t>>* g_paalignRecord;

class CBinaryInputStream
{
public:
    CBinaryInputStream();
    explicit CBinaryInputStream(const std::string& filePath);

    CBinaryInputStream(const CBinaryInputStream&) = delete;
    CBinaryInputStream& operator=(const CBinaryInputStream&) = delete;

    // Reads the whole file with one read. A world file is parsed with
    // millions of 1-4 byte reads, which cost far more through an ifstream.
    void OpenFile(const std::string& filePath);
    void OpenMemory(int cb, const void* pv);
    // Owns the buffer (a decompressed world file).
    void OpenBuffer(std::vector<std::uint8_t>&& ab);

    [[nodiscard]] bool FIsOpen() const { return m_bisk != BISK_Nil; }

    // A raw read of cb bytes: the original's Read (0x117968), traced as such.
    void ReadBytes(void* destination, std::size_t cb)
    {
        Trace("Read");
        ReadRaw(destination, cb);
    }

    // The same without the trace (the readers' own bytes).
    void ReadRaw(void* destination, std::size_t cb)
    {
        // m_ib never exceeds m_cb, and m_cb is 0 while closed.
        if (cb != 0 && destination != nullptr && cb <= m_cb - m_ib)
        {
            std::memcpy(destination, m_pb + m_ib, cb);
            m_ib += cb;
            return;
        }

        ReadBytesSlow(destination, cb);
    }

    // Copies up to cb bytes at absolute offset ib without moving the read
    // position. Bytes past the end are left untouched, like a short read.
    std::size_t CbCopyAt(std::size_t ib, void* destination, std::size_t cb) const;

    void Align(int n);

    [[nodiscard]] std::size_t GetOffset();

    // Readers. Each reports itself to the load trace (loadtrace.h) under the label the reference's parse maps
    // use (tools/loadmap.py; note its labels vec4/mat/mat4 are ReadMatrix/ReadMatrix4/ReadVector4).
    byte U8Read() { Trace("U8"); return RawU8(); }               // orig 0x117BD0
    uint16_t U16Read() { Trace("U16"); return Raw<uint16_t>(); } // orig 0x117C20
    uint32_t U32Read() { Trace("U32?"); return Raw<uint32_t>(); } // orig 0x117C80
    int8_t S8Read() { Trace("S8"); return static_cast<int8_t>(RawU8()); } // orig 0x117D00
    int16_t S16Read() { Trace("S16"); return Raw<int16_t>(); }   // orig 0x117D50
    int32_t S32Read() { Trace("S32"); return Raw<int32_t>(); }   // orig 0x117DB8
    float F32Read() { Trace("F32"); return Raw<float>(); }       // orig 0x117E38

    // orig 0x117968 Read(cb, pv): pv null skips
    void Read(std::size_t cb, void* pv) { Trace("Read"); RawRead(pv, cb); }
    // orig 0x1179B8 Skip
    void Skip(std::size_t cb) { Trace("1179b8"); RawRead(nullptr, cb); }
    // orig 0x117F90 ReadCb12 / 0x117FB0 ReadCb8
    void ReadCb12(void* pv) { Trace("117f90"); RawRead(pv, 12); }
    void ReadCb8(void* pv) { Trace("117fb0"); RawRead(pv, 8); }
    // orig 0x118458: a 64-byte file key record
    void ReadRecord64(void* pv, KEYK keyk = KEYK_Other, int id = -1, int lang = -1)
    {
        Trace("rec64");
        RawRead(pv, 64);
        if (g_pfnNoteKeyRecord) g_pfnNoteKeyRecord(pv, keyk, id, lang);
    }

    glm::vec2 ReadVector2();
    glm::vec3 ReadVector();   // orig 0x117EB0 (12 bytes)
    glm::vec4 ReadVector4();  // orig 0x117F70 (16 bytes)

    glm::mat3 ReadMatrix();   // orig 0x117ED0 (3 vectors)
    glm::mat4 ReadMatrix4();  // orig 0x117F18 (4 vectors; the fourth is the translation)

    void ReadGeom(GEOM* pgeom);                     // orig 0x117FD0
    void ReadBsp(const GEOM* pgeom, BSP* pbsp);     // orig 0x118220

    std::shared_ptr<char[]> ReadStringAlloc();
    // orig 0x118380: U16 length + characters (allocated by the caller's allocator in the original)
    std::string ReadStringSw();
    // orig 0x118420: skips a string
    void SkipStringSw();
    // The Japanese build's text strings (orig JP 0x1184C8 / 0x118570): U16 count + 16-bit characters, kept as the
    // port's text (region.h AppendCh16)
    std::string ReadWideStringSw();
    void SkipWideStringSw();
    // The same read as SkipStringSw (traced as 0x118420), keeping the characters (the debug strings)
    std::string ReadSkippedStringSw();

    std::string ReadFixedString32NoAlloc();
    std::shared_ptr<char[]> ReadFixedString32();

    void Close();

    ~CBinaryInputStream();

    BISK m_bisk = BISK_Nil;

    // OpenMemory uses a non-owning view of its buffer; OpenFile views m_abFile.
    const std::uint8_t* m_pb = nullptr;
    std::size_t m_cb = 0;
    std::size_t m_ib = 0;

private:
    void ReadBytesSlow(void* destination, std::size_t cb);
    void Trace(const char* kind) const { if (loadtrace::g_fOn) loadtrace::Read(kind, m_ib); }
    byte RawU8() { byte value{}; ReadRaw(&value, 1); return value; }
    template <typename T> T Raw() { T value{}; ReadRaw(&value, sizeof(T)); return value; }
    void RawRead(void* pv, std::size_t cb);

    std::vector<std::uint8_t> m_abFile;
};
