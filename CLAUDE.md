# Engine Relay — Claude Code Project Context

This file is read automatically by Claude Code when you open this repository.

---

## What This Repo Is

A standalone SKSE plugin (C++23, CommonLibSSE powerof3 fork) that provides
**non-destructive runtime behavior graph augmentation** for Skyrim SE/AE.

Engine Relay allows SKSE plugins to inject sub-behavior HKX files into
Skyrim's live Havok behavior graphs at runtime without requiring Nemesis,
Pandora, or file patching.

**GitHub:** https://github.com/Cassieandstuff/Engine-Relay

---

## Repository Layout

```
Engine-Relay/
├── src/
│   ├── cpp/                     # Implementation (.cpp files)
│   └── header/                  # Private headers
├── include/                     # Engine Relay's own internal headers (PCH, etc.)
│                                # ER's PUBLIC API headers (ERInterface.h, ER_API.h)
│                                # live in ../SDK/EngineRelay/ — Engine Relay
│                                # consumes its own API from the SDK like everyone else.
├── behavior_src/
│   ├── character/               # Humanoid YAML behavior sources
│   └── dragon/                  # Dragon YAML behavior sources
├── external/
│   ├── CommonLibSSE/            # Submodule (powerof3 fork)
│   └── HKBuild/                 # Submodule (https://github.com/Cassieandstuff/HKBuild)
├── cmake/                       # CMake helpers
├── docs/
├── CMakeLists.txt               # Standalone root
├── CMakePresets.json
└── vcpkg.json                   # ryml, clib-util, clib-utils-qtr, spdlog, xbyak
```

---

## Key Architecture

Engine Relay uses **Option-B file interception** to serve a dynamically-generated
`EngineRelay.hkx` to Havok at runtime:

1. `BehaviorFileInterceptor` hooks `BSResource::LooseFileLocation::DoCreateStream`
   (vtable slot 3) and intercepts opens of `enginerelay.hkx` / `dragonenginerelay.hkx`.

2. On first intercept, `BehaviorGenerator::GenerateBytes()` builds the HKX in memory
   using `HkxErWriter` — a native C++ Havok packfile writer with no subprocesses.

3. The generated HKX contains a root state machine with one `hkbBehaviorReferenceGenerator`
   per registered sub-behavior, plus wildcard transitions (event → state ID).

4. `BehaviorPatcher` hooks `hkbBehaviorGraph::Activate` (vtable slot 4) to:
   - Skip runtime injection for EngineRelay.hkb graphs (already pre-baked)
   - Reserve `numStaticNodes` ID space for sub-behavior node fix-up
   - Set `startStateID` from the pending ER event (so Havok enters the correct state)
   - Walk and assign IDs to sub-behavior nodes (0xFFFF → valid static IDs)

### Sub-systems

| Sub-system | File | Purpose |
|---|---|---|
| File interception | `BehaviorFileInterceptor.cpp` | Serve generated HKX |
| HKX generation | `HkxErWriter.cpp` | Native Havok packfile writer |
| Graph patching | `BehaviorPatcher.cpp` | Activate hook + node ID fix-up |
| Physics states | `CharacterStateAllocator.cpp` | Multiplex `kUserState1` |
| Camera states | `CameraStateManager.cpp` | Multiplex `ERCameraState` |
| Config loader | `ConfigLoader.cpp` | YAML/INI config from `SKSE/Plugins/EngineRelay/` |

---

## Public API

### Path A — SKSE Messaging (the canonical pattern)

Consumer mods include `EngineRelay/ERInterface.h` (resolved from `../SDK/` or wherever
`SCT_SDK_FOLDER` points) and listen for the `"EngineRelay"` SKSE message type. ER
dispatches `ERInterface*` during its own `kPostLoad` handler.

```cpp
static const EngineRelay::ERInterface* g_er = nullptr;

SKSE::GetMessagingInterface()->RegisterListener("EngineRelay",
    [](SKSE::MessagingInterface::Message* msg) {
        if (msg->type == EngineRelay::ERInterface::kMessage)
            g_er = static_cast<const EngineRelay::ERInterface*>(msg->data);
    });
```

This is the **only supported integration pattern** for consumer mods. No `.lib`
linking against `EngineRelay.dll`. Same approach a third-party mod author would
use to depend on Engine Relay.

### Path B — Direct DLL Link (deprecated for consumers)

`SDK/EngineRelay/ER_API.h` exists for Engine Relay's own internal use — the producer
needs symbol-level access to its own implementation. External consumers should
NOT use this path; use Path A (SKSE messaging) instead.

---

## Build

Requires:
- Visual Studio 2022 (MSVC v143, C++23)
- vcpkg with `VCPKG_ROOT` env var set
- CMake 3.21+
- .NET 10 SDK (for HKBuild)

```
cmake --preset release
cmake --build build/release
```

Env vars:
- `SKYRIM_MODS_FOLDER` — deploy `EngineRelay.dll` to `<MODS>/EngineRelay/SKSE/Plugins/`
- `SKYRIM_OWRT_FOLDER` — also deploy to overwrite/output folder if set
- `COMMONLIB_SSE_FOLDER` — override the CommonLibSSE submodule with a local clone

---

## Behavior Graph Names

| Creature | Graph name (.hkb) | Intercepted filename (.hkx) |
|---|---|---|
| Human/player | `EngineRelay.hkb` | `enginerelay.hkx` |
| Dragon | `DragonEngineRelay.hkb` | `dragonenginerelay.hkx` |

Sub-behavior mods register with `graphName = "EngineRelay.hkb"` (humanoid)
or `graphName = "DragonEngineRelay.hkb"` (dragon).

---

## Action Gate Variables

Behavior graph bool variables injected by ER to suppress vanilla actions:

| ActionGate enum | Graph variable |
|---|---|
| `WeaponDraw` | `ER_Gate_WeaponDraw` |
| `WeaponSheathe` | `ER_Gate_WeaponSheathe` |
| `WeaponEquip` | `ER_Gate_WeaponEquip` |
| `CutsceneActions` | `ER_Gate_CutsceneActions` |

---

## Wildcard Gating (Runtime C++ — WildcardGate.cpp)

Havok's `hkbStateMachine` evaluates parent wildcard transitions BEFORE child
generators see events. Without gating, global wildcards in the root SM (0_master)
yank actors out of ER sub-behaviors before the sub-behavior can handle events
internally.

**Solution:** Pure C++ runtime patching in `WildcardGate.cpp`. Hooks
`RE::Character::InitHavok` (vtable slot 0x66), which fires after the behavior
system is fully loaded and linked. Walks the entire generator tree and assigns a
custom `ERCondition` object to every unguarded `FLAG_IS_GLOBAL_WILDCARD`
transition, then clears `FLAG_DISABLE_CONDITION` so Havok evaluates it.

`ERCondition` is a custom vtable-patched condition object (NOT `hkbExpressionCondition`
— that requires expression linking which crashes post-link injection). Its `isTrue()`
reads `ER_Active` directly from `hkbBehaviorGraph::variableValueSet` at offset
`0xD8`. When `ER_Active==0` (normal gameplay) it returns `true` — wildcards fire.
When `ER_Active==1` (actor inside a sub-behavior) it returns `false` — wildcards
are suppressed and events fall through to the active sub-behavior.

| Component | File | Purpose |
|---|---|---|
| Runtime gate | `WildcardGate.cpp` | Hook + walk + patch all global wildcards |
| Variable set | `Plugin.cpp:SendEvent()` | `SetGraphVariableBool("ER_Active", true)` after `NotifyAnimationGraph` succeeds |
| Variable clear | `Plugin.cpp:Deactivate()` | `SetGraphVariableBool("ER_Active", false)` |

**Key details:**
- No Pandora/Nemesis dependency — works with any mod setup
- Only `FLAG_IS_GLOBAL_WILDCARD` (0x0400) transitions are gated; local wildcards untouched
- Transitions already conditioned (`t.condition != nullptr`) are skipped — no double-gating
- `FLAG_DISABLE_CONDITION` is cleared on patched transitions so Havok evaluates the condition
- `ER_Active` is set AFTER `NotifyAnimationGraph` returns — the entry wildcard fires while
  `ER_Active==0` (condition TRUE), then subsequent events are suppressed
- `Deactivate()` resets `ER_Active=0` BEFORE firing the exit event — the exit wildcard fires normally
- Generator walk covers: `hkbStateMachine`, `hkbModifierGenerator`, `hkbBlenderGenerator`,
  `BSCyclicBlendTransitionGenerator`, `hkbManualSelectorGenerator`, `BSiStateTaggingGenerator`,
  `BSOffsetAnimationGenerator`, `BSBoneSwitchGenerator`
- Graphs without `ER_Active` declared (non-humanoid creatures) are skipped automatically

---

## Extracted From

This project was extracted from the CutsceneFramework monorepo where it was
known as **Behavior Switchboard**. The rename to Engine Relay happened at
extraction time. The monorepo still contains mods that depend on Engine Relay
(True Flight, Take to the Sky, Cutscene Framework) — those will be updated to
import this repo as a dependency in a future session.
