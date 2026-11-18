// The cutscene editor's presentation track (included by editor_cutscene.inc; Sly 2): the letterbox, title cards,
// captions and fades as clips of their own, written as the game's own commands. notes/CUTSCENES_2026-11-18.md
// "Round 3".
//   - letterbox: "Pause game" with the subtitle box (its kinds' bit 1: the bars) to its "Pause game" off; a new one is
//     a track of its own [Track][Wait][Pause on 1][Wait length][Pause off 1][End] (the game counts the pauses: the
//     scene's own is not ended by it)
//   - title card: the game's own (0x44: the job's name, as long as the game shows it)
//   - caption: the game's hint box (0x53, the HUD's ticker) with a text of its own (a new string of the level's
//     Text.json) for its seconds: the game shows it while the HUD is up, not while a scene has the game paused
//   - (SafeHouse addition) the custom presentation, the cutscene's rule (mod.json "custom_presentation"): the game shows
//     its captions in the scene's pause too (the subtitle box / a caption box of its own) and a title card is a hint
//     whose "no waiting" word is 0x21 (cut::SCPTB_TITLECARD | 1): its own text, large on a band, for its seconds
//     (Sly2-Release/cutpresent.cpp; the original's hints all have 1, its scripts no rules)
//   - fade: a fade object (cid 0x74, a copy of the level's: its colours +0x50 -> +0x54 -> +0x58, fade in / hold / out
//     options 983 / 985 / 984) started by the script's own event (0x09 with the fade's oid as its value; the script's
//     on-event triggers it), as the game's exit fades are (Paris 27254's event 5 on 27256)

namespace
{
constexpr int c_eopidFadeFrom = 980, c_eopidFadeColour = 981, c_eopidFadeTo = 982, c_eopidFadeIn = 983, c_eopidFadeOut = 984, c_eopidFadeHold = 985,
              c_eopidFadeKind = 986;
const char* SzPresentKind(int k)
{
    static const char* s_a[] = {"Letterbox", "Title card", "Caption", "Fade"};
    return k >= 0 && k < 4 ? s_a[k] : "?";
}
}  // namespace

bool CCutsceneEditor::FFadeClip(const CSCMD& c) const
{
    if (c.kind != 0x09) return false;
    LO* plo = PloOfOid(c.an[0]);
    return plo && FIsLoDerivedFrom(plo, EDCID(0x74));
}

bool CCutsceneEditor::FFadeOf(int oidFade, FADEINFO& fi) const
{
    LO* plo = PloOfOid(oidFade);
    if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x74))) return false;
    fi = FADEINFO();
    e.FOptionFloat(plo, EDEOPID(c_eopidFadeIn), fi.tIn);
    e.FOptionFloat(plo, EDEOPID(c_eopidFadeHold), fi.tHold);
    e.FOptionFloat(plo, EDEOPID(c_eopidFadeOut), fi.tOut);
    const int ifield = e.IfieldOption(plo, EDEOPID(c_eopidFadeColour));
    std::string rel;
    std::size_t ib = 0;
    const std::vector<uint8_t>* pab = nullptr;
    if (ifield >= 0 && e.FFieldBytes(ifield, 4, rel, ib, pab)) std::memcpy(fi.rgba, pab->data() + ib, 4);
    return true;
}

bool CCutsceneEditor::FSetFadeOption(int oidFade, int eopid, const void* pv, const std::string& label)
{
    LO* plo = PloOfOid(oidFade);
    const int ifield = plo ? e.IfieldOption(plo, EDEOPID(eopid)) : -1;
    if (ifield < 0) { e.Status(T("the fade's option was not found")); return false; }
    return e.FSetOption(ifield, pv, 4, label);
}

// the presentation clips' times (after Layout's): a letterbox until its "Pause game" off, a fade its three phases
void CCutsceneEditor::LayoutPresentation()
{
    for (CSCMD& c : m_acmd)
    {
        c.icPair = -1;
        if (c.kind == 0x12 && c.an[0])
        {
            // (its off: the next of its block, else the next anywhere)
            const CSCMD* poff = nullptr;
            for (const CSCMD& q : m_acmd)
                if (q.ic > c.ic && q.kind == 0x12 && !q.an[0] && q.icBlock == c.icBlock) { poff = &q; break; }
            if (!poff)
                for (const CSCMD& q : m_acmd)
                    if (q.ic > c.ic && q.kind == 0x12 && !q.an[0]) { poff = &q; break; }
            if (poff) { c.icPair = poff->ic; c.t1 = std::max(c.t1, poff->t0); }
        }
        FADEINFO fi;
        if (FFadeClip(c) && FFadeOf(c.an[0], fi)) c.t1 = c.t0 + std::max(0.05f, fi.tIn + fi.tHold + fi.tOut);
        if (c.kind == 0x53) c.t1 = std::max(c.t1, c.t0 + std::max(0.05f, c.G(0x14)));
    }
    for (const CSCMD& c : m_acmd) m_tEnd = std::max(m_tEnd, c.t1);
}

// a span on a track of its own: [Track][Wait t][on][Wait length][off][End]
bool CCutsceneEditor::FAddSpanAtTime(float t, float len, int kindOn, const std::vector<std::pair<int, double>>& aOn, int kindOff,
                                     const std::vector<std::pair<int, double>>& aOff, const std::string& label)
{
    if (!m_decodeNote.empty()) { e.Status(m_decodeNote); return false; }
    const int iBefore = InsertPoint(t);
    const float tWait = std::max(0.0f, t - TFreeAt(iBefore));
    len = std::max(c_dtFrame, std::round(len * 60.0f) / 60.0f);
    int i = iBefore;
    bool fOk = FInsertCommand(i++, 0x00, 0, {{0x10, -1}}, label);
    if (fOk && tWait > 0.02f) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, std::round(tWait * 60.0f) / 60.0f}, {0x14, -1}, {0x18, -1}}, label); }
    const int icOn = i;
    if (fOk) { Decode(); fOk = FInsertCommand(i++, kindOn, 0, aOn, label); }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, len}, {0x14, -1}, {0x18, -1}}, label); }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, kindOff, 0, aOff, label); }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x01, 0, {}, label); }
    Decode();
    if (fOk) m_isel = icOn;
    return fOk;
}

// A presentation clip at t: 0 letterbox, 1 title card, 2 caption (text), 3 fade (colour rgb, fade in / out)
bool CCutsceneEditor::FAddPresentation(int kind, float t, float len, const std::string& text, const float rgb[3], float tIn, float tOut, std::string& error)
{
    char sz[32];
    std::snprintf(sz, sizeof sz, "%.2f", t);
    const std::string label = std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T(SzPresentKind(kind)) + " " + T("at") + " " + sz + " s";
    switch (kind)
    {
    case 0: return FAddSpanAtTime(t, len, 0x12, {{0x10, 1}, {0x14, 1}}, 0x12, {{0x10, 0}, {0x14, 1}}, label) || (error = T("the commands could not be written"), false);
    case 1:
        if (!FCustomPresent()) return FAddAtTime(t, 0x44, 0, {}, label) || (error = T("the commands could not be written"), false);
        [[fallthrough]];
    case 2:
    {
        // (a new string: the level's next text id)
        int id = 1;
        for (const ce::TextString& s : e.m_atext) id = std::max(id, s.id + 1);
        if (id > 32767) { error = T("no free text id"); return false; }
        if (!e.FSetText(id, text.empty() ? std::string((kind == 1 ? T("Title card") : T("Caption"))) : text, error)) return false;
        const int b = kind == 1 ? cut::SCPTB_TITLECARD | 1 : 1;   // (a custom title card: the hint's "no waiting" word 0x21)
        return FAddAtTime(t, 0x53, 0, {{0x10, id}, {0x14, std::max(c_dtFrame, len)}, {-2, b}}, label) || (error = T("the commands could not be written"), false);
    }
    case 3:
    {
        if (!m_pscpt) { error = T("no script"); return false; }
        // (the script needs a module of its own: its group must have scripts)
        if (m_pscpt && !m_pscpt->pframe)
        {
            bool fScripts = false;
            for (const EDITOBJREC& r : e.m_rec.aobj)
                if (r.plo == m_pscpt)
                {
                    std::string rel;
                    std::size_t ib = 0, cb = 0;
                    const int iobj = int(&r - e.m_rec.aobj.data());
                    if (e.FRecordRange(iobj, rel, ib, cb))
                        for (const ce::GroupInfo& g : e.m_index.groups) fScripts = fScripts || (g.objectsPart == rel && !g.scriptsPart.empty());
                    break;
                }
            if (!fScripts) { error = T("the script's group has no scripts: a fade needs the script's own event handler (make the cutscene with New)"); return false; }
        }
        int iobj = -1;
        for (std::size_t i = 0; i < e.m_rec.aobj.size() && iobj < 0; ++i)
            if (e.m_rec.aobj[i].cid == EDCID(0x74) && e.m_rec.aobj[i].plo && e.m_rec.aobj[i].depth == 0) iobj = int(i);
        if (iobj < 0) { error = T("this level has no fade object to copy"); return false; }
        const int oidFade = e.CopyRecord(iobj, e.CopyGroup(), nullptr, false, error);
        if (!oidFade) return false;
        Decode();
        const uint8_t r = uint8_t(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f), g = uint8_t(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f),
                      b = uint8_t(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        const uint8_t aFrom[4] = {r, g, b, 0}, aCol[4] = {r, g, b, 0x80};
        const float tHold = std::max(0.0f, len - tIn - tOut);
        const int32_t kindFade = 1;   // (over the world, under the screen objects)
        bool fOk = FSetFadeOption(oidFade, c_eopidFadeFrom, aFrom, label) && FSetFadeOption(oidFade, c_eopidFadeColour, aCol, label) &&
                   FSetFadeOption(oidFade, c_eopidFadeTo, aFrom, label) && FSetFadeOption(oidFade, c_eopidFadeIn, &tIn, label) &&
                   FSetFadeOption(oidFade, c_eopidFadeHold, &tHold, label) && FSetFadeOption(oidFade, c_eopidFadeOut, &tOut, label) &&
                   FSetFadeOption(oidFade, c_eopidFadeKind, &kindFade, label);
        if (!fOk) { error = T("the fade's options could not be set"); return false; }
        // (the script's on-event shows it when the event with its oid comes)
        std::vector<std::string> adiag;
        const std::string name = "$fade-" + std::to_string(oidFade);
        if (!e.FWriteObjectHandler(m_pscpt, "on-event", {"self", "event"}, "(if (<=> %1 " + std::to_string(oidFade) + ") ((<geto> (<find-object> " + name + " #obj:0) .trigger)))",
                                   {{name, oidFade}}, adiag, error, true))
            return false;
        Decode();
        return FAddAtTime(t, 0x09, 0, {{0x10, oidFade}}, label) || (error = T("the commands could not be written"), false);
    }
    default: error = "?"; return false;
    }
}

void CCutsceneEditor::SetCustomPresent(bool f)
{
    cane::mods::CutsceneRule r = RuleOf();
    if (r.customPresentation == f) return;
    r.customPresentation = f;
    SetRule(r);
}

void CCutsceneEditor::DrawPresentPanel()
{
    ImGui::SeparatorText(T("Presentation at the playhead"));
    bool fCustom = FCustomPresent();
    if (ImGui::Checkbox(T("Custom presentation (SafeHouse)"), &fCustom)) SetCustomPresent(fCustom);
    EdTip(T("A SafeHouse addition for this cutscene (off: the game's own presentation): its captions show while the scene has the game paused too (as its subtitle, or in a caption box of their own while a line is spoken), and a title card shows the text you type (large, on a band across the picture, for its length), translated with the level's text. Only SafeHouse plays it this way; saved in the package's mod.json."));
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##prk", T(SzPresentKind(m_presentKind))))
    {
        for (int k = 0; k < 4; ++k)
            if (ImGui::Selectable(T(SzPresentKind(k)), k == m_presentKind)) m_presentKind = k;
        ImGui::EndCombo();
    }
    if (m_presentKind != 1 || fCustom)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputFloat(TL("length", "prlen").c_str(), &m_presentLen, 0, 0, "%.2f s");
    }
    if (m_presentKind == 2 || (m_presentKind == 1 && fCustom))
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##prtext", (m_presentKind == 1 ? T("the title card's text") : T("the caption's text")), m_presentText, sizeof m_presentText);
    }
    if (m_presentKind == 3)
    {
        ImGui::ColorEdit3(TL("colour", "prcol").c_str(), m_presentRgb, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputFloat(TL("in", "prin").c_str(), &m_presentIn, 0, 0, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputFloat(TL("out", "prout").c_str(), &m_presentOut, 0, 0, "%.2f");
        EdTip(T("The fade's seconds to the colour (in) and back (out); the colour held between them"));
    }
    if (ImGui::Button(T("Add it at the playhead")))
    {
        std::string error;
        if (!FAddPresentation(m_presentKind, m_t, m_presentLen, m_presentText, m_presentRgb, std::max(0.0f, m_presentIn), std::max(0.0f, m_presentOut), error)) e.Status(error);
    }
    EdTip((fCustom ? T("Letterbox: the scene's bars (the game paused) for its length; title card: your text, large on a band (custom presentation); caption: your text (in the scene's pause as its subtitle, else the game's hint box); fade: the screen to a colour and back (a fade object the script starts). Each on a track of its own: drag it, drag its end for its length.") : T("Letterbox: the scene's bars (the game paused) for its length; title card: the game's (the job's name); caption: the game's hint box with your text (shown while the game is not paused: before / after the scene's pause); fade: the screen to a colour and back (a fade object the script starts). Each on a track of its own: drag it, drag its end for its length.")));
}

// the selected presentation clip's own settings
void CCutsceneEditor::DrawPresentInspector(CSCMD& c)
{
    const std::string label0 = std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": ";
    if (c.kind == 0x12 && c.an[0] && c.icPair >= 0)
    {
        float len = c.t1 - c.t0;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputFloat(TL("letterbox length", "lblen").c_str(), &len, 0, 0, "%.2f s", ImGuiInputTextFlags_EnterReturnsTrue))
            FMoveClip(c.icPair, std::max(c_dtFrame, len) - (c.t1 - c.t0));
        EdTip(T("Moves the \"Pause game\" off that ends it (its track's wait before it)"));
    }
    if (c.kind == 0x44) ImGui::TextWrapped("%s", T("The game's title card: the job's name, shown as long as the game shows it (the scene waits for it)."));
    if (c.kind == 0x53)
    {
        const bool fCustom = FCustomPresent();
        if (FTitleCardClip(c))
            ImGui::TextWrapped("%s", (fCustom ? T("A title card of your text (custom presentation): large on a band across the picture for its length.") : T("A title card of your text: shown only with the custom presentation on (off: the game shows it as a hint).")));
        else if (fCustom) ImGui::TextWrapped("%s", T("A caption (custom presentation): in the scene's pause as its subtitle (a caption box while a line is spoken), else the game's hint box."));
    }
    if (c.kind == 0x53)
    {
        std::string s = e.StrText(c.an[0]);
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s", s.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##captext", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            std::string error;
            if (!e.FSetText(c.an[0], buf, error)) e.Status(error);
        }
        EdTip((FTitleCardClip(c) ? T("The title card's text (the level's string of this id; translated in the package's language files)") : T("The caption's text (the level's string of this id: every hint with this id shows it)")));
    }
    FADEINFO fi;
    if (FFadeClip(c) && FFadeOf(c.an[0], fi))
    {
        const int oidFade = c.an[0];
        float rgb[3] = {fi.rgba[0] / 255.0f, fi.rgba[1] / 255.0f, fi.rgba[2] / 255.0f};
        ImGui::TextDisabled("%s %s", T("fade object"), NameOf(oidFade).c_str());
        // (written when the mouse lets go of the picker: one undo step)
        if (m_fadePendingOid == oidFade) std::memcpy(rgb, m_fadeRgbPending, sizeof rgb);
        if (ImGui::ColorEdit3(TL("colour", "fadecol").c_str(), rgb, ImGuiColorEditFlags_NoInputs)) { m_fadePendingOid = oidFade; std::memcpy(m_fadeRgbPending, rgb, sizeof rgb); }
        if (m_fadePendingOid == oidFade && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_fadePendingOid = -1;
            const uint8_t a[4] = {uint8_t(rgb[0] * 255.0f + 0.5f), uint8_t(rgb[1] * 255.0f + 0.5f), uint8_t(rgb[2] * 255.0f + 0.5f), fi.rgba[3]};
            const uint8_t a0[4] = {a[0], a[1], a[2], 0};
            FSetFadeOption(oidFade, c_eopidFadeColour, a, label0 + T("fade colour"));
            FSetFadeOption(oidFade, c_eopidFadeFrom, a0, label0 + T("fade colour"));
            FSetFadeOption(oidFade, c_eopidFadeTo, a0, label0 + T("fade colour"));
        }
        struct { const char* name; int eopid; float g; } aph[] = {{"fade in", c_eopidFadeIn, fi.tIn}, {"hold", c_eopidFadeHold, fi.tHold}, {"fade out", c_eopidFadeOut, fi.tOut}};
        for (auto& ph : aph)
        {
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputFloat((std::string(T(ph.name)) + "##" + ph.name).c_str(), &ph.g, 0, 0, "%.2f s", ImGuiInputTextFlags_EnterReturnsTrue))
            {
                const float g = std::max(0.0f, ph.g);
                FSetFadeOption(oidFade, ph.eopid, &g, label0 + T(ph.name));
            }
            if (&ph != &aph[2]) ImGui::SameLine();
        }
    }
}

// a presentation clip's end dragged by dt (true: it had one)
bool CCutsceneEditor::FResizePresentation(int i, float dt)
{
    CSCMD& c = m_acmd[std::size_t(i)];
    if (c.kind == 0x12 && c.an[0] && c.icPair >= 0) return FMoveClip(c.icPair, dt);
    FADEINFO fi;
    if (FFadeClip(c) && FFadeOf(c.an[0], fi))
    {
        const float g = std::max(0.0f, fi.tHold + std::round(dt * 60.0f) / 60.0f);
        return FSetFadeOption(c.an[0], c_eopidFadeHold, &g, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("hold"));
    }
    return false;
}
bool CCutsceneEditor::FPresentEdge(const CSCMD& c) const { return (c.kind == 0x12 && c.an[0] && c.icPair >= 0) || FFadeClip(c); }

// the fade at the playhead (over the 3D view): its colour and how much it covers
bool CCutsceneEditor::FFadeAt(float t, float rgba[4]) const
{
    for (const CSCMD& c : m_acmd)
    {
        FADEINFO fi;
        if (!FFadeClip(c) || t < c.t0 || t >= c.t1 || !FFadeOf(c.an[0], fi)) continue;
        float u = t - c.t0, a = 1.0f;
        if (u < fi.tIn) a = fi.tIn > 0 ? u / fi.tIn : 1.0f;
        else if ((u -= fi.tIn) >= fi.tHold) a = fi.tOut > 0 ? 1.0f - (u - fi.tHold) / fi.tOut : 0.0f;
        rgba[0] = fi.rgba[0] / 255.0f, rgba[1] = fi.rgba[1] / 255.0f, rgba[2] = fi.rgba[2] / 255.0f;
        rgba[3] = std::clamp(a, 0.0f, 1.0f) * std::min(1.0f, fi.rgba[3] / 128.0f);
        return true;
    }
    return false;
}
