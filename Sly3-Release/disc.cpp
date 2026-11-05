#include "disc.h"

#include "sly3/addr.h"

#include "../platform/brx_decompress.h"
#include "../platform/games.h"
#include "../platform/platform.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

CDisc g_disc;

namespace
{
constexpr uint32_t SECTOR = 2048;
using orig3::ELF_LBA, orig3::ELF_SIZE, orig3::SEG0_VA, orig3::SEG0_OFFSET, orig3::SEG0_FILESZ, orig3::SEG1_VA,
    orig3::SEG1_OFFSET, orig3::SEG1_FILESZ;

uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

bool ReadAt(const std::string& path, uint64_t offset, void* data, size_t cb)
{
    std::ifstream in(cane::path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(cb));
    return static_cast<size_t>(in.gcount()) == cb;
}
}

FK FkFromRecord(const void* record64)
{
    const uint8_t* p = static_cast<const uint8_t*>(record64);
    FK fk;
    fk.isector = (Le32(p + 0x00) ^ Le32(p + 0x34)) + orig3::WAD_LBA;  // (Sly 2: the raw sectors from 0xCA0; Sly 3: SLY3.WAD there)
    fk.cb = Le32(p + 0x04) ^ Le32(p + 0x3C);
    fk.id = Le32(p + 0x08) ^ Le32(p + 0x38);
    return fk;
}

bool CDisc::FOpen(const std::string& path, std::string& error)
{
    std::vector<uint8_t> elf(ELF_SIZE);
    if (!ReadAt(path, uint64_t(ELF_LBA) * SECTOR, elf.data(), elf.size()))
    {
        error = "cannot read " + path;
        return false;
    }
    if (std::memcmp(elf.data(), "\x7F" "ELF", 4) != 0 || Le32(elf.data() + 0x18) != orig3::ENTRY ||
        cane::sha1Hex(elf.data(), elf.size()) != orig3::ELF_SHA1)
    {
        error = path + " is not the USA Sly 3 disc (SCUS-97464, SCUS_974.64 SHA-1 " + orig3::ELF_SHA1 + ")";
        return false;
    }
    m_abElf = std::move(elf);
    m_path = path;
    return true;
}

std::vector<uint8_t> CDisc::ReadFile(const FK& fk) const
{
    std::vector<uint8_t> ab(fk.cb);
    if (!ReadAt(m_path, uint64_t(fk.isector) * SECTOR, ab.data(), ab.size()))
        throw std::runtime_error("disc read failed at sector " + std::to_string(fk.isector));
    return ab;
}

bool CDisc::FReadBytes(uint64_t ib, void* pv, std::size_t cb) const { return ReadAt(m_path, ib, pv, cb); }

std::vector<uint8_t> CDisc::ReadWorld(const FK& fk) const
{
    const std::vector<uint8_t> ab = ReadFile(fk);
    return cane::decompressBrx(ab);
}

const uint8_t* CDisc::PbElf(uint32_t va, uint32_t cb) const
{
    if (va >= SEG0_VA && va + cb <= SEG0_VA + SEG0_FILESZ) return m_abElf.data() + SEG0_OFFSET + (va - SEG0_VA);
    if (va >= SEG1_VA && va + cb <= SEG1_VA + SEG1_FILESZ) return m_abElf.data() + SEG1_OFFSET + (va - SEG1_VA);
    throw std::out_of_range("address outside the executable's data");
}

uint32_t CDisc::U32Elf(uint32_t va) const { return Le32(PbElf(va, 4)); }

std::string CDisc::StrElf(uint32_t va) const
{
    const char* p = reinterpret_cast<const char*>(PbElf(va, 1));
    return std::string(p, strnlen(p, 256));
}

std::vector<WORLD3> CDisc::Worlds() const
{
    std::vector<WORLD3> a;
    for (int i = 0; i < orig3::CWORLD; ++i)
    {
        WORLD3 w;
        w.iworld = i;
        w.name = StrElf(U32Elf(orig3::VA_AWORLD_NAME + 4 * uint32_t(i)));
        w.episode = static_cast<int>(U32Elf(orig3::VA_AWORLD_EPISODE + 4 * uint32_t(i)));
        w.vaFk = orig3::VA_AWORLD_FK + orig3::CB_FK * uint32_t(i);  // orig3: 0x25E070
        w.fk = FkFromRecord(PbElf(w.vaFk, orig3::CB_FK));
        a.push_back(std::move(w));
    }
    return a;
}

std::vector<MOVIE3> CDisc::Movies() const
{
    std::vector<MOVIE3> a;
    for (int i = 0; i < orig3::CMOVIE_FK; ++i)
    {
        MOVIE3 m;
        m.vaFk = orig3::VA_AMOVIE_FK + orig3::CB_FK * uint32_t(i);
        m.fk = FkFromRecord(PbElf(m.vaFk, orig3::CB_FK));
        m.grf = m.fk.cb & orig3::CB_MOVIE_FLAG;
        m.fk.cb &= ~orig3::CB_MOVIE_FLAG;
        a.push_back(m);
    }
    return a;
}

int CDisc::IworldFirstOfEpisode(int episode) const
{
    if (episode < 0 || episode >= CEPISODE3) return -1;
    return static_cast<int>(U32Elf(orig3::VA_AEPISODE_WORLD + 4 * uint32_t(episode)));
}

std::vector<std::pair<uint32_t, FK>> CDisc::ElfKeys() const
{
    std::vector<std::pair<uint32_t, FK>> keys;
    for (const WORLD3& w : Worlds()) keys.emplace_back(w.vaFk, w.fk);
    for (const MOVIE3& m : Movies()) keys.emplace_back(m.vaFk, m.fk);
    return keys;
}
