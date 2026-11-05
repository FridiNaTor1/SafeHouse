#pragma once

// Sly 3's world text (orig3: 0x14FD20 LoadTextFromBrx): UTF-16 strings (a U16 count of 16-bit characters, 0x119868;
// Sly 2's were 8-bit). The world keeps the chosen language's strings in SW +0x4E54 (count) / +0x4E58 ({S32 id,
// wchar_t* string} x count) and one more string at +0x4E5C.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CBinaryInputStream;

struct TEXT3
{
    std::vector<int> alanguage;                            // the block's languages (U8 each)
    std::vector<std::pair<int, std::u16string>> atext;    // +0x4E58: (id, string of the chosen language)
    std::u16string strExtra;                               // +0x4E5C
};

// orig3: 0x14FD20 (language: the global 0x36B120; a string of another language is skipped, 0x119910)
void LoadTextFromBrx3(TEXT3* ptext, int language, CBinaryInputStream* pbis);
// The string of text id (nullptr: none)
const std::u16string* PstrText3(const TEXT3& text, int id);

std::string Utf8FromUtf16(const std::u16string& str);

// --list-text W [grep]: a world's strings (as UTF-8), or those containing grep
int ListText3Command(const std::string& arg, const std::string& grep);
