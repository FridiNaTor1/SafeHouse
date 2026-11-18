// The map editor's sound and music (notes/EDITOR_PLAN.md round 5, item 6; Sly 2): the Sound tab. The level's ambient
// sound emitters (class 0x2B: each sound record is a list option 99 {sound id, falloff start, falloff end, volume,
// pitch}, soundglue.cpp AMBREC) with their values editable, their sound played in the editor (the sample WAV the
// installation's manifest names for the bank sound the level's sound table maps the id to), moved to the view and
// added (a copy); the level's music tracks (the world's sound block: its music list, {id, file key} each) played and
// replaced by any other track (its key taken from a level that uses it). Preview: SDL audio (muted under
// SDL_AUDIODRIVER=dummy), one sound at a time.
#if EDGAME_SLY2
#include "sound.h"
#endif
#include <SDL.h>

namespace edimpl
{

#if EDGAME_SLY2
namespace
{
struct PREVIEW
{
    SDL_AudioDeviceID dev = 0;
    std::string file;
} s_preview;
}  // namespace

void CEditor::StopPreview()
{
    if (s_preview.dev) { SDL_CloseAudioDevice(s_preview.dev); s_preview.dev = 0; }
    s_preview.file.clear();
}

// a WAV played in the editor (false: it does not open)
bool CEditor::FPlayPreview(const std::string& path)
{
    StopPreview();
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { Status(std::string("audio: ") + SDL_GetError()); return false; }
    SDL_AudioSpec spec;
    Uint8* pb = nullptr;
    Uint32 cb = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &pb, &cb)) { Status(std::string(T("could not play")) + " " + path); return false; }
    // (a music track's stems: its first two channels; a mono effect as it is)
    std::vector<uint8_t> ab(pb, pb + cb);
    SDL_FreeWAV(pb);
    if (spec.channels > 2 && SDL_AUDIO_BITSIZE(spec.format) == 16)
    {
        const int c = spec.channels;
        std::vector<uint8_t> st;
        st.reserve(ab.size() / std::size_t(c) * 2);
        for (std::size_t i = 0; i + std::size_t(c) * 2 <= ab.size(); i += std::size_t(c) * 2)
            st.insert(st.end(), ab.begin() + std::ptrdiff_t(i + (c >= 3 ? 2 : 0)), ab.begin() + std::ptrdiff_t(i + (c >= 3 ? 6 : 4)));   // (the normal stem's L / R)
        ab.swap(st);
        spec.channels = 2;
    }
    spec.callback = nullptr;
    s_preview.dev = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
    if (!s_preview.dev) { Status(std::string("audio: ") + SDL_GetError()); return false; }
    SDL_QueueAudio(s_preview.dev, ab.data(), Uint32(std::min<std::size_t>(ab.size(), std::size_t(spec.freq) * spec.channels * 2 * 60)));   // (a minute at most)
    SDL_PauseAudioDevice(s_preview.dev, 0);
    s_preview.file = path;
    std::printf("editor: playing %s\n", path.c_str());
    return true;
}

// the installation's manifest (read once): the bank samples per world and the music tracks
void CEditor::ReadSoundManifest()
{
    if (m_fSoundManifest) return;
    m_fSoundManifest = true;
    std::vector<uint8_t> ab;
    if (!ce::readFile(m_inst.dir / "extraction_manifest.json", ab)) return;
    const nlohmann::json j = nlohmann::json::parse(std::string(ab.begin(), ab.end()), nullptr, false);
    if (!j.is_object() || !j.contains("files")) return;
    for (const auto& f : j["files"])
    {
        const std::string kind = f.value("kind", "");
        if (kind == "music")
        {
            MUSICTRACK t;
            t.id = f.value("music_id", -1);
            t.name = f.value("name", "");
            t.path = f.value("path", "");
            t.seconds = f.value("seconds", 0.0f);
            if (t.id >= 0) m_amusicAll.push_back(t);
        }
        else if (kind == "bank" && f.contains("worlds") && f["worlds"].is_array())
        {
            bool fThis = false;
            for (const auto& w : f["worlds"]) fThis |= w.is_number_integer() && w.get<int>() == m_eo.record;
            if (!fThis || !f.contains("samples")) continue;
            for (const auto& s : f["samples"])
                for (const auto& n : s.value("sounds", nlohmann::json::array()))
                    if (n.is_number_integer() && !m_mpsoundSample.count(n.get<int>())) m_mpsoundSample[n.get<int>()] = s.value("path", "");
        }
    }
    std::sort(m_amusicAll.begin(), m_amusicAll.end(), [](const MUSICTRACK& a, const MUSICTRACK& b) { return a.id < b.id; });
}

// a sound id's sample (the level's sound table: id -> bank sound -> its first sample's WAV; "": none)
std::string CEditor::SampleOfSound(int sfxid)
{
    ReadSoundManifest();
    const int isound = sfxid >= 0 && sfxid < int(g_sound.aisoundFromId.size()) ? g_sound.aisoundFromId[std::size_t(sfxid)] : -1;
    const auto it = m_mpsoundSample.find(isound);
    return it == m_mpsoundSample.end() || it->second.empty() ? std::string() : ce::utf8(m_inst.dir / ce::pathOf(it->second));
}

// the world's sound block: where its music list's entries are ({S16 id, 64-byte key} each); false: not parsed
bool CEditor::FMusicEntries(const std::vector<uint8_t>& ab, std::vector<std::size_t>& aib) const
{
    aib.clear();
    std::size_t i = 0;
    auto need = [&](std::size_t n) { return i + n <= ab.size(); };
    auto s16 = [&]() { int16_t v; std::memcpy(&v, ab.data() + i, 2); i += 2; return int(v); };
    auto s32 = [&]() { int32_t v; std::memcpy(&v, ab.data() + i, 4); i += 4; return int(v); };
    if (!need(66)) return false;
    i = 64;   // the bank's key
    const int n = s16();
    if (n < 0 || !need(std::size_t(n) + 2)) return false;
    i += std::size_t(n);
    s16();   // (the id table's size)
    if (!need(std::size_t(n) * 2 + 2)) return false;
    i += std::size_t(n) * 2;
    const int nt = s16();
    if (nt < 0) return false;
    if (nt > 0) i += 64;
    i += std::size_t(nt) * 2;
    if (!need(1)) return false;
    const int nl = ab[i++];
    i += std::size_t(nl);
    if (!need(4)) return false;
    const int ne = s32();
    if (ne < 0 || ne > 100000) return false;
    for (int e = 0; e < ne; ++e)
    {
        if (!need(4 + std::size_t(nl) * 69 + 4)) return false;
        i += 4 + std::size_t(nl) * (64 + 1 + 4);
        const int c = s32();
        if (c < 0 || !need(std::size_t(c) * 8)) return false;
        i += std::size_t(c) * 8;
    }
    if (!need(4)) return false;
    const int no = s32();
    if (no < 0 || !need(std::size_t(no) * 66)) return false;
    for (int o = 0; o < no; ++o) { aib.push_back(i); i += 66; }
    return true;
}

std::string CEditor::SoundPart() const
{
    if (m_index.json.contains("parts"))
        for (const auto& p : m_index.json["parts"])
            if (p.value("kind", "") == "sound" && p.contains("file")) return m_index.relDir + "/" + p["file"].get<std::string>();
    return std::string();
}

// the music tracks' 64-byte keys (each from a level whose sound block lists it; read once)
void CEditor::ReadMusicKeys()
{
    if (m_fMusicKeys) return;
    m_fMusicKeys = true;
    for (const ce::WorldEntry& we : m_inst.worlds)
    {
        if (!we.indexBase.empty()) continue;
        ce::WorldIndex wi;
        std::string e2;
        if (!wi.load(m_inst, we, e2) || !wi.json.contains("parts")) continue;
        std::string rel;
        for (const auto& p : wi.json["parts"])
            if (p.value("kind", "") == "sound" && p.contains("file")) rel = wi.relDir + "/" + p["file"].get<std::string>();
        std::vector<uint8_t> ab;
        std::vector<std::size_t> aib;
        if (rel.empty() || !ce::readFile(m_inst.dir / ce::pathOf(rel), ab) || !FMusicEntries(ab, aib)) continue;   // (the installed bytes: a project's swap is not a track's key)
        for (std::size_t ib : aib)
        {
            int16_t v;
            std::memcpy(&v, ab.data() + ib, 2);
            if (!m_mpMusicKey.count(v)) m_mpMusicKey[v] = std::vector<uint8_t>(ab.begin() + std::ptrdiff_t(ib + 2), ab.begin() + std::ptrdiff_t(ib + 66));
        }
    }
}

bool CEditor::FMusicKey(int id, uint8_t key[64])
{
    ReadMusicKeys();
    const auto it = m_mpMusicKey.find(id);
    if (it == m_mpMusicKey.end()) return false;
    std::memcpy(key, it->second.data(), 64);
    return true;
}

// the level's music entry i played by another track
bool CEditor::FReplaceMusic(int i, int idNew, std::string& error)
{
    const std::string rel = SoundPart();
    std::vector<uint8_t> ab;
    std::vector<std::size_t> aib;
    if (rel.empty() || !m_proj.readPart(rel, ab) || !FMusicEntries(ab, aib)) { error = T("the level's sound block is not read"); return false; }
    if (i < 0 || i >= int(aib.size())) { error = "no music entry " + std::to_string(i); return false; }
    // (the entry keeps its id, which the level's scripts and jobs push; its file key becomes the other track's)
    uint8_t key[64];
    if (!FMusicKey(idNew, key)) { error = std::string(T("no level has track")) + " " + std::to_string(idNew); return false; }
    m_proj.begin(Fmt(T("Music %d: track %d"), i, idNew), false);
    const bool f = m_proj.patch(rel, aib[std::size_t(i)] + 2, key, 64);
    m_proj.end();
    if (!f) { error = "could not write"; return false; }
    Status(Fmt(T("music %d of this level is now track %d (the game plays it where the level played the old one)"), i, idNew));
    return true;
}

std::vector<int> CEditor::MusicOfLevel()
{
    std::vector<int> a;
    std::vector<uint8_t> ab;
    std::vector<std::size_t> aib;
    const std::string rel = SoundPart();
    if (!rel.empty() && m_proj.readPart(rel, ab) && FMusicEntries(ab, aib))
        for (std::size_t ib : aib)
        {
            int16_t v;
            std::memcpy(&v, ab.data() + ib, 2);
            a.push_back(v);
        }
    return a;
}

// the track an entry's key plays (its own id when the key is its own or unknown)
int CEditor::TrackOfMusicEntry(int i)
{
    ReadMusicKeys();
    std::vector<uint8_t> ab;
    std::vector<std::size_t> aib;
    const std::string rel = SoundPart();
    if (rel.empty() || !m_proj.readPart(rel, ab) || !FMusicEntries(ab, aib) || i < 0 || i >= int(aib.size())) return -1;
    int16_t v;
    std::memcpy(&v, ab.data() + aib[std::size_t(i)], 2);
    for (const auto& [id, key] : m_mpMusicKey)
        if (std::memcmp(key.data(), ab.data() + aib[std::size_t(i)] + 2, 64) == 0) return id;
    return v;
}

std::string CEditor::MusicFile(int id)
{
    ReadSoundManifest();
    for (const MUSICTRACK& t : m_amusicAll)
        if (t.id == id) return t.name;
    return std::to_string(id);
}

void CEditor::DrawSoundTab()
{
    ReadSoundManifest();
    ImGui::TextColored(st::vec(st::muted), "%s", T("The level's ambient sounds and its music. Play listens in the editor; the play test plays them in the game."));
    if (!s_preview.file.empty())
    {
        ImGui::TextColored(st::vec(st::cta), "%s %s", T("Playing"), ce::pathOf(s_preview.file).filename().string().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Stop"))) StopPreview();
    }
    // ---- music
    ImGui::SeparatorText(T("Music"));
    const std::vector<int> amus = MusicOfLevel();
    for (std::size_t i = 0; i < amus.size(); ++i)
    {
        ImGui::PushID(int(i));
        const int idPlay = TrackOfMusicEntry(int(i));
        if (idPlay == amus[i]) ImGui::Text("%zu: %s", i, MusicFile(amus[i]).c_str());
        else ImGui::Text("%zu: %s -> %s", i, MusicFile(amus[i]).c_str(), MusicFile(idPlay).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Play")))
            for (const MUSICTRACK& t : m_amusicAll)
                if (t.id == idPlay) FPlayPreview(ce::utf8(m_inst.dir / ce::pathOf(t.path)));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##rep", T("Replace with...")))
        {
            for (const MUSICTRACK& t : m_amusicAll)
                if (ImGui::Selectable(t.name.c_str()))
                {
                    std::string error;
                    if (!FReplaceMusic(int(i), t.id, error)) Status(error);
                }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if (amus.empty()) ImGui::TextDisabled("%s", T("(this level has no music of its own)"));
    // ---- ambient sound emitters
    std::vector<LO*> aemit;
    for (const auto& up : m_psw->aploAll)
        if (up->Cid() == 0x2B && m_mpobj.count(up.get())) aemit.push_back(up.get());
    char head[96];
    std::snprintf(head, sizeof(head), "%s (%zu)", T("Ambient sounds"), aemit.size());
    ImGui::SeparatorText(head);
    if (ImGui::Button(T("Add an ambient sound at the view"))) AddSoundEmitter(PosViewGround() + glm::vec3(0, 0, 100.0f));
    ImGui::BeginChild("emitters");
    for (LO* pe : aemit)
    {
        ImGui::PushID(pe);
        if (ImGui::Selectable(NameOfObject(pe).c_str(), m_plSel == pe, 0, ImVec2(160, 0))) Select(pe);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("At the view"))) MoveObjectTo(pe, PosViewGround() + glm::vec3(0, 0, 100.0f));
        const auto it = m_mpobj.find(pe);
        int isnd = 0;
        for (int ifield : it->second.aiarr)
        {
            const EDITFIELD& f = m_rec.afield[std::size_t(ifield)];
            if (f.eopid != 99) continue;
            std::string rel;
            std::vector<LISTEL> ael;
            const std::vector<uint8_t>* pab = nullptr;
            if (!FListElements(ifield, rel, ael) || ael.size() < 5 || !(pab = m_proj.partRef(rel))) continue;
            auto val = [&](int k) {
                const LISTEL& e = ael[std::size_t(k)];
                if (e.fFloat) { float g; std::memcpy(&g, pab->data() + e.ib, 4); return double(g); }
                int32_t n = 0;
                if (e.cb == 4) std::memcpy(&n, pab->data() + e.ib, 4);
                else if (e.cb == 2) { int16_t s; std::memcpy(&s, pab->data() + e.ib, 2); n = s; }
                else n = (*pab)[e.ib];
                return double(n);
            };
            ImGui::PushID(ifield);
            ImGui::Indent(20);
            const int sfx = int(val(0));
            ImGui::Text("%s %d", T("sound"), sfx);
            ImGui::SameLine();
            const std::string wav = SampleOfSound(sfx);
            ImGui::BeginDisabled(wav.empty());
            if (ImGui::SmallButton(T("Play"))) FPlayPreview(wav);
            ImGui::EndDisabled();
            static const char* s_aname[] = {"##sfx", "##s0", "##s1", "##vol", "##pitch"};
            const char* alabel[] = {"", T("falloff start"), T("falloff end"), T("volume"), T("pitch")};
            for (int k = 1; k < 5; ++k)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                float g = float(val(k));
                ImGui::DragFloat(s_aname[k], &g, k < 3 ? 10.0f : 0.01f, 0.0f, k < 3 ? 100000.0f : 10.0f, k < 3 ? "%.0f" : "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", alabel[k]);
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    std::string error;
                    if (!FSetListElement(ifield, k, g, error)) Status(error);
                }
            }
            ImGui::Unindent(20);
            ImGui::PopID();
            ++isnd;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void CEditor::MoveObjectTo(LO* plo, const glm::vec3& pos)
{
    PLACE p;
    if (!FPlace(plo, p)) { Status(T("this object's transform is not stored in the world file (made at run time)")); return; }
    SetPlaceWorld(p, glm::translate(glm::mat4(1.0f), pos) * glm::mat4(glm::mat3(MatPlace(p))));
    FRecordPlace(p, NameOfObject(plo));
}

int CEditor::AddSoundEmitter(const glm::vec3& pos)
{
    std::string error, from;
    int oid = 0;
    // (a copy of one of the level's that plays something: a top-level emitter with a sound record, option 99)
    int iobj = -1;
    for (LO* p : m_psw->mpcidaplo[0x2B])
    {
        if (p->paloParent || iobj >= 0) continue;
        bool fSound = false;
        for (const OPT& o : p->aopt) fSound |= o.eopid == 99;
        const int i = IobjOfLo(p);
        if (fSound && i >= 0 && m_rec.aobj[std::size_t(i)].depth == 0) iobj = i;
    }
    if (iobj < 0) iobj = IobjExampleOf(0x2B);
    if (iobj >= 0) oid = CopyRecord(iobj, CopyGroup(), &pos, true, error);
    else oid = ImportRecordOfClass(0x2B, pos, from, error);
    if (!oid) { Status(std::string(T("Add an ambient sound at the view")) + ": " + error); return 0; }
    std::printf("editor: sound emitter %d at %.0f %.0f %.0f\n", oid, pos.x, pos.y, pos.z);
    return oid;
}

bool CEditor::FSoundVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "music")   // music: the level's tracks
    {
        const auto a = MusicOfLevel();
        for (std::size_t i = 0; i < a.size(); ++i)
            std::printf("editor: music %zu: track %d %s, plays %s\n", i, a[i], MusicFile(a[i]).c_str(), MusicFile(TrackOfMusicEntry(int(i))).c_str());
        return true;
    }
    if (verb == "musicset")   // musicset:I:TRACK
    {
        std::string error;
        const std::size_t c = arg.find(':');
        if (c == std::string::npos || !FReplaceMusic(std::atoi(arg.c_str()), std::atoi(arg.c_str() + c + 1), error)) Status("musicset: " + error);
        return true;
    }
    if (verb == "musicplay")   // musicplay:TRACK
    {
        ReadSoundManifest();
        for (const MUSICTRACK& t : m_amusicAll)
            if (t.id == std::atoi(arg.c_str())) FPlayPreview(ce::utf8(m_inst.dir / ce::pathOf(t.path)));
        return true;
    }
    if (verb == "sounds")   // sounds: the ambient emitters and their sounds (and samples)
    {
        for (const auto& up : m_psw->aploAll)
        {
            if (up->Cid() != 0x2B || !m_mpobj.count(up.get())) continue;
            for (const OPT& o : up->aopt)
                if (o.eopid == 99 && o.aval.size() >= 5)
                {
                    const std::string wav = SampleOfSound(o.aval[0].n);
                    std::printf("editor: sound emitter %d: sound %d falloff %d..%d volume %.2f pitch %.2f sample %s\n", up->oid, o.aval[0].n, o.aval[1].n, o.aval[2].n,
                                o.aval[3].ag[0], o.aval[4].ag[0], wav.empty() ? "-" : ce::pathOf(wav).filename().string().c_str());
                }
        }
        return true;
    }
    if (verb == "soundplay")   // soundplay:SFXID
    {
        const std::string wav = SampleOfSound(std::atoi(arg.c_str()));
        if (wav.empty() || !FPlayPreview(wav)) Status("soundplay: no sample for " + arg);
        return true;
    }
    if (verb == "soundadd") { AddSoundEmitter(PosViewGround() + glm::vec3(0, 0, 100.0f)); return true; }
    if (verb == "soundset" && m_plSel)   // soundset:K:V (the selected emitter's first sound, value K 0..4)
    {
        const auto it = m_mpobj.find(m_plSel);
        if (it == m_mpobj.end()) return true;
        for (int ifield : it->second.aiarr)
            if (m_rec.afield[std::size_t(ifield)].eopid == 99)
            {
                std::string error;
                const std::size_t c = arg.find(':');
                if (c == std::string::npos || !FSetListElement(ifield, std::atoi(arg.c_str()), std::atof(arg.c_str() + c + 1), error)) Status("soundset: " + error);
                break;
            }
        return true;
    }
    return false;
}
#else
void CEditor::DrawSoundTab() { ImGui::TextDisabled("%s", "Sound: not for " EDGAME_NAME " yet"); }
bool CEditor::FSoundVerb(const std::string&, const std::string&) { return false; }
void CEditor::StopPreview() {}
#endif

}  // namespace edimpl
