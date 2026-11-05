// The game host's interim binocucom / dialog / HUD glue (talk3.h).

#include "talk3.h"

#include "hud3.h"

#include "../lo.h"
#include "../sw.h"

#include <cstdio>
#include <cstdlib>
#include <string>


namespace talk3
{
namespace
{
struct TALK3
{
    SW* psw = nullptr;
    int cframe = 0;
    int state = -1, stateTarget = -1, cframeReady = 0;  // actor +0xF80 and where it goes
    LO* ploActor = nullptr;
    LO* ploSpeaker = nullptr;
    bool fShown = false, fHud = false, fHint = false, fTitle = false;
};
TALK3 s_t;

bool FTrace()
{
    static const bool s_f = std::getenv("SLY3_TALKPRINT") != nullptr;
    return s_f;
}

void Show(int oid, int oidParent, bool fShow, int idPart = -1)
{
    SCREEN3* ps = PscreenFind3(oid, oidParent);
    if (FTrace()) std::printf("talk3: frame %d screen %d (parent %d) %s%s\n", s_t.cframe, oid, oidParent, fShow ? "shown" : "hidden", ps ? "" : ": none");
    if (!ps) return;
    if (fShow) ShowScreen3(ps, idPart);
    else HideScreen3(ps);
}

// the binocucom's screen objects (m_ext: render3.md "m_ext's objects"); the window under Bentley occludes the HUD
void ShowBinocScreens(bool fShow)
{
    // the windows 1045 / 1038 have a copy under each player: the looking actor's (Sly 24: the blue frame; Bentley's
    // are green). TODO(sly3): the left window's head (0x304, Bentley 25) is not drawn yet
    const int oidActor = s_t.ploActor ? s_t.ploActor->oid : 24;
    Show(1042, -1, fShow, 960);  // (part 960: the binocucom's frame)
    Show(1039, -1, fShow);
    Show(1045, oidActor, fShow);
    Show(1038, oidActor, fShow);
    if (SCREEN3* ps = PscreenFind3(1038, oidActor)) SetScreenOccluding3(ps, fShow);
    if (!fShow) ClearHeadCameras3();
}

LO* PloChild(LO* ploParent, int oid, int cid)
{
    if (!s_t.psw || !ploParent) return nullptr;
    for (LO* plo : s_t.psw->mpcidaplo[std::size_t(cid)])
        if (plo->oid == oid && plo->paloParent == ploParent) return plo;
    return nullptr;
}

// The heads' cameras: the class-0x06 cameras 772 / 773 under each actor, in the actor's placed frame (the instance
// matrix the scene draws it at). TODO(sly3): their animated pose (the binocucom's head animation moves them).
void SetHeads()
{
    if (!s_t.psw) return;
    const int aoid[2] = {25, 24};  // 0x304: the speaker (Bentley), 0x305: the actor (Sly)
    for (int i = 0; i < 2; ++i)
    {
        LO* ploActor = PloFindSwObjectByOid(s_t.psw, aoid[i]);
        if (!ploActor) continue;
        LO* ploCam = nullptr;
        for (int oidCam : {772, 773})
            if (!ploCam) ploCam = PloChild(ploActor, oidCam, 0x06);
        if (!ploCam) continue;
        glm::mat4 matActor(1.0f);
        if (auto it = s_t.psw->mpmatPlayerInstance.find(ploActor); it != s_t.psw->mpmatPlayerInstance.end()) matActor = it->second;
        else if (const LOX* plox = dynamic_cast<const LOX*>(ploActor)) matActor = plox->mat;
        const LOX* ploxCam = dynamic_cast<const LOX*>(ploCam);
        if (!ploxCam) continue;
        const glm::mat4 m = matActor * ploxCam->mat;
        HEADCAM3 hc;
        hc.id = 0x304 + i;
        hc.oidActor = aoid[i];
        hc.pos = glm::vec3(m[3]);
        hc.dir = glm::normalize(glm::vec3(m[0]));
        hc.left = glm::normalize(glm::vec3(m[1]));
        hc.up = glm::normalize(glm::vec3(m[2]));
        SetHeadCamera3(hc);
    }
}
}  // namespace

void Reset(SW* psw)
{
    s_t = TALK3{};
    s_t.psw = psw;
}

void Update(int cframe)
{
    s_t.cframe = cframe;
    if (s_t.state != s_t.stateTarget && cframe >= s_t.cframeReady)
    {
        s_t.state = s_t.stateTarget;
        if (FTrace()) std::printf("talk3: frame %d binocucom state %d\n", cframe, s_t.state);
        if (s_t.state == -1 && s_t.fShown)
        {
            ShowBinocScreens(false);
            s_t.fShown = false;
        }
    }
    if (s_t.fShown) SetHeads();
}

void OpenBinoc(LO* ploActor, LO* ploSpeaker)
{
    s_t.ploActor = ploActor;
    s_t.ploSpeaker = ploSpeaker;
    s_t.state = 0;
    s_t.stateTarget = 1;
    s_t.cframeReady = s_t.cframe + 40;
    if (!s_t.fShown) ShowBinocScreens(true);
    s_t.fShown = true;
    if (FTrace()) std::printf("talk3: frame %d binocucom open (actor %d, speaker %d)\n", s_t.cframe, ploActor ? ploActor->oid : -1,
                              ploSpeaker ? ploSpeaker->oid : -1);
}

void CloseBinoc(LO*)
{
    s_t.state = 0;
    s_t.stateTarget = -1;
    s_t.cframeReady = s_t.cframe + 20;
    if (FTrace()) std::printf("talk3: frame %d binocucom close\n", s_t.cframe);
}

int BinocState(LO*) { return s_t.state; }

void SetSpeaker(LO*, LO* ploSpeaker)
{
    s_t.ploSpeaker = ploSpeaker;
    s_t.state = 0;
    s_t.stateTarget = 1;
    s_t.cframeReady = s_t.cframe + 60;
    if (FTrace()) std::printf("talk3: frame %d binocucom speaker %d\n", s_t.cframe, ploSpeaker ? ploSpeaker->oid : -1);
}

void ShowTitleScreen(bool fShow)
{
    if (fShow == s_t.fTitle) return;
    s_t.fTitle = fShow;
    Show(1699, -1, fShow);
}

void ShowHint(int idText)
{
    g_hud3.idPrompt = idText;
    if (!s_t.fHint) Show(1058, -1, true);
    s_t.fHint = true;
}

void ClearHint()
{
    if (s_t.fHint) Show(1058, -1, false);
    s_t.fHint = false;
}

void ShowCardMessage(int msg, int prompt)
{
    SetHudString3(847, StrTextOfId3(msg));
    SetHudString3(848, prompt == 2 ? "&2S&. : " + StrTextOfId3(69) + "          No : &2T&." : prompt == 1 ? "&2X&. : OK" : "");
    Show(1466, -1, true);
}

void HideCardMessage() { Show(1466, -1, false); }

void ShowGameplayHud()
{
    if (s_t.fHud) return;
    s_t.fHud = true;
    Show(7465, 24, true);
}
}  // namespace talk3
