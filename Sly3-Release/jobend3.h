#pragma once

// Sly 3's job end screens: the job's end state (MISSION +0xB8, orig3 SetJobEndState 0x1878E0, its update
// in the job's 0x1875B0) on the screens SW +0x433C (1066: the result / "job complete" stamp, a part per character)
// and *0x4797C0 (1067: Job Failed!, string 851 the job's +0xC8, "Press X to try again").
//   0 none; 1 complete: 1066 shown, part 0x42C + 2 k (k the job's character, its mask +0xB0: 1 Sly .. 0x80); 2 its
//   out part 0x42D + 2 k, then closed; 3 a challenge's result (TODO(sly3): the challenge manager 0x4BC230 (the
//   strings 998 / 999 / 1110..1112 read it), not in the goal path); 4 the failing script (+0xC4); 5 failed: 1067
//   shown; 7 its "Press X to try again" (after 0.5 s): the job restarts; 8 done (waits for the restart / exit).
// The game logic plugs in its side (the script code: JobCompleteClose 0x1874E8, the reward / save 0x187270, the restart
// 0x186FE8); a null hook is skipped.

struct SW;
struct JOB3;

struct JOBENDHOOKS3
{
    void (*pfnRewards)(JOB3*) = nullptr;       // orig3 0x187270 (the job's rewards / progress save)
    void (*pfnCompleteClose)(JOB3*) = nullptr; // orig3 0x1874E8 JobCompleteClose (its exit task, the next job)
    void (*pfnRestart)(JOB3*) = nullptr;       // orig3 0x186FE8 (RequestTaskStartByOid 0x189F38 on its checkpoint)
};
extern constinit JOBENDHOOKS3 g_jobendhooks3;

// orig3: 0x001871C0 JobComplete's screen half: with a character (+0xB0 non-zero) the stamp (state 1), else at once
// (the rewards and the close). Call it after the broadcast 0x24.
void JobEndComplete3(JOB3* pjob);
// orig3: 0x00186EE0 / 0x00186F90 the job failed: its failure line (text id: +0xC8, string 851) and Job Failed (state 5)
void JobEndFail3(JOB3* pjob, int idText);
// the job's end state (0: none) and its failure line
int NJobEndState3(const JOB3* pjob);
int IdJobFailText3();
// each frame (the HUD's update): the states' waits and input
void UpdateJobEnd3(SW* psw);
// a world's start: no job ending (the screens hidden)
void ResetJobEnd3();
