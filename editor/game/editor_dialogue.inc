// The map editor (editor_impl.h, shared: editor/game/editor_dialogue.inc): the world's dialogue lines (World/Sound.bin's
// dialog table, sound.cpp LoadMusicFromBrx: id = the subtitle's Text.json id, speaker oid, per language the audio key,
// headset flag and length, the speaker's animation events) and their text (World/Text.json).
#include "editor_impl.h"

#if CANE_EDITOR_3D && EDFEAT_DIALOGUE

#include <nlohmann/json.hpp>

#include <set>

#if EDGAME_SLY2
#include "textmod.h"   // (CpOfCp1252 / ByteOfCp1252: the discs' typographic marks)
#endif

namespace edimpl
{

#if EDGAME_SLY2
namespace
{
// Sly 2's Text.json keeps the disc's bytes as Latin-1 code points (worldparts.cpp StrRead: byte-exact), so the English
// text's 0x92 apostrophes and 0x85 ellipses are the C1 controls U+0092 / U+0085 there, which no font draws ("?"). The
// editor shows them as the Windows-1252 marks they are (textmod.cpp's decoding, as the game and the launcher's
// localisation tool read them) and stores typed marks back as those bytes.
void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}
}  // namespace
#endif

std::string GameTextShown(const std::string& s)
{
#if EDGAME_SLY2
    if (s.find('\xC2') == std::string::npos) return s;
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const auto b = static_cast<unsigned char>(s[i]);
        const auto b2 = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        if (b == 0xC2 && b2 >= 0x80 && b2 < 0xA0) { AppendUtf8(out, CpOfCp1252(b2)); ++i; }
        else out += s[i];
    }
    return out;
#else
    return s;
#endif
}

std::string GameTextStored(const std::string& s)
{
#if EDGAME_SLY2
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        int n = 1;
        if (c >= 0xF0) { cp = c & 7; n = 4; }
        else if (c >= 0xE0) { cp = c & 15; n = 3; }
        else if (c >= 0xC0) { cp = c & 31; n = 2; }
        for (int k = 1; k < n && i + std::size_t(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + std::size_t(k)]) & 63);
        if (const int b = cp >= 0x100 ? ByteOfCp1252(cp) : -1; b >= 0) AppendUtf8(out, uint32_t(b));
        else out.append(s, i, std::size_t(n));
        i += std::size_t(n);
    }
    return out;
#else
    return s;
#endif
}

bool CEditor::FParseDialogue(std::vector<DLGLINE>& a, std::vector<std::string>& alang, std::string& error) const
{
    a.clear();
    alang.clear();
    std::vector<uint8_t> ab;
    if (!m_proj.readPart(m_index.relDir + "/World/Sound.bin", ab)) { error = "no World/Sound.bin"; return false; }
    std::size_t i = 0;
    bool fOk = true;
    auto need = [&](std::size_t cb) { if (i + cb > ab.size()) fOk = false; return fOk; };
    auto s16 = [&]() -> int { if (!need(2)) return 0; int16_t v; std::memcpy(&v, ab.data() + i, 2); i += 2; return v; };
    auto s32 = [&]() -> int { if (!need(4)) return 0; int32_t v; std::memcpy(&v, ab.data() + i, 4); i += 4; return v; };
    auto u8 = [&]() -> int { if (!need(1)) return 0; return ab[i++]; };
    i = 64;   // the bank key
    const int n = s16();
    i += std::size_t(std::max(0, n));
    s16();
    i += std::size_t(std::max(0, n)) * 2;
    const int nt = s16();
    if (nt > 0) i += 64 + std::size_t(nt) * 2;
    const int nl = u8();
    for (int k = 0; k < nl && fOk; ++k)
    {
        static const char* s_alang[] = {"English", "French", "German", "Italian", "Spanish", "Dutch", "Japanese", "Korean"};
        const int id = u8();
        alang.push_back(id >= 0 && id < 8 ? s_alang[id] : "language " + std::to_string(id));
    }
    const int ne = s32();
    if (!fOk || ne < 0 || ne > 100000) { error = "Sound.bin: not the expected dialog table"; return false; }
    for (int e = 0; e < ne && fOk; ++e)
    {
        DLGLINE l;
        l.ibId = i;
        l.id = s16();
        l.oidSpeaker = s16();
        for (int k = 0; k < nl && fOk; ++k)
        {
            l.aibLang.push_back(i);
            i += 64;
            l.afNoHeadset.push_back(u8() != 0);
            float t = 0;
            if (need(4)) { std::memcpy(&t, ab.data() + i, 4); i += 4; }
            l.atLength.push_back(t);
        }
        l.cevent = s32();
        l.ibEvents = i;
        if (l.cevent < 0 || l.cevent > 10000) fOk = false;
        else i += std::size_t(l.cevent) * 8;
        if (fOk) a.push_back(std::move(l));
    }
    if (!fOk) { error = "Sound.bin: the dialog table ends early"; return false; }
    return true;
}

void CEditor::ReloadText()
{
    m_atext.clear();
    std::vector<uint8_t> ab;
    std::string error;
    if (!m_index.textPart.empty() && m_proj.readPart(m_index.textPart, ab)) ce::readTextStrings(ab, m_atext, m_textLanguage, error);
    for (ce::TextString& t : m_atext) t.text = GameTextShown(t.text);
}

bool CEditor::FSetText(int id, const std::string& textShown, std::string& error)
{
    const std::string text = GameTextStored(textShown);   // (the marks as the disc's bytes)
    std::vector<uint8_t> ab;
    if (m_index.textPart.empty() || !m_proj.readPart(m_index.textPart, ab)) { error = "no Text.json"; return false; }
    nlohmann::json j = nlohmann::json::parse(ab.begin(), ab.end(), nullptr, false);
    if (j.is_discarded() || !j.contains("strings")) { error = "Text.json does not parse"; return false; }
    const std::string lang = j.contains("languages") && j["languages"].is_array() && !j["languages"].empty() ? j["languages"][0].get<std::string>() : "English";
    // narrow text (Sly 2's) is Latin-1, one byte a character: refuse what it cannot show (wide text: Sly 3's, UTF-16)
    for (std::size_t k = 0; !j.value("wide", false) && k < text.size(); ++k)
    {
        const unsigned char c = static_cast<unsigned char>(text[k]);
        if (c < 0x80) continue;
        if ((c == 0xC2 || c == 0xC3) && k + 1 < text.size() && (static_cast<unsigned char>(text[k + 1]) & 0xC0) == 0x80) { ++k; continue; }
        error = "only Latin-1 characters can be shown by the game";
        return false;
    }
    bool fFound = false;
    for (auto& s : j["strings"])
        if (s.value("id", -1) == id) { s[lang] = text; fFound = true; break; }
    if (!fFound) j["strings"].push_back({{"id", id}, {lang, text}});
    const std::string out = j.dump(1);
    std::vector<uint8_t> block;
    if (!FTextJsonToBlock(j, block, error)) return false;
    m_proj.begin("Text " + std::to_string(id), true);
    m_proj.setPart(m_index.textPart, std::vector<uint8_t>(out.begin(), out.end()));
    m_proj.end();
    ReloadText();
    return true;
}

void CEditor::DrawDialogue()
{
    std::vector<DLGLINE> a;
    std::vector<std::string> alang;
    std::string error;
    if (!FParseDialogue(a, alang, error)) { ImGui::TextColored(st::vec(st::danger), "%s", error.c_str()); return; }
    ImGui::TextColored(st::vec(st::muted), "%zu lines (%zu languages). Enter applies an edit; the subtitle is the line's text id.", a.size(), alang.size());
    if (!ImGui::BeginTable("dlg", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable, ImVec2(0, -1))) return;
    ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("Speaker", ImGuiTableColumnFlags_WidthFixed, 74);
    ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Text");
    ImGui::TableSetupColumn("Events", ImGuiTableColumnFlags_WidthFixed, 54);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    const std::string relSound = m_index.relDir + "/World/Sound.bin";
    ImGuiListClipper clip;
    clip.Begin(int(a.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
        {
            DLGLINE& l = a[std::size_t(r)];
            ImGui::PushID(r);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", l.id);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            int sp = l.oidSpeaker;
            if (ImGui::InputInt("##sp", &sp, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (sp < -32768 || sp > 32767) Status(T("rejected: a speaker oid is 16-bit"));
                else { const int16_t s = int16_t(sp); m_proj.begin("Speaker of line " + std::to_string(l.id)); m_proj.patch(relSound, l.ibId + 2, &s, 2); m_proj.end(); }
            }
            if (ImGui::IsItemHovered())
                if (LO* p = PloOid(l.oidSpeaker)) ImGui::SetTooltip("%s %d", SzClass(p->Cid()), l.oidSpeaker);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float t = l.atLength.empty() ? 0.0f : l.atLength[0];
            if (ImGui::InputFloat("##t", &t, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (!std::isfinite(t) || t <= 0 || t > 600) Status(T("rejected: a length is 0..600 seconds"));
                else
                {
                    m_proj.begin("Length of line " + std::to_string(l.id));
                    for (std::size_t k = 0; k < l.aibLang.size(); ++k) m_proj.patch(relSound, l.aibLang[k] + 65, &t, 4);   // (every language)
                    m_proj.end();
                }
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            std::string text = StrText(l.id);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", text.c_str());
            if (ImGui::InputText("##tx", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::string e2;
                if (FSetText(l.id, buf, e2)) Status(Fmt(T("text of line %d changed"), l.id));
                else Status("text: " + e2);
            }
            // the speaker's events (S16 delay in 1/60 s, S16 target 0..5, S32 value: a float for target 3)
            ImGui::TableNextColumn();
            char lbl[24];
            std::snprintf(lbl, sizeof(lbl), "%d##ev", l.cevent);
            if (l.cevent > 0 && ImGui::SmallButton(lbl)) ImGui::OpenPopup("events");
            else if (l.cevent == 0) ImGui::TextDisabled("-");
            EdTip(T("The speaker's animation events during the line: delay after the previous one (1/60 s), target (0..5), value."));
            if (ImGui::BeginPopup("events"))
            {
                const std::vector<uint8_t>* pab = m_proj.partRef(relSound);
                ImGui::TextColored(st::vec(st::muted), T("Line %d: delay (1/60 s), target, value"), l.id);
                for (int k = 0; pab && k < l.cevent; ++k)
                {
                    const std::size_t ib = l.ibEvents + std::size_t(k) * 8;
                    if (ib + 8 > pab->size()) break;
                    int16_t dt, tg;
                    int32_t v;
                    std::memcpy(&dt, pab->data() + ib, 2);
                    std::memcpy(&tg, pab->data() + ib + 2, 2);
                    std::memcpy(&v, pab->data() + ib + 4, 4);
                    int a3[3] = {dt, tg, v};
                    float g;
                    std::memcpy(&g, &v, 4);
                    ImGui::PushID(k);
                    ImGui::SetNextItemWidth(150);
                    bool f = ImGui::InputInt2("##dt", a3, ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90);
                    if (tg == 3) { if (ImGui::InputFloat("##g", &g, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { std::memcpy(&a3[2], &g, 4); f = true; } }
                    else f |= ImGui::InputInt("##v", &a3[2], 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::PopID();
                    if (!f) continue;
                    std::string e3;
                    if (!FSetDialogueEvent(l, k, a3[0], a3[1], a3[2], e3)) Status("event: " + e3);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    ImGui::EndTable();
}

bool CEditor::FSetDialogueEvent(const DLGLINE& l, int k, int dt60, int itarget, int32_t value, std::string& error)
{
    if (k < 0 || k >= l.cevent) { error = "no such event"; return false; }
    if (dt60 < 0 || dt60 > 32767 || itarget < -1 || itarget > 5) { error = "rejected: delay 0..32767, target -1..5"; return false; }
    if (itarget == 3) { float g; std::memcpy(&g, &value, 4); if (!std::isfinite(g)) { error = "rejected: not a number"; return false; } }
    uint8_t b[8];
    const int16_t s0 = int16_t(dt60), s1 = int16_t(itarget);
    std::memcpy(b, &s0, 2);
    std::memcpy(b + 2, &s1, 2);
    std::memcpy(b + 4, &value, 4);
    m_proj.begin("Event " + std::to_string(k) + " of line " + std::to_string(l.id));
    const bool fOk = m_proj.patch(m_index.relDir + "/World/Sound.bin", l.ibEvents + std::size_t(k) * 8, b, 8);
    m_proj.end();
    if (!fOk) { error = "could not write"; return false; }
    Status("event " + std::to_string(k) + " of line " + std::to_string(l.id) + ": delay " + std::to_string(dt60) + ", target " + std::to_string(itarget));
    return true;
}

}  // namespace edimpl

#endif
