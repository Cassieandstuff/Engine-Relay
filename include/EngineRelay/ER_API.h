#pragma once

// ============================================================================
// Engine Relay — Public API
//
// Register new sub-behaviors at runtime. The Switchboard patches them into
// the actor's behavior graph in memory at game start — no Nemesis/Pandora
// needed.
//
// Cross-DLL linkage: ER defines ER_API as dllexport. Consumer plugins
// (like TrueFlight) see dllimport. Both are handled automatically —
// ER's CMakeLists sets the ER_EXPORTS define.
//
// Usage from another SKSE plugin:
//
//   #include <EngineRelay/ER_API.h>
//
//   // During kPostLoad:
//   EngineRelay::Registration reg;
//   reg.modName      = "MyMod";
//   reg.behaviorPath = "Behaviors\\MyMod_Combat.hkx";
//   reg.eventName    = "MyMod_EnterCombat";
//   reg.graphName    = "0_Master.hkb";  // humanoid root graph
//   reg.animations   = {
//       "Animations\\MyMod\\attack01.hkx",
//       "Animations\\MyMod\\attack02.hkx"
//   };
//   reg.variables    = {
//       { "MyMod_IsActive", EngineRelay::VariableType::Bool, 0 },
//       { "MyMod_Speed",    EngineRelay::VariableType::Float, 0 }
//   };
//   EngineRelay::Register(reg);
//
//   // At runtime — from gameplay code:
//   actor->NotifyAnimationGraph("MyMod_EnterCombat");
//   // Havok handles the rest.
//
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <EngineRelay/ERInterface.h>

#ifdef ER_EXPORTS
#   define ER_API __declspec(dllexport)
#else
#   define ER_API __declspec(dllimport)
#endif

namespace EngineRelay {

    /// Variable type enum matching Havok's hkbVariableInfo::VariableType.
    enum class VariableType : std::int8_t {
        Bool    = 0,  // VARIABLE_TYPE_BOOL
        Int8    = 1,  // VARIABLE_TYPE_INT8
        Int16   = 2,  // VARIABLE_TYPE_INT16
        Int32   = 3,  // VARIABLE_TYPE_INT32
        Float   = 4,  // VARIABLE_TYPE_REAL
    };

    /// A variable to register on the root behavior graph.
    /// The sub-behavior can read/write it, and the SKSE plugin can
    /// poll it via actor->GetGraphVariableBool/Float/Int.
    struct Variable {
        std::string  name;          ///< Variable name (e.g. "TF_IsFlying")
        VariableType type;          ///< Type (Bool, Int32, Float)
        std::int32_t initialValue;  ///< Initial value as raw 4-byte word.
                                    ///< Bool: 0 or 1. Int32: direct value.
                                    ///< Float: use std::bit_cast<int32_t>(1.0f).
    };

    enum class ActionGate;

    struct Registration {
        /// Display name for logging. Must be unique across mods.
        std::string modName;

        /// Path to the mod's sub-behavior .hkx file, relative to the
        /// behavior project root (e.g. meshes/actors/character/ for humanoids).
        /// Must match the convention used by vanilla BehaviorReferenceGenerators.
        /// e.g. "Behaviors\\MyMod_Combat.hkx"
        std::string behaviorPath;

        /// Event name that triggers entry into this sub-behavior.
        /// The Switchboard registers this event on the behavior graph.
        /// The mod sends it via actor->NotifyAnimationGraph().
        std::string eventName;

        /// Animation .hkx paths to append to the character's animation list.
        /// These are the animations your sub-behavior's clip generators
        /// reference. Paths relative to Data/.
        std::vector<std::string> animations;

        /// Variables to register on the root behavior graph.
        /// These are visible to both the sub-behavior and the parent graph,
        /// allowing SKSE code to poll/set them via GetGraphVariableBool etc.
        std::vector<Variable> variables;

        /// Root behavior graph name to inject into.
        /// This must match the graph's hkb filename exactly.
        /// Humanoid characters: "0_Master.hkb"
        /// Creatures: "WolfBehavior.hkb", "DeerBehavior.hkb", etc.
        std::string graphName;

        /// Optional. Scopes variable/event injection to a specific character
        /// archetype. This is the Havok project file path passed to
        /// CreateSymbolIdMap, e.g. "Actors\\Character\\DefaultMale.hkx".
        ///
        /// ER matches this (case-insensitively) against the project path to
        /// decide whether to inject variables and events for a given character.
        ///
        /// If left empty, ER injects for ALL character archetypes. This is
        /// the safe backward-compatible default — AppendEvent and AppendVariable
        /// are idempotent, so injecting into an extra graph is harmless.
        ///
        /// Typical values:
        ///   "Actors\\Character\\DefaultMale.hkx"   — humanoid player / NPCs
        ///   "Actors\\Character\\DefaultFemale.hkx" — humanoid female NPCs
        std::string projectPath;

        /// Vanilla action categories that this sub-behavior needs to suppress.
        /// ER automatically injects the corresponding ER_Gate_* bool variables
        /// into the behavior graph (default 0) so that 0_master gate transitions
        /// work without the mod shipping a static graphdata declaration.
        /// Call ActivateGates/DeactivateGates at runtime to flip these variables.
        /// Empty by default — backward compatible with existing registrations.
        std::vector<ActionGate> actionGates;
    };

    /// Register a sub-behavior. Call during kPostLoad.
    /// Returns true if the registration was accepted.
    /// Returns false if a registration with the same modName already exists.
    ER_API bool Register(const Registration& reg);

    /// Check if a mod is registered.
    ER_API bool IsRegistered(const std::string& modName);

    /// Get the number of registrations (all creature types).
    ER_API size_t GetRegistrationCount();

    /// Send a registered event to an actor's behavior graph by numeric ID.
    /// Bypasses NotifyAnimationGraph (which can't resolve ER-injected events)
    /// and queues the event directly on the Havok character's event queue.
    ///
    /// @param actor   The actor to send the event to.
    /// @param modName The mod name used during Register().
    /// @returns true if the event was queued successfully.
    ER_API bool SendEvent(RE::Actor* actor, const std::string& modName);

    // ========================================================================
    // Character State Allocator
    //
    // Havok's hkpCharacterStateManager has 5 unused user state slots
    // (kUserState1–kUserState5). ER acts as the central allocator for
    // these slots so multiple mods can coexist without conflicts.
    //
    // Consumer plugins provide a ERPhysicsCallbacks struct with
    // function pointers. ER creates an internal bhkCharacterState subclass
    // that dispatches to these callbacks, avoiding cross-DLL vtable issues.
    //
    // Usage:
    //
    //   void MyUpdate(RE::hkpCharacterContext& ctx,
    //                 const RE::hkpCharacterInput& in,
    //                 RE::hkpCharacterOutput& out, void* userData) {
    //       auto* myData = static_cast<MyFlightData*>(userData);
    //       out.velocity = myData->desiredVelocity;
    //   }
    //
    //   EngineRelay::ERPhysicsCallbacks cbs;
    //   cbs.updateFn     = &MyUpdate;
    //   cbs.changeFn     = &MyChange;
    //   cbs.userData      = &mySharedData;
    //
    //   auto slot = EngineRelay::RegisterCharacterState("TrueFlight", cbs);
    //   // slot == kUserState1 (or whichever was free)
    //
    //   // To enter: ctrl->wantState = slot;
    //   // To leave: changeFn writes next state via context.
    //
    // ========================================================================

    // StateUpdateFn / StateChangeFn / StateEnterFn / StateLeaveFn and
    // ERPhysicsCallbacks are defined in ERInterface.h (included above).

    /// Register a custom character state. ER assigns the next free
    /// kUserState slot. Returns the assigned hkpCharacterStateType, or
    /// kTotal (11) if all 5 user slots are exhausted.
    ///
    /// @param modName   Unique name for logging/diagnostics.
    /// @param callbacks Callback struct — updateFn and changeFn required.
    /// @returns The Havok state type (kUserState1–5) assigned to this mod,
    ///          or kTotal on failure. Use this value for ctrl->wantState.
    /// @deprecated Use RegisterLogicalState instead.
    [[deprecated("Use RegisterLogicalState — see ER_API.h for migration notes.")]]
    ER_API RE::hkpCharacterStateType RegisterCharacterState(
        const std::string& modName,
        const ERPhysicsCallbacks& callbacks);

    /// Check if a character state slot is registered for a mod.
    /// @deprecated Use IsLogicalStateRegistered instead.
    [[deprecated("Use IsLogicalStateRegistered.")]]
    ER_API bool IsCharacterStateRegistered(const std::string& modName);

    /// Get the Havok state type assigned to a mod, or kTotal if not registered.
    /// @deprecated Use GetERStateHandle instead.
    [[deprecated("Use GetERStateHandle.")]]
    ER_API RE::hkpCharacterStateType GetCharacterStateSlot(const std::string& modName);

    /// Install all registered character states into an actor's state manager.
    /// Call this when you have access to the actor and its character controller
    /// is initialised (e.g. kPostLoadGame, or on first gameplay tick).
    /// Safe to call multiple times — already-installed slots are skipped.
    /// @deprecated Use InstallLogicalStateHost instead.
    [[deprecated("Use InstallLogicalStateHost.")]]
    ER_API void InstallCharacterStates(RE::Actor* actor);

    // ========================================================================
    // Logical State API
    //
    // ER reserves a single physical Havok user slot (kUserState1) as its
    // "host state" and multiplexes an unlimited number of mod-owned logical
    // states inside it.  Only one logical state can be active per actor at a
    // time; the host state's Havok callbacks dispatch to whichever mod's
    // callbacks are currently active for that actor.
    //
    // Migration from RegisterCharacterState:
    //
    //   Old (deprecated):
    //     auto slot = ER::RegisterCharacterState("MyMod", cbs);
    //     fpc.SetHoverStateSlot(slot);          // store kUserState1
    //     ER::InstallCharacterStates(actor);   // on kPostLoadGame
    //     ctrl->wantState = slot;               // on engage
    //
    //   New:
    //     auto h = ER::RegisterLogicalState("MyMod::State", cbs);
    //     ER::InstallLogicalStateHost(actor);  // on kPostLoadGame
    //     ctrl->wantState = ER::GetLogicalStateHostSlot(); // on engage
    //     ER::ActivateLogicalState(actor, h);  // on engage
    //     ER::DeactivateLogicalState(actor);   // on disengage
    //
    // ========================================================================

    // ERStateHandle, kInvalidERState, kPriorityDefault, and
    // kPriorityCutscene are defined in ERInterface.h (included above).

    /// Register a logical state. Call during kPostLoad.
    ///
    /// @param modName   Unique name for logging/diagnostics.
    /// @param callbacks Callback struct — updateFn and changeFn required.
    /// @param priority  Activation priority (default: kPriorityDefault = 0).
    ///                  When ActivateLogicalState is called, if another state
    ///                  is already active with a strictly higher priority the
    ///                  new activation is refused and returns false.
    ///                  If the new activation wins, any currently active lower-
    ///                  or equal-priority state is saved as a "preempted" handle
    ///                  and automatically restored when DeactivateLogicalState
    ///                  is called.  Use kPriorityCutscene (255) to guarantee
    ///                  unconditional takeover.
    /// @returns A handle to use with ActivateLogicalState, or
    ///          kInvalidERState if registration failed.
    ER_API ERStateHandle RegisterLogicalState(
        const std::string& modName,
        const ERPhysicsCallbacks& callbacks,
        std::uint32_t priority = kPriorityDefault);

    /// Check if a logical state is registered for a mod.
    ER_API bool IsLogicalStateRegistered(const std::string& modName);

    /// Get the handle for a named logical state, or kInvalidERState.
    ER_API ERStateHandle GetERStateHandle(const std::string& modName);

    /// Get the physical Havok slot that ER uses as its host state.
    /// Always kUserState1. Use this value for ctrl->wantState when entering
    /// the ER-managed physics state.
    ER_API RE::hkpCharacterStateType GetLogicalStateHostSlot();

    /// Install the ER host state into an actor's Havok state manager.
    /// Must be called before the actor can enter any logical state.
    /// Safe to call multiple times — already-installed actors are skipped.
    /// Call at kPostLoadGame (or when the actor's controller is available).
    ER_API bool InstallLogicalStateHost(RE::Actor* actor);

    /// Make a logical state the active state for an actor.
    ///
    /// The host state's Havok callbacks (update/change/enter/leave) will be
    /// routed to the callbacks registered under this handle.
    ///
    /// Priority rules:
    ///   - If another state is already active with a strictly higher priority,
    ///     this call is refused and returns false.
    ///   - Otherwise the new state becomes active.  Any previously active state
    ///     is saved as the "preempted" handle and will be automatically restored
    ///     when DeactivateLogicalState is called (single level of preemption).
    ///
    /// Does not force a Havok state transition — call
    ///   ctrl->wantState = GetLogicalStateHostSlot()
    /// separately to enter the ER host state.
    /// @returns true if the state was activated; false if blocked by priority.
    ER_API bool ActivateLogicalState(RE::Actor* actor, ERStateHandle handle);

    /// Clear the active logical state for an actor so the host state's
    /// callbacks become no-ops.  If a lower-priority state was preempted when
    /// this state was activated, that state is automatically restored.
    /// Does not force a Havok state transition.
    ER_API void DeactivateLogicalState(RE::Actor* actor);

    /// Get the currently active logical state handle for an actor.
    /// Returns kInvalidERState if none is active or the actor is not
    /// tracked.
    ER_API ERStateHandle GetActiveLogicalState(RE::Actor* actor);

    // ========================================================================
    // Action Gate API
    //
    // Mod-agnostic booleans that suppress specific vanilla event transitions
    // inside 0_master.hkx.  When a gate is set to true the corresponding
    // vanilla event is swallowed (self-transition on the current state); when
    // false the event flows through normally.
    //
    // Preferred usage — declare gates in your Registration or via
    // RegisterActionGates(), then call ActivateGates/DeactivateGates at the
    // appropriate runtime moment.  ER resolves variable names internally so
    // your mod never needs to know them.
    //
    //   // During kPostLoad — declare which actions you will suppress:
    //   EngineRelay::RegisterActionGates(
    //       "MyMod", { EngineRelay::ActionGate::WeaponDraw });
    //
    //   // When entering your custom state:
    //   EngineRelay::ActivateGates(actor, "MyMod");
    //
    //   // When leaving your custom state:
    //   EngineRelay::DeactivateGates(actor, "MyMod");
    //
    // Lower-level escape hatch (still available):
    //
    //   EngineRelay::SetActionGate(
    //       actor, EngineRelay::ActionGate::WeaponDraw, true);
    //   EngineRelay::ClearAllActionGates(actor);
    //
    // ========================================================================

    /// Vanilla action categories that can be independently suppressed.
    enum class ActionGate {
        WeaponDraw,    ///< Suppresses the "weaponDraw" event transition.
        WeaponSheathe, ///< Suppresses the "weaponSheathe" event transition.
        WeaponEquip,   ///< Suppresses all force-equip / Unequip event transitions.
        CutsceneActions, ///< Suppresses core vanilla action-start events (sprint/sneak/attack/cast/jump/etc.).
    };

    /// Declare which action gates a mod will use, without a full sub-behavior
    /// Registration.  Call once during kPostLoad before ActivateGates is used.
    /// If the mod already registered via Register() with a non-empty actionGates
    /// field, calling this a second time for the same modName is a no-op.
    ///
    /// @param modName  Unique mod name for lookup by ActivateGates/DeactivateGates.
    /// @param gates    The set of ActionGate values this mod may suppress.
    /// @returns true if the gates were recorded; false if modName was already registered.
    ER_API bool RegisterActionGates(const std::string& modName,
        const std::vector<ActionGate>& gates);

    /// Set or clear a single action gate on an actor's behavior graph.
    /// @param actor    The actor whose graph variables should be written.
    /// @param gate     Which vanilla action category to suppress or restore.
    /// @param suppress true to suppress the action; false to restore it.
    ER_API void SetActionGate(RE::Actor* actor, ActionGate gate, bool suppress);

    /// Clear every action gate on an actor (set all to false / pass-through).
    /// Call this when a custom gameplay state ends so vanilla behaviour is
    /// fully restored regardless of which gates were set.
    ER_API void ClearAllActionGates(RE::Actor* actor);

    /// Activate only the gates declared for a specific mod.
    /// Sets each ER_Gate_* variable to true on the actor's behavior graph.
    /// Logs a warning if the mod has no registered gates.
    ///
    /// @param actor    The actor whose graph variables should be written.
    /// @param modName  The modName used in Register() or RegisterActionGates().
    ER_API void ActivateGates(RE::Actor* actor, const std::string& modName);

    /// Deactivate only the gates declared for a specific mod.
    /// Sets each ER_Gate_* variable to false on the actor's behavior graph.
    /// Does NOT clear gates belonging to other mods (unlike ClearAllActionGates).
    ///
    /// @param actor    The actor whose graph variables should be written.
    /// @param modName  The modName used in Register() or RegisterActionGates().
    ER_API void DeactivateGates(RE::Actor* actor, const std::string& modName);

    // ========================================================================
    // Logical Camera State API
    //
    // ER owns a single TESCameraState subclass (ERCameraState) and
    // multiplexes an unlimited number of mod-registered logical camera states
    // through it.  Only one logical camera state can be active at a time.
    // ER handles priority arbitration, preemption, and vanilla-TPS restoration.
    //
    // Usage:
    //
    //   // During kPostLoad:
    //   EngineRelay::ERCameraCallbacks cbs;
    //   cbs.updateFn = [](RE::TESCamera* cam, float dt, void* ud) {
    //       // Write world-space pos+rot to cam->cameraRoot->local, then:
    //       RE::NiUpdateData ud2{ 0.f, RE::NiUpdateData::Flag::kNone };
    //       cam->cameraRoot->Update(ud2);
    //   };
    //   cbs.userData = &myData;
    //   auto h = EngineRelay::RegisterLogicalCameraState("MyMod::Cam", cbs);
    //
    //   // When entering custom camera:
    //   EngineRelay::ActivateLogicalCameraState(h);
    //
    //   // When leaving:
    //   EngineRelay::DeactivateLogicalCameraState();
    //
    // Thread safety:
    //   updateFn is called from the render thread (inside TESCameraState::Update).
    //   beginFn / endFn are called from the render thread as well.
    //   Activate / Deactivate / Register are called from the game thread.
    //   Internal handles use std::atomic; the state registry is append-only
    //   after kPostLoad (same guarantee as the logical physics state API).
    //
    // Save / load:
    //   ER clears the active handle on kPreLoadGame and restores vanilla TPS.
    //   Mods must re-call ActivateLogicalCameraState from their own
    //   kPostLoadGame handler if they need to restore camera state after a load.
    //
    // Engine eviction / forced state changes:
    //   When the engine forces vanilla TPS back (e.g. a menu opens or another
    //   mod calls SetState directly), ERCameraState::End() is called.
    //   ER dispatches endFn and then auto-clears the active handle so the
    //   next ActivateLogicalCameraState() call is not blocked by a stale handle.
    //   Mods that want to respond to forced eviction should poll
    //   GetActiveLogicalCameraState() from their per-frame game-thread code.
    //
    //   IMPORTANT: endFn is called on the render thread.  Do NOT call any
    //   ER game-thread API (e.g. ActivateLogicalCameraState,
    //   DeactivateLogicalCameraState) from within endFn.
    //
    // Detecting ER camera ownership:
    //   ERCameraState uses id=kAnimated, the same value used by ADC and TF.
    //   Checking currentState->id == kAnimated is not a reliable ownership test.
    //   To detect whether ER's camera is active, call:
    //     EngineRelay::GetActiveLogicalCameraState() != kInvalidERCameraState
    //   This is the only authoritative test for ER camera ownership.
    //
    // Preemption stack:
    //   Up to 8 levels of preemption are supported.  A strictly higher-priority
    //   activation pushes the displaced handle onto the stack.
    //   DeactivateLogicalCameraState pops in LIFO order, restoring each level.
    //   If the stack is full, the oldest displaced handle is dropped (with a
    //   warning) and TPS restore still works correctly on the final deactivation.
    // ========================================================================

    // CameraStateUpdateFn / CameraStateBeginFn / CameraStateEndFn /
    // CameraCompassYawFn, ERCameraCallbacks, ERCameraHandle, and
    // kInvalidERCameraState are defined in ERInterface.h (included above).

    /// Register a logical camera state.  Call during kPostLoad.
    ///
    /// @param modName   Unique name for logging / diagnostics.
    /// @param callbacks Callback struct — updateFn is required.
    /// @param priority  Activation priority (default kPriorityDefault = 0).
    ///                  A higher-priority activation blocks lower-priority ones.
    ///                  Equal-priority activations replace each other without
    ///                  saving a preempted handle.  Use kPriorityCutscene (255)
    ///                  to guarantee unconditional takeover.
    /// @returns A handle for use with ActivateLogicalCameraState, or
    ///          kInvalidERCameraState on failure.
    ER_API ERCameraHandle RegisterLogicalCameraState(
        const std::string&            modName,
        const ERCameraCallbacks& callbacks,
        std::uint32_t                 priority = kPriorityDefault);

    /// Activate a logical camera state.
    /// Installs ERCameraState into PlayerCamera (if not already current) and
    /// routes Update/Begin/End dispatches to this handle's callbacks.
    ///
    /// Priority rules mirror ActivateLogicalState:
    ///   - Refused (returns false) if a strictly higher-priority state is active.
    ///   - If strictly higher than current, current is saved as the preempted
    ///     handle and auto-restored when DeactivateLogicalCameraState is called.
    ///
    /// Does NOT require an actor — camera state is global (PlayerCamera singleton).
    /// @returns true if activated; false if blocked by priority.
    ER_API bool ActivateLogicalCameraState(ERCameraHandle handle);

    /// Deactivate the current logical camera state.
    /// If a preempted state was saved on the stack, the most recently displaced
    /// state is automatically restored (LIFO).
    /// If no state remains active, vanilla ThirdPersonState is restored.
    /// If the engine already evicted ERCameraState (End() fired before this
    /// call), the preemption stack is cleaned up and this function returns
    /// without calling SetState (the engine already handled the swap).
    ER_API void DeactivateLogicalCameraState();

    /// Check if a logical camera state is registered for a mod.
    ER_API bool IsLogicalCameraStateRegistered(const std::string& modName);

    /// Get the handle for a named logical camera state,
    /// or kInvalidERCameraState if not registered.
    ER_API ERCameraHandle GetERCameraHandle(const std::string& modName);

    /// Get the currently active logical camera state handle,
    /// or kInvalidERCameraState if none is active.
    ///
    /// This is the authoritative test for ER camera ownership.
    /// ERCameraState uses id=kAnimated, which is also used by other mods;
    /// checking currentState->id == kAnimated is NOT reliable.
    /// Use:
    ///   EngineRelay::GetActiveLogicalCameraState() != kInvalidERCameraState
    ER_API ERCameraHandle GetActiveLogicalCameraState();

    // ========================================================================
    // Unified State Registration API
    //
    // ERRegistration bundles the three ER components — sub-behavior graph,
    // Havok physics user state, and custom camera state — behind a single
    // modName and shared priority.  Fill at least one optional field.
    //
    // Activation is ALL-OR-NOTHING: Activate() performs a dry-run priority
    // check on every opted-in component before touching any of them.  If any
    // component would be blocked by a higher-priority active state, the call
    // returns false and nothing changes.  This prevents split ownership where
    // mod A owns the behavior graph and mod B owns the camera.
    //
    // Usage:
    //
    //   ERRegistration reg;
    //   reg.modName  = "TrueFlight";
    //   reg.priority = kPriorityDefault;
    //
    //   reg.behavior.emplace();
    //   reg.behavior->behaviorPath = "Behaviors\\TrueFlight\\TrueFlight.hkx";
    //   reg.behavior->eventName    = "TF_EnterFlight";
    //   reg.behavior->graphName    = "EngineRelay.hkb";
    //
    //   reg.physicsState.emplace();
    //   reg.physicsState->updateFn = &FlightUpdate;
    //   reg.physicsState->changeFn = &FlightChange;
    //
    //   reg.cameraState.emplace();
    //   reg.cameraState->updateFn = &FlightCameraUpdate;
    //
    //   auto h = EngineRelay::Register(reg);
    //
    //   // To enter (all-or-nothing):
    //   EngineRelay::Activate(actor, h);
    //
    //   // To exit (always succeeds):
    //   EngineRelay::Deactivate(actor, h);
    //
    // ========================================================================

    /// Opt-in descriptor for the sub-behavior graph component.
    struct ERBehaviorDesc {
        std::string              behaviorPath;  ///< e.g. "Behaviors\\MyMod\\MyMod.hkx"
        std::string              eventName;     ///< Trigger event (sent via NotifyAnimationGraph).
        std::string              graphName;     ///< Target graph, e.g. "EngineRelay.hkb".
        std::string              projectPath;   ///< Optional archetype filter.
        std::vector<std::string> animations;    ///< Animation .hkx paths to inject.
        std::vector<Variable>    variables;     ///< Behavior graph variables to inject.
        std::vector<ActionGate>  actionGates;   ///< Vanilla action categories to suppress.
    };

    /// Unified registration for an ER mod state.
    /// Fill at least one optional field to form a valid registration.
    struct ERRegistration {
        std::string   modName;                               ///< Unique mod name.
        std::uint32_t priority{ kPriorityDefault };          ///< Shared priority for all components.

        std::optional<ERBehaviorDesc>         behavior;     ///< Sub-behavior graph (opt-in).
        std::optional<ERPhysicsCallbacks> physicsState; ///< Havok physics user state (opt-in).
        std::optional<ERCameraCallbacks>  cameraState;  ///< Custom camera state (opt-in).
    };

    /// Opaque handle returned by Register(ERRegistration).
    using ERHandle = std::uint32_t;

    /// Sentinel: invalid / registration-failed handle.
    static constexpr ERHandle kInvalidERHandle = 0xFFFFFFFFu;

    /// Register a unified mod state.  At least one optional component must be set.
    /// @returns An opaque handle on success, or kInvalidERHandle on failure.
    ER_API ERHandle Register(const ERRegistration& reg);

    /// Activate all opted-in components for the given actor.
    /// Performs a dry-run priority check first — if any component would be
    /// blocked, nothing activates and this returns false.
    /// @returns true if all components activated; false if blocked by priority.
    ER_API bool Activate(RE::Actor* actor, ERHandle handle);

    /// Deactivate all opted-in components for the given actor.
    /// Always succeeds — no priority check on exit.
    /// If the behavior component is active, sends "IdleForceDefaultState" to
    /// return the actor's behavior graph to 0_Master's default idle state.
    ER_API void Deactivate(RE::Actor* actor, ERHandle handle);

    /// Request that Havok enter the ER host physics state for this actor.
    /// Sets ctrl->wantState to the ER host slot (kUserState1) so Havok
    /// transitions on the next physics tick.
    ///
    /// If the actor is already in the host state (e.g. transitioning from
    /// hover to flight while keeping the same physics callbacks active),
    /// this is a no-op — no redundant state transition is issued.
    ///
    /// Call AFTER Activate() when you want physics to begin.
    /// Requires the handle to have a physicsState component.
    ER_API void EnterPhysicsState(RE::Actor* actor, ERHandle handle);

    /// Request that Havok exit the ER host physics state for this actor.
    /// Sets ctrl->wantState to kOnGround so Havok returns to normal ground
    /// physics on the next tick.
    ///
    /// If the actor is not currently in the ER host state, this is a no-op.
    ///
    /// Separate from Deactivate() — controls when Havok transitions, not
    /// when ER bookkeeping is torn down.
    /// Requires the handle to have a physicsState component.
    ER_API void ExitPhysicsState(RE::Actor* actor, ERHandle handle);

}
