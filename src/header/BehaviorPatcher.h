#pragma once

#include "PCH.h"
#include <EngineRelay/ER_API.h>

namespace EngineRelay {

    /// Behavior graph variable names for each ActionGate, indexed by the enum
    /// value.  Shared between Plugin.cpp and BehaviorPatcher.cpp so both can
    /// resolve ActionGate → variable name without duplicating the table.
    inline constexpr std::string_view kGateVarNames[] = {
        "ER_Gate_WeaponDraw",      // ActionGate::WeaponDraw
        "ER_Gate_WeaponSheathe",   // ActionGate::WeaponSheathe
        "ER_Gate_WeaponEquip",     // ActionGate::WeaponEquip
        "ER_Gate_CutsceneActions", // ActionGate::CutsceneActions
    };
    static_assert(std::size(kGateVarNames) ==
        static_cast<std::size_t>(ActionGate::CutsceneActions) + 1,
        "kGateVarNames must have one entry per ActionGate enumerator.");

    /// Core runtime patcher — vtable hook on hkbBehaviorGraph::Activate (slot 0x04).
    /// Fires per-graph, per-character. Two code paths:
    ///
    ///   Option-A (non-switchboard root graphs, e.g. "0_Master.hkb"):
    ///     InjectRegistrations() adds BRGs, StateInfos, and wildcard transitions
    ///     to the live graph before original Activate builds index maps.
    ///     (Currently a no-op — all registrations target a switchboard graph.)
    ///
    ///   Option-B (switchboard graphs — primary path):
    ///     Recognized by IsSwitchboardGraph(). The pre-baked HKX already contains
    ///     all BRGs and transitions; InjectRegistrations is skipped. Instead:
    ///     1. numStaticNodes is bumped to reserve ID space for sub-behavior nodes.
    ///     2. The pending ER event (set by SetPendingEREvent before
    ///        NotifyAnimationGraph) identifies the active registration; BSB sets
    ///        rootSM->startStateID to its 1-based state index so Havok starts
    ///        the SM in the correct state (no A-pose on first entry).
    ///     3. Sub-behavior node IDs (trueflight.hkx etc.) are fixed up by
    ///        WalkAndAssignNodeIDs before original Activate builds the partition
    ///        table.
    ///
    namespace BehaviorPatcher {

        /// Install the vtable hook on hkbBehaviorGraph::Activate.
        /// Call once during SKSEPluginLoad.
        void Install();

        /// Clear the double-patch guard. Must be called before any graph
        /// reactivation (save load, fast travel) so that BSB re-injects
        /// into the freshly rebuilt graph.
        void ClearPatchedSet();

        /// Add a registration to the pending list.
        /// Thread-safe. Called from ER_API::Register() and ConfigLoader.
        void AddRegistration(const Registration& reg);

        /// Get the current registration list.
        const std::vector<Registration>& GetRegistrations();

        /// Store the event name that is about to trigger a BSB state transition.
        /// Must be called BEFORE actor->NotifyAnimationGraph() so that ER_Activate
        /// can read it synchronously and set the SM's startStateID to the correct
        /// pre-baked state, avoiding A-pose on first entry.
        void SetPendingEREvent(const std::string& eventName);

    }

}
