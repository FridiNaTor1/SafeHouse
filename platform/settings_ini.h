#pragma once
// A small INI document for the SafeHouse settings files (platform/sly2_settings.h): sections, "key = value" lines,
// whole-line comments (";" or "#"), case-insensitive section and key lookup, the order of the file kept. Reading never
// fails: a line that is not a section, a key or a comment is skipped. Text is UTF-8 (a BOM is skipped); line ends are
// LF or CRLF. Writing goes through a temporary file and an atomic replace (platform.h atomicReplace).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "platform.h"

namespace cane::ini {
namespace fs = std::filesystem;

inline std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}
inline std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}
inline bool equalNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
// A key's canonical form for lookups: lower case, without spaces, "_" or "-" ("Invert Turret" == "invert_turret").
inline std::string keyForm(std::string_view s) {
    std::string out;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '_' && c != '-') out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

struct Entry {
    std::string key, value;
};
struct Section {
    std::string name;
    std::vector<Entry> entries;
};

struct Document {
    std::vector<Section> sections; // the entries before the first header are in a section named ""

    Section* section(std::string_view name) {
        for (auto& s : sections)
            if (keyForm(s.name) == keyForm(name)) return &s;
        return nullptr;
    }
    const Section* section(std::string_view name) const { return const_cast<Document*>(this)->section(name); }
    // The value of a key (the last one when the file repeats it), or null.
    const std::string* find(std::string_view sectionName, std::string_view key) const {
        const Section* s = section(sectionName);
        if (!s) return nullptr;
        const std::string form = keyForm(key);
        for (auto it = s->entries.rbegin(); it != s->entries.rend(); ++it)
            if (keyForm(it->key) == form) return &it->value;
        return nullptr;
    }
    void set(std::string_view sectionName, std::string_view key, std::string value) {
        Section* s = section(sectionName);
        if (!s) {
            sections.push_back({ std::string(sectionName), {} });
            s = &sections.back();
        }
        const std::string form = keyForm(key);
        for (auto& e : s->entries)
            if (keyForm(e.key) == form) {
                e.value = std::move(value);
                return;
            }
        s->entries.push_back({ std::string(key), std::move(value) });
    }
};

// Parses INI text. A value keeps everything after the first "=", trimmed; a trailing comment needs whitespace before
// its ";" or "#" ("Jump = Space ; the default"). Quotes around a value are removed.
inline Document parse(std::string_view text) {
    Document doc;
    doc.sections.push_back({ "", {} });
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    size_t pos = 0, current = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            if (end == text.size()) break;
            continue;
        }
        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) {
                // (a repeated header continues its section)
                const std::string name = trim(std::string_view(line).substr(1, close - 1));
                if (!doc.section(name)) doc.sections.push_back({ name, {} });
                current = size_t(doc.section(name) - doc.sections.data());
            }
            if (end == text.size()) break;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = trim(std::string_view(line).substr(0, eq));
            std::string value = trim(std::string_view(line).substr(eq + 1));
            // (a quoted value is kept whole, a " ;" / " #" inside it included (an audio device's name); a comment may
            //  follow the closing quote)
            const size_t close = value.size() >= 2 && value.front() == '"' ? value.find('"', 1) : std::string::npos;
            if (close != std::string::npos &&
                (close + 1 == value.size() || std::isspace(static_cast<unsigned char>(value[close + 1]))))
                value = value.substr(0, close + 1);
            else
                for (size_t i = 1; i < value.size(); ++i)
                    if ((value[i] == ';' || value[i] == '#') && std::isspace(static_cast<unsigned char>(value[i - 1]))) {
                        value = trim(std::string_view(value).substr(0, i));
                        break;
                    }
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            if (!key.empty()) doc.sections[current].entries.push_back({ std::move(key), std::move(value) });
        }
        if (end == text.size()) break;
    }
    return doc;
}

// Builds a commented INI file in order.
class Writer {
public:
    void comment(std::string_view text) {
        // (multi-line comments: one "; " per line)
        size_t pos = 0;
        while (true) {
            const size_t end = text.find('\n', pos);
            const std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            m_out << (line.empty() ? ";" : "; ") << line << '\n';
            m_any = true;
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
    }
    void blank() { m_out << '\n'; }
    void section(std::string_view name) {
        if (m_any) m_out << '\n';
        m_out << '[' << name << "]\n";
        m_any = true;
    }
    void value(std::string_view key, std::string_view value) { m_out << key << " = " << value << '\n'; m_any = true; }
    std::string str() const { return m_out.str(); }

private:
    std::ostringstream m_out;
    bool m_any = false;
};

inline bool readFile(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !in.bad();
}

// Writes <file>.tmp, flushes it, then replaces <file>. Creates the parent folder.
inline bool writeFileAtomic(const fs::path& file, std::string_view text) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    if (ec) return false;
    fs::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), std::streamsize(text.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temporary, ec);
            return false;
        }
    }
    if (!atomicReplace(temporary, file)) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}
} // namespace cane::ini
