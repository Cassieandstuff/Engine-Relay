#pragma once

#include "PCH.h"
#include <BehaviorSwitchboard/BSB_API.h>

// ============================================================================
// CameraStateManager — internal BSB module
//
// Owns the singleton BSBCameraState (a TESCameraState subclass) and
// multiplexes an unlimited number of mod-registered logical camera states
// through it via the same priority + preemption model as CharacterStateAllocator.
//
// Thread safety:
//   BSBCameraState::Update() and End() are called from the render thread.
//   Activate/Deactivate/Register are called from the game thread.
//   s_activeHandle and s_activePriority are std::atomic — safe for render-thread
//   reads and game-thread writes, and for the render-thread clear in End().
//   s_preemptionStack is a std::vector guarded by s_camMutex — only game-thread
//   functions access it.  End() intentionally skips the stack.
//   s_cameraStates is append-only after kPostLoad — safe to read by index from
//   the render thread without a lock.
// ============================================================================

namespace BehaviorSwitchboard::CameraStateManager {

    /// Allocate the singleton BSBCameraState.  Call once from SKSEPluginLoad.
    void Init();

    /// Register a logical camera state.  Call during kPostLoad.
    /// Returns a handle or kInvalidLogicalCameraState on failure.
    LogicalCameraHandle RegisterLogicalCameraState(
        const std::string& modName,
        const LogicalCameraCallbacks& callbacks,
        std::uint32_t priority);

    /// Check if a logical camera state is registered.
    bool IsLogicalCameraStateRegistered(const std::string& modName);

    /// Get the handle for a named logical camera state, or kInvalidLogicalCameraState.
    LogicalCameraHandle GetLogicalCameraHandle(const std::string& modName);

    /// Returns true if a camera state with the given priority would be accepted
    /// right now (no strictly higher-priority state is currently active).
    /// Does NOT modify any state.
    bool CanActivateLogicalCameraState(std::uint32_t priority);

    /// Install BSBCameraState into PlayerCamera (if not already current) and
    /// route Update/Begin/End dispatches to the given handle's callbacks.
    /// Returns false if blocked by a higher-priority active state.
    bool ActivateLogicalCameraState(LogicalCameraHandle handle);

    /// Deactivate the current logical camera state.
    /// Restores a preempted state if one was saved, otherwise restores vanilla TPS.
    void DeactivateLogicalCameraState();

    /// Get the currently active logical camera state handle,
    /// or kInvalidLogicalCameraState if none is active.
    LogicalCameraHandle GetActiveLogicalCameraState();

    /// Clear all active/preempted handles on kPreLoadGame / kNewGame.
    /// Restores vanilla TPS if BSBCameraState is currently installed.
    void OnPreLoadGame();

}  // namespace BehaviorSwitchboard::CameraStateManager
