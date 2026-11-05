// Sly 3's world text (text3.h).

#include "text3.h"

#include "brx3.h"

#include "../bis.h"
#include "../disc.h"

#include <cstdio>
#include <cstdlib>

namespace
{
// orig3: 0x119868 (a U16 count, then count 16-bit characters; the original adds a terminating 0)
std::u16string ReadWideString(CBinaryInputStream* pbis)
{
    const int c = pbis->U16Read();
    std::u16string str(std::size_t(c), u'\0');
    for (int i = 0; i < c; ++i) str[std::size_t(i)] = static_cast<char16_t>(pbis->U16Read());
    return str;
}
// orig3: 0x119910 (the same, skipped)
void SkipWideString(CBinaryInputStream* pbis)
{
    pbis->Skip(std::size_t(pbis->U16Read()) * 2);
}
}  // namespace

void LoadTextFromBrx3(TEXT3* ptext, int language, CBinaryInputStream* pbis)
{
    const int clang = pbis->U8Read();
    int ilang = 0;  // the original's index starts at 0 and takes the last match (movz)
    ptext->alanguage.clear();
    for (int j = 0; j < clang; ++j)
    {
        ptext->alanguage.push_back(pbis->U8Read());
        if (ptext->alanguage.back() == language) ilang = j;
    }
    const int c = pbis->S16Read();
    ptext->atext.clear();
    ptext->atext.reserve(std::size_t(std::max(c, 0)));
    for (int i = 0; i < c; ++i)
    {
        const int id = pbis->S16Read();
        std::u16string str;
        for (int j = 0; j < clang; ++j)
        {
            if (j == ilang) str = ReadWideString(pbis);
            else SkipWideString(pbis);
        }
        ptext->atext.emplace_back(id, std::move(str));
    }
    for (int j = 0; j < clang; ++j)
    {
        if (j == ilang) ptext->strExtra = ReadWideString(pbis);
        else SkipWideString(pbis);
    }
    // (then 0x25E7B0 on 0x389AB0: TODO(sly3) its role)
}

const std::u16string* PstrText3(const TEXT3& text, int id)
{
    for (const auto& [idText, str] : text.atext)
        if (idText == id) return &str;
    return nullptr;
}

std::string Utf8FromUtf16(const std::u16string& str)
{
    std::string out;
    for (std::size_t i = 0; i < str.size(); ++i)
    {
        uint32_t c = str[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < str.size() && str[i + 1] >= 0xDC00 && str[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (str[++i] - 0xDC00);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)),
                out += char(0x80 | (c & 0x3F));
    }
    return out;
}

int ListText3Command(const std::string& arg, const std::string& grep)
{
    for (const WORLD3& wd : g_disc.Worlds())
    {
        char* pchEnd = nullptr;
        const unsigned long n = std::strtoul(arg.c_str(), &pchEnd, 0);
        if (wd.name != arg && !(pchEnd != arg.c_str() && !*pchEnd && static_cast<unsigned long>(wd.iworld) == n)) continue;
        if (!wd.fk.cb)
        {
            std::printf("world %s: no file\n", wd.name.c_str());
            return 0;
        }
        std::vector<uint8_t> ab = g_disc.ReadWorld(wd.fk);
        WORLDWALK3 walk;
        std::string error;
        if (!FWalkWorld3(ab, walk, error) && walk.ibText == 0)
        {
            std::fprintf(stderr, "%s: %s\n", wd.name.c_str(), error.c_str());
            return 1;
        }
        CBinaryInputStream bis;
        bis.OpenMemory(int(ab.size()), ab.data());
        bis.m_ib = walk.ibText;
        TEXT3 text;
        LoadTextFromBrx3(&text, 0, &bis);
        std::printf("%s: text block %zu..%zu, languages", wd.name.c_str(), walk.ibText, bis.m_ib);
        for (int l : text.alanguage) std::printf(" %d", l);
        std::printf(", %zu strings\n", text.atext.size());
        uint32_t chMax = 0;
        for (const auto& [id, str] : text.atext)
        {
            for (char16_t ch : str) chMax = std::max<uint32_t>(chMax, ch);
            const std::string sz = Utf8FromUtf16(str);
            if (grep.empty() || sz.find(grep) != std::string::npos) std::printf("%6d %s\n", id, sz.c_str());
        }
        std::printf("extra: %s\nthe largest character: U+%04X\n", Utf8FromUtf16(text.strExtra).c_str(), chMax);
        return 0;
    }
    std::fprintf(stderr, "no world %s (--list-files)\n", arg.c_str());
    return 2;
}
