#pragma once

#include "PCH.h"
#include <BehaviorSwitchboard/BSB_API.h>

namespace BehaviorSwitchboard::CharacterStateAllocator {

    // ── Legacy multi-slot API (deprecated, kept for compatibility) ──────────

    /// Register a custom character state. Assigns the next free kUserState slot.
    /// Returns the assigned hkpCharacterStateType, or kTotal on failure.
    /// @deprecated Use RegisterLogicalState + ActivateLogicalState instead.
    RE::hkpCharacterStateType Register(const std::string& modName,
                                       const CharacterStateCallbacks& callbacks);

    /// Check if a character state is registered for a mod.
    bool IsRegistered(const std::string& modName);

    /// Get the assigned slot for a mod, or kTotal if not registered.
    RE::hkpCharacterStateType GetSlot(const std::string& modName);

    /// Install all registered character states into a character's state manager.
    /// @deprecated Use InstallLogicalStateHost instead.
    void InstallStates(RE::bhkCharacterController* controller);

    /// Get the number of registered character states (legacy + logical combined).
    size_t GetCount();

    // ── Logical state API ───────────────────────────────────────────────────

    /// Register a logical state. Returns a handle or kInvalidLogicalState.
    /// @param priority  Activation priority (default 0 = kPriorityDefault).
    LogicalStateHandle RegisterLogicalState(const std::string& modName,
                                             const CharacterStateCallbacks& callbacks,
                                             std::uint32_t priority = 0);

    /// Check if a logical state is registered for a named mod.
    bool IsLogicalStateRegistered(const std::string& modName);

    /// Get the handle for a named logical state, or kInvalidLogicalState.
    LogicalStateHandle GetLogicalStateHandle(const std::string& modName);

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
                              LogicalStateHandle handle);

    /// Clear the active logical state for a controller.
    void DeactivateLogicalState(RE::bhkCharacterController* controller);

    /// Get the active logical state handle for a controller.
    LogicalStateHandle GetActiveLogicalState(RE::bhkCharacterController* controller);

    /// Clear all per-actor host entries.  Call on kPreLoadGame / kNewGame so
    /// stale controller pointers are not reused after the game reloads.
    void ClearActorEntries();

}  // namespace BehaviorSwitchboard::CharacterStateAllocator
