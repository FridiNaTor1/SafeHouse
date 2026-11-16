// PC addition: the unravelled world files (worldparts.h).
#include "worldparts.h"

#include "eval.h"
#include "ref.h"

#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;

std::vector<WPMARK>* g_pawpmark = nullptr;

namespace
{
const char* const s_aszWpk[WPK_Max] = {"groups", "sound", "progress", "world-records", "text", "nodes", "options",
                                       "bulk", "scripts", "graphics", "collision", "objects", "tail"};
const char* const s_aszWpkFile[WPK_Max] = {"Groups.bin", "Sound.bin", "Progress.bin", "World Records.bin", "Text.json", "Nodes.bin",
                                           "Options.bin", "Bulk.bin", "Scripts.splice", "Graphics.bin", "Collision.bin", "Objects.bin",
                                           "Tail.bin"};
// (the language ids' names: region.h LANGUAGE)
const char* const s_aszLang[] = {"English", "French", "Italian", "German", "Spanish", "Dutch", "Portuguese", "Danish",
                                 "Finnish", "Norwegian", "Swedish", "Japanese", "Korean", "Chinese"};
constexpr int c_lang = int(sizeof(s_aszLang) / sizeof(s_aszLang[0]));

// Parts this small go into the index as hex (a group's bulk header: 8 bytes; an empty collision mesh: 4).
constexpr std::size_t c_cbInline = 32;

std::string SzSafe(std::string s)
{
    for (char& c : s)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "_" : s;
}

std::string Hex(const uint8_t* pb, std::size_t cb)
{
    static const char* const s_hex = "0123456789abcdef";
    std::string s;
    s.reserve(cb * 2);
    for (std::size_t i = 0; i < cb; ++i)
    {
        s += s_hex[pb[i] >> 4];
        s += s_hex[pb[i] & 15];
    }
    return s;
}

bool FUnhex(const std::string& s, std::vector<uint8_t>& ab)
{
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); i += 2)
    {
        const int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        ab.push_back(uint8_t(a << 4 | b));
    }
    return true;
}

// A little-endian reader over a part (throws past its end).
struct RD
{
    const uint8_t* pb;
    std::size_t cb, ib = 0;
    void Need(std::size_t n) const
    {
        if (n > cb - ib) throw std::runtime_error("past the end of the part");
    }
    uint8_t U8() { Need(1); return pb[ib++]; }
    uint16_t U16() { Need(2); uint16_t v; std::memcpy(&v, pb + ib, 2); ib += 2; return v; }
    uint32_t U32() { Need(4); uint32_t v; std::memcpy(&v, pb + ib, 4); ib += 4; return v; }
};
struct WR
{
    std::vector<uint8_t>& ab;
    void U8(uint32_t v) { ab.push_back(uint8_t(v)); }
    void U16(uint32_t v) { U8(v & 0xFF); U8(v >> 8 & 0xFF); }
    void U32(uint32_t v) { U16(v & 0xFFFF); U16(v >> 16); }
};

// ---------------------------------------------------------------------------------------------------------
// The text (LoadTextFromBrx 0x13DF80): U8 clang, clang U8 language ids, S16 c, c x (S16 id, clang strings);
// the Japanese build's strings are 16-bit (and one more string per language after them: the glyphs its text uses).
// A narrow string's bytes are kept as the characters U+0000..U+00FF (Latin-1: the USA / European fonts), a wide one's
// 16-bit units as U+0000..U+FFFF (surrogates cannot be: such a block stays binary).

void AppendCp(std::string& s, uint32_t ch)
{
    if (ch < 0x80) s += char(ch);
    else if (ch < 0x800)
    {
        s += char(0xC0 | ch >> 6);
        s += char(0x80 | (ch & 0x3F));
    }
    else
    {
        s += char(0xE0 | ch >> 12);
        s += char(0x80 | (ch >> 6 & 0x3F));
        s += char(0x80 | (ch & 0x3F));
    }
}

// UTF-8 to code points (false: invalid, or above U+FFFF)
bool FCodepoints(const std::string& s, std::vector<uint32_t>& ach)
{
    for (std::size_t i = 0; i < s.size();)
    {
        const uint8_t b = uint8_t(s[i]);
        uint32_t ch;
        int n;
        if (b < 0x80) { ch = b; n = 0; }
        else if ((b & 0xE0) == 0xC0) { ch = b & 0x1F; n = 1; }
        else if ((b & 0xF0) == 0xE0) { ch = b & 0x0F; n = 2; }
        else return false;
        if (i + 1 + n > s.size()) return false;
        for (int k = 1; k <= n; ++k)
        {
            const uint8_t c = uint8_t(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            ch = ch << 6 | (c & 0x3F);
        }
        ach.push_back(ch);
        i += 1 + n;
    }
    return true;
}

std::string StrRead(RD& rd, bool fWide)
{
    const uint16_t cch = rd.U16();
    std::string s;
    for (uint16_t i = 0; i < cch; ++i)
    {
        const uint32_t ch = fWide ? rd.U16() : rd.U8();
        if (ch >= 0xD800 && ch < 0xE000) throw std::runtime_error("a surrogate in a 16-bit string");
        AppendCp(s, ch);
    }
    return s;
}

void StrWrite(WR& wr, const std::string& s, bool fWide)
{
    std::vector<uint32_t> ach;
    if (!FCodepoints(s, ach)) throw std::runtime_error("not UTF-8: " + s.substr(0, 40));
    if (ach.size() > 0xFFFF) throw std::runtime_error("a string longer than 65535 characters");
    wr.U16(uint32_t(ach.size()));
    for (uint32_t ch : ach)
    {
        if (fWide) wr.U16(ch);
        else
        {
            if (ch > 0xFF) throw std::runtime_error("a character the font has not (above U+00FF) in: " + s.substr(0, 40));
            wr.U8(ch);
        }
    }
}

std::string SzLangOf(int id) { return id >= 0 && id < c_lang ? s_aszLang[id] : "Language " + std::to_string(id); }
int IdLangOf(const std::string& name)
{
    for (int i = 0; i < c_lang; ++i)
        if (name == s_aszLang[i]) return i;
    if (name.rfind("Language ", 0) == 0) return std::atoi(name.c_str() + 9);
    return -1;
}

// ---------------------------------------------------------------------------------------------------------
// The scripts (LoadSwSpliceFromBrx 0x1FF858 after the group's S32 debug flag): U32 c modules, each a serialized
// list (PpairSerializeIn 0x12A3D8) and, with the debug flag, three strings. A list is cells, each a tag byte (bit 7:
// another cell follows; 0xFF: the list ends), with the debug flag S32 + 6 S16 (the cell's source position), and its
// payload by tag. The text is S-expressions (CONVERTER.md "Scripts"):
//   ( ... )       a list whose last cell ends it; ( ... !) one ended by 0xFF after its last cell; () just 0xFF
//   define set! if lambda ...   the special forms (tags 0..20); #tagN another tag-only cell
//   123  1.5  #f32:7fc00000     an integer, a float (always with '.' or 'e'), a float's bits
//   #t #f #bool:N  symN  <name> #bif:N  #obj:N   booleans, symbols, built-ins, object words
//   #vec(x y z) #mat(16) #clq(x y z) #lm(x y) #smp(x y z)
//   @(s32 a b c d e f)          (debug worlds) the next cell's source position

const char* const s_aszTagWord[21] = {"#none", "set!", "define", nullptr, nullptr, "assert", "debug", "if", "or", "and", "cond",
                                      "else", "case", "let", "while", "lambda", "begin", "import", "quote", nullptr, "."};
bool FTagOnly(int tagk) { return (tagk >= 0 && tagk <= 20 && tagk != 3 && tagk != 4 && tagk != 19); }

// the built-ins' names usable as <name> (unique, no spaces / parentheses / quotes / ';')
const std::unordered_map<std::string, int>& MpBifName()
{
    static const std::unordered_map<std::string, int> s_mp = [] {
        std::unordered_map<std::string, int> mp;
        std::map<std::string, int> mpc;
        for (int i = 0; i < BIFK_Max; ++i)
            if (g_mpbifkbif[i].achz) ++mpc[g_mpbifkbif[i].achz];
        for (int i = 0; i < BIFK_Max; ++i)
        {
            const char* sz = g_mpbifkbif[i].achz;
            if (!sz || !*sz || mpc[sz] != 1 || std::strpbrk(sz, " \t\r\n()\";")) continue;
            mp[sz] = i;
        }
        return mp;
    }();
    return s_mp;
}
const char* SzBifName(uint32_t bifk)
{
    if (bifk >= uint32_t(BIFK_Max) || !g_mpbifkbif[bifk].achz) return nullptr;
    const auto& mp = MpBifName();
    const auto it = mp.find(g_mpbifkbif[bifk].achz);
    return it != mp.end() && uint32_t(it->second) == bifk ? g_mpbifkbif[bifk].achz : nullptr;
}

std::string StrFloat(uint32_t bits)
{
    float g;
    std::memcpy(&g, &bits, 4);
    char sz[48];
    if (std::isfinite(g))
        for (int prec = 6; prec <= 9; ++prec)
        {
            std::snprintf(sz, sizeof(sz), "%.*g", prec, double(g));
            const float h = std::strtof(sz, nullptr);
            uint32_t bh;
            std::memcpy(&bh, &h, 4);
            if (bh != bits) continue;
            std::string s = sz;
            if (s.find_first_of(".e") == std::string::npos) s += ".0";
            return s;
        }
    std::snprintf(sz, sizeof(sz), "#f32:%08x", bits);
    return sz;
}

// A list read from the binary form, as a tree for the printer.
struct SNODE
{
    std::string atom;          // (a cell's text, or a list's debug prefix)
    bool fList = false;
    bool fBang = false;        // a list ended by 0xFF after its last cell
    std::vector<SNODE> a;
    std::size_t cchFlat = 0;
};

void ReadSpliceList(RD& rd, bool fDebug, SNODE& node, int depth)
{
    if (depth > 4000) throw std::runtime_error("lists nested too deep");
    node.fList = true;
    for (;;)
    {
        const uint8_t b = rd.U8();
        if (b == 0xFF)
        {
            node.fBang = !node.a.empty();
            return;
        }
        SNODE cell;
        std::string prefix;
        if (fDebug)
        {
            char sz[96];
            const int32_t s = int32_t(rd.U32());
            int16_t as[6];
            for (int16_t& x : as) x = int16_t(rd.U16());
            std::snprintf(sz, sizeof(sz), "@(%d %d %d %d %d %d %d) ", s, as[0], as[1], as[2], as[3], as[4], as[5]);
            prefix = sz;
        }
        const int tagk = b & 0x7F;
        const bool fNext = (b & 0x80) != 0;
        char sz[64];
        auto floats = [&](const char* name, int c) {
            std::string s = std::string(name) + "(";
            for (int i = 0; i < c; ++i) s += (i ? " " : "") + StrFloat(rd.U32());
            return s + ")";
        };
        if (FTagOnly(tagk)) cell.atom = s_aszTagWord[tagk];
        else
            switch (tagk)
            {
            case TAGK_S32: std::snprintf(sz, sizeof(sz), "%d", int32_t(rd.U32())); cell.atom = sz; break;
            case TAGK_F32: cell.atom = StrFloat(rd.U32()); break;
            case TAGK_Bool:
            {
                const uint32_t v = rd.U32();
                if (v <= 1) cell.atom = v ? "#t" : "#f";
                else { std::snprintf(sz, sizeof(sz), "#bool:%u", v); cell.atom = sz; }
                break;
            }
            case TAGK_Symid: std::snprintf(sz, sizeof(sz), "sym%u", rd.U32()); cell.atom = sz; break;
            case TAGK_Bifk:
            {
                const uint32_t v = rd.U32();
                if (const char* name = SzBifName(v)) cell.atom = std::string("<") + name + ">";
                else { std::snprintf(sz, sizeof(sz), "#bif:%u", v); cell.atom = sz; }
                break;
            }
            case TAGK_Basic: std::snprintf(sz, sizeof(sz), "#obj:%u", rd.U32()); cell.atom = sz; break;
            case TAGK_Vector: cell.atom = floats("#vec", 3); break;
            case TAGK_Matrix: cell.atom = floats("#mat", 16); break;
            case TAGK_Clq: cell.atom = floats("#clq", 3); break;
            case TAGK_Lm: cell.atom = floats("#lm", 2); break;
            case TAGK_Smp: cell.atom = floats("#smp", 3); break;
            case TAGK_Pair: ReadSpliceList(rd, fDebug, cell, depth + 1); break;
            default: std::snprintf(sz, sizeof(sz), "#tag%d", tagk); cell.atom = sz; break;
            }
        if (cell.fList) cell.atom = prefix;
        else cell.atom = prefix + cell.atom;
        node.a.push_back(std::move(cell));
        if (!fNext) return;
    }
}

std::size_t CchFlat(SNODE& n)
{
    if (!n.fList) return n.cchFlat = n.atom.size();
    std::size_t c = n.atom.size() + 2 + (n.fBang ? 2 : 0);
    for (std::size_t i = 0; i < n.a.size(); ++i) c += CchFlat(n.a[i]) + (i ? 1 : 0);
    return n.cchFlat = c;
}

void PrintFlat(const SNODE& n, std::string& out)
{
    out += n.atom;
    if (!n.fList) return;
    out += '(';
    for (std::size_t i = 0; i < n.a.size(); ++i)
    {
        if (i) out += ' ';
        PrintFlat(n.a[i], out);
    }
    if (n.fBang) out += " !";
    out += ')';
}

// Lisp style: a list that does not fit the line puts its head on the first line and its other elements one per line
void PrintPretty(const SNODE& n, std::size_t indent, std::string& out)
{
    if (!n.fList || indent + n.cchFlat <= 110)
    {
        PrintFlat(n, out);
        return;
    }
    out += n.atom;
    out += '(';
    const std::size_t indentChild = indent + n.atom.size() + 1;
    for (std::size_t i = 0; i < n.a.size(); ++i)
    {
        if (i)
        {
            out += '\n';
            out.append(indentChild, ' ');
        }
        PrintPretty(n.a[i], indentChild, out);
    }
    if (n.fBang) out += " !";
    out += ')';
}

std::string StrQuoted(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20 || c >= 0x7F)
        {
            char sz[8];
            std::snprintf(sz, sizeof(sz), "\\x%02x", c);
            out += sz;
        }
        else out += char(c);
    }
    return out + "\"";
}

// The text's tokens: '(' ')' a "string" or an atom (#vec / @ followed by '(' are atoms too: the parser takes the list)
struct TOK
{
    enum K { Open, Close, Str, Atom, End } k;
    std::string s;
    int line;
};

struct LEXER
{
    const std::string& t;
    std::size_t i = 0;
    int line = 1;
    TOK Next()
    {
        for (;;)
        {
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
                if (t[i++] == '\n') ++line;
            if (i < t.size() && t[i] == ';')
            {
                while (i < t.size() && t[i] != '\n') ++i;
                continue;
            }
            break;
        }
        if (i >= t.size()) return {TOK::End, "", line};
        const char c = t[i];
        if (c == '(') { ++i; return {TOK::Open, "(", line}; }
        if (c == ')') { ++i; return {TOK::Close, ")", line}; }
        if (c == '"')
        {
            std::string s;
            ++i;
            for (;;)
            {
                if (i >= t.size()) throw std::runtime_error("line " + std::to_string(line) + ": a string without its end");
                char d = t[i++];
                if (d == '"') break;
                if (d == '\n') ++line;
                if (d == '\\' && i < t.size())
                {
                    d = t[i++];
                    if (d == 'x' && i + 1 < t.size())
                    {
                        d = char(std::strtoul(t.substr(i, 2).c_str(), nullptr, 16));
                        i += 2;
                    }
                }
                s += d;
            }
            return {TOK::Str, s, line};
        }
        const std::size_t i0 = i;
        while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '(' && t[i] != ')' && t[i] != '"' && t[i] != ';') ++i;
        return {TOK::Atom, t.substr(i0, i - i0), line};
    }
};

struct SPLICEWRITER
{
    LEXER lex;
    bool fDebug = false;
    std::vector<uint8_t>& ab;
    TOK tok{TOK::End, "", 0};
    void Advance() { tok = lex.Next(); }
    [[noreturn]] void Fail(const std::string& what) const
    {
        throw std::runtime_error("line " + std::to_string(tok.line) + ": " + what + (tok.s.empty() ? "" : " (at '" + tok.s + "')"));
    }
    void Expect(TOK::K k, const char* what)
    {
        if (tok.k != k) Fail(std::string("expected ") + what);
        Advance();
    }
    void U8(uint32_t v) { ab.push_back(uint8_t(v)); }
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) ab.push_back(uint8_t(v >> (8 * i))); }
    void U16(uint32_t v) { ab.push_back(uint8_t(v)); ab.push_back(uint8_t(v >> 8)); }
    long Integer(const std::string& s)
    {
        char* pe = nullptr;
        const long v = std::strtol(s.c_str(), &pe, 10);
        if (s.empty() || *pe) Fail("not an integer");
        return v;
    }
    uint32_t UnsignedAfter(const std::string& s, std::size_t n, int base = 10)
    {
        char* pe = nullptr;
        const unsigned long v = std::strtoul(s.c_str() + n, &pe, base);
        if (s.size() <= n || *pe) Fail("not a number");
        return uint32_t(v);
    }
    uint32_t FloatBits(const std::string& s)
    {
        if (s.rfind("#f32:", 0) == 0) return UnsignedAfter(s, 5, 16);
        char* pe = nullptr;
        const float g = std::strtof(s.c_str(), &pe);
        if (s.empty() || *pe) Fail("not a float");
        uint32_t bits;
        std::memcpy(&bits, &g, 4);
        return bits;
    }
    // the cell's tag byte is written by the caller (it needs to know whether another cell follows)
    void Floats(int c)
    {
        Expect(TOK::Open, "'('");
        for (int i = 0; i < c; ++i)
        {
            if (tok.k != TOK::Atom) Fail("expected a float");
            U32(FloatBits(tok.s));
            Advance();
        }
        Expect(TOK::Close, "')'");
    }
    // A list: tok is '('
    void List()
    {
        Expect(TOK::Open, "'('");
        bool fFirst = true;
        std::size_t ibTagPrev = 0;
        for (;;)
        {
            if (tok.k == TOK::Close)
            {
                Advance();
                if (fFirst) U8(0xFF);   // () : the list ends at once
                return;                 // (the last cell's tag has no "next" bit)
            }
            if (tok.k == TOK::Atom && tok.s == "!")
            {
                if (fFirst) Fail("'!' in an empty list");
                ab[ibTagPrev] |= 0x80;
                U8(0xFF);
                Advance();
                Expect(TOK::Close, "')' after '!'");
                return;
            }
            if (tok.k == TOK::End) Fail("a list without its ')'");
            if (!fFirst) ab[ibTagPrev] |= 0x80;
            fFirst = false;
            ibTagPrev = ab.size();
            Cell();
        }
    }
    void Cell()
    {
        // (debug worlds) @(s32 a b c d e f) first
        uint8_t abDebug[16] = {};
        bool fHeader = false;
        if (tok.k == TOK::Atom && tok.s == "@")
        {
            Advance();
            Expect(TOK::Open, "'(' after '@'");
            for (int i = 0; i < 7; ++i)
            {
                if (tok.k != TOK::Atom) Fail("expected a number in @(...)");
                const long v = Integer(tok.s);
                if (i == 0)
                {
                    const int32_t s32 = int32_t(v);
                    std::memcpy(abDebug, &s32, 4);
                }
                else
                {
                    const int16_t s = int16_t(v);
                    std::memcpy(abDebug + 4 + (i - 1) * 2, &s, 2);
                }
                Advance();
            }
            Expect(TOK::Close, "')'");
            fHeader = true;
        }
        if (fHeader != fDebug) Fail(fDebug ? "a cell without its @(...) position (the scripts have debug-info 1)" : "@(...) in scripts without debug-info");
        const std::size_t ibTag = ab.size();
        U8(0);
        if (fDebug) ab.insert(ab.end(), abDebug, abDebug + 16);
        auto tag = [&](int t) { ab[ibTag] = uint8_t(t); };
        if (tok.k == TOK::Open)
        {
            tag(TAGK_Pair);
            List();
            return;
        }
        if (tok.k != TOK::Atom) Fail("expected a value");
        const std::string s = tok.s;
        Advance();
        for (int t = 0; t <= 20; ++t)
            if (FTagOnly(t) && s == s_aszTagWord[t]) { tag(t); return; }
        if (s.rfind("#tag", 0) == 0) { tag(int(UnsignedAfter(s, 4)) & 0x7F); return; }
        if (s == "#t" || s == "#f") { tag(TAGK_Bool); U32(s == "#t"); return; }
        if (s.rfind("#bool:", 0) == 0) { tag(TAGK_Bool); U32(UnsignedAfter(s, 6)); return; }
        if (s.rfind("sym", 0) == 0) { tag(TAGK_Symid); U32(UnsignedAfter(s, 3)); return; }
        if (s.rfind("#bif:", 0) == 0) { tag(TAGK_Bifk); U32(UnsignedAfter(s, 5)); return; }
        if (s.rfind("#obj:", 0) == 0) { tag(TAGK_Basic); U32(UnsignedAfter(s, 5)); return; }
        if (s.size() > 2 && s.front() == '<' && s.back() == '>')
        {
            const auto& mp = MpBifName();
            const auto it = mp.find(s.substr(1, s.size() - 2));
            if (it == mp.end()) Fail("no built-in named " + s);
            tag(TAGK_Bifk);
            U32(uint32_t(it->second));
            return;
        }
        if (s == "#vec") { tag(TAGK_Vector); Floats(3); return; }
        if (s == "#mat") { tag(TAGK_Matrix); Floats(16); return; }
        if (s == "#clq") { tag(TAGK_Clq); Floats(3); return; }
        if (s == "#lm") { tag(TAGK_Lm); Floats(2); return; }
        if (s == "#smp") { tag(TAGK_Smp); Floats(3); return; }
        if (s.rfind("#f32:", 0) != 0 && !std::strchr("0123456789+-.", s[0]))
            Fail("unknown word '" + s + "' (a symbol is symN, a built-in <name>; the special forms: define set! if lambda ...)");
        if (s.rfind("#f32:", 0) == 0 || s.find_first_of(".eE") != std::string::npos)
        {
            tag(TAGK_F32);
            U32(FloatBits(s));
            return;
        }
        tag(TAGK_S32);
        U32(uint32_t(int32_t(Integer(s))));
    }
};

// ---------------------------------------------------------------------------------------------------------

bool FReadFileBytes(const std::string& path, std::vector<uint8_t>& ab)
{
    std::ifstream in(cane::path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff cb = in.tellg();
    in.seekg(0);
    ab.resize(std::size_t(cb));
    in.read(reinterpret_cast<char*>(ab.data()), cb);
    return bool(in) || cb == 0;
}
}  // namespace

std::string StrFnv64(const uint8_t* pb, std::size_t cb)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < cb; ++i) h = (h ^ pb[i]) * 0x100000001b3ull;
    char sz[24];
    std::snprintf(sz, sizeof(sz), "%016llx", static_cast<unsigned long long>(h));
    return sz;
}

bool FTextBlockToJson(const uint8_t* pb, std::size_t cb, bool fWide, ojson& j, std::string& error)
{
    try
    {
        RD rd{pb, cb};
        const int clang = rd.U8();
        std::vector<std::string> aname;
        std::set<std::string> setName;
        for (int i = 0; i < clang; ++i)
        {
            aname.push_back(SzLangOf(rd.U8()));
            if (!setName.insert(aname.back()).second) throw std::runtime_error("a language twice");
        }
        const int c = int16_t(rd.U16());
        j = ojson::object();
        j["format"] = "sly2-text";
        j["wide"] = fWide;
        j["languages"] = aname;
        ojson as = ojson::array();
        for (int i = 0; i < c; ++i)
        {
            ojson e;
            e["id"] = int16_t(rd.U16());
            for (int k = 0; k < clang; ++k) e[aname[std::size_t(k)]] = StrRead(rd, fWide);
            as.push_back(std::move(e));
        }
        j["strings"] = std::move(as);
        if (fWide)
        {
            ojson g = ojson::object();
            for (int k = 0; k < clang; ++k) g[aname[std::size_t(k)]] = StrRead(rd, true);
            j["glyphs"] = std::move(g);
        }
        if (rd.ib != cb) throw std::runtime_error(std::to_string(cb - rd.ib) + " bytes after the strings");
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("text: ") + e.what();
        return false;
    }
}

bool FTextJsonToBlock(const nlohmann::json& j, std::vector<uint8_t>& ab, std::string& error)
{
    try
    {
        if (j.value("format", "") != "sly2-text") throw std::runtime_error("not a sly2-text file");
        const bool fWide = j.value("wide", false);
        WR wr{ab};
        const auto& alang = j.at("languages");
        wr.U8(uint32_t(alang.size()));
        std::vector<std::string> aname;
        for (const auto& l : alang)
        {
            aname.push_back(l.get<std::string>());
            const int id = IdLangOf(aname.back());
            if (id < 0 || id > 255) throw std::runtime_error("unknown language " + aname.back());
            wr.U8(uint32_t(id));
        }
        const auto& as = j.at("strings");
        if (as.size() > 0x7FFF) throw std::runtime_error("too many strings");
        wr.U16(uint32_t(as.size()));
        for (const auto& e : as)
        {
            wr.U16(uint32_t(e.at("id").get<int>()) & 0xFFFF);
            for (const std::string& name : aname)
            {
                // (a language left out of an entry: an empty string)
                const auto it = e.find(name);
                StrWrite(wr, it != e.end() ? it->get<std::string>() : std::string(), fWide);
            }
        }
        if (fWide)
            for (const std::string& name : aname) StrWrite(wr, j.at("glyphs").value(name, std::string()), true);
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("text: ") + e.what();
        return false;
    }
}

bool FSpliceToText(const uint8_t* pb, std::size_t cb, const std::string& title, std::string& text, std::string& error)
{
    try
    {
        RD rd{pb, cb};
        const uint32_t fDebug = rd.U32();
        const uint32_t c = rd.U32();
        text = "; " + title + "\n";
        text += "; Sly 2's splice scripts, compiled (S-expressions): edit and the game compiles them back when the world loads.\n"
                "; The forms: notes/UNRAVEL_2026-11-10.md \"Scripts\". symN: a symbol (its id), <name>: a built-in.\n";
        char sz[96];
        std::snprintf(sz, sizeof(sz), "(debug-info %u)\n", fDebug);
        text += sz;
        if (fDebug > 1) throw std::runtime_error("debug flag not 0 / 1");
        for (uint32_t i = 0; i < c; ++i)
        {
            SNODE node;
            ReadSpliceList(rd, fDebug != 0, node, 0);
            CchFlat(node);
            std::snprintf(sz, sizeof(sz), "\n(module %u\n  ", i);
            text += sz;
            PrintPretty(node, 2, text);
            if (fDebug)
                for (int k = 0; k < 3; ++k)
                {
                    const uint16_t cch = rd.U16();
                    rd.Need(cch);
                    text += "\n  " + StrQuoted(std::string(reinterpret_cast<const char*>(pb + rd.ib), cch));
                    rd.ib += cch;
                }
            text += ")\n";
        }
        if (c == 0) text += "; (no modules)\n";
        if (rd.ib != cb) throw std::runtime_error(std::to_string(cb - rd.ib) + " bytes after the modules");
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("scripts: ") + e.what();
        return false;
    }
}

bool FSpliceFromText(const std::string& text, std::vector<uint8_t>& ab, std::string& error)
{
    SPLICEWRITER w{LEXER{text}, false, ab};
    try
    {
        w.Advance();
        w.Expect(TOK::Open, "(debug-info N)");
        if (w.tok.k != TOK::Atom || w.tok.s != "debug-info") w.Fail("expected (debug-info N)");
        w.Advance();
        if (w.tok.k != TOK::Atom) w.Fail("expected 0 or 1");
        const long f = w.Integer(w.tok.s);
        if (f != 0 && f != 1) w.Fail("expected 0 or 1");
        w.fDebug = f != 0;
        w.Advance();
        w.Expect(TOK::Close, "')'");
        w.U32(uint32_t(f));
        const std::size_t ibCount = ab.size();
        w.U32(0);
        uint32_t c = 0;
        while (w.tok.k != TOK::End)
        {
            w.Expect(TOK::Open, "(module N ...)");
            if (w.tok.k != TOK::Atom || w.tok.s != "module") w.Fail("expected (module N ...)");
            w.Advance();
            if (w.tok.k != TOK::Atom || w.Integer(w.tok.s) != long(c)) w.Fail("expected module " + std::to_string(c) + " (the modules in order)");
            w.Advance();
            if (w.tok.k != TOK::Open) w.Fail("expected the module's list");
            w.List();
            if (w.fDebug)
                for (int k = 0; k < 3; ++k)
                {
                    if (w.tok.k != TOK::Str) w.Fail("expected the module's 3 debug strings");
                    if (w.tok.s.size() > 0xFFFF) w.Fail("a string too long");
                    w.U16(uint32_t(w.tok.s.size()));
                    ab.insert(ab.end(), w.tok.s.begin(), w.tok.s.end());
                    w.Advance();
                }
            w.Expect(TOK::Close, "')' (the module's end)");
            ++c;
        }
        std::memcpy(ab.data() + ibCount, &c, 4);
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("scripts: ") + e.what();
        return false;
    }
}

bool FParseObjects(const uint8_t* pb, std::size_t cb, int iwe, std::vector<WORLDOBJREC>& arec, std::string& error)
{
    try
    {
        RD rd{pb, cb};
        int c = int16_t(rd.U16());
        while (c-- > 0)
        {
            WORLDOBJREC r;
            r.iwe = iwe;
            r.ib = rd.ib;
            r.k = int8_t(rd.U8());
            if (r.k > 0 || r.k < -2)
            {
                rd.Need(std::size_t(std::abs(r.k)) * 2);
                rd.ib += std::size_t(std::abs(r.k)) * 2;
            }
            r.cid = int16_t(rd.U16());
            r.oid = int32_t(rd.U32());
            rd.U16();
            const uint32_t cbData = rd.U32();
            rd.Need(cbData);
            rd.ib += cbData;
            r.cb = rd.ib - r.ib;
            arec.push_back(r);
        }
        // (then the group's Align(16): under 16 bytes of padding)
        if (cb - rd.ib >= 16) throw std::runtime_error(std::to_string(cb - rd.ib) + " bytes after the objects");
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("objects: ") + e.what();
        return false;
    }
}

bool FUnravelWorld(const std::vector<uint8_t>& ab, const std::vector<WPMARK>& amarkIn, const std::vector<std::pair<std::size_t, std::size_t>>& aalign,
                   const std::vector<std::string>& agroupName,
                   bool fWide, ojson& index, std::vector<WORLDPARTFILE>& afile, std::vector<std::string>& awarning, std::string& error)
{
    std::string StrClassName(int cid);   // cls.h
    // the marks in file order, each part to the next mark (the last to the file's end)
    std::vector<WPMARK> amark = amarkIn;
    if (amark.empty() || amark[0].ib != 0 || amark[0].wpk != WPK_Groups)
    {
        error = "the world's parts were not marked from its start";
        return false;
    }
    for (std::size_t i = 1; i < amark.size(); ++i)
        if (amark[i].ib < amark[i - 1].ib)
        {
            error = "the world's part marks are out of order";
            return false;
        }
    if (amark.back().ib > ab.size())
    {
        error = "a part mark past the file's end";
        return false;
    }
    std::map<int, std::string> mpdirGroup;
    std::set<std::string> setDir;
    for (std::size_t i = 0; i < agroupName.size(); ++i)
    {
        // ("<nnn> <name>": the groups' names are "FK$X<name>" (the original's tool prefix), shown without it)
        char sz[16];
        std::snprintf(sz, sizeof(sz), "%03zu ", i);
        const std::string& name = agroupName[i];
        std::string dir = "World/" + std::string(sz) + SzSafe(name.rfind("FK$X", 0) == 0 ? name.substr(4) : name);
        mpdirGroup[int(i)] = dir;
    }
    index = ojson::object();
    index["format"] = SLY2_WORLD_PARTS_FORMAT;
    index["version"] = SLY2_WORLD_PARTS_VERSION;
    index["size"] = ab.size();
    index["fnv64"] = StrFnv64(ab.data(), ab.size());
    ojson aparts = ojson::array();
    ojson agroups = ojson::array();
    for (std::size_t i = 0; i < agroupName.size(); ++i) agroups.push_back({{"index", i}, {"name", agroupName[i]}, {"folder", mpdirGroup[int(i)]}});
    for (std::size_t i = 0; i < amark.size(); ++i)
    {
        const WPMARK& m = amark[i];
        const std::size_t ibEnd = i + 1 < amark.size() ? amark[i + 1].ib : ab.size();
        const uint8_t* pb = ab.data() + m.ib;
        const std::size_t cb = ibEnd - m.ib;
        if (cb == 0 && m.wpk == WPK_Tail) continue;
        ojson p = ojson::object();
        p["kind"] = s_aszWpk[m.wpk];
        if (m.iwe >= 0) p["group"] = m.iwe;
        p["size"] = cb;
        {
            ojson aa = ojson::array();
            for (const auto& [ibAlign, cbPad] : aalign)
                // (an Align at a part's start that skipped nothing is the previous part's end: its trailing Align)
                if (ibAlign >= m.ib && ibAlign + cbPad <= ibEnd && !(ibAlign == m.ib && cbPad == 0 && i > 0))
                    aa.push_back({ibEnd - ibAlign, cbPad});
            if (!aa.empty()) p["aligns"] = std::move(aa);
        }
        const std::string dir = m.iwe >= 0 && mpdirGroup.count(m.iwe) ? mpdirGroup[m.iwe] : std::string("World");
        std::string rel = dir + "/" + s_aszWpkFile[m.wpk];
        std::vector<uint8_t> abOut;
        std::string err;
        bool fText = false;
        if (m.wpk == WPK_Text)
        {
            ojson j;
            std::vector<uint8_t> abBack;
            if (FTextBlockToJson(pb, cb, fWide, j, err) && FTextJsonToBlock(j, abBack, err) && abBack.size() == cb &&
                std::equal(abBack.begin(), abBack.end(), pb))
            {
                const std::string s = j.dump(1) + "\n";
                abOut.assign(s.begin(), s.end());
                p["format"] = "text-json";
                fText = true;
            }
            else awarning.push_back("the text stays binary: " + (err.empty() ? std::string("it does not compile back to its bytes") : err));
        }
        else if (m.wpk == WPK_Scripts && cb > 8)
        {
            std::string s;
            std::vector<uint8_t> abBack;
            const std::string title = "group " + std::to_string(m.iwe) + (m.iwe >= 0 && std::size_t(m.iwe) < agroupName.size() ? " " + agroupName[std::size_t(m.iwe)] : "");
            if (FSpliceToText(pb, cb, title, s, err) && FSpliceFromText(s, abBack, err) && abBack.size() == cb &&
                std::equal(abBack.begin(), abBack.end(), pb))
            {
                abOut.assign(s.begin(), s.end());
                p["format"] = "splice-text";
                fText = true;
            }
            else
                awarning.push_back("group " + std::to_string(m.iwe) + "'s scripts stay binary: " +
                                   (err.empty() ? std::string("they do not compile back to their bytes") : err));
        }
        if (!fText)
        {
            if (m.wpk == WPK_Text) rel = dir + "/Text.bin";
            if (m.wpk == WPK_Scripts) rel = dir + "/Scripts.bin";
            if (m.wpk == WPK_Objects)
            {
                std::vector<WORLDOBJREC> arec;
                if (FParseObjects(pb, cb, m.iwe, arec, err))
                {
                    ojson ao = ojson::array();
                    for (const WORLDOBJREC& r : arec)
                    {
                        ojson o = {{"oid", r.oid}, {"cid", r.cid}, {"class", StrClassName(r.cid)}, {"offset", r.ib}, {"size", r.cb}};
                        if (r.k) o["k"] = r.k;
                        ao.push_back(std::move(o));
                    }
                    p["objects"] = std::move(ao);
                }
                else awarning.push_back("group " + std::to_string(m.iwe) + ": " + err + " (no object table)");
            }
            // (a group without scripts (an empty table, 8 bytes), the empty blocks: in the index)
            if (cb <= c_cbInline && m.wpk != WPK_Objects)
            {
                p["hex"] = Hex(pb, cb);
                aparts.push_back(std::move(p));
                continue;
            }
            abOut.assign(pb, pb + cb);
        }
        p["file"] = rel;
        afile.push_back({rel, std::move(abOut)});
        aparts.push_back(std::move(p));
    }
    index["groups"] = std::move(agroups);
    index["parts"] = std::move(aparts);
    // the check: the parts put back together (as the game does, FRavelWorld) are the file
    {
        std::map<std::string, const std::vector<uint8_t>*> mp;
        for (const WORLDPARTFILE& f : afile) mp[f.rel] = &f.ab;
        std::vector<uint8_t> abBack;
        std::string err;
        const bool fOk = FRavelWorldFrom(nlohmann::json::parse(index.dump()), [&](const std::string& rel, std::vector<uint8_t>& abFile) {
            const auto it = mp.find(rel);
            if (it == mp.end()) return false;
            abFile = *it->second;
            return true;
        }, abBack, err, nullptr, nullptr, "");
        if (!fOk || abBack != ab)
        {
            error = "the parts do not put the world back together" + (err.empty() ? std::string() : ": " + err);
            return false;
        }
    }
    return true;
}

std::set<int> SetLevelGroups(const nlohmann::json& index)
{
    std::set<int> set;
    if (index.contains("groups"))
        for (const auto& g : index["groups"])
            if (g.value("level", false)) set.insert(g.value("index", -1));
    return set;
}

void MarkLevelGroups(ojson& index, const std::string& code)
{
    if (code.empty() || !index.contains("groups")) return;
    index["code"] = code;
    for (auto& g : index["groups"])
    {
        std::string name = g.value("name", "");
        if (name.rfind("FK$X", 0) == 0) name = name.substr(4);
        if (name == code || name.rfind(code + "_", 0) == 0) g["level"] = true;
    }
}

bool FRavelWorld(const nlohmann::json& index, const std::string& relDir, const PFNPARTPATH& pfnPath, std::vector<uint8_t>& ab,
                 std::string& error, const PFNKEEPOBJ& pfnKeep, const std::set<int>* psetDrop)
{
    return FRavelWorldFrom(index, [&](const std::string& rel, std::vector<uint8_t>& abFile) { return FReadFileBytes(pfnPath(rel), abFile); }, ab, error,
                           pfnKeep, psetDrop, relDir);
}

bool FRavelWorldFrom(const nlohmann::json& index, const PFNPARTBYTES& pfnBytes, std::vector<uint8_t>& ab, std::string& error,
                     const PFNKEEPOBJ& pfnKeep, const std::set<int>* psetDrop, const std::string& relDir)
{
    try
    {
        if (index.value("format", "") != SLY2_WORLD_PARTS_FORMAT || index.value("version", 0) > SLY2_WORLD_PARTS_VERSION)
            throw std::runtime_error("not a world index this game reads");
        ab.clear();
        ab.reserve(index.value("size", std::size_t(0)));
        const std::string prefix = relDir.empty() ? std::string() : relDir + "/";
        std::vector<uint8_t> abPart;
        auto pad16 = [&](std::vector<uint8_t>& a) {
            while ((ab.size() + a.size()) % 16) a.push_back(0);
        };
        for (const auto& p : index.at("parts"))
        {
            abPart.clear();
            // (a dropped group: its blocks empty as the loader reads them: the bulk header's size 0 (its key kept),
            // no CLUTs / bitmaps / shader animations / shaders and the bulk's Align(16), no collision mesh, no objects
            // and the group's Align(16))
            const std::string kind = p.value("kind", "");
            if (psetDrop && p.contains("group") && psetDrop->count(p["group"].get<int>()) && kind != "scripts")
            {
                if (kind == "bulk")
                {
                    std::vector<uint8_t> abBulk;
                    if (!FUnhex(p.value("hex", std::string()), abBulk) || abBulk.size() != 8) throw std::runtime_error("a group's bulk header not 8 bytes");
                    abPart.assign(4, 0);
                    abPart.insert(abPart.end(), abBulk.begin() + 4, abBulk.end());
                }
                else if (kind == "graphics") { abPart.assign(8, 0); pad16(abPart); }
                else if (kind == "collision") abPart.assign(4, 0);
                else if (kind == "objects" && pfnKeep)
                {
                    // (pfnKeep: the dropped group's objects still wanted, its proxies: they place the other groups'
                    // instances, the guests' among them; their own data has no textures)
                    std::vector<uint8_t> abFile;
                    const std::string rel = prefix + p.at("file").get<std::string>();
                    if (!pfnBytes(rel, abFile)) throw std::runtime_error("cannot read " + rel);
                    std::vector<WORLDOBJREC> arec;
                    std::string err;
                    if (!FParseObjects(abFile.data(), abFile.size(), p.value("group", -1), arec, err)) throw std::runtime_error(rel + ": " + err);
                    abPart.assign(2, 0);
                    int c = 0;
                    for (const WORLDOBJREC& r : arec)
                        if (pfnKeep(r))
                        {
                            abPart.insert(abPart.end(), abFile.begin() + std::ptrdiff_t(r.ib), abFile.begin() + std::ptrdiff_t(r.ib + r.cb));
                            ++c;
                        }
                    const int16_t c16 = int16_t(c);
                    std::memcpy(abPart.data(), &c16, 2);
                    pad16(abPart);
                }
                else if (kind == "objects") { abPart.assign(2, 0); pad16(abPart); }
                else throw std::runtime_error("a group part of kind " + kind);
                ab.insert(ab.end(), abPart.begin(), abPart.end());
                continue;
            }
            if (p.contains("hex"))
            {
                if (!FUnhex(p["hex"].get<std::string>(), abPart)) throw std::runtime_error("bad hex in a part");
            }
            else
            {
                const std::string rel = prefix + p.at("file").get<std::string>();
                if (rel.find("..") != std::string::npos) throw std::runtime_error("a part outside the world's folder");
                std::vector<uint8_t> abFile;
                if (!pfnBytes(rel, abFile)) throw std::runtime_error("cannot read " + rel);
                const std::string fmt = p.value("format", "bin");
                std::string err;
                if (fmt == "text-json")
                {
                    nlohmann::json j = nlohmann::json::parse(abFile.begin(), abFile.end(), nullptr, true, true);
                    if (!FTextJsonToBlock(j, abPart, err)) throw std::runtime_error(rel + ": " + err);
                }
                else if (fmt == "splice-text")
                {
                    if (!FSpliceFromText(std::string(abFile.begin(), abFile.end()), abPart, err)) throw std::runtime_error(rel + ": " + err);
                }
                else abPart = std::move(abFile);
            }
            // (the loader's Align(16)s in the part, from its end: padded for where the part now lies; the original
            // padding bytes where it lies as on the disc)
            if (p.contains("aligns") && !abPart.empty())
            {
                const std::size_t L = abPart.size();
                std::vector<std::pair<std::size_t, std::size_t>> apos;
                for (const auto& a : p["aligns"])
                {
                    const std::size_t e = a.at(0).get<std::size_t>(), pad = a.at(1).get<std::size_t>();
                    if (e > L || L - e + pad > L) throw std::runtime_error("a part shorter than its alignment");
                    apos.emplace_back(L - e, pad);
                }
                std::sort(apos.begin(), apos.end());
                std::vector<uint8_t> abNew;
                std::size_t cur = 0;
                for (const auto& [pos, pad] : apos)
                {
                    if (pos < cur) throw std::runtime_error("overlapping alignments in a part");
                    abNew.insert(abNew.end(), abPart.begin() + std::ptrdiff_t(cur), abPart.begin() + std::ptrdiff_t(pos));
                    const std::size_t padNew = (16 - (ab.size() + abNew.size()) % 16) % 16;
                    if (padNew == pad) abNew.insert(abNew.end(), abPart.begin() + std::ptrdiff_t(pos), abPart.begin() + std::ptrdiff_t(pos + pad));
                    else abNew.insert(abNew.end(), padNew, 0);
                    cur = pos + pad;
                }
                abNew.insert(abNew.end(), abPart.begin() + std::ptrdiff_t(cur), abPart.end());
                abPart = std::move(abNew);
            }
            if (pfnKeep && !psetDrop && p.value("kind", "") == "objects")
            {
                // the records kept, the count made again, then the group's padding to 16 (its Align)
                std::vector<WORLDOBJREC> arec;
                std::string err;
                if (!FParseObjects(abPart.data(), abPart.size(), p.value("group", -1), arec, err)) throw std::runtime_error(err);
                std::vector<uint8_t> abNew(2);
                int c = 0;
                for (const WORLDOBJREC& r : arec)
                    if (pfnKeep(r))
                    {
                        abNew.insert(abNew.end(), abPart.begin() + std::ptrdiff_t(r.ib), abPart.begin() + std::ptrdiff_t(r.ib + r.cb));
                        ++c;
                    }
                const int16_t c16 = int16_t(c);
                std::memcpy(abNew.data(), &c16, 2);
                while ((ab.size() + abNew.size()) % 16) abNew.push_back(0);
                abPart = std::move(abNew);
            }
            ab.insert(ab.end(), abPart.begin(), abPart.end());
        }
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------------------------------------
// The tools

#include "bis.h"
#include "cls.h"
#include "disc.h"
#include "region.h"
#include "sw.h"

#include <memory>

namespace
{
bool FWriteBytes(const fs::path& p, const void* pv, std::size_t cb)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(pv), std::streamsize(cb));
    return bool(out);
}
}  // namespace

int WorldUnpackCommand(const std::string& in, const std::string& dir, int filter)
{
    // IN: a world file (decompressed, as a mod's), or a world record of the open disc / installation (a number)
    InitClasses();
    std::vector<uint8_t> ab;
    FK fk{};
    std::string name;
    if (!in.empty() && in.find_first_not_of("0123456789") == std::string::npos)
    {
        std::string error;
        int filterRec = 0;
        if (!FResolveWorld(in, fk, filterRec, error)) { std::fprintf(stderr, "world-unpack: %s\n", error.c_str()); return 1; }
        if (filter < 0) filter = filterRec;
        ab = g_disc.ReadWorld(fk);
        name = g_disc.SzWorldName(fk.isector);
        if (name.empty()) name = "World " + in;
    }
    else
    {
        if (!FReadFileBytes(in, ab)) { std::fprintf(stderr, "world-unpack: cannot read %s\n", in.c_str()); return 1; }
        name = cane::utf8(cane::path(in).stem());
    }
    std::vector<WPMARK> amark;
    auto psw = std::make_unique<SW>();
    psw->filter = filter < 0 ? 0 : filter;
    std::size_t cbRead = 0, cbFile = 0;
    std::vector<std::pair<std::size_t, std::size_t>> aalign;
    g_pawpmark = &amark;
    g_paalignRecord = &aalign;
    const bool fOk = FLoadWorldBytes(psw.get(), fk, ab, "", &cbRead, &cbFile);
    g_pawpmark = nullptr;
    g_paalignRecord = nullptr;
    if (!fOk) { std::fprintf(stderr, "world-unpack: the world did not load\n"); return 1; }
    std::vector<std::string> agroup;
    for (const WE& we : psw->awe) agroup.push_back(we.StrName());
    ojson index;
    std::vector<WORLDPARTFILE> afile;
    std::vector<std::string> awarning;
    std::string error;
    if (!FUnravelWorld(ab, amark, aalign, agroup, FWideText(), index, afile, awarning, error)) { std::fprintf(stderr, "world-unpack: %s\n", error.c_str()); return 1; }
    const fs::path root = cane::path(dir);
    for (const WORLDPARTFILE& f : afile)
        if (!FWriteBytes(root / cane::path(f.rel), f.ab.data(), f.ab.size())) { std::fprintf(stderr, "world-unpack: cannot write %s\n", f.rel.c_str()); return 1; }
    const std::string s = index.dump(1) + "\n";
    if (!FWriteBytes(root / cane::path(name + ".world.json"), s.data(), s.size())) return 1;
    for (const std::string& w : awarning) std::printf("warning: %s\n", w.c_str());
    std::printf("world-unpack: %s: %zu parts, %zu files\n", name.c_str(), index["parts"].size(), afile.size());
    return 0;
}

int WorldPackCommand(const std::string& indexPath, const std::string& out)
{
    nlohmann::json index;
    {
        std::ifstream in(cane::path(indexPath));
        if (!in) { std::fprintf(stderr, "world-pack: cannot read %s\n", indexPath.c_str()); return 1; }
        try { in >> index; }
        catch (const std::exception& e) { std::fprintf(stderr, "world-pack: %s: %s\n", indexPath.c_str(), e.what()); return 1; }
    }
    const fs::path dir = cane::path(indexPath).parent_path();
    std::vector<uint8_t> ab;
    std::string error;
    if (!FRavelWorld(index, "", [&](const std::string& rel) { return cane::utf8(dir / cane::path(rel)); }, ab, error))
    {
        std::fprintf(stderr, "world-pack: %s\n", error.c_str());
        return 1;
    }
    if (!FWriteBytes(cane::path(out), ab.data(), ab.size())) { std::fprintf(stderr, "world-pack: cannot write %s\n", out.c_str()); return 1; }
    const std::string h = StrFnv64(ab.data(), ab.size());
    std::printf("world-pack: %zu bytes%s\n", ab.size(), h == index.value("fnv64", "") ? " (the original world)" : " (changed from the original)");
    return 0;
}

int WorldCheckCommand(const std::string& dir)
{
    const fs::path root = cane::path(dir);
    nlohmann::json m;
    {
        std::ifstream in(root / "extraction_manifest.json");
        if (!in) { std::fprintf(stderr, "world-check: %s has no extraction_manifest.json\n", dir.c_str()); return 1; }
        in >> m;
    }
    int c = 0, cBad = 0;
    for (const auto& f : m.at("files"))
    {
        if (f.value("layout", "") != "parts") continue;
        const std::string rel = f.at("path").get<std::string>();
        nlohmann::json index;
        std::ifstream in(root / cane::path(rel));
        std::vector<uint8_t> ab;
        std::string error;
        const std::string relDir = rel.find('/') == std::string::npos ? "" : rel.substr(0, rel.rfind('/'));
        bool fOk = false;
        try
        {
            in >> index;
            fOk = FRavelWorld(index, relDir, [&](const std::string& r) { return cane::utf8(root / cane::path(r)); }, ab, error);
        }
        catch (const std::exception& e) { error = e.what(); }
        ++c;
        if (!fOk || ab.size() != index.value("size", std::size_t(0)) || StrFnv64(ab.data(), ab.size()) != index.value("fnv64", ""))
        {
            ++cBad;
            std::printf("DIFFERS %s%s\n", rel.c_str(), error.empty() ? "" : (": " + error).c_str());
        }
    }
    std::printf("world-check: %d worlds, %d identical to the disc's, %d differ\n", c, c - cBad, cBad);
    return cBad ? 1 : 0;
}
