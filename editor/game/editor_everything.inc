// The map editor's "everything editable" tools (notes/EDITOR_PLAN.md step 10): any option the game knows added to an
// object (found by name or number; its value entered by its kind from the executable's option table, brx.cpp
// FEditorOptionInfo), any of its options (value or list) removed, and the classes this level has made from the palette
// (editor_build.inc "Every class in this level"). Options are read at the load: an added / removed one shows after the
// world loads again (done at once).

namespace edimpl
{

namespace
{
// the option's value bytes from the typed numbers (k: its kind; grf 0x401 lists: c18 values as CRefs of kind 6, S32)
bool FOptionValueBytes(int k, int grf, int cList, const std::vector<double>& av, std::vector<uint8_t>& ab, std::string& error)
{
    ab.clear();
    auto put = [&](const void* pv, std::size_t cb) { ab.insert(ab.end(), static_cast<const uint8_t*>(pv), static_cast<const uint8_t*>(pv) + cb); };
    if (grf & 0x400)
    {
        for (int i = 0; i < cList; ++i)
        {
            const uint16_t kRef = 6;
            const int32_t v = i < int(av.size()) ? int32_t(av[std::size_t(i)]) : 0;
            put(&kRef, 2);
            put(&v, 4);
        }
        return true;
    }
    if (grf & 1) { error = T("a method option without values: added with no value"); return true; }
    const OPTTYPE t = OptType(k);
    if (t.cb == 0) { error = T("this kind of option has no value the editor can enter"); return false; }
    if (t.cf)
        for (int i = 0; i < t.cf; ++i) { const float g = i < int(av.size()) ? float(av[std::size_t(i)]) : 0.0f; put(&g, 4); }
    else
    {
        const int cbEach = t.cb / std::max(1, t.ci);
        for (int i = 0; i < t.ci; ++i)
        {
            const int32_t v = i < int(av.size()) ? int32_t(av[std::size_t(i)]) : 0;
            if (cbEach == 1) { const uint8_t b = uint8_t(v); put(&b, 1); }
            else if (cbEach == 2) { const int16_t s = int16_t(v); put(&s, 2); }
            else put(&v, 4);
        }
    }
    return true;
}
}  // namespace

bool CEditor::FAddAnyOption(LO* plo, int eopid, const std::vector<double>& av, std::string& error)
{
    int k = 0, grf = 0;
    if (!plo || !FEditorOptionInfo(eopid, k, grf)) { error = T("no such option"); return false; }
    // (a list option's value count: the descriptor's +0x18)
    int cList = 1;
#if EDGAME_SLY2
    if (grf & 0x400) cList = std::max(1, int(g_disc.U32Elf(0x2AC880u + uint32_t(eopid) * 0x28u + 0x18)));
#endif
    std::vector<uint8_t> ab;
    std::string note;
    if (!FOptionValueBytes(k, grf, cList, av, ab, note)) { error = note; return false; }
    return FAddOption(plo, eopid, ab, std::string(T("Add option")) + " " + StrOption(eopid) + " " + T("to") + " " + NameOfObject(plo));
}

void CEditor::DrawOptionTools(LO* plo, const EDOBJ* po)
{
#if EDFEAT_OPTIONS
    if (!plo || !ImGui::TreeNodeEx(T("Add or remove options"))) return;
    EdTip(T("Any option the game knows, by name or number; its value by its kind. Options are read at the load: the world loads again."));
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##optfind", T("Name or number"), m_szOptFind, sizeof(m_szOptFind));
    std::string f = m_szOptFind;
    for (char& ch : f) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (!f.empty())
    {
        int c = 0;
        ImGui::BeginChild("##optfound", ImVec2(0, 90), ImGuiChildFlags_Border);
        for (int e = 0; e < 4096 && c < 40; ++e)
        {
            int k = 0, grf = 0;
            if (!FEditorOptionInfo(e, k, grf) || (k == 0 && grf == 0)) continue;
            std::string s = StrOption(e), sl = s;
            for (char& ch : sl) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (sl.find(f) == std::string::npos) continue;
            ++c;
            if (ImGui::Selectable((s + "  (" + OptType(k).name + ((grf & 0x400) ? ", list" : "") + ")").c_str(), m_eopidAdd == e)) m_eopidAdd = e;
        }
        ImGui::EndChild();
    }
    if (m_eopidAdd >= 0)
    {
        int k = 0, grf = 0;
        FEditorOptionInfo(m_eopidAdd, k, grf);
        const OPTTYPE t = OptType(k);
        const int cv = (grf & 0x400) ? 4 : std::max(1, t.cf ? t.cf : t.ci);
        ImGui::Text("%s:", StrOption(m_eopidAdd).c_str());
        for (int i = 0; i < cv && i < 4; ++i)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::PushID(i);
            ImGui::InputDouble("##v", &m_avAdd[i], 0, 0, "%.3g");
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Add")))
        {
            std::string error;
            if (!FAddAnyOption(plo, m_eopidAdd, std::vector<double>(m_avAdd, m_avAdd + 4), error)) Status(std::string(T("add option")) + ": " + error);
        }
    }
    // remove one of its options
    if (po && (!po->aiopt.empty() || !po->aiarr.empty()))
    {
        std::vector<int> a = po->aiopt;
        a.insert(a.end(), po->aiarr.begin(), po->aiarr.end());
        ImGui::SetNextItemWidth(260);
        const std::string cur = m_ifieldRemove >= 0 && m_ifieldRemove < int(m_rec.afield.size()) && m_rec.afield[std::size_t(m_ifieldRemove)].plo == plo
                                    ? StrOption(m_rec.afield[std::size_t(m_ifieldRemove)].eopid) : std::string(T("(an option to remove)"));
        if (ImGui::BeginCombo("##optremove", cur.c_str()))
        {
            for (int i : a)
                if (ImGui::Selectable((StrOption(m_rec.afield[std::size_t(i)].eopid) + "##" + std::to_string(i)).c_str(), i == m_ifieldRemove)) m_ifieldRemove = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(cur.empty() || m_ifieldRemove < 0);
        if (ImGui::SmallButton(T("Remove")) && m_ifieldRemove >= 0 && m_rec.afield[std::size_t(m_ifieldRemove)].plo == plo)
        {
            FRemoveOption(m_ifieldRemove, std::string(T("Remove option")) + " " + StrOption(m_rec.afield[std::size_t(m_ifieldRemove)].eopid) + " " + T("of") + " " + NameOfObject(plo));
            m_ifieldRemove = -1;
        }
        ImGui::EndDisabled();
    }
    ImGui::TreePop();
#else
    (void)plo; (void)po;
#endif
}

// --editor-do verbs
bool CEditor::FEverythingVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "addopt")   // addopt:EOPID:V1,V2..: an option added to the selection
    {
        const auto c = arg.find(':');
        std::vector<double> av;
        if (c != std::string::npos)
        {
            std::stringstream ss(arg.substr(c + 1));
            std::string t;
            while (std::getline(ss, t, ',')) av.push_back(std::atof(t.c_str()));
        }
        std::string error;
        if (!m_plSel || !FAddAnyOption(m_plSel, std::atoi(arg.c_str()), av, error)) Status("addopt: " + error);
        return true;
    }
    if (verb == "removeopt")   // removeopt:EOPID: the selection's (first) option of that number removed
    {
        const int e = std::atoi(arg.c_str());
        const int i = m_plSel ? IfieldOption(m_plSel, e) : -1;
        int iarr = -1;
        if (i < 0 && m_plSel)
            if (const auto it = m_mpobj.find(m_plSel); it != m_mpobj.end())
                for (int j : it->second.aiarr)
                    if (m_rec.afield[std::size_t(j)].eopid == e) { iarr = j; break; }
        if (i >= 0 || iarr >= 0) FRemoveOption(i >= 0 ? i : iarr, "Remove option " + std::to_string(e));
        else Status("removeopt: the selection has no option " + arg);
        return true;
    }
    if (verb == "hasopt")   // hasopt:EOPID
    {
        int n = 0;
        const bool f = m_plSel && FOptionInt(m_plSel, std::atoi(arg.c_str()), n);
        std::printf("editor: option %s %s%s\n", arg.c_str(), f ? "present" : "absent", f ? (" = " + std::to_string(n)).c_str() : "");
        return true;
    }
    if (verb == "makeclass")   // makeclass:CID (hex): a copy of the level's example of any class, in front of the camera
    {
        const int cid = int(std::strtol(arg.c_str(), nullptr, 16));
        const int iobj = IobjExampleOf(cid);
        std::string error;
        glm::vec3 p = PosViewGround();
        const int oid = iobj >= 0 ? CopyRecord(iobj, CopyGroup(), &p, true, error) : 0;
        if (!oid) Status("makeclass: " + (iobj < 0 ? std::string("no example") : error));
        else std::printf("editor: made class 0x%X oid %d\n", cid, oid);
        return true;
    }
    return false;
}

}  // namespace edimpl
