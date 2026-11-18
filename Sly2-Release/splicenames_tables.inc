// The port's names for Sly 2's splice numbers (splicenames.cpp). The game has none: these are ours, from what the
// engine does with each number (the port's sources: the events' senders, the option ids' getters / setters / methods
// by class (the option descriptors 0x2AC880 and the classes' option tables 0x279AD0), the built-ins 97..168).
// An option id is one symbol of the original's scripts: the classes that have it give it their own meaning, so the
// names are the shared idea (e.g. trigger: a warp's warp, a pad's press, a use's use, a cutscene's start). Names are
// unique (FSpliceRaw maps them back) and never one of the special forms.

// ---- the engine's entry points: symbols 0..63 a module's code defines (the object's module frame binds them; the
// engine looks them up when the event happens, update.cpp HandleLoSpliceEvent)
struct SPLICEEVENTNAME
{
    int symid;
    const char* name;
    const char* args[4];   // the handler's arguments after the object (null: none)
    const char* what;      // (the export's legend)
};
const SPLICEEVENTNAME c_aevent[] = {
    {0, "on-start", {}, "the object starts in the world (after the load)"},
    {1, "on-update", {}, "every update (at most once per the object's update interval: defer-object-update)"},
    {2, "on-begin", {}, "an animation / state machine / AI job starts"},
    {3, "on-end", {}, "an animation / state machine / AI job ends (retracted)"},
    {4, "on-event", {"event"}, "a timed event of an animation, or a command list's event (with its word)"},
    {5, "on-activate", {}, "turned on: entered, touched, pressed, used, picked up, collected, warped"},
    {6, "on-deactivate", {}, "turned off: left, released, done"},
    {7, "on-guard-state", {"added-states", "removed-states"}, "a guard's state flags changed"},
    {8, "on-state-change", {"old-state", "new-state"}, "a state machine went from one state to another"},
    {9, "on-members-change", {}, "a tracking trigger's set of objects inside changed"},
    {10, "on-contact-sent", {"receiver", "filter"}, "this object touched another (the receiver)"},
    {11, "on-contact", {"sender", "filter"}, "another object touched this one"},
    {12, "on-heal", {"health", "power"}, "a player got health / power"},
    {13, "can-heal?", {}, "asked whether a player may pick up health (true: yes)"},
    {14, "on-attach", {"partner"}, "an instance was placed by its proxy (the proxy and the object each get the other)"},
    {15, "on-release", {"partner"}, "an instance was taken back by its proxy"},
    {16, "on-event-16", {}, "(an engine event; its sender is not known yet)"},
    {17, "on-job-event", {"job-event", "about"}, "an event broadcast in the current job (with the object it is about)"},
    {18, "on-refused", {"refused-state"}, "a task refused a state change"},
    {19, "on-task-start", {}, "the task became the current one"},
    {20, "on-task-end", {}, "the task stopped being the current one"},
    {21, "on-task-state", {}, "the task's state changed"},
    {22, "on-task-state-changing", {}, "the task's state is about to change"},
    {23, "on-task-timer", {"timer-kind"}, "the task's time ran out (0) or a script fired it (n)"},
    {24, "can-act?", {"action"}, "asked whether a guard may take an action (true: yes)"},
    {25, "on-spawn", {"spawned"}, "a spawner made a new guard"},
    {26, "on-shop", {}, "the shop was closed (every player)"},
    {27, "on-projectile-end", {}, "a projectile ended"},
    {28, "on-detect", {"detected", "old-detect-state", "new-detect-state"}, "a detector's entry changed state"},
};

// ---- the fixed parameter symbols (every script uses them in the same role)
struct SPLICEFIXEDSYM
{
    int symid;
    const char* name;
};
const SPLICEFIXEDSYM c_afixed[] = {
    {1002, "self"},    // the object the handler runs for (every entry point's first argument)
    {990, "this"},     // the same in the animation / state machine handlers (an ASEGA / SMA: its owner is .owner)
    {991, "event"},    // on-event's event word
    // the two functions the original's scripts call but no script of the game defines (notes/SCRIPTNAMES3_2026-11-18.md):
    // looked up, unbound (nothing), applied: the call does nothing. A mod may define them in the module (a define of
    // the name at its top) to give the call a meaning.
    {1533, "on-aim-setup-failed"},  // the guards' aim managers' on-start, when their guard / aim point / launcher is missing
    {1407, "on-guard-knocked-out"}, // the final job's (3471, Clock-La) on-job-event for %guard-hit / %guard-hit-by-player
};

// ---- option ids (the scripts' (<geto> obj N), (<seto!> obj N v), (<addo> ...), (<ensureo> ...)); ids above 1232
// are the scripts' own (the object's sidebag): those get names from what the scripts store in them
struct SPLICEOPTNAME
{
    int optid;
    const char* name;
};
const SPLICEOPTNAME c_aopt[] = {
    // every object (LO)
    {3, "is-class?"},             // method (cid): of that class (0x132B10)
    {12, "fire-activate"},        // method: the object's event 5 (on-activate) and the tasks' broadcast
    {14, "activation-radius"},
    {15, "activation-kind"},
    {28, "oid"},
    {29, "instance-oid"},         // the last oid of the instance chain
    {30, "is-oid?"},              // method (oid): this object or its instance has the oid
    {31, "proxy"},                // the proxy that placed this instance
    {32, "parent"},
    {33, "set-parent"},           // method (parent, keep flags)
    {34, "position"},             // world position (vector)
    {35, "rotation"},             // world rotation (matrix)
    {36, "rotation-degrees"},     // Euler angles in degrees
    {37, "move-to"},              // method (object): placed at the object
    {39, "add"},                  // method: put into the world (shown, running)
    {40, "remove"},               // method: taken out of the world (hidden, stopped)
    {41, "in-world?"},
    {63, "velocity"},
    {64, "angular-velocity"},
    {67, "mask-own"},
    // models (ALO)
    {69, "lock-position"},        // linear constraint (0x1F5720)
    {70, "lock-rotation"},        // angular constraint (0x1F57C0)
    {90, "attached"},             // its attachment (0x1F6838)
    {91, "physics-root"},
    {110, "show-meshes"},         // method (id): the mesh groups with the id shown
    {111, "hide-meshes"},         // method (id): hidden
    {112, "collision-on"},        // method (tag): the geometry entries with the tag on
    {113, "collision-off"},       // method (tag): off
    {125, "set-contacts"},        // method (filter bits, on): the contacts of those filter bits
    {127, "fade-out"},            // method (seconds)
    {128, "fade-in"},             // method (seconds)
    {129, "fade"},                // the fade's value (0: removed)
    {166, "shader-speed"},        // method: the shader animations' speeds
    {178, "ignore-locked"},       // drawn over the HUD / not locked (SetSoIgnoreLocked)
    {182, "animation"},           // method (aseg oid): the object's running animation of that clip (ASEGA)
    {183, "state-machine"},       // method (sm oid, -1 any): the object's state machine runner (SMA)
    {187, "local-velocity"},
    {188, "local-angular-velocity"},
    {195, "find-tagged"},         // method (tag): the first entry with the tag
    // animations (ASEG / ASEGA), state machines (SM / SMA)
    {214, "apply-to"},            // method (object, ...): an ASEG / SM run on an object; a noise template made at a point
    {217, "clip-speed"},          // the ASEG's speed
    {222, "clip-length"},
    {233, "add-item"},            // method (n): a breakable's item / a cutscene's command / a camera jolt
    {235, "trigger"},             // method: a warp / exit / use / pad / cutscene / vault / dock fired (a use: by an object)
    {236, "seek"},                // method: an animation's time set (kind, time, speed); a breakable's hit kind; a command
    {237, "retract"},             // method: an animation / state machine stopped
    {244, "burst"},               // method: an explosion / burst
    {248, "snap"},                // method (f): the animation's transforms at the clip's values now
    {250, "clip"},                // the ASEGA's ASEG
    {251, "owner"},               // the ASEGA's / SMA's object
    {252, "clip-time"},
    {253, "speed"},               // the ASEGA's speed
    {255, "retracted?"},          // the animation retracted / the noise over
    // sounds, music, dialogue (the sound object: find-world-object, and the cutscenes)
    {264, "play-dialog"},         // method (id, ...): a dialogue line (its sound object)
    {265, "stop-dialog"},
    {266, "dialog-playing?"},     // method (id)
    {267, "dialog-length"},       // method (id)
    {273, "push-music"},          // method (song): music pushed (StartMusic)
    {274, "pop-music"},           // method (song) (StopMusic)
    {277, "channel-volume"},      // method (channel, volume)
    // the guards and their AI
    {292, "add-command"},         // method: a command list's / cutscene's command
    {342, "state"},               // the zone's / actor's / vehicle's / tango's / task's state
    {350, "scale"},
    {353, "reach"},               // method (a guard's / actor's reach)
    {357, "immune-to"},           // damage kinds the actor ignores
    {358, "immune-change"},
    {359, "add-action"},          // method (oid): a guard's action object
    {360, "clear-actions"},       // method: the guard's actions emptied
    {369, "state-segment"},       // method (state, oid, flags): what a state plays (the guards' kind table, hack enemies, birds)
    {370, "kind-entry"},          // method: a guard's kind entry
    {371, "kind-table"},          // method: a guard's kind table entry
    {380, "max-health"},
    {381, "health"},              // hit points left
    {386, "chase-target"},        // the object a guard pursues
    {399, "patrol"},              // a guard's patrol (command list)
    {405, "in-state?"},           // method (state): the state is in the guard's mask
    {407, "hears-noises"},         // +0xE64: free to hear noises (0x208D70)
    {415, "can-fall"},             // +0x4A0: the off-the-ground watch on (0x20D020; scripts turn it off)
    {425, "nav-link"},
    {435, "pocket"},              // the guard's pocket (pickpocket loot)
    {437, "sees-always"},         // detector flags (the guard's detector)
    {438, "sees-never"},
    {441, "start-commands"},      // method: the guard's authored command list started
    {442, "stop-commands"},       // method: the reactions told to stop their command list
    {446, "station"},             // method (object, offset): the guard's / actor's station
    {454, "reset"},               // method: the full reset (a guard, a hack enemy, the world)
    // triggers, pads, uses, exits
    {478, "goal-state"},          // the state machine's goal state (a state's oid)
    {481, "current-state"},       // the state machine's current state
    {493, "destroy"},             // method: a projectile ends now / a hack enemy dies
    {498, "count"},               // a projectile's / launcher's count
    {503, "target"},              // the aim target (camera, launcher, projectile)
    {504, "target-offset"},       // the aim point in the target's frame
    {508, "fire-at"},             // method (object, point): shot at (a missile, a launcher, a hack piece)
    {519, "add-object"},          // a trigger's / use's / pad's objects (one appended)
    {528, "trigger-state"},       // a trigger's / pad's state
    {530, "untrigger"},           // method: a trigger / pad left, a cutscene stopped
    {533, "objects-inside"},      // a trigger's objects inside (a list)
    {534, "last-object"},         // the first object inside / the use's user / the barrier's last object
    {560, "emitter-start"},       // method
    {561, "emitter-stop"},        // method
    {562, "emitter-paused?"},
    {616, "burst-with"},          // method (params): a burst with parameters
    // cutscenes / scripts (class SCPT)
    {638, "start"},               // method: a cutscene / scripted sequence started (with the player)
    {640, "add-alarm"},           // method (n)
    {641, "remove-alarm"},        // method (n)
    {642, "has-alarm?"},          // method (n)
    {643, "script-object"},
    // the players (actors)
    {710, "carried"},
    {734, "jump-state"},          // method (kind): the state of a jump kind
    {735, "booty"},               // the actor's booty (a loot record)
    {736, "give-booty"},          // method (n)
    {741, "in-water"},            // e.g. Ep3's water buckets on entry / exit
    {742, "busy?"},
    {752, "cane-reach"},          // method (object): Sly's cane reach toward an object
    {761, "checkpoint"},          // an exit's live checkpoint (oid)
    {771, "exit-states"},
    {795, "radius"},               // a warp's +0xE8 / a noise template's +0x54 radius / a mesh entry's
    {806, "control-enabled"},     // the player's controller on
    {810, "controller"},          // the player's controller state (buttons / sticks)
    {827, "booty-record"},
    {831, "record"},              // a pickup's / booty's current record
    // the world (find-player-object is the world: its options)
    {844, "player"},              // the world's current player
    {845, "player-or-forced"},
    {846, "forced-player"},
    {855, "respawn-point"},       // the checkpoint point a death outside a job reloads
    {856, "respawn-key"},
    {859, "paused?"},
    {861, "show-abandon-icon"},   // method (object): the abandon icon on a tracked target (the safe house door in a job)
    {900, "stick-x"},
    {901, "stick-y"},
    {908, "button-pressed?"},     // method (mask): pressed this frame (taken)
    {909, "button-down?"},        // method (mask)
    // breakables
    {929, "hit-by"},              // who hit it last
    {935, "hit-flags"},
    {936, "hit"},                 // method: a hit from a script
    // screens / HUD objects, the HUD prompt (bif 137)
    {979, "show"},                // method: a screen object shown / a HUD prompt (id, seconds)
    {980, "hide"},                // method: hidden / the prompt ended
    {981, "select-part"},         // method (n)
    {982, "set-fraction"},        // method (fraction, part)
    {983, "set-time"},            // method (time, part)
    {985, "text-arg-0"},           // a screen object's string argument 0 (+0xCC; screenrt.cpp)
    {986, "text-arg-1"},           // its string argument 1 (+0xD0)
    {1014, "lock"},               // the booty stand's lock
    // proxies (the instances a proxy places)
    {1019, "instantiate"},        // method: an instance placed; its object
    {1020, "instance"},           // the latest placed instance's object
    {1021, "instance-free"},      // method
    {1023, "keep-instance"},      // method (n): the row kept
    {1024, "release-instance"},   // method (n)
    {1045, "rope-length"},
    {1046, "rope-mode"},
    // jobs and tasks (MISSION / TASK)
    {1062, "aim-object"},           // method (mode, priority, object, ...): a transform handle aimed at an object
    {1067, "broadcast-job-event-1d"},  // method: the job broadcasts its event 0x1D (0x162DD0)
    {1068, "restart-at-checkpoint"},  // method: the job restarted at its checkpoint
    {1069, "end-job"},            // method: the job ended
    {1078, "times-played"},
    {1079, "reset-job"},          // method
    {1080, "done?"},              // a task done / a job's last task done
    {1081, "current?"},           // the current task / job
    {1082, "progress"},
    {1083, "task-object"},
    {1093, "job"},                // a task's job
    {1104, "request-state"},      // method (state, by): a task asked to change state (0 unavailable, 1 available, 2 done)
    {1105, "force-state"},        // method (state, by)
    {1106, "fire-timer"},         // method (n): the task's on-task-timer
    {1111, "force-available"},      // method: the task forced to available (1) and the job chosen again
    {1131, "marker-task"},
    {1154, "allow-states"},       // method: the states the select may pick
    {1169, "dance-score"},
    {1171, "actor"},              // an AI job's actor
    // (more, from the methods' thunks)
    {13, "fire-deactivate"},      // method: the object's event 6 (on-deactivate)
    {114, "contact-kind"},        // method (kind): the contact filter of the object's entries
    {194, "record-of-kind"},      // method (k, kind)
    {269, "alert-music-on"},      // method: the music's alert mode (counted)
    {270, "alert-music-off"},
    {271, "sneak-music-on"},
    {272, "sneak-music-off"},
    {275, "music-time"},
    {276, "master-volume"},       // method (volume)
    {392, "detector-targets"},    // method
    {443, "runs-commands?"},      // method (list): the reactions run that command list
    {458, "detector"},            // method: the guard's detector
    {492, "end-flight"},          // method (seconds): a projectile's flight ends within the time
    {720, "launch-to"},           // method (point, ...): the actor launched to a point
    {738, "put-back-booty"},      // method
    {939, "toss-bentley"},        // method: Murray tosses Bentley
    {964, "blast"},               // method
    {1060, "aim-point"},          // method (mode, priority, a world point)
    {1061, "aim-rotation"},       // method (mode, priority, a world rotation)
    {1173, "chase-graph"},        // method
    // (round 2: the engine's other options by their descriptors (0x2AC880) and the port's fields)
    {987, "text-arg-2"}, {988, "text-arg-3"}, {989, "text-arg-4"}, {990, "text-arg-5"},
    {71, "gravity"},              // the physics state's gravity (0x1F5860 / 0x1F5880)
    {108, "contains-point?"},     // method (point): inside the shape (0x227C90)
    {119, "sub-object"},          // an ALO's 0x12 child made on demand (0x1F7B98)
    {122, "entry-mode"},          // a mesh entry handle's (find-tagged) mode
    {123, "entry-value"},         // a mesh entry handle's value; a hack enemy's points at its death
    {130, "fade-rate"},           // the fade's speed (a nonzero one adds the object back: 0x1F7D90)
    {197, "physics-radius"},      // the physics region entry's radius (0x1F83B0)
    {198, "mass"},                // its mass (0x1F8410)
    {200, "nav-region"},          // the navigation region the object is in (0x259B20)
    {216, "apply-at"},            // method (object, time kind, t, speed, ...): an ASEG ensured on an object
    {231, "add-command-36"},      // method: a cutscene command 0x36 (four ints, three floats)
    {260, "sound-source"},        // the sound object's source object (0x1FB2F8)
    {262, "play-range"},          // method (near, far): the sound played over a distance range
    {283, "flag-158"},            // a cid 0x04 object's flag +0x158 (the safe houses' scripts toggle it)
    {339, "look-at"},             // a camera zone's look-at object (+0x190)
    {351, "size"},                // a player's / guard's size (0x204DA8)
    {355, "heading"},             // a player's / guard's heading (0x259D18)
    {368, "attack-enabled"},      // a guard's default attack (+0xA8)
    {383, "count-340"},           // a guard's +0x340
    {384, "count-344"},           // a guard's +0x344 (scripts copy 383 into it)
    {387, "add-target"},          // method (object): an object the guard watches (0x2085B0)
    {388, "add-target-class"},    // method (cid): a class the guard targets (0x208700)
    {390, "clear-target-classes"},  // method (0x103630)
    {393, "exclude-class"},       // method (cid): a class left out when choosing a target (0x208920)
    {395, "clear-excluded-classes"},  // method (0x2089E8)
    {396, "add-immediate-class"}, // method (cid): an immediate target class (0x208A68)
    {398, "clear-immediate-classes"},  // method (0x208B30)
    {404, "states"},              // a guard's state mask as a list (0x209A88)
    {408, "alarm-template"},      // a guard's alarm template (+0xE60; none: the world's)
    {410, "low-priority-target"}, // an actor ranked last by the guards (+0x10F0); a guard's +0x364
    {416, "hear-range"},          // a guard's +0x314 (the kind table's state 9 / 10 reads it)
    {417, "hear-range-far"},      // a guard's +0x318
    {426, "set-object-lists"},    // method (list, list): the guard's two object lists (guardlists.cpp)
    {431, "more-prizes"},         // a pickpocket giver's +0x490: more prizes allowed
    {439, "view-volume"},         // the detector's view volume (+0xE8)
    {440, "view-volume-2"},       // the detector's second view volume (+0xEC)
    {444, "flag-f30"},            // a guard's +0xF30
    {445, "flag-f34"},            // a guard's +0xF34
    {447, "lowest-noise"},        // a guard's +0xE68: the lowest noise it hears
    {451, "noise-template"},      // a guard's +0xE74
    {467, "shooter-target"},      // a spotlight shooter's (cid 0x10) object (+0xE0)
    {486, "contact-radius"},      // a projectile's radius against objects (+0x15C)
    {487, "player-contact-radius"},  // against a player (+0x160)
    {497, "death-burst"},         // a hack enemy's / projectile's explosion at its end (+0x190 / +0x168)
    {500, "launched-by"},         // a projectile's launcher (+0x200)
    {501, "lands-on-contact?"},   // a carryable lands on what it touches (0x224638); a projectile's +0x1E4
    {509, "launch-speed"},        // a projectile's speed (+0x260)
    {512, "target-speed"},        // a homing projectile's speed (+0x270)
    {513, "fast-speed"},          // its speed limit (+0x274)
    {521, "rearm-time"},          // a trigger's / pad's re-arm time (+0x1C8 / +0x1E8)
    {529, "toucher"},             // a pad's toucher (+0x2F8)
    {545, "spin"},                // a turret's (cid 0x70) rotation speed (clq, +0x70)
    {554, "emitter-data"},        // an emitter's data (0x159A08)
    {622, "add-near-command"},    // method: a command list's command 0 (the near object) / a cutscene's command
    {625, "add-range-command"},   // method: a command list's range command / a cutscene's
    {628, "add-look-command"},    // method: a command list's look command / a cutscene's
    {639, "running?"},            // a cutscene runs (0x1DFFD8)
    {646, "clear-commands"},      // method: the cutscene's commands cleared (0x1DFFE0)
    {647, "event-context"},       // the cutscene's event runner's lookup context (0x1E0128)
    {670, "hold"},                // method (object): an actor holds the object (none: lets go; 0x1421A0)
    {718, "power-max"},           // an actor's +0x1300
    {719, "power"},               // an actor's power (0x1441A0)
    {726, "held-object"},         // an actor's object in reach (+0x9A0)
    {747, "actor-flag"},          // an actor's (0x1443F8)
    {750, "give-ability"},        // method (slot, ability): an ability put in a slot (0x141FC8)
    {751, "ability-slot"},        // method (ability): the slot holding it, -1 none (0x141E78)
    {756, "pocket-object"},       // Sly's +0x1BCC (the pickpocketed object)
    {760, "triangle-summon"},     // Bentley's ability triangle summons (+0x1870)
    {778, "path-speed"},          // a path's float +0x08 / a cid 0x09's +0x108
    {779, "path-mode"},           // a path's +0x10
    {782, "path-kind"},           // a path's +0x1C
    {805, "status"},              // an actor's / guard's status (vt slot 77: 2 knocked out ...)
    {811, "forced-buttons"},      // a player's forced buttons (+0x154)
    {819, "pickup-state"},        // a pickup's state (+0x150)
    {862, "world-value-4768"},    // the world's +0x4768
    {863, "world-value-4764"},    // the world's +0x4764
    {886, "value-150"}, {887, "value-154"},
    {898, "value-60"},
    {917, "value-18c"}, {918, "value-190"},
    {930, "last-contact-filter"}, // a breakable's last contact filter (+0x170)
    {942, "home-position"},       // a cid 0x2C object's point (+0xF0)
    {950, "user"},                // a use's (cid 0x24) user (+0x180)
    {955, "range"},               // a noise template's height range (lm, +0x58)
    {960, "carry-kind"},          // a carryable's kind (+0x15C)
    {961, "dart-like?"},          // a carryable of kind 7 / 11 (0x224610)
    {962, "carry-contact-filter"},  // a carryable's contact filter (+0x174)
    {970, "commands-near-sly"},   // method (command list): what a cid 0x3C guard runs near Sly (0x1BA980)
    {974, "near-distance"},       // a cid 0x3C guard's +0x1088
    {1002, "flag-114"},           // a cid 0x7F screen's +0x114
    {1005, "booty-burst"},        // a timed booty's explosion when it is taken (+0x164)
    {1006, "booty-scale"},        // a booty's carried scale (+0x170)
    {1012, "booty-carrier"},      // a booty's carrier (+0x160)
    {1017, "instance-free?"},     // method: one of the proxy's instances is free (0x1CB2C0)
    {1025, "instances"},          // a proxy's attached instances (a list, latest first)
    {1034, "value-f4"}, {1035, "value-f8"},
    {1042, "rope-start"},         // a rope's first end's object (0x1E7930)
    {1043, "rope-end"},           // its last end's object (0x1E7968)
    {1044, "rope-rest-length"},   // the rope's length (0x1E79A0 / 0x1E79F0)
    {1056, "rope-sim"},           // the rope's simulation (+0x88)
    {1059, "handle-value"},       // a transform handle's +0xA8
    {1156, "missile-aim"},        // method (pos): where the boss's missile aims (0x155FB0)
    {1182, "state-distance-rule"},  // method (state, near, far): a hack enemy state's distance rule
    {1183, "state-angle-rule"},
    {1212, "vehicle-value"},
    {1217, "vehicle-method-1217"},
    {1219, "vehicle-value-of"},   // method (n)
    {1220, "vehicle-set"},        // method (n, value)
    {1224, "pair-camera"},        // method (on): Rajan's two-actor camera (0x1CBF90)
    {1225, "combination"},        // a vault's combination (+0x1F0)
    {1226, "vault-line"},         // a vault's line (+0x1F4)
    {1228, "vault-reward"},       // a vault's reward (+0x200, a power-up)
    {1229, "vault-sly-line"},     // Sly's line at the vault (+0x1FC)
    // round 3 (notes/SCRIPTNAMES3_2026-11-18.md): the last engine ids the scripts use, from their descriptors (classes,
    // field, getter / setter / thunk) and the port's code; a leading "?" marks a name that is a guess from the field
    // and how the scripts use it (its meaning not traced in the code)
    {141, "unsafe-ground"},       // an ALO's flag bit 29 (0x1F7F20; the floor's +0x148 bit 29: not safe to stand on)
    {147, "?range-low"},          // a cid 0x8C object's lm +0x50
    {148, "?range-high"},         // its lm +0x58
    {167, "set-shader-offsets"},  // method (u, v): the object's shader animations' offsets (0x1012C0 -> 0x1F8038)
    {174, "?child-of-oid"},       // method (oid): an object of its own (0x101368)
    {176, "?alo-value-a0"},       // an ALO's int +0xA0
    {179, "contact-entry-mode"},  // an ALO's contact-entry mode (SetAloEntryMode 0x1F7A68)
    {220, "?volume-kind"},        // a volume's / zone's / clip's kind (+0x6C / +0xE4 / +0x128; the scripts set 2 / 3)
    {268, "dialog-seconds"},      // method (dialog): the line's length in seconds (0x1021D0 -> 0x1FCFD0)
    {302, "camera-mrd-ratio"},    // the camera's MRD ratio (SetCmMrdRatio 0x14F730)
    {310, "?camera-zone-curve"},  // a camera zone's current entry's curve (+0x10 through 0x221958)
    {333, "?zone-value-170"},     // a zone's int +0x170
    {334, "?zone-value-174"},     // a zone's int +0x174
    {374, "set-reaction"},        // method (kind, a, b): a guard's reaction array entry (thunk 0x1033E0 -> 0x208018)
    {418, "sleep-meter-full"},    // a guard's +0xE84: what the sleep meter refills to (aihit.cpp)
    {419, "sleep-meter"},         // a guard's +0xE88: lowered by sleep darts, asleep at 0
    {436, "detector-flags"},      // a guard's detector's flags (+0xF8 of its detector, PMF 0x206378)
    {459, "?relation-to"},        // method (actor) of a guard / spotlight (0x104108 / 0x1042D0): the scripts compare it with 1
    {464, "?holding-target"},     // a guard's +0x3D4 (cleared when the target is lost: aipursuit.cpp 0x176060)
    {514, "?launch-vector"},      // a cid 0x52 thrown object's +0x280
    {573, "?emit-direction"},     // an emitter's / explosion's vector +0x40 (+0x10)
    {580, "?emit-offset"},        // an emitter's vector +0x60
    {667, "interact-with"},       // method (object): the player's interaction of kind 5 with it, nothing ends it (0x10A848)
    {711, "?player-value-424"},   // a player's int +0x424
    {740, "?interaction-state"},  // a player's +0x10E4 (its interaction sub-object's; the scripts test 0 / 1)
    {744, "?player-flag-12f0"},   // a player's flag +0x12F0 (1 by default)
    {757, "dart-kind"},           // Bentley's dart kind (+0x16D8: 1 Ep3's red noise darts ...)
    {753, "request-hypnosis"},    // method (on): Sly's hypnosis request (+0x1D30; 0x10B598 -> 0x18DF50: the Contessa's eye cones)
    {759, "log-idle"},            // Bentley's log idle (+0x181C, Ep7's log rolling)
    {787, "?attached-object"},    // a cid 0x58 object's object +0x190
    {853, "?world-curve-45d0"},   // the world's clq +0x45D0
    {854, "?world-range-45e0"},   // the world's lm +0x45E0
    {902, "?stick2-x"},           // a controller's (cid 0x89) stick field +0x18 (900 / 901: +0x10 / +0x14)
    {903, "?stick2-y"},           // +0x1C
    {905, "?stick-value-24"},     // +0x24
    {916, "?turret-object"},      // a turret's (cid 0x45) object +0x1C8
    {937, "take-object"},         // method (object): Murray takes it (0x10D260)
    {938, "throw-object"},        // method (object): Murray throws (0x10D2E8)
    {940, "set-rage"},            // method (on): Murray's rage (0x10D3E8 -> 0x1B9CA0)
    {941, "thunder-flop-entrance"},  // method: Murray's Thunder Flop entrance (0x10D468 -> 0x1B9D00)
    {959, "noise-source"},        // a noise's (cid 0x93) place it is heard from (+0x30; the dart's apply-to makes one)
    {1022, "keep-frame?"},        // a proxy's +0xE4: streaming binds keep the object's own scripts frame when 0
    {1040, "?display-on"},        // a recon subject's (cid 0x2E) flag +0x10C (the code displays)
    {1172, "?state-value"},       // (no class has it: the scripts pass it to .request-state)
    {1197, "?attachment-count"},  // a vehicle attachment's (cid 0x99) +0x24
    {1198, "?attachment-limit"},  // +0x28
    {1200, "?fire-rate"},         // +0x50 (clq)
    {1201, "?spread"},            // +0x60 (lm)
    {1202, "?launch-range"},      // +0x48 (lm)
    {1203, "projectile-gravity"}, // +0x68: the scripts give it to their projectiles' .gravity
    {1205, "?attachment-mode"},   // +0x80
    {1206, "?attachment-value"},  // +0x7C
    {1207, "attachment-shape"},   // +0x84 (the robot tank's shape)
    {1208, "?aim-range"},         // +0x88 (lm)
    {1209, "?distance-range"},    // +0x90 (lm)
    {1213, "?ammo"},              // +0x70
    {1214, "fire"},               // method: the attachment fires (0x110478)
    {1227, "dialog-bottles"},     // a vault's bottles line (+0x1F8)
};

// ---- built-ins: readable names for the game's (97..168) where the converter has none or a misleading one
struct SPLICEBIFALIAS
{
    int bifk;
    const char* alias;
};
const SPLICEBIFALIAS c_abif[] = {
    {97, "world-index"},          // the current world's index
    {98, "world-kind"},
    {99, "episode"},
    {100, "current-job"},         // (converter: object-3E0F9C)
    {101, "current-task"},        // (object-3E0FA0)
    {102, "checkpoint-task"},     // (object-3E0FA4)
    {103, "suck"},                // the game's dynamic difficulty, 0 .. 1 (the current job's suck value: 0x123B60 -> 0x154C90)
    {104, "zero"},
    {106, "cancel-callback"},
    {113, "find-objects-named"},  // (find-object-113)
    {116, "find-objects-under"},
    {117, "world"},               // (find-player-object: the world object; its .player is the player)
    {118, "sound"},               // (find-world-object: the sound object: dialogue / music)
    {119, "camera"},              // (find-camera-object)
    {129, "new-sound"},
    {130, "rumble"},              // (joy-rumble: kind, seconds)
    {131, "get-progress"},        // (a path of symbols)
    {132, "set-progress"},
    {133, "inherited"},           // (a symbol's binding in the group's module: e.g. the group's own on-start)
    {134, "void"},
    {135, "bits-and"},
    {136, "bits-or"},
    {137, "hud-prompt"},          // the HUD prompt object (.show / .hide)
    {138, "item-acquired?"},      // (ThiefNet item n bought)
    {139, "set-item-acquired"},
};

// ---- option arguments that are oids (a method's argument index, or a seto!'s value: -1)
struct SPLICEOIDARG
{
    int optid;
    int iarg;
};
const SPLICEOIDARG c_aoidarg[] = {
    {182, 0}, {183, 0}, {478, -1}, {761, -1}, {359, 0}, {30, 0},
};

// ---- the jobs' broadcast events (task.cpp BroadcastTaskEvent, 0x1619A0): what on-job-event's job-event is; shown as
// %name in its case clauses
struct SPLICEJOBEVENT
{
    int n;
    const char* name;
};
const SPLICEJOBEVENT c_ajobevent[] = {
    {1, "world-end"},
    {4, "world-start"},
    {5, "world-loaded"},
    {9, "photo-9"}, {10, "photo-taken"}, {11, "photo-11"},
    {14, "pad-14"},
    {15, "player-hit"},
    {16, "player-down"},          // the eyes shut: the job restarts at its checkpoint
    {18, "pickup-collected"},
    {19, "detected"},
    {20, "guard-state-3"},
    {21, "guard-hit-kind"},
    {22, "guard-state-12"},
    {23, "guard-hit"},
    {24, "guard-hit-by-player"},
    {26, "object-activated"},     // an object's on-activate (entered, touched, pressed, used, picked up ...)
    {27, "object-deactivated"},
    {28, "job-begin"},
    {29, "job-event-1d"},
    {30, "checkpoint-restart"},
    {31, "job-restart"},
    {32, "job-complete"},
    {33, "job-activated"},
    {34, "job-deactivated"},
    {35, "task-activated"},
    {36, "task-deactivated"},
    {37, "task-state-changed"},
    {38, "task-state-changing"},
    {39, "task-refused"},
    {51, "alarm"},
    {59, "item-bought"},
};

// ---- what an engine method's arguments are (a symbol or a script-made option passed there takes the name)
struct SPLICEMETHODARG
{
    int optid;
    int iarg;
    const char* name;
};
const SPLICEMETHODARG c_amethodarg[] = {
    {264, 0, "dialog"}, {266, 0, "dialog"}, {267, 0, "dialog"},
    {979, 0, "text"}, {979, 1, "seconds"},
    {369, 0, "states"}, {369, 1, "clip"}, {369, 2, "flags"},
    {359, 0, "action"},
    {1104, 0, "state"}, {1104, 1, "by"}, {1105, 0, "state"}, {1105, 1, "by"},
    {750, 0, "slot"}, {750, 1, "ability"}, {751, 0, "ability"},
    {508, 0, "target"}, {214, 0, "target"}, {216, 0, "target"},
    {670, 0, "held"}, {387, 0, "watched"}, {388, 0, "target-class"}, {393, 0, "class"}, {396, 0, "target-class"},
    {182, 0, "clip"}, {183, 0, "state-machine"},
    {273, 0, "song"}, {274, 0, "song"},
    {110, 0, "mesh-id"}, {111, 0, "mesh-id"}, {112, 0, "tag"}, {113, 0, "tag"}, {195, 0, "tag"},
    {127, 0, "seconds"}, {128, 0, "seconds"},
    {37, 0, "place"}, {33, 0, "new-parent"}, {3, 0, "class"}, {30, 0, "oid"},
    {405, 0, "state"}, {1023, 0, "instance"}, {1024, 0, "instance"},
    {446, 0, "station"}, {736, 0, "amount"}, {1106, 0, "timer"},
    {908, 0, "buttons"}, {909, 0, "buttons"}, {734, 0, "jump-kind"},
    {108, 0, "point"}, {720, 0, "point"}, {1060, 2, "point"}, {1156, 0, "from"},
};

// ---- class ids (round 3): find-objects' first argument (bif 120: the objects of a class) shown as &name. Known names
// from the port's classes (cls.cpp); a leading "?" marks a guess from what the scripts do with them.
struct SPLICECLASSNAME
{
    int cid;
    const char* name;
};
const SPLICECLASSNAME c_acls[] = {
    {0x04, "?marker"},            // (a LOX kind: the safe houses' flagged markers)
    {0x06, "proxy"},
    {0x07, "?proxy-kind-7"},      // (a proxy's kind: the night club counts those in the world)
    {0x0B, "?path-kind-b"},       // (a PATH kind)
    {0x1D, "emitter"},
    {0x27, "?lox-kind-27"},       // (a LOX kind the temple's scripts add)
    {0x30, "object"},             // ALO: every animated / drawn object
    {0x31, "?static-object"},     // SO: the world's own geometry and its likes
    {0x33, "step-object"},        // PO / STEP: the base of the actors and the guards
    {0x38, "guard"},              // STEPGUARD: every guard
    {0x39, "?swarmer"},           // (the mini swarmers' choppers; the episodes' macros remove them all)
    {0x3A, "?coin-guard"},        // (drops coins in a range)
    {0x3F, "?boss-guard"},        // (Carmelita's script looks it up)
    {0x42, "?animal-guard"},      // (the goats', geese's and moose's scripts)
    {0x47, "?hack-object"},       // (cid 0x47 / 0x4A: the hacking games)
    {0x50, "?flyer"},             // (a looping flight clip)
    {0x58, "jump-target"},        // (the jump's own targets: 0x197F98)
    {0x64, "mine"},               // Bentley's mines (0x65), the guard trap (0x66)
    {0x6D, "animation"},          // ASEG
    {0x6F, "state-machine"},      // SM
    {0x77, "spawner"},
    {0x84, "mission"},
    {0x85, "task"},
};
