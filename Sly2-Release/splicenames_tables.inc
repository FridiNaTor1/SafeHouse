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
    {407, "guard-flag-407"},
    {415, "guard-flag-415"},
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
    {795, "field-795"},
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
    {985, "screen-field-985"},
    {986, "screen-field-986"},
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
    {1062, "method-1062"},
    {1067, "job-event-1d"},       // method: the job's event 0x1D
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
    {1111, "task-method-1111"},
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
    {103, "bif-103"},
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
