#pragma once

// ============================================================================
// EngineRelay — SKSE Inter-Plugin Interface
//
// This is the ONLY header external mods need for BSB integration.
// No compile-time linkage against EngineRelay.dll is required.
//
// Discovery (call during SKSEPluginLoad, before any SKSE messages fire):
//
//   static const EngineRelay::ERInterface* g_bsb = nullptr;
//
//   SKSE::GetMessagingInterface()->RegisterListener("EngineRelay",
//       [](SKSE::MessagingInterface::Message* msg) {
//           if (msg->type == EngineRelay::ERInterface::kMessage)
//               g_bsb = static_cast<const EngineRelay::ERInterface*>(msg->data);
//       });
//
// BSB dispatches the interface pointer synchronously during its own kPostLoad
// handler.  SKSE delivers it to all registered "EngineRelay" listeners
// before BSB's Dispatch() call returns, so g_bsb is set before your own
// kPostLoad fires (provided BSB was loaded before your plugin, which SKSE's
// load-order rules enforce).
//
// Register camera and physics states in your kPostLoad handler:
//
//   case SKSE::MessagingInterface::kPostLoad:
//       if (g_bsb) {
//           EngineRelay::ERCameraCallbacks cbs;
//           cbs.updateFn = &MyCameraUpdate;
//           myHandle = g_bsb->RegisterLogicalCameraState("MyMod", &cbs, 0);
//       }
//       break;
//
// RE:: types used in the callback signatures are provided by CommonLib, which
// all SKSE plugins require.
// ============================================================================

#include <cstdint>

namespace EngineRelay {

// ── Priority constants ────────────────────────────────────────────────────────

/// Default priority for logical states and camera states.
/// Equal-priority activations replace the current one without preemption.
static constexpr std::uint32_t kPriorityDefault  = 0;

/// Reserved for Cutscene Framework.
/// A state with this priority unconditionally takes over any lower-priority
/// active state and cannot itself be preempted.
static constexpr std::uint32_t kPriorityCutscene = 255;

// ── Handle types ──────────────────────────────────────────────────────────────

using ERStateHandle  = std::uint32_t;
using ERCameraHandle = std::uint32_t;

static constexpr ERStateHandle  kInvalidERState       = 0xFFFFFFFFu;
static constexpr ERCameraHandle kInvalidERCameraState = 0xFFFFFFFFu;

// ── Character state callback types ────────────────────────────────────────────
// Backed by Havok kUserState1 (the BSB host slot).
// All callbacks receive the same userData pointer passed at registration.

using StateUpdateFn = void(*)(RE::hkpCharacterContext& context,
                              const RE::hkpCharacterInput& input,
                              RE::hkpCharacterOutput& output,
                              void* userData);
using StateChangeFn = void(*)(RE::hkpCharacterContext& context,
                              const RE::hkpCharacterInput& input,
                              RE::hkpCharacterOutput& output,
                              void* userData);
using StateEnterFn  = void(*)(RE::hkpCharacterContext& context,
                              RE::hkpCharacterStateType prevState,
                              const RE::hkpCharacterInput& input,
                              RE::hkpCharacterOutput& output,
                              void* userData);
using StateLeaveFn  = void(*)(RE::hkpCharacterContext& context,
                              RE::hkpCharacterStateType nextState,
                              const RE::hkpCharacterInput& input,
                              RE::hkpCharacterOutput& output,
                              void* userData);

struct ERPhysicsCallbacks {
    StateUpdateFn updateFn{ nullptr };  ///< Per-tick physics update (required).
    StateChangeFn changeFn{ nullptr };  ///< Per-tick state transition (required).
    StateEnterFn  enterFn { nullptr };  ///< Called on state entry (optional).
    StateLeaveFn  leaveFn { nullptr };  ///< Called on state exit (optional).
    void*         userData{ nullptr };  ///< Opaque pointer passed to all callbacks.
};

// ── Camera callback types ─────────────────────────────────────────────────────

/// Per-frame camera update.  Write the desired world-space transform to
/// camera->cameraRoot->local and call NiAVObject::Update() to propagate.
/// dt == RE::Main::QFrameAnimTime().
using CameraStateUpdateFn = void(*)(RE::TESCamera* camera, float dt, void* userData);

/// Called when ERCameraState becomes the active PlayerCamera state.
using CameraStateBeginFn  = void(*)(RE::TESCamera* camera, void* userData);

/// Called when ERCameraState is replaced by another state.
/// Dispatched on the render thread — do NOT call any BSB game-thread API
/// (e.g. ActivateLogicalCameraState) from within this callback.
using CameraStateEndFn    = void(*)(RE::TESCamera* camera, void* userData);

/// Optional: return the world-space yaw BSB should write to PlayerCamera::yaw
/// each frame for compass sync.  If null, BSB derives yaw from the camera
/// root's forward vector.
using CameraCompassYawFn  = float(*)(RE::TESCamera* camera, void* userData);

struct ERCameraCallbacks {
    CameraStateUpdateFn  updateFn    { nullptr };  ///< Per-frame update (required).
    CameraStateBeginFn   beginFn     { nullptr };  ///< State entered (optional).
    CameraStateEndFn     endFn       { nullptr };  ///< State left (optional).
    CameraCompassYawFn   compassYawFn{ nullptr };  ///< Compass heading override (optional).
    void*                userData    { nullptr };  ///< Opaque pointer passed to all callbacks.
};

// ── ERInterface ──────────────────────────────────────────────────────────────
//
// Flat function-pointer table broadcast by EngineRelay during its own
// kPostLoad handler.  Layout is stable within a major version; new fields are
// appended only.  Consumers should check `version >= kVersion` before calling
// functions added after the version they were written against.
//
// The struct is owned by BSB (static storage).  The pointer you receive in the
// listener callback remains valid for the lifetime of the game process.

struct ERInterface {
    /// SKSE message type dispatched by BSB.
    /// Register with: RegisterListener("EngineRelay", cb)
    /// and check: msg->type == ERInterface::kMessage
    static constexpr std::uint32_t kMessage = 0;
    static constexpr std::uint32_t kVersion = 1;

    std::uint32_t version{ kVersion };

    // ── Logical Camera State API ──────────────────────────────────────────────

    /// Register a logical camera state.  Call during kPostLoad after receiving
    /// the interface.  Returns kInvalidERCameraState on failure.
    ERCameraHandle (*RegisterLogicalCameraState)(
        const char*                   modName,
        const ERCameraCallbacks* callbacks,
        std::uint32_t                 priority);

    /// Activate a logical camera state, installing ERCameraState into
    /// PlayerCamera.  Returns false if blocked by a higher-priority active state.
    bool (*ActivateLogicalCameraState)(ERCameraHandle handle);

    /// Deactivate the current logical camera state, restoring a preempted
    /// state if one was saved, otherwise restoring vanilla TPS.
    void (*DeactivateLogicalCameraState)();

    /// Returns the currently active logical camera handle, or
    /// kInvalidERCameraState if none is active.
    ERCameraHandle (*GetActiveLogicalCameraState)();

    // ── Logical Physics State API ─────────────────────────────────────────────

    /// Register a logical physics state multiplexed through the BSB host slot
    /// (Havok kUserState1).  Returns kInvalidERState on failure.
    ERStateHandle (*RegisterLogicalState)(
        const char*                    modName,
        const ERPhysicsCallbacks* callbacks,
        std::uint32_t                  priority);

    /// Install the BSB host state into an actor's Havok state manager.
    /// Must be called before ActivateLogicalState.  Safe to call multiple times.
    bool (*InstallLogicalStateHost)(RE::Actor* actor);

    /// Make a logical state active for an actor.
    /// Returns false if blocked by a higher-priority active state.
    bool (*ActivateLogicalState)(RE::Actor* actor, ERStateHandle handle);

    /// Clear the active logical state for an actor, restoring a preempted state
    /// if one was saved.  Does not force a Havok state transition.
    void (*DeactivateLogicalState)(RE::Actor* actor);

    /// Returns the currently active logical state handle for an actor, or
    /// kInvalidERState if none.
    ERStateHandle (*GetActiveLogicalState)(RE::Actor* actor);

    /// Request that Havok enter the BSB host physics state (kUserState1) for
    /// an actor on the next physics tick.
    /// No-op if the actor is already in the host state — safe to call during
    /// a hover→flight transition where physics stays active.
    void (*EnterPhysicsHost)(RE::Actor* actor);

    /// Request that Havok exit the BSB host physics state for an actor,
    /// returning it to kOnGround on the next physics tick.
    /// No-op if the actor is not currently in the host state.
    void (*ExitPhysicsHost)(RE::Actor* actor);
};

}  // namespace EngineRelay
