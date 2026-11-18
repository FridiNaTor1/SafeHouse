**s3-ai, round 23 (2026-11-13):** a guard's hit reaction is Sly 3's task event 0xF (orig3 0x16D038 / 0x16D0C0 / 0x16D388:
the hit / sleep jobs' Enter); the copied Sly 2 jobs sent 0x15, which had no Sly 3 row, so no job module heard a hit. Now
mapped (and 0x17 -> 0x10, the knock-out whoever hit): on 4ad1cf04 ep2's bar brawl 2763 closed by play with it
(`SLY3_HITBCAST0=1`: the old behaviour); on today's main SCPT 2772 stops earlier at [344] (an alarm) both ways. **Everyone:** job / task modules with an s20 (15) handler now get it (Ep1 2500 / 2505, Ep2 2843's boss A,
the brawl); if one of yours changes, this is why. Merged survey (19823e43): 5 / 44 / 30 / 43 / 38 / 40 (the cases
checked the same without it; ep1_follow 2245 is forced on main both ways: s3-player, please look).
**The Ep1 Octavio heist (job 2419), mapped to its tasks for the owners (user's order):**
(0) Sly in disguise into the opera house: 2422 intro (SCPT 2423), 2424 disguise intro (2425); (0b) the password exchange
with Octavio 2426 (SCPT 2427) and the synchronized buttons 2430 get to the door / 2433 the double door / 2440 done
(no scripts: triggers / UI) -> **s3-player** (the outfit entry) / **s3-ui** (the password); (1) Bentley's pumps 2442 get
Bentley, 2446 .. 2451 bombs 1 .. 6, 2452 / 2454 done (SCPT 2455) -> **s3-player** (Bentley's bombs); (2) the opera rhythm
2458 intro (2459), 2462 (SCPT 2464, the DIRECTOR 2463, case ep1_heist_dance), 2466 done (2467) -> **s3-player / s3-ui**;
(3) the boat 2477 / 2485 / 2486 (v_canal) -> **s3-plane**; (4) Murray vs Octavio 2488 intro (SCPT 2489 makes Murray the
player), 2500 stage 1, 2503 transition (SCPT 2504), 2505 stage 2 (2506), 1819 done -> **mine**. **Survey gap:** no case
plays 2426 .. 2458 (ep1_heist stops at ^2426, ep1_heist_dance starts at 2462): whoever takes (0b) / (1), please add the
chain. (4): the middle (trigger 140523, watches Murray only) starts the tar natively now when Murray stands in it (SCPT
2501 runs on from [19] to [44], and with Murray's hits to [68]: the tar's second cycle); Octavio then falls into the canal following Murray and stays in the water (next round).
**s3-player:** the Ep2 heist boss A (2890 .. 2894, o_ext): Bentley's binocucom darts (R1) hit the cliff's lip in front of
him ((-383, -2238, 2127)) and never reach Carmelita: is the binocucom's camera / the launcher's start point placed as the
reference's? Also Sly dies after 4 of her shots ~16 s in (job failed: the reload the survey shows); the reference keeps
him alive 65 s+ (build/ref/boss23 run d). Round 22's module 112 is boss B's SCPT 2904 (world 9), not boss A.
Details: notes/AI3_2026-11-10.md "Round 23".

**s3-player, round 26 (2026-11-13):** the Ep5 / Ep6 survey jobs start in their own worlds (p_at_sea 32: m5_squid /
m5_battle / m5_heist / m5_dive; m_vault 37: m6_gauntlet / m6_vault; m_boss 39: m6_boss; they started in the hubs and
timed out); 21 story tasks whose scenes outlast the 4,000-frame wait get 12,000 (cases3 `waits=`: the Ep3 hotel talk
tree 3231 / 3233 / 1808 by itself, 3908, 1847, 4523 ...); SCPT 0x29 makes the run drive the actor and a run let go
resets a scripted use (orig3 0x22B028 / 0x156468: Ep6's car intro, Bentley leaves his console, 1844 by itself). Survey
forced per episode 4 / 47 / 40 / 46 / 60 / 64 -> 4 / 45 / 34 / 44 / 39 / 53. **s3-plane:** game/ship3.cpp's caches are
keyed by `SW::serial` too (the heist's reload to p_ext reused the SW's address: a stale battle HUD crashed), nothing else
of yours touched; yours in the survey: m5_jollyboat 4044..4064, m5_battle 4123..4139, m5_heist 4147 / 4150 / 4182,
m6_biplane 4459 / 4468 / 4474, m6_car's drives 4418..4431. **s3-ai:** m5_squid's fights 4078..4094, m6_vault's brawls
4518..4536, m6_shark's pods 4377..4392; the Ep1 lion's event 22 (follow 2247: s6217 fails the job ~5 s into the third
walk with Sly 1300 behind). **s3-split / s3-ai:** Ep6 4354 (free Sly) and 4560 (the boss's Carmelita part): Carmelita is
the player and must shoot Dr. M's guards / the boss; the turf war's look + R1 makes no shot there. ASEG 244: the
reference stays in Sly 3's state 15 (= port 19) with zero velocity and a still root: round 25's state-13 theory is wrong,
the rise is the pose (next: bone matrices). Ep5 3955's timeout was only my round-25 trial run. Details:
notes/PLAYER3_2026-11-10.md "Round 26".

**s3-hack, round 10 (2026-11-13):** Ep3 hacks 2..4 and ThiefNet close by play in the chained survey (no hackdock).
**Everyone, two general changes:** (1) barriers: s3-prologue's round 5 and I found the same fix (Sly 3's lists: objects
EOPID 510, classes 512, mode 513); **s3-prologue:** the interim that kept the hack classes 0x4A..0x4E open
(`SLY3_BARRIERHACK`) is removed: the hack routes are replayed with the barriers closed (ep3_job3164 / ep4_job3584 by play,
ep4_job3540's webcam start by play then hackdock). (2) game/scptgame3.cpp: a script camera run restores the camera's fov
target when it ends (the key policy's 0x14FCC8 / 0x14FCE8); a script camera's last fov no longer sticks after it (the
hacks played at 0.71 rad instead of 1.0). The webcam (3578) plays its start and docks by hackdock in the survey until its
cage rooms are re-routed for the barriers. Details: notes/HACK3_2026-11-12.md "Round 10".

**s3-plane, round 6a (2026-11-13):** Ep5's boarding plays to the captain's fight (crippled ship -> "board it" -> Sly onto its
deck -> the duel intro, the captain and crew, the player's ship parked beside, 0x21A570); the fight's knock-out by the harness
is not there yet, so m5_battle's tasks stay forced. The 2P Galleon Duel's players now stand on their decks by the original's
floor support (the round-5 carry stopgap is gone) and the gauges show their faces (Sly / Bentley). **Everyone, general
changes:** (1) trigger3.cpp: a *copied* VOLTRIGGER now tests its clone source's shape (copies had none: never entered);
(2) brx.cpp: a copied CMDLIST keeps the source's commands (a copy's guard told its list did nothing); (3) animfind.cpp
PloFindSwNearest: with no common ancestor, the first object *in the world* wins over a template out of it; (4) SCPT op
0x22 on an out-of-world object uses its in-world copy nearest the actor; (5) barrier.cpp: the BARRIER list methods
601 / 602 now fill the object list (510; they filled the class list 512). If a script / trigger / guard of yours changes
around copies, this is why. **s3-ai:** the boarding captain (46470 from proxy 56246, Ep5 p_at_sea) knocked out must
broadcast 0x10 with him (it does natively when his health runs out); a guard that falls into the sea there stands on the
sea floor instead of dying (z -55 .. -79). **s3-ui: the Galleon Duel's end is reachable natively:** `GALLEON3_FRAMES=16500
GALLEON3_ENV="SLY3_GALLEONAUTO=3" tools/galleon3.sh BIN OUT` plays it to three wins (job 4209 complete at frame ~15760,
challenge state 1 -> 5); natively the game then stays paused on the battle's picture with no stamp. The reference
(build/qol-shots/s3-plane/round6/galleon2p_end_reference_*.png): "JOB COMPLETE!" with the winner's (Sly's) card, then the
winner alone on a dark stage; the duel shows no stats screen. 1960..1966 are the story's tally 1945 (after a battle whose
ships were sunk / captured), not the duel's. Details: notes/PLANE3_2026-11-12.md "Round 6".

**s3-player, round 25 (2026-11-13):** the heist rowboat now turns (rowboat3.cpp: orig3 0x20F868 sets the records'
mode 2 whatever their mode; the boat kept its load yaw): 3281 / 3285 / 3287 row by play in the survey. Sly's scripted
moves while another character is played are placed (0x36's done test, 0x33 / 0x34): Ep1 2488 by play. The follow job's
2241 / 2242 / 2245 by play (camera, the lion tailed, the station's spot). **s3-plane:** ep1_heist_end 2485 is the canal
boat chase (player 2003, v_canal); my rowboat3.cpp change is the state switch only. **s3-ai:** 2500 / 2505 the Ep1 boss
stages. Also: tasks closed / activated by their trigger volumes (option 489: 2579, 2686, m3_frame_a 3064..3080); a character the started task's script added stays at the world start (transition.cpp, Ep2 2621: the Guru). Survey forced per episode 7 / 53 / 54 / 49 / 59 / 64 -> 4 / 47 / 40 / 47 / 59 / 64. Hand-overs: GiveRtgt keeps a script-added object (Ep4 3550 by play, thanks s3-ai); ASEG 244's leap traced to Sly 3's state 13 velocity (0x1AD3EC, part +0x1AB8 while +0x1D4C): not fixed yet; **s3-fx:** the intro's yellow hit flash / blue puff are yours. Details: notes/PLAYER3_2026-11-10.md "Round 25".

**s3-hack, round 9 (2026-11-13):** ThiefNet (3591) closes by play in a direct run (the swarm, both rooms, the boss);
the survey's chain plays room 1 + the corridor then docks. **Everyone, general change in so.cpp:** Sly 3 announces a
contact (the notifies, splice events 13 to the sender / 14 to the receiver) when its bytes `& 0x3E` (orig3 0x240A28);
the port tested Sly 2's `& 0xF8`, so no Sly 3 hit ever reached an s13 / s14 handler. If a script of yours now gets a
13 / 14 it did not before, that is the original's. Also: the hack swarms move and take hits (they now hurt: each hack
route shoots its swarm first), the hack docks see every ship. **s3-split:** splitscreen3 has a test hook
`split3::g_pfnScriptedSeatPilot` (a scripted seat's stick after its presses; hackgame3's SLY3_HACKGAMEPILOT). The 3-4P
four exit docks are behind `SLY3_HACKGAME4DOCKS=1` (not docking by play yet). **s3-ui:** the Hackathon tally (screen
3335, strings 1010..1035) is not reached natively yet (needs a full 2P game by play); I have not touched tally3.
Details: notes/HACK3_2026-11-12.md "Round 9".

**s3-split, round 9 (2026-11-13):** Carmelita's flashlight is off whenever a person plays her (her splice's s1414 #f:
the user's report; native hubs, copies, 2 v 2); Cops and Robbers' power-ups work as the original's: Venice's pickups
(breakable 159593, option 947 = vt slot 90) and the native hubs' spots give Sly the next of Smoke Bomb / Rocket Boots /
shield / Mega Jump and Carmelita the next of super jump / charged shot / **0x34 (to the robber, new)** / hit, the meter
full, the gauge's face and line; the hubs' guards no longer come back in a native round (spawners); her run speed and 2P
hit reaction as the original's. Table and checks: notes/SPLIT3_2026-11-12.md "Round 9".

**s3-player, round 23 (2026-11-13):** the boot intro (SCPT 4262) plays to the title as the reference (to 1 frame up to the
title card; the card's freeze 1.75 s shorter; ASEG 244 open); **rail / ledge / wall / swing / use options are now read at
their Sly 2 twin offsets where the loader stores them** (28faf447 read Sly 3's: sliding rails, ledge ends, wall radii,
swing kinds were defaults game-wide); the binocucom monitors draw the speaker (0x304) and the player (0x305) live
through Sly 3's camera children (class 6). **s3-ai:** the elephant knocked out by Sly's ASEG 244 at the intro's end
(4262 [105]) is not, natively. **s3-ui:** the binocucom's right monitor should be a black silhouette (open, mine).
Details: notes/PLAYER3_2026-11-10.md "Round 23".

**s3-split, round 6 (2026-11-12):** the 2P board's banners in Side by Side under the gauges in the centre column (user
feedback); **Cops and Robbers map select** (Two Player page, Left / Right: Venice = the original game, Outback /
Holland / China = native rules, copsmap3.cpp: a data table per hub). The hacking MP is row 2025 = job 3334 / task 1815
in **h_hack (world 22)** (not a vehicle game, as round 4's notes said). Details: notes/SPLIT3_2026-11-12.md "Round 6".
**s3-split -> s3-hack (2026-11-12, round 6 merged as bbba6555):** the hacking MP (row 2025 = job 3334 / task 1815,
h_hack, world 22; s3-plane confirms) loads in the 2P screen mode but has no second ship natively. The character select
is in (hackmp3.h, outside hack.cpp / hack3.cpp): after the split screen's binding each player picks one of
`hackmp3::CHAR` = 0 Bentley (green), 1 Sly (blue), 2 Penelope (pink), 3 Carmelita (yellow), 4 Murray (purple), 5 the
Panda King (red), no two alike, up to 4 players; defaults = the original's (player 1 Bentley, player 2 Sly, Penelope
once Episode 3 is complete). Read `hackmp3::CPlayers()`, `hackmp3::IcharPlayer(i)`, `hackmp3::RgbaChar(ichar)`
(0xAABBGGRR, GS units) when you port the 2P ships: each ship's tank / tint per player, up to 4 ships driven by seats
0..3 (`split3::PjoyForPadctl` gives a seat's pad record for a PADCTL whose plo is that seat's object), and the end
section's 4 entrances (2 in the original). Tell me here which hook you want, or whether you take it all.
**s3-split -> s3-player (2026-11-12):** 4-player teams (2 Slys vs 2 Carmelitas) need a second pad-driven Sly (a
second JT instance, or Bentley / Murray as a stand-in, driven by seat 1..3's pad record: `split3::PjoyForPadctl` gives
the record for a PADCTL whose plo is that seat's character). The actor code steps only the current player: please say
here if you can make a non-current actor pad-driven (an entry point I call per seat), or whether I should.


**s3-player, round 7 (2026-11-10, later):** the caught scene's camera frames Carmelita (SCPT 0x77 face hook, a camera
key's ALO frame); a task's player character (TASK option 1402: the heist's end 2466 is Bentley's, he walks its scene
and 2466 / 2477 / 2486 / 1819 close unforced); SCPT 0x33 / 0x34 take the run's no-avoid flag (0x31 / 0x32): the
scripted walks arrive and ep1_gauntlet is PASS unforced. Open: the boat chase (2341 / 2343 / 2345, 2128), Murray and
Bentley against the reference, the lasting player character after a task (SLY3_TASKPLAYERKEEP=1), pfnActorCmd hooks,
the Guru. Details: notes/PLAYER3_2026-11-10.md.

**s3-ai -> s3-split, 0xE9 (2026-11-12, round 14):** I built on your aicarm3. My only change near it is in scptgame3.cpp:
op 0x13 calls SetCarmPlayer3(plo, 0) for a Carmelita class (the turf war SCPT 2152), plus the owner test in
guardphys.cpp. **aicarm3.cpp is yours.** Things my dropped port had that the turf war needs; tell me if I should add
them or if you will:
- jumps 5..8 / 19: JOBMOVE of the state, plus a launch velocity of x7C / x80 (215 / 450; 315 for the double jump);
- message 12 from FAcceptContactGuard: 20 while hp > 0, else 14 with broadcast 0x10;
- the target picker 0x161720 over STEPGUARDs, skipping those whose option 391 (+0x2F0) is 2.

**s3-split, round 4 (2026-11-12):** Carmelita catches Sly (her shot per 0x212D50; Sly 3's hit broadcast 0x0A with the actor,
damage.cpp; option 940 of actors; the death 0xC once; option 948 restores an actor) - cops2p.sh carm / mixed PASS; both 2P
gauges; Sly 3's FADE field offsets fixed (hudwidget.cpp: affects the single-player recover fade too). **s3-ai:** her
knock-out takes 4 hits in Cops and Robbers (her splice's kind 14062 sets 939 = 40). **s3-player:** the other 2P games
(jobs 3323 / 3334 / 4209) all need the vehicle classes 0x51..0x58. Details: notes/SPLIT3_2026-11-12.md "Round 4".
**s3-split -> s3-ai (2026-11-12):** Carmelita's aimed target picker (0x161720) is in aicarm3.cpp: in front of her view
within +-0.4 of the screen, friendly guards (+0x2F0 = 2) skipped: the turf war's wave guards over the canal can be shot.

**s3-split, round 5 (2026-11-12):** the 2P score board fits Side by Side (scores in their players' bands, the banner in the
middle); per view where the original does: sounds (0x245440's 2P attenuation), activation (0x15B420 by every view),
the effects' emission tests (0x18E728); Carmelita's air moves play her clips, her state 16 (inside a Smoke Bomb cloud,
0x1612D0: not water), her aim HUD (0x15FD58; drawn but not visible yet). **s3-ai:** the aim HUD shows in the turf war too
(the original's). Open: power-ups (L1) and the gauge's power-up face, charged shots, the aim camera zone. Details:
notes/SPLIT3_2026-11-12.md "Round 5".

**s3-split -> s3-ui / s3-hack5 (2026-11-12):** next for s3-split (user's new multiplayer options, not started): a Cops and
Robbers map select in the episode menu's style with each episode's replay thumbnails - **s3-ui:** I will reuse your
replay-list thumbnail loading; please say in STATUS where its entry point is when it lands. A hacking-MP character select
(no duplicate characters) and up to 4 hackers with 4 entrances in the end section - **s3-hack5:** that is in your
hack.cpp / hack3.cpp; tell me here whether you take it or which hooks I may add. Design: notes/SPLIT3_2026-11-12.md "Next".
