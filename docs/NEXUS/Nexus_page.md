# Behavior Switchboard — Runtime Behavior Injection Framework for Skyrim

**Add entirely new animation state machines to Skyrim at runtime — no Nemesis, no Pandora, no file conflicts.**

---

## Overview

Behavior Switchboard (BSB) is a modder's framework that lets SKSE plugins inject custom behavior sub-graphs directly into Skyrim's Havok behavior system at runtime. Instead of patching vanilla HKX files at deploy time (Nemesis/Pandora style), BSB hooks into the behavior graph activation pipeline and silently stitches your state machine in before Havok builds its internal index maps.

For players, this means you can have multiple complex animation mods installed simultaneously without patch stacking, patch order conflicts, or regeneration headaches.

For mod authors, it means you ship a `.hkx` sub-behavior file and call one API function — Havok handles everything else.

---

## Features

### Non-Destructive Behavior Injection
- Injects sub-behavior state machines into the master behavior graph **without modifying any files on disk**
- Works alongside Nemesis and Pandora — BSB-registered mods need no patches
- Multiple mods can coexist without event or variable name conflicts (BSB validates uniqueness at registration)
- Re-injects automatically after save/load — no stale state between sessions

### Simple Registration API
- Register via **C++ API** (`BSB_API.h`) from your own SKSE plugin
- Or drop a **YAML config file** into `Data/SKSE/Plugins/BehaviorSwitchboard/` — no SKSE plugin required
- Register animation paths, behavior-graph variables, and event names in a single call
- Works for humanoid characters (`0_Master.hkb`) and creature graphs

### Custom Physics State Multiplexer
- Havok's character state manager has only 5 spare user slots — BSB makes them effectively unlimited
- Reserve a single physical Havok slot and multiplex any number of mod-owned logical states through it
- Priority arbitration: a higher-priority state blocks lower-priority ones; lower-priority state is saved and auto-restored on deactivation
- Lock-free per-actor dispatch — game thread writes, physics thread reads, no contention

### Action Gate API
- Suppress specific vanilla action transitions (weapon draw/sheathe, equip, combat actions) while your custom state is active
- Per-mod gate declarations: activate/deactivate your entire gate set in one call
- Automatically injects the required behavior graph variables — no static `graphdata` declaration needed in your ESP

### Logical Camera State API
- BSB owns one custom `TESCameraState` and multiplexes camera control across any number of registered mods
- Same priority model as physics states — Cutscene Framework gets unconditional priority, other mods stack below it
- Automatic vanilla third-person restore on save/load and menu open
- Safe cross-thread design: game-thread activations, render-thread camera callbacks

---

## For Mod Authors

### What You Ship
1. A compiled behavior sub-behavior `.hkx` file (built with [HKBuild](https://github.com/Cassieandstuff/Cutscene-Framework), xEdit, or any Havok tool)
2. Optionally: a YAML config file if you don't need a full SKSE plugin

### What You Don't Need
- No Nemesis patch
- No Pandora patch
- No changes to vanilla HKX files
- No graph XML editing
- No patch order management

### C++ API (10-second version)
```cpp
#include <BehaviorSwitchboard/BSB_API.h>

// During kPostLoad:
BehaviorSwitchboard::Registration reg;
reg.modName      = "MyMod";
reg.behaviorPath = "Behaviors\\MyMod_Combat.hkx";
reg.eventName    = "MyMod_EnterCombat";
reg.graphName    = "0_Master.hkb";
reg.animations   = { "Animations\\MyMod\\attack01.hkx" };
BehaviorSwitchboard::Register(reg);

// At runtime:
actor->NotifyAnimationGraph("MyMod_EnterCombat");
// Havok loads your sub-behavior on demand.
```

### YAML Config (no SKSE plugin needed)
Drop a file into `Data/SKSE/Plugins/BehaviorSwitchboard/mymod.yml`:
```yaml
modName:   MyMod
behavior:  meshes\actors\character\behaviors\mymod_combat.hkx
event:     MyMod_EnterCombat
graphName: 0_Master.hkb

animations:
  - meshes\actors\character\animations\mymod\attack01.hkx

variables:
  - name:  MyMod_IsActive
    type:  Bool
    value: 0
```

---

## Requirements

- Skyrim Special Edition or Anniversary Edition
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

---

## Installation

**With a mod manager (Mod Organizer 2 / Vortex):**
1. Download and install the main file
2. Enable the mod
3. Launch via SKSE

**Manual:**
1. Copy `BehaviorSwitchboard.dll` to `Data/SKSE/Plugins/`
2. Launch via SKSE

---

## How It Works (Technical Summary)

BSB hooks `hkbBehaviorGraph::Activate` (vtable index 04) and fires **before** Havok builds its internal `stateIDToIndexMap`, `eventIDMap`, and `variableIDMap`. At that moment BSB:

1. Appends your event names and variables to the shared graph string/data tables
2. Allocates an `hkbBehaviorReferenceGenerator` pointing at your `.hkx`
3. Wraps it in an `hkbStateMachineStateInfo` and adds a wildcard transition on the root SM
4. Bumps the graph's `nextUniqueID`

Then original `Activate` runs — Havok builds all index maps with your data already present. Your sub-behavior is fully addressable with no post-hoc patching.

Wildcard transitions mean your event triggers from **any state in the root SM** — full generality for enter transitions. Exit logic lives inside your own sub-behavior.

---

## Compatibility

- **Nemesis / Pandora:** Fully compatible — BSB doesn't touch any files patched by behavior patchers
- **Other SKSE plugins:** Each mod gets its own uniquely named event and variables; BSB validates uniqueness at registration and logs conflicts
- **Multiple BSB mods:** Unlimited registrations per graph

---

## Known Mods Using Behavior Switchboard

- **True Flight** — Physics-based player flight with custom Havok physics state
- **Cutscene Framework** — Scene graph cutscene engine (uses logical camera and action gate APIs)

---

## Source Code

Behavior Switchboard is open source and part of the Cutscene Framework project:
[GitHub Repository](https://github.com/Cassieandstuff/Cutscene-Framework)

The public API header (`include/BehaviorSwitchboard/BSB_API.h`) is the integration point for other SKSE plugins.

---

## Troubleshooting

**Nothing happens when my event fires**
- Confirm your `.hkx` path is relative to the behavior project root (`meshes/actors/character/`)
- Use backslashes in the path (`Behaviors\\MyMod.hkx`, not `Behaviors/MyMod.hkx`)
- Check the BSB log at `Documents/My Games/Skyrim Special Edition/SKSE/BehaviorSwitchboard.log`

**Registration rejected at startup**
- Event names must be a single token with no spaces
- `modName`, `behaviorPath`, `eventName`, and `graphName` are all required
- Each mod must have a unique `modName`, `eventName`, and variable names within the same graph

**Behavior loads but exits immediately**
- Your sub-behavior needs its own exit transition. BSB only handles entry. Add a return event in your `.hkx` that sends the actor back to a vanilla root state.

---

## Credits

- **CommonLibSSE** (powerof3 fork) — SKSE plugin framework
- **Cutscene Framework** — the larger project this is part of

---

## Support

Found a bug or have a question?
- Open an issue on [GitHub](https://github.com/Cassieandstuff/Cutscene-Framework)
- Post in the comments below

---

*No Nemesis. No Pandora. Just drop a .hkx and send an event.*
