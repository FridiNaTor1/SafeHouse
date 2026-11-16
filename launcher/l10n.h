#pragma once
// The launcher's languages (launcher/lang/<code>.json, embedded at build time by cmake/embed-lang.cmake).
//
// Keys are the English source strings: tr("Install") returns the active language's text, or the key itself when the
// language lacks it (so English needs no table and a missing string shows in English). trf() is tr() plus printf: a
// translation keeps the English format specifiers in the same order (scripts/test-launcher-l10n.py checks it). A
// "Label##id" key is translated before the "##" and keeps its ImGui id. N_() marks a literal translated later (tables
// of labels): the test extracts every tr / trf / N_ literal from the launcher sources and compares them with en.json.
//
// The language: launcher.json "language" ("system" or absent: the system's language when the launcher has it, else
// English; or a code from languages()). SAFEHOUSE_LANGUAGE=<code> overrides both (tests, screenshots).
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cwchar>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "launcher_lang.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#elif defined(__SWITCH__)
#include <switch.h>
#endif

#define N_(text) text

namespace cane::l10n {
struct Language {
    std::string code;     // "fr", "pt", "ja", ...
    std::string name;     // its own name: "Français"
    std::string english;  // "French"
    std::string script;   // "latin", "cyrillic", "japanese": the fonts it needs (launcher/ui/theme.h)
};
struct Table {
    Language language;
    std::unordered_map<std::string, std::string> strings;
};

namespace detail {
inline std::shared_ptr<const Table> parse(const CaneLangBlob& blob) {
    auto table = std::make_shared<Table>();
    try {
        const auto j = nlohmann::json::parse(blob.data, blob.data + blob.size);
        const auto& meta = j.at("language");
        table->language = {meta.at("code").get<std::string>(), meta.at("name").get<std::string>(),
                           meta.at("english").get<std::string>(), meta.value("script", std::string("latin"))};
        for (const auto& [key, value] : j.at("strings").items())
            if (value.is_string() && !value.get_ref<const std::string&>().empty()) table->strings.emplace(key, value.get<std::string>());
    } catch (const std::exception&) {
        table->language.code = blob.code;
        table->language.name = table->language.english = blob.code;
    }
    return table;
}
// Every embedded table, English first (parsed once; never freed, so the pointers tr() returns stay valid).
inline const std::vector<std::shared_ptr<const Table>>& tables() {
    static const auto all = [] {
        std::vector<std::shared_ptr<const Table>> out;
        for (const auto& blob : cane_lang_blobs) out.push_back(parse(blob));
        return out;
    }();
    return all;
}
struct State {
    std::atomic<const Table*> active{nullptr};
    std::atomic<int> generation{0};
    std::string setting = "system";
    std::mutex mutex;  // the "##" cache below
    std::unordered_map<const Table*, std::unordered_map<std::string, std::unique_ptr<std::string>>> ids;
};
inline State& state() {
    static State s;
    return s;
}
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}
}  // namespace detail

// The launcher's languages, English first, then by code as in launcher/lang.
inline std::vector<Language> languages() {
    std::vector<Language> out;
    for (const auto& t : detail::tables()) out.push_back(t->language);
    return out;
}
inline const Table* find(const std::string& code) {
    for (const auto& t : detail::tables())
        if (t->language.code == code) return t.get();
    return nullptr;
}

// A locale name ("pt_BR.UTF-8", "fr-CA", "nb", "ja_JP@x", "zh-Hans-CN") -> the launcher language it picks, or "".
inline std::string match(std::string locale) {
    locale = detail::lower(locale);
    if (auto cut = locale.find_first_of(".@"); cut != std::string::npos) locale.resize(cut);
    for (char& c : locale)
        if (c == '_') c = '-';
    if (locale.empty() || locale == "c" || locale == "posix") return {};
    if (find(locale)) return locale;
    const std::string primary = locale.substr(0, locale.find('-'));
    // Norwegian: Bokmål ("nb"), the old "no" and Nynorsk ("nn") all read the Bokmål text.
    if (primary == "no" || primary == "nn") return find("nb") ? "nb" : "";
    // Chinese: Simplified for the mainland / Singapore / "Hans", Traditional otherwise.
    if (primary == "zh") {
        const bool hant = locale.find("hant") != std::string::npos || locale.find("-tw") != std::string::npos ||
                          locale.find("-hk") != std::string::npos || locale.find("-mo") != std::string::npos;
        const std::string code = hant ? "zh-Hant" : "zh-Hans";
        return find(code) ? code : "";
    }
    if (find(primary)) return primary;
    // A regional table without a plain one ("pt-BR" only): any of the same language.
    for (const auto& t : detail::tables())
        if (detail::lower(t->language.code).rfind(primary + "-", 0) == 0) return t->language.code;
    return {};
}

// The system's preferred languages, most wanted first (raw locale names).
inline std::vector<std::string> systemLocales() {
    std::vector<std::string> out;
    auto env = [&](const char* name) {
        const char* v = std::getenv(name);
        return v && *v ? std::string(v) : std::string();
    };
#if defined(_WIN32)
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
        std::wstring buffer(size, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size))
            for (const wchar_t* p = buffer.c_str(); *p; p += std::wcslen(p) + 1) {
                std::string name;
                for (const wchar_t* c = p; *c; ++c) name += static_cast<char>(*c < 128 ? *c : '?');
                out.push_back(name);
            }
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (out.empty() && GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
        std::string s;
        for (const wchar_t* c = name; *c; ++c) s += static_cast<char>(*c < 128 ? *c : '?');
        out.push_back(s);
    }
#elif defined(__SWITCH__)
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code))) setMakeLanguage(code, &language);
        setExit();
    }
    static const char* const names[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                        "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (static_cast<size_t>(language) < std::size(names)) out.push_back(names[language]);
#else
    // POSIX: LANGUAGE (a list, GNU gettext) unless the locale is C, then LC_ALL, LC_MESSAGES, LANG.
    const std::string messages = !env("LC_ALL").empty() ? env("LC_ALL") : !env("LC_MESSAGES").empty() ? env("LC_MESSAGES") : env("LANG");
    if (const std::string list = env("LANGUAGE"); !list.empty() && messages != "C" && messages != "POSIX") {
        size_t start = 0;
        while (start <= list.size()) {
            const size_t end = std::min(list.find(':', start), list.size());
            if (end > start) out.push_back(list.substr(start, end - start));
            start = end + 1;
        }
    }
    if (!messages.empty()) out.push_back(messages);
#if defined(__APPLE__)
    if (CFArrayRef preferred = CFLocaleCopyPreferredLanguages()) {
        for (CFIndex i = 0; i < CFArrayGetCount(preferred); ++i) {
            char name[64];
            auto value = static_cast<CFStringRef>(CFArrayGetValueAtIndex(preferred, i));
            if (CFStringGetCString(value, name, sizeof(name), kCFStringEncodingUTF8)) out.push_back(name);
        }
        CFRelease(preferred);
    }
#elif defined(__ANDROID__)
    for (const char* property : {"persist.sys.locale", "ro.product.locale"}) {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get(property, value) > 0) out.push_back(value);
    }
#endif
#endif
    return out;
}
// The launcher language the system asks for: the first preferred language the launcher has, else English.
inline std::string systemLanguage() {
    for (const auto& locale : systemLocales())
        if (auto code = match(locale); !code.empty()) return code;
    return "en";
}

// "system" or a code (an unknown code reads as "system").
inline void setLanguage(const std::string& setting) {
    auto& s = detail::state();
    std::string code;
    if (const char* forced = std::getenv("SAFEHOUSE_LANGUAGE"); forced && *forced) code = match(forced);
    if (code.empty() && setting != "system" && find(setting)) code = setting;
    if (code.empty()) code = systemLanguage();
    const Table* table = find(code);
    if (!table) table = detail::tables().front().get();
    {
        std::lock_guard lock(s.mutex);
        s.setting = find(setting) ? setting : "system";
    }
    if (s.active.exchange(table) != table) s.generation.fetch_add(1);
}
// English until setLanguage() runs: the command-line modes (--install, --verify, ...) and the tests print English.
inline const Table& active() {
    const Table* t = detail::state().active.load();
    return t ? *t : *detail::tables().front();
}

inline std::string current() { return active().language.code; }
inline std::string setting() {
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);
    return s.setting;
}
// Bumped whenever the active language changes (the fonts and cached layouts follow it).
inline int generation() { return detail::state().generation.load(); }

inline const char* tr(const char* key) {
    if (!key || !*key) return key;
    const Table& t = active();
    if (t.strings.empty()) return key;
    if (auto it = t.strings.find(key); it != t.strings.end()) return it->second.c_str();
    const char* id = std::strstr(key, "##");
    if (!id || id == key) return key;
    auto it = t.strings.find(std::string(key, id));
    if (it == t.strings.end()) return key;
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);
    auto& slot = s.ids[&t][key];
    if (!slot) slot = std::make_unique<std::string>(it->second + id);
    return slot->c_str();
}
// A dynamic string (an error message, a stage name): its translation when it is a key, else itself.
inline std::string tr(const std::string& key) { return tr(key.c_str()); }
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string trf(const char* format, ...) {
    const char* translated = tr(format);
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    const int n = std::vsnprintf(nullptr, 0, translated, copy);
    va_end(copy);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, translated, args);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    va_end(args);
    return out;
}

// Every character a table prints (the fonts load these glyphs): its strings and every language's own name.
inline std::u32string codepoints(const Table& t) {
    std::u32string out;
    auto add = [&](const std::string& s) {
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            char32_t cp = c;
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (extra) cp = c & (0x3F >> extra);
            for (int k = 1; k <= extra && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            i += 1 + extra;
            if (cp >= 0x80) out.push_back(cp);
        }
    };
    for (const auto& [key, value] : t.strings) add(value);
    for (const auto& other : detail::tables()) add(other->language.name);
    return out;
}
}  // namespace cane::l10n

namespace cane {
using l10n::tr;
using l10n::trf;
}  // namespace cane
