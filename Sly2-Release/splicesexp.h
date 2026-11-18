#pragma once

// The splice scripts' text as S-expressions (the converter's text and the readable one: worldparts.cpp's LEXER's
// tokens), with each node's place in the text (splicenames.cpp, splicediag.cpp).

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace splicesexp
{
// ---------------------------------------------------------------------------------------------------------
// the text: S-expressions as the converter prints them (worldparts.cpp LEXER: '(' ')' "strings" atoms ; comments);
// an atom glued to a following '(' (#vec(...), @(...)) is the list's prefix
struct NODE
{
    std::string atom;          // an atom; a list's prefix
    bool fList = false;
    std::vector<NODE> a;
    int line = 0;
    int col = 0;               // (1-based, in bytes)
    int lineEnd = 0, colEnd = 0;   // a list's ')' (an atom: its last character)
};

struct PARSER
{
    const std::string& t;
    std::size_t i = 0;
    int line = 1;
    std::size_t ibLine = 0;   // where the line starts
    int Col() const { return int(i - ibLine) + 1; }
    [[noreturn]] void Fail(const std::string& what) { throw std::runtime_error("line " + std::to_string(line) + ":" + std::to_string(Col()) + ": " + what); }
    void Skip()
    {
        for (;;)
        {
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
                if (t[i++] == '\n') { ++line; ibLine = i; }
            if (i < t.size() && t[i] == ';')
            {
                while (i < t.size() && t[i] != '\n') ++i;
                continue;
            }
            break;
        }
    }
    bool FEnd() { Skip(); return i >= t.size(); }
    NODE Node()
    {
        Skip();
        if (i >= t.size()) Fail("unexpected end");
        NODE n;
        n.line = line;
        n.col = Col();
        const char c = t[i];
        if (c == ')') Fail("unexpected ')'");
        if (c == '"')
        {
            const std::size_t i0 = i++;
            while (i < t.size() && t[i] != '"')
            {
                if (t[i] == '\\') ++i;
                else if (t[i] == '\n') { ++line; ibLine = i + 1; }
                ++i;
            }
            if (i >= t.size()) Fail("a string without its end");
            ++i;
            n.atom = t.substr(i0, i - i0);
            n.lineEnd = line;
            n.colEnd = Col() - 1;
            return n;
        }
        if (c != '(')
        {
            const std::size_t i0 = i;
            while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '(' && t[i] != ')' && t[i] != '"' && t[i] != ';') ++i;
            n.atom = t.substr(i0, i - i0);
            n.lineEnd = line;
            n.colEnd = Col() - 1;
            if (i < t.size() && t[i] == '(') { List(n); }
            return n;
        }
        List(n);
        return n;
    }
    void List(NODE& n)
    {
        ++i;   // '('
        n.fList = true;
        for (;;)
        {
            Skip();
            if (i >= t.size()) Fail("a list without its ')' (it starts at line " + std::to_string(n.line) + ":" + std::to_string(n.col) + ")");
            if (t[i] == ')') { n.lineEnd = line; n.colEnd = Col(); ++i; return; }
            n.a.push_back(Node());
        }
    }
};

inline bool FInt(const std::string& s, long* pv = nullptr)
{
    if (s.empty()) return false;
    char* pe = nullptr;
    const long v = std::strtol(s.c_str(), &pe, 10);
    if (*pe) return false;
    if (pv) *pv = v;
    return true;
}
inline int SymOf(const std::string& s)
{
    if (s.size() < 4 || s.compare(0, 3, "sym") != 0) return -1;
    for (std::size_t k = 3; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return -1;
    return std::atoi(s.c_str() + 3);
}
inline bool FIsHead(const NODE& n, const char* sz) { return n.fList && n.atom.empty() && !n.a.empty() && !n.a[0].fList && n.a[0].atom == sz; }
inline const NODE* PArg(const NODE& n, std::size_t i) { return n.fList && i < n.a.size() ? &n.a[i] : nullptr; }
}  // namespace splicesexp
