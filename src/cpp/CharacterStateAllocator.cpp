#include "PCH.h"
#include "CharacterStateAllocator.h"

#include <array>
#include <atomic>
#include <unordered_map>

namespace BehaviorSwitchboard::CharacterStateAllocator {

    // =========================================================================
    // BSBHostStateObj — raw memory layout for the single BSB host state
    //
    // One instance is allocated per actor (per bhkCharacterController) rather
    // than shared across all actors.  The vtable is shared; the per-actor
    // ActorHostData pointer embedded in the object lets the trampolines locate
    // the correct active logical state for the calling character without a
    // global table lookup.
    //
    // Memory layout mirrors the previous BSBCharacterStateObj but replaces the
    // embedded CharacterStateCallbacks with a lean ActorHostData pointer.
    // =========================================================================

    /// Per-actor logical-state selector.  Written by ActivateLogicalState /
    /// DeactivateLogicalState from the game thread; read by Havok callbacks on
    /// the physics thread — hence the atomics.
    struct ActorHostData {
        std::atomic<LogicalStateHandle> activeHandle{ kInvalidLogicalState };
        std::atomic<LogicalStateHandle> preemptedHandle{ kInvalidLogicalState };
        std::atomic<std::uint32_t>      activeHandlePriority{ 0 };
    };

    struct BSBHostStateObj
    {
        // --- hkReferencedObject (0x10) ---
        void*         vtable;             // 0x00
        std::uint16_t memSizeAndFlags;    // 0x08
        std::uint16_t referenceCount;     // 0x0A
        std::uint32_t pad0C;              // 0x0C

        // --- BSB extension ---
        RE::hkpCharacterStateType slot;   // 0x10  always kUserState1
        std::uint32_t             pad14;  // 0x14
        ActorHostData*            hostData;  // 0x18  per-actor; never null when installed
    };

    // ── Self-cast helpers ────────────────────────────────────────────────────
    static BSBHostStateObj* Self(RE::hkpCharacterState* base) {
        return reinterpret_cast<BSBHostStateObj*>(base);
    }
    static const BSBHostStateObj* Self(const RE::hkpCharacterState* base) {
        return reinterpret_cast<const BSBHostStateObj*>(base);
    }

    // =========================================================================
    // Logical state registry (global, immutable after kPostLoad)
    // =========================================================================

    struct LogicalStateDef {
        std::string             modName;
        std::uint32_t           priority{ 0 };
        CharacterStateCallbacks callbacks;
    };

    static std::mutex                    s_logicalMutex;
    static std::vector<LogicalStateDef>  s_logicalStates;

    // =========================================================================
    // Per-actor tracking
    //
    // Keyed by bhkCharacterController*.  unordered_map guarantees that
    // references to existing elements are stable after insertions (C++11 §23).
    // BSBHostStateObj.hostData always points into a heap-allocated ActorHostData
    // so that ClearActorEntries() can drop the map without dangling the pointer
    // stored in an already-installed stateObj.
    // =========================================================================

    struct ActorEntry {
        ActorHostData*   hostData{ nullptr };   // heap-allocated, lifetime = game session
        BSBHostStateObj* hostObj{ nullptr };    // heap-allocated, lifetime = game session
        bool             installed{ false };
    };

    static std::mutex                                              s_actorMutex;
    static std::unordered_map<RE::bhkCharacterController*, ActorEntry> s_actorEntries;

    // Lifetime tracking for heap objects (matches existing BehaviorPatcher pattern).
    static std::vector<void*> s_allocations;

    // =========================================================================
    // Legacy registration storage (deprecated multi-slot path)
    //
    // Kept so that old consumers calling RegisterCharacterState still compile
    // and return kUserState1.  The callbacks they provide are forwarded as
    // logical state registrations.
    // =========================================================================

    struct LegacySlotEntry {
        std::string               modName;
        LogicalStateHandle        logicalHandle{ kInvalidLogicalState };
        RE::hkpCharacterStateType slot{ RE::hkpCharacterStateType::kTotal };
    };

    static std::mutex                 s_legacyMutex;
    static std::vector<LegacySlotEntry> s_legacyEntries;

    // =========================================================================
    // Custom vtable (shared by all BSBHostStateObj instances)
    //
    // Trampolines: look up ActorHostData.activeHandle, find the LogicalStateDef,
    // and dispatch to that def's callbacks with the mod's own userData.
    //
    // Vtable indices (MSVC x64, bhkCharacterState):
    //   00 = destructor
    //   01 = deleting destructor
    //   02 = Unk_02
    //   03 = GetType
    //   04 = EnterState
    //   05 = LeaveState
    //   06 = Update
    //   07 = Change
    //   08 = Unk_08 (bhkCharacterState extra)
    // =========================================================================

    static constexpr int kVtableSize = 16;
    static std::uintptr_t s_customVtable[kVtableSize] = {};
    static bool           s_vtableBuilt = false;

    // ── Trampoline helpers ───────────────────────────────────────────────────

    /// Look up the active LogicalStateDef for an ActorHostData.
    /// Returns nullptr if no state is active or the handle is out of range.
    static const LogicalStateDef* GetActiveDef(const ActorHostData* hostData)
    {
        if (!hostData) return nullptr;
        const LogicalStateHandle h = hostData->activeHandle.load(std::memory_order_relaxed);
        if (h == kInvalidLogicalState) return nullptr;
        // s_logicalStates is append-only after kPostLoad — safe to read without lock.
        if (h >= s_logicalStates.size()) return nullptr;
        return &s_logicalStates[h];
    }

    // ── Trampolines ──────────────────────────────────────────────────────────

    static RE::hkpCharacterStateType Trampoline_GetType(const RE::hkpCharacterState* a_this)
    {
        return Self(a_this)->slot;  // always kUserState1
    }

    static void Trampoline_EnterState(RE::hkpCharacterState* a_this,
        RE::hkpCharacterContext& ctx, RE::hkpCharacterStateType prevState,
        const RE::hkpCharacterInput& input, RE::hkpCharacterOutput& output)
    {
        auto* def = GetActiveDef(Self(a_this)->hostData);
        if (def && def->callbacks.enterFn) {
            def->callbacks.enterFn(ctx, prevState, input, output, def->callbacks.userData);
        }
    }

    static void Trampoline_LeaveState(RE::hkpCharacterState* a_this,
        RE::hkpCharacterContext& ctx, RE::hkpCharacterStateType nextState,
        const RE::hkpCharacterInput& input, RE::hkpCharacterOutput& output)
    {
        auto* def = GetActiveDef(Self(a_this)->hostData);
        if (def && def->callbacks.leaveFn) {
            def->callbacks.leaveFn(ctx, nextState, input, output, def->callbacks.userData);
        }
    }

    static void Trampoline_Update(RE::hkpCharacterState* a_this,
        RE::hkpCharacterContext& ctx, const RE::hkpCharacterInput& input,
        RE::hkpCharacterOutput& output)
    {
        auto* def = GetActiveDef(Self(a_this)->hostData);
        if (def && def->callbacks.updateFn) {
            def->callbacks.updateFn(ctx, input, output, def->callbacks.userData);
        }
    }

    static void Trampoline_Change(RE::hkpCharacterState* a_this,
        RE::hkpCharacterContext& ctx, const RE::hkpCharacterInput& input,
        RE::hkpCharacterOutput& output)
    {
        auto* def = GetActiveDef(Self(a_this)->hostData);
        if (def && def->callbacks.changeFn) {
            def->callbacks.changeFn(ctx, input, output, def->callbacks.userData);
        }
    }

    static void Trampoline_Unk08(RE::hkpCharacterState* /*a_this*/) {}

    // ── Vtable construction ──────────────────────────────────────────────────

    static void BuildCustomVtable()
    {
        if (s_vtableBuilt) return;

        REL::Relocation<std::uintptr_t*> flyingVtbl{ RE::VTABLE_bhkCharacterStateFlying[0] };
        auto* srcVtable = flyingVtbl.get();

        for (int i = 0; i < kVtableSize; ++i) {
            s_customVtable[i] = srcVtable[i];
        }

        s_customVtable[0x03] = reinterpret_cast<std::uintptr_t>(&Trampoline_GetType);
        s_customVtable[0x04] = reinterpret_cast<std::uintptr_t>(&Trampoline_EnterState);
        s_customVtable[0x05] = reinterpret_cast<std::uintptr_t>(&Trampoline_LeaveState);
        s_customVtable[0x06] = reinterpret_cast<std::uintptr_t>(&Trampoline_Update);
        s_customVtable[0x07] = reinterpret_cast<std::uintptr_t>(&Trampoline_Change);
        s_customVtable[0x08] = reinterpret_cast<std::uintptr_t>(&Trampoline_Unk08);

        s_vtableBuilt = true;
        LOG_INFO("CharacterStateAllocator: custom vtable built from bhkCharacterStateFlying.");
    }

    // ── Object construction ──────────────────────────────────────────────────

    static BSBHostStateObj* BuildHostObj(ActorHostData* hostData)
    {
        BuildCustomVtable();

        auto* obj = static_cast<BSBHostStateObj*>(std::calloc(1, sizeof(BSBHostStateObj)));
        if (!obj) {
            LOG_ERROR("CharacterStateAllocator: failed to allocate BSBHostStateObj.");
            return nullptr;
        }
        s_allocations.push_back(obj);

        obj->vtable          = &s_customVtable[0];
        obj->memSizeAndFlags = 0x10;
        obj->referenceCount  = 1;
        obj->pad0C           = 0;
        obj->slot            = RE::hkpCharacterStateType::kUserState1;
        obj->hostData        = hostData;

        return obj;
    }

    // =========================================================================
    // Public interface — logical state API
    // =========================================================================

    LogicalStateHandle RegisterLogicalState(const std::string& modName,
                                             const CharacterStateCallbacks& callbacks,
                                             std::uint32_t priority)
    {
        if (!callbacks.updateFn || !callbacks.changeFn) {
            LOG_ERROR("CharacterStateAllocator: '{}' — updateFn and changeFn are required.",
                modName);
            return kInvalidLogicalState;
        }

        std::lock_guard lock(s_logicalMutex);

        // Duplicate check.
        for (std::size_t i = 0; i < s_logicalStates.size(); ++i) {
            if (s_logicalStates[i].modName == modName) {
                LOG_WARN("CharacterStateAllocator: logical state '{}' already registered "
                         "at handle {}.", modName, i);
                return static_cast<LogicalStateHandle>(i);
            }
        }

        const LogicalStateHandle handle =
            static_cast<LogicalStateHandle>(s_logicalStates.size());

        LogicalStateDef def;
        def.modName   = modName;
        def.priority  = priority;
        def.callbacks = callbacks;
        s_logicalStates.push_back(std::move(def));

        LOG_INFO("CharacterStateAllocator: logical state '{}' registered as handle {} "
                 "(priority={}).", modName, handle, priority);
        return handle;
    }

    bool IsLogicalStateRegistered(const std::string& modName)
    {
        std::lock_guard lock(s_logicalMutex);
        for (const auto& def : s_logicalStates) {
            if (def.modName == modName) return true;
        }
        return false;
    }

    LogicalStateHandle GetLogicalStateHandle(const std::string& modName)
    {
        std::lock_guard lock(s_logicalMutex);
        for (std::size_t i = 0; i < s_logicalStates.size(); ++i) {
            if (s_logicalStates[i].modName == modName)
                return static_cast<LogicalStateHandle>(i);
        }
        return kInvalidLogicalState;
    }

    RE::hkpCharacterStateType GetHostSlot()
    {
        return RE::hkpCharacterStateType::kUserState1;
    }

    bool InstallLogicalStateHost(RE::bhkCharacterController* controller)
    {
        if (!controller) return false;

        auto* stateManager = const_cast<RE::hkpCharacterStateManager*>(
            controller->context.stateManager);
        if (!stateManager) {
            LOG_WARN("CharacterStateAllocator: controller has no stateManager.");
            return false;
        }

        std::lock_guard lock(s_actorMutex);

        // Already installed?
        auto it = s_actorEntries.find(controller);
        if (it != s_actorEntries.end() && it->second.installed) {
            LOG_DEBUG("CharacterStateAllocator: logical state host already installed "
                      "for this controller — skipping.");
            return true;
        }

        // Allocate per-actor host data (heap, lifetime = game session).
        auto* hostData = static_cast<ActorHostData*>(
            std::calloc(1, sizeof(ActorHostData)));
        if (!hostData) {
            LOG_ERROR("CharacterStateAllocator: failed to allocate ActorHostData.");
            return false;
        }
        new (hostData) ActorHostData();   // construct in-place (initialises atomic)
        s_allocations.push_back(hostData);

        // Build the per-actor host state object.
        auto* hostObj = BuildHostObj(hostData);
        if (!hostObj) {
            // hostData already tracked; just return failure
            return false;
        }

        // Install at kUserState1.
        const std::uint32_t slotIdx =
            static_cast<std::uint32_t>(RE::hkpCharacterStateType::kUserState1);
        auto* existingSlot = stateManager->registeredState[slotIdx];
        if (existingSlot) {
            // If the slot already holds one of our own BSBHostStateObj instances
            // (identified by vtable pointer), adopt it for this controller rather
            // than allocating a duplicate.  This happens when TrueFlight calls
            // InstallLogicalStateHost at kPostLoadGame and then again from
            // FlightPhysicsController::Engage — the bhkCharacterController pointer
            // may differ between the two calls (Havok re-initialises physics), so
            // the s_actorEntries guard above misses the second call even though the
            // state manager already has our object installed.
            auto* existing = reinterpret_cast<BSBHostStateObj*>(existingSlot);
            if (existing->vtable == &s_customVtable[0]) {
                ActorEntry& entry = s_actorEntries[controller];
                entry.hostData  = existing->hostData;
                entry.hostObj   = existing;
                entry.installed = true;
                LOG_DEBUG("CharacterStateAllocator: existing BSB host state adopted "
                          "for new controller — no re-install needed.");
                return true;
            }
            LOG_WARN("CharacterStateAllocator: kUserState1 occupied by unknown state — overwriting.");
        }
        stateManager->registeredState[slotIdx] =
            reinterpret_cast<RE::hkpCharacterState*>(hostObj);

        ActorEntry& entry = s_actorEntries[controller];
        entry.hostData  = hostData;
        entry.hostObj   = hostObj;
        entry.installed = true;

        LOG_INFO("CharacterStateAllocator: BSB logical-state host installed at kUserState1.");
        return true;
    }

    bool CanActivateLogicalState(RE::bhkCharacterController* controller,
                                 std::uint32_t priority)
    {
        if (!controller) return false;

        std::lock_guard lock(s_actorMutex);
        const auto it = s_actorEntries.find(controller);
        if (it == s_actorEntries.end() || !it->second.hostData) {
            // Host not yet installed — priority check is vacuously true.
            // ActivateLogicalState will fail for a different reason if called;
            // Activate() installs the host before calling it.
            return true;
        }
        const ActorHostData* hostData = it->second.hostData;
        const LogicalStateHandle currentHandle =
            hostData->activeHandle.load(std::memory_order_relaxed);
        if (currentHandle == kInvalidLogicalState) return true;
        const std::uint32_t currentPriority =
            hostData->activeHandlePriority.load(std::memory_order_relaxed);
        return priority >= currentPriority;
    }

    bool ActivateLogicalState(RE::bhkCharacterController* controller,
                              LogicalStateHandle handle)
    {
        if (!controller) return false;
        if (handle == kInvalidLogicalState) return false;

        std::uint32_t incomingPriority = 0;
        {
            std::lock_guard lk(s_logicalMutex);
            if (handle >= s_logicalStates.size()) {
                LOG_WARN("CharacterStateAllocator: ActivateLogicalState — "
                         "handle {} out of range.", handle);
                return false;
            }
            incomingPriority = s_logicalStates[handle].priority;
        }

        std::lock_guard lock(s_actorMutex);
        auto it = s_actorEntries.find(controller);
        if (it == s_actorEntries.end() || !it->second.hostData) {
            LOG_WARN("CharacterStateAllocator: ActivateLogicalState — "
                     "controller not tracked. Call InstallLogicalStateHost first.");
            return false;
        }

        ActorHostData* hostData = it->second.hostData;

        // Priority check: refuse if a strictly higher-priority state is active.
        const std::uint32_t currentPriority =
            hostData->activeHandlePriority.load(std::memory_order_relaxed);
        const LogicalStateHandle currentHandle =
            hostData->activeHandle.load(std::memory_order_relaxed);

        if (currentHandle != kInvalidLogicalState &&
            incomingPriority < currentPriority)
        {
            LOG_WARN("CharacterStateAllocator: ActivateLogicalState — "
                     "handle {} (priority={}) blocked by active handle {} (priority={}).",
                     handle, incomingPriority, currentHandle, currentPriority);
            return false;
        }

        // Snapshot the displaced handle so DeactivateLogicalState can restore it.
        // Only save if the incoming priority is strictly higher (a true preemption).
        if (currentHandle != kInvalidLogicalState &&
            incomingPriority > currentPriority)
        {
            hostData->preemptedHandle.store(currentHandle, std::memory_order_relaxed);
            LOG_INFO("CharacterStateAllocator: preempted handle {} (priority={}) "
                     "saved — will restore on deactivation.", currentHandle, currentPriority);
        } else {
            // Equal priority or no current state: clear any stale preempted handle.
            hostData->preemptedHandle.store(kInvalidLogicalState, std::memory_order_relaxed);
        }

        hostData->activeHandle.store(handle, std::memory_order_relaxed);
        hostData->activeHandlePriority.store(incomingPriority, std::memory_order_relaxed);

        LOG_INFO("CharacterStateAllocator: logical state handle {} activated (priority={}).",
            handle, incomingPriority);
        return true;
    }

    void DeactivateLogicalState(RE::bhkCharacterController* controller)
    {
        if (!controller) return;

        std::lock_guard lock(s_actorMutex);
        auto it = s_actorEntries.find(controller);
        if (it != s_actorEntries.end() && it->second.hostData) {
            ActorHostData* hostData = it->second.hostData;

            // Restore a preempted lower-priority state if one was saved.
            const LogicalStateHandle preempted =
                hostData->preemptedHandle.load(std::memory_order_relaxed);

            if (preempted != kInvalidLogicalState) {
                // s_logicalStates is append-only after kPostLoad; no lock needed
                // for a stable index read during gameplay.
                std::uint32_t restoredPriority = 0;
                if (preempted < s_logicalStates.size()) {
                    restoredPriority = s_logicalStates[preempted].priority;
                }
                hostData->activeHandle.store(preempted, std::memory_order_relaxed);
                hostData->activeHandlePriority.store(restoredPriority, std::memory_order_relaxed);
                hostData->preemptedHandle.store(kInvalidLogicalState, std::memory_order_relaxed);
                LOG_INFO("CharacterStateAllocator: preempted handle {} (priority={}) "
                         "restored on deactivation.", preempted, restoredPriority);
                return;
            }

            // No preempted state — just clear.
            hostData->activeHandle.store(kInvalidLogicalState, std::memory_order_relaxed);
            hostData->activeHandlePriority.store(0, std::memory_order_relaxed);
            hostData->preemptedHandle.store(kInvalidLogicalState, std::memory_order_relaxed);
            LOG_INFO("CharacterStateAllocator: logical state deactivated.");
        }
    }

    LogicalStateHandle GetActiveLogicalState(RE::bhkCharacterController* controller)
    {
        if (!controller) return kInvalidLogicalState;
        std::lock_guard lock(s_actorMutex);
        auto it = s_actorEntries.find(controller);
        if (it == s_actorEntries.end() || !it->second.hostData)
            return kInvalidLogicalState;
        return it->second.hostData->activeHandle.load(std::memory_order_relaxed);
    }

    void ClearActorEntries()
    {
        std::lock_guard lock(s_actorMutex);
        // The ActorHostData and BSBHostStateObj pointers in each entry remain
        // valid (tracked in s_allocations) — we only drop the controller→entry
        // mapping.  New entries will be created when InstallLogicalStateHost is
        // called after the next game load.
        s_actorEntries.clear();
        LOG_INFO("CharacterStateAllocator: per-actor entries cleared.");
    }

    // =========================================================================
    // Public interface — legacy multi-slot API (compatibility shim)
    //
    // RegisterCharacterState callers receive kUserState1 (the fixed host slot)
    // and their callbacks are forwarded to RegisterLogicalState.  The caller's
    // ctrl->wantState = slot usage continues to work unchanged because the host
    // is always at kUserState1.
    // =========================================================================

    RE::hkpCharacterStateType Register(const std::string& modName,
                                        const CharacterStateCallbacks& callbacks)
    {
        // Forward to logical state registration.
        const LogicalStateHandle handle =
            ::BehaviorSwitchboard::CharacterStateAllocator::RegisterLogicalState(modName, callbacks);
        if (handle == kInvalidLogicalState) {
            return RE::hkpCharacterStateType::kTotal;
        }

        std::lock_guard lock(s_legacyMutex);

        // Already have a legacy entry?
        for (const auto& le : s_legacyEntries) {
            if (le.modName == modName) return le.slot;
        }

        LegacySlotEntry le;
        le.modName      = modName;
        le.logicalHandle = handle;
        le.slot          = RE::hkpCharacterStateType::kUserState1;
        s_legacyEntries.push_back(le);

        LOG_INFO("CharacterStateAllocator (legacy): '{}' mapped to logical handle {} "
                 "— returning kUserState1 for backward compatibility.", modName, handle);
        return RE::hkpCharacterStateType::kUserState1;
    }

    bool IsRegistered(const std::string& modName)
    {
        return IsLogicalStateRegistered(modName);
    }

    RE::hkpCharacterStateType GetSlot(const std::string& modName)
    {
        std::lock_guard lock(s_legacyMutex);
        for (const auto& le : s_legacyEntries) {
            if (le.modName == modName) return le.slot;
        }
        // If registered via new API only, still return kUserState1.
        if (IsLogicalStateRegistered(modName))
            return RE::hkpCharacterStateType::kUserState1;
        return RE::hkpCharacterStateType::kTotal;
    }

    void InstallStates(RE::bhkCharacterController* controller)
    {
        // Delegate to the new host-installation path.
        InstallLogicalStateHost(controller);
    }

    size_t GetCount()
    {
        std::lock_guard lk(s_logicalMutex);
        return s_logicalStates.size();
    }

}  // namespace BehaviorSwitchboard::CharacterStateAllocator
