#pragma once

#include "PCH.h"
#include <BehaviorSwitchboard/BSB_API.h>

namespace BehaviorSwitchboard {

    /// Scans Data/SKSE/Plugins/BehaviorSwitchboard/ for config files and
    /// converts each into a Registration. This is the file-based alternative
    /// to the C++ API — mod authors who don't write SKSE plugins can drop a
    /// config file instead.
    ///
    /// ── Preferred format: YAML (.yml or .yaml) ──────────────────────────
    ///
    ///   modName:   MyMod
    ///   behavior:  Behaviors\mymod\mymod_combat.hkx
    ///   event:     MyMod_EnterCombat
    ///   graphName: BehaviorSwitchboard.hkb
    ///
    ///   animations:
    ///     - meshes\actors\character\animations\mymod\attack01.hkx
    ///     - meshes\actors\character\animations\mymod\attack02.hkx
    ///
    ///   variables:                          # optional
    ///     - name:  MyMod_IsActive
    ///       type:  Bool                     # Bool | Int8 | Int16 | Int32 | Float
    ///       value: 0
    ///     - name:  MyMod_Speed
    ///       type:  Float
    ///       value: 0
    ///
    /// ── Legacy format: INI (.ini) — DEPRECATED ──────────────────────────
    ///
    ///   .ini files are still accepted but will emit a deprecation warning.
    ///   Migrate to YAML — the .ini format will be removed in a future
    ///   version. See the BSB reference documentation for migration details.
    ///
    namespace ConfigLoader {

        /// Scan the config directory and return all parsed registrations.
        std::vector<Registration> LoadConfigs();

    }

}
