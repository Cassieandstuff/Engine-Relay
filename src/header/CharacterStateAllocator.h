#pragma once

#include "PCH.h"
#include <EngineRelay/ER_API.h>

namespace EngineRelay::CharacterStateAllocator {

    // ── Logical state API ───────────────────────────────────────────────────

    /// Register a logical state. Returns a handle or kInvalidERState.
    /// @param priority  Activation priority (default 0 = kPriorityDefault).
    ERStateHandle RegisterLogicalState(const std::string& modName,
                                             const ERPhysicsCallbacks& callbacks,
                                             std::uint32_t priority = 0);

    /// Check if a logical state is registered for a named mod.
    bool IsLogicalStateRegistered(const std::string& modName);

    /// Get the handle for a named logical state, or kInvalidERState.
    ERStateHandle GetERStateHandle(const std::string& modName);

    /// The physical Havok slot reserved as the BSB host state.
    /// Always kUserState1.
    RE::hkpCharacterStateType GetHostSlot();

    /// Install the BSB host state into a character controller's state manager.
    /// Creates per-actor tracking state if needed.
    /// Safe to call multiple times for the same controller.
    bool InstallLogicalStateHost(RE::bhkCharacterController* controller);

    /// Returns true if a logical state with the given priority would be
    /// accepted right now for this controller (no strictly higher-priority
    /// state is currently active).  Does NOT modify any state.
    bool CanActivateLogicalState(RE::bhkCharacterController* controller,
                                 std::uint32_t priority);

    /// Set the active logical state for a controller.
    bool ActivateLogicalState(RE::bhkCharacterController* controller,
                              ERStateHandle handle);

    /// Clear the active logical state for a controller.
    void DeactivateLogicalState(RE::bhkCharacterController* controller);

    /// Get the active logical state handle for a controller.
    ERStateHandle GetActiveLogicalState(RE::bhkCharacterController* controller);

    /// Clear all per-actor host entries.  Call on kPreLoadGame / kNewGame so
    /// stale controller pointers are not reused after the game reloads.
    void ClearActorEntries();

}  // namespace EngineRelay::CharacterStateAllocator
