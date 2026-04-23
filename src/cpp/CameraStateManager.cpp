#include "PCH.h"
#include "CameraStateManager.h"

#include <atomic>
#include <mutex>
#include <vector>

// ============================================================================
// CameraStateManager
//
// BSBCameraState is the single TESCameraState subclass BSB installs into
// PlayerCamera.  It dispatches Begin/End/Update to whichever logical camera
// state is currently active (s_activeHandle).
//
// Lifecycle
// ---------
//   Init()                      — allocates BSBCameraState once at plugin load
//   ActivateLogicalCameraState  — installs BSBCameraState into PlayerCamera
//                                 (if not already current) and sets active handle
//   DeactivateLogicalCameraState — clears handle, restores vanilla TPS unless
//                                  a preempted state is being restored
//   OnPreLoadGame               — forces cleanup before a new game loads
//
// Refcounting
// -----------
//   s_bsbCameraState holds one BSTSmartPointer reference for the session.
//   SetState() causes PlayerCamera to hold a second reference while active.
//   Restoring vanilla TPS drops PlayerCamera's reference; BSB's copy keeps
//   the object alive for future re-activation.
//
// Engine eviction invariant (Issue 1)
// ------------------------------------
//   s_activeHandle is valid if and only if BSBCameraState is the current
//   PlayerCamera state.  BSBCameraState::End() clears the active handle and
//   priority atomics after dispatching endFn, so any subsequent call to
//   ActivateLogicalCameraState() sees a clean slate rather than a stale handle
//   that incorrectly blocks lower-priority activations.
//   endFn is called on the render thread — mods must not call game-thread API
//   functions (e.g. ActivateLogicalCameraState) from within endFn.
//
// Preemption stack (Issue 6)
// --------------------------
//   s_preemptionStack holds displaced handles when a strictly higher-priority
//   activation preempts the current one.  DeactivateLogicalCameraState pops
//   the stack, restoring each layer in LIFO order.  Stack depth is capped at 8.
//   s_preemptionStack is guarded by s_camMutex; only game-thread code accesses
//   it.  End() intentionally skips the stack (render-thread safety) — the
//   stack is cleaned up by the next ActivateLogicalCameraState or
//   DeactivateLogicalCameraState call on the game thread.
// ============================================================================

namespace BehaviorSwitchboard::CameraStateManager {

    // =========================================================================
    // Logical camera state registry
    // =========================================================================

    struct LogicalCameraStateDef {
        std::string            modName;
        std::uint32_t          priority{ 0 };
        LogicalCameraCallbacks callbacks;
    };

    static std::mutex                            s_camMutex;
    static std::vector<LogicalCameraStateDef>    s_cameraStates;

    // ── Active-state atomics (render thread reads, game thread writes) ───────
    //
    // These are std::atomic so BSBCameraState::End() (render thread) can clear
    // them safely.  The preemption stack below is NOT atomic — only game-thread
    // code may access it (under s_camMutex).

    static std::atomic<LogicalCameraHandle>  s_activeHandle{ kInvalidLogicalCameraState };
    static std::atomic<std::uint32_t>        s_activePriority{ 0 };

    // ── Preemption stack (game thread only, guarded by s_camMutex) ───────────
    //
    // Each entry is a {handle, priority} pair for a displaced logical state.
    // Pushed by ActivateLogicalCameraState on strict preemption; popped by
    // DeactivateLogicalCameraState.  Capped at kPreemptionStackMaxDepth.

    static constexpr std::size_t kPreemptionStackMaxDepth = 8;

    using PreemptionEntry = std::pair<LogicalCameraHandle, std::uint32_t>;
    static std::vector<PreemptionEntry>  s_preemptionStack;

    // ── Singleton camera state object ─────────────────────────────────────────
    //
    // Held as a BSTSmartPointer so the object survives PlayerCamera swapping
    // it out (e.g. menu-open restores vanilla TPS) without being deleted.

    static RE::BSTSmartPointer<RE::TESCameraState> s_bsbCameraState;

    // ── Saved TPS slot snapshot ───────────────────────────────────────────────
    //
    // Captured at first ActivateLogicalCameraState call for the session
    // (when s_activeHandle was kInvalid) so RestoreVanillaTPS uses the value
    // that was live at engagement time rather than the potentially modified
    // cameraStates[kThirdPerson] slot.
    // Cleared by RestoreVanillaTPS after use and by OnPreLoadGame.

    static RE::BSTSmartPointer<RE::TESCameraState> s_savedTPS;

    // =========================================================================
    // BSBCameraState — the singleton TESCameraState subclass
    //
    // Update() is called by the engine each frame while this is the active
    // camera state.  It dispatches to the registered updateFn, passing the
    // owning TESCamera pointer and the current frame delta time.
    //
    // Begin() / End() are called by PlayerCamera when this state becomes
    // active or is replaced.  They dispatch to beginFn / endFn respectively
    // so consuming mods can react to engine-driven state changes (e.g. a menu
    // opening that forces vanilla TPS back in).
    //
    // GetTranslation / GetRotation return the last cached world-space values
    // written by the most recent Update() for use by audio and screen-effects
    // subsystems (they do NOT move the camera root — Update() does that).
    // =========================================================================

    class BSBCameraState : public RE::TESCameraState {
    public:
        RE::NiPoint3  m_lastPos{};  // world-space position — cached each Update()
        RE::NiMatrix3 m_lastRot{};  // world-space orientation — cached each Update()

        // Called by the engine when this state becomes the active camera state.
        void Begin() override
        {
            const LogicalCameraHandle h =
                s_activeHandle.load(std::memory_order_relaxed);
            if (h == kInvalidLogicalCameraState) return;
            // s_cameraStates is append-only after kPostLoad — safe without lock.
            if (h >= s_cameraStates.size()) return;
            const auto& def = s_cameraStates[h];
            if (def.callbacks.beginFn) {
                def.callbacks.beginFn(camera, def.callbacks.userData);
            }
        }

        // Called by the engine when another state replaces this one.
        // Dispatches endFn so consuming mods learn the camera was taken away.
        // After endFn returns, the active handle and priority are cleared so
        // that a subsequent ActivateLogicalCameraState() on the game thread is
        // not wrongly blocked by the now-stale handle.
        //
        // NOTE: endFn is invoked on the render thread.  Mods must not call any
        // BSB game-thread API (e.g. ActivateLogicalCameraState) from within endFn.
        void End() override
        {
            const LogicalCameraHandle h =
                s_activeHandle.load(std::memory_order_relaxed);
            if (h == kInvalidLogicalCameraState) return;
            if (h >= s_cameraStates.size()) return;
            const auto& def = s_cameraStates[h];
            if (def.callbacks.endFn) {
                def.callbacks.endFn(camera, def.callbacks.userData);
            }
            // Clear handle and priority atomics so s_activeHandle is valid if
            // and only if BSBCameraState is the current PlayerCamera state.
            // The preemption stack (s_preemptionStack) is NOT cleared here —
            // it is mutex-guarded and must not be touched from the render thread.
            // It will be cleaned up by the next game-thread Activate/Deactivate call.
            s_activeHandle.store(kInvalidLogicalCameraState, std::memory_order_relaxed);
            s_activePriority.store(0, std::memory_order_relaxed);
        }

        // Called each frame by the engine while this is the active camera state.
        // Dispatches to the active mod's updateFn, then caches cameraRoot's
        // resulting world-space transform for GetTranslation / GetRotation.
        // After the updateFn returns, writes PlayerCamera::yaw so the HUD
        // compass stays in sync.  If the mod provides compassYawFn, that
        // override is used; otherwise yaw is derived from the camera root's
        // forward vector (cameraRoot->world.rotate column 1: fwd.x, fwd.y).
        void Update(RE::BSTSmartPointer<RE::TESCameraState>& /*a_next*/) override
        {
            if (!camera) return;

            const LogicalCameraHandle h =
                s_activeHandle.load(std::memory_order_relaxed);
            if (h == kInvalidLogicalCameraState) return;
            if (h >= s_cameraStates.size()) return;

            const auto& def = s_cameraStates[h];
            if (def.callbacks.updateFn) {
                const float dt = RE::Main::QFrameAnimTime();
                def.callbacks.updateFn(camera, dt, def.callbacks.userData);

                // Cache the resulting world-space transform so that
                // GetTranslation / GetRotation return sensible values for
                // audio listener positioning, screen-space effects, etc.
                if (camera->cameraRoot) {
                    m_lastPos = camera->cameraRoot->world.translate;
                    m_lastRot = camera->cameraRoot->world.rotate;
                }

                // Write PlayerCamera::yaw so the HUD compass tracks correctly.
                // ThirdPersonState::Update() normally owns this write; when
                // BSBCameraState is active we must do it ourselves.
                auto* pcam = static_cast<RE::PlayerCamera*>(camera);
                float yaw;
                if (def.callbacks.compassYawFn) {
                    yaw = def.callbacks.compassYawFn(camera, def.callbacks.userData);
                } else if (camera->cameraRoot) {
                    // Derive yaw from the forward column (col 1) of the world rotation.
                    // entry[row][col]: col 1 = fwd; atan2(fwd.x, fwd.y) gives yaw.
                    const auto& rot = m_lastRot;
                    yaw = std::atan2(rot.entry[0][1], rot.entry[1][1]);
                } else {
                    yaw = pcam->yaw;  // no root yet — leave unchanged
                }
                pcam->yaw = yaw;
            }
        }

        void GetTranslation(RE::NiPoint3& a_out)  override { a_out = m_lastPos; }
        void GetRotation(RE::NiQuaternion& a_out) override { a_out = RE::NiQuaternion(m_lastRot); }

        void SaveGame(RE::BGSSaveFormBuffer*)  override {}
        void LoadGame(RE::BGSLoadFormBuffer*)  override {}
        void Revert(RE::BGSLoadFormBuffer*)    override {}
    };

    // =========================================================================
    // Internal helpers
    // =========================================================================

    /// Restore the vanilla ThirdPersonState in PlayerCamera if BSBCameraState
    /// is currently installed.  Called when no logical state remains active.
    /// Uses s_savedTPS (captured at engagement time) if available, so the
    /// correct slot is restored even if another mod modified cameraStates after
    /// we installed BSBCameraState.  Clears s_savedTPS after use.
    static void RestoreVanillaTPS()
    {
        auto* pcam = RE::PlayerCamera::GetSingleton();
        if (!pcam || !s_bsbCameraState) return;

        // Only restore if BSBCameraState is actually the current state.
        if (pcam->currentState.get() != s_bsbCameraState.get()) return;

        RE::TESCameraState* toRestore =
            s_savedTPS ? s_savedTPS.get()
                       : pcam->cameraStates[RE::CameraState::kThirdPerson].get();

        if (toRestore) {
            pcam->SetState(toRestore);
            LOG_INFO("CameraStateManager: vanilla ThirdPersonState restored.");
        }

        s_savedTPS = {};  // clear snapshot — next session will re-capture
    }

    // =========================================================================
    // Public interface
    // =========================================================================

    void Init()
    {
        auto* raw = new BSBCameraState();
        raw->id   = RE::CameraState::kAnimated;
        // Wrap in BSTSmartPointer explicitly; constructor calls IncRefCount.
        s_bsbCameraState = RE::BSTSmartPointer<RE::TESCameraState>{ static_cast<RE::TESCameraState*>(raw) };
        LOG_INFO("CameraStateManager: BSBCameraState allocated (id=kAnimated).");
    }

    LogicalCameraHandle RegisterLogicalCameraState(
        const std::string&            modName,
        const LogicalCameraCallbacks& callbacks,
        std::uint32_t                 priority)
    {
        if (modName.empty()) {
            LOG_ERROR("CameraStateManager: RegisterLogicalCameraState — empty modName.");
            return kInvalidLogicalCameraState;
        }
        if (!callbacks.updateFn) {
            LOG_ERROR("CameraStateManager: '{}' — updateFn is required.", modName);
            return kInvalidLogicalCameraState;
        }

        std::lock_guard lock(s_camMutex);

        // Duplicate check — return existing handle.
        for (std::size_t i = 0; i < s_cameraStates.size(); ++i) {
            if (s_cameraStates[i].modName == modName) {
                LOG_WARN("CameraStateManager: logical camera state '{}' already "
                         "registered at handle {}.", modName, i);
                return static_cast<LogicalCameraHandle>(i);
            }
        }

        const LogicalCameraHandle handle =
            static_cast<LogicalCameraHandle>(s_cameraStates.size());

        LogicalCameraStateDef def;
        def.modName   = modName;
        def.priority  = priority;
        def.callbacks = callbacks;
        s_cameraStates.push_back(std::move(def));

        LOG_INFO("CameraStateManager: logical camera state '{}' registered "
                 "as handle {} (priority={}).", modName, handle, priority);
        return handle;
    }

    bool IsLogicalCameraStateRegistered(const std::string& modName)
    {
        std::lock_guard lock(s_camMutex);
        for (const auto& def : s_cameraStates) {
            if (def.modName == modName) return true;
        }
        return false;
    }

    LogicalCameraHandle GetLogicalCameraHandle(const std::string& modName)
    {
        std::lock_guard lock(s_camMutex);
        for (std::size_t i = 0; i < s_cameraStates.size(); ++i) {
            if (s_cameraStates[i].modName == modName)
                return static_cast<LogicalCameraHandle>(i);
        }
        return kInvalidLogicalCameraState;
    }

    bool CanActivateLogicalCameraState(std::uint32_t priority)
    {
        const LogicalCameraHandle currentHandle =
            s_activeHandle.load(std::memory_order_relaxed);
        if (currentHandle == kInvalidLogicalCameraState) return true;
        const std::uint32_t currentPriority =
            s_activePriority.load(std::memory_order_relaxed);
        return priority >= currentPriority;
    }

    bool ActivateLogicalCameraState(LogicalCameraHandle handle)
    {
        if (handle == kInvalidLogicalCameraState) {
            LOG_WARN("CameraStateManager: ActivateLogicalCameraState — invalid handle.");
            return false;
        }

        std::uint32_t incomingPriority;
        {
            std::lock_guard lk(s_camMutex);
            if (handle >= s_cameraStates.size()) {
                LOG_WARN("CameraStateManager: ActivateLogicalCameraState — "
                         "handle {} out of range.", handle);
                return false;
            }
            incomingPriority = s_cameraStates[handle].priority;
        }

        // Priority check: refuse if a strictly higher-priority state is active.
        const LogicalCameraHandle currentHandle =
            s_activeHandle.load(std::memory_order_relaxed);
        const std::uint32_t currentPriority =
            s_activePriority.load(std::memory_order_relaxed);

        if (currentHandle != kInvalidLogicalCameraState &&
            incomingPriority < currentPriority)
        {
            LOG_WARN("CameraStateManager: ActivateLogicalCameraState — "
                     "handle {} (priority={}) blocked by active handle {} (priority={}).",
                     handle, incomingPriority, currentHandle, currentPriority);
            return false;
        }

        {
            std::lock_guard lk(s_camMutex);

            if (currentHandle != kInvalidLogicalCameraState &&
                incomingPriority > currentPriority)
            {
                // Strict preemption — push the displaced handle onto the stack.
                if (s_preemptionStack.size() < kPreemptionStackMaxDepth) {
                    s_preemptionStack.emplace_back(currentHandle, currentPriority);
                    LOG_INFO("CameraStateManager: preempted handle {} (priority={}) "
                             "pushed to stack (depth={}).",
                             currentHandle, currentPriority, s_preemptionStack.size());
                } else {
                    LOG_WARN("CameraStateManager: preemption stack depth limit ({}) "
                             "reached — displaced handle {} dropped.",
                             kPreemptionStackMaxDepth, currentHandle);
                }
            } else {
                // Equal priority or first activation — any stale stack entries are
                // no longer relevant (e.g. End() fired and cleared the active handle).
                if (!s_preemptionStack.empty()) {
                    LOG_INFO("CameraStateManager: clearing stale preemption stack "
                             "({} entries) on non-preempting activation.",
                             s_preemptionStack.size());
                    s_preemptionStack.clear();
                }
            }
        }

        s_activeHandle.store(handle, std::memory_order_relaxed);
        s_activePriority.store(incomingPriority, std::memory_order_relaxed);

        // Install BSBCameraState into PlayerCamera if not already current.
        auto* pcam = RE::PlayerCamera::GetSingleton();
        if (pcam && s_bsbCameraState) {
            // On first activation for this session (going from no active state),
            // snapshot the TPS slot so RestoreVanillaTPS uses the engagement-time
            // value even if another mod modifies cameraStates[kThirdPerson] later.
            if (currentHandle == kInvalidLogicalCameraState && !s_savedTPS) {
                s_savedTPS = pcam->cameraStates[RE::CameraState::kThirdPerson];
            }
            // Set the camera back-pointer on BSBCameraState so Begin()/Update()/End()
            // always receive a valid TESCamera*.
            // TESCameraState::camera is set by the engine at startup for the
            // pre-allocated vanilla cameraStates[] entries, but NOT for
            // externally-allocated states like BSBCameraState.  The old per-mod
            // custom states (FlightCamState, DialogueCameraState) set this explicitly
            // before every SetState() call — we must do the same here.
            s_bsbCameraState->camera = pcam;
            if (pcam->currentState.get() != s_bsbCameraState.get()) {
                pcam->SetState(s_bsbCameraState.get());
            }
        }

        LOG_INFO("CameraStateManager: logical camera state handle {} activated "
                 "(priority={}).", handle, incomingPriority);
        return true;
    }

    void DeactivateLogicalCameraState()
    {
        // If the engine already evicted BSBCameraState (End() cleared the active
        // handle), the preemption stack is stale — clean it up and return.
        // RestoreVanillaTPS is not needed here since the engine already swapped
        // the state out; calling SetState again from this path would be re-entrant.
        if (s_activeHandle.load(std::memory_order_relaxed) == kInvalidLogicalCameraState) {
            std::lock_guard lk(s_camMutex);
            if (!s_preemptionStack.empty()) {
                LOG_INFO("CameraStateManager: DeactivateLogicalCameraState called with "
                         "no active state — clearing stale stack ({} entries).",
                         s_preemptionStack.size());
                s_preemptionStack.clear();
            }
            return;
        }

        PreemptionEntry top{ kInvalidLogicalCameraState, 0 };
        {
            std::lock_guard lk(s_camMutex);
            if (!s_preemptionStack.empty()) {
                top = s_preemptionStack.back();
                s_preemptionStack.pop_back();
            }
        }

        if (top.first != kInvalidLogicalCameraState) {
            // Restore the preempted lower-priority state.
            // BSBCameraState stays in PlayerCamera — no TPS swap needed.
            s_activeHandle.store(top.first, std::memory_order_relaxed);
            s_activePriority.store(top.second, std::memory_order_relaxed);
            LOG_INFO("CameraStateManager: preempted handle {} (priority={}) restored.",
                top.first, top.second);
            return;
        }

        // Stack empty — dispatch endFn BEFORE clearing s_activeHandle so the
        // callback can identify the dying state and perform any necessary cleanup.
        // BSBCameraState::End() will fire inside RestoreVanillaTPS() → SetState(),
        // but by then s_activeHandle is already kInvalidLogicalCameraState so
        // it becomes a no-op — this explicit dispatch is the authoritative one.
        {
            const LogicalCameraHandle dying =
                s_activeHandle.load(std::memory_order_relaxed);
            if (dying != kInvalidLogicalCameraState) {
                // s_cameraStates is append-only after kPostLoad — safe without lock.
                if (dying < s_cameraStates.size()) {
                    const auto& def = s_cameraStates[dying];
                    if (def.callbacks.endFn) {
                        auto* pcam = RE::PlayerCamera::GetSingleton();
                        if (pcam) def.callbacks.endFn(pcam, def.callbacks.userData);
                    }
                }
            }
        }

        s_activeHandle.store(kInvalidLogicalCameraState, std::memory_order_relaxed);
        s_activePriority.store(0, std::memory_order_relaxed);

        // RestoreVanillaTPS → SetState → BSBCameraState::End() fires, but
        // s_activeHandle is now kInvalidLogicalCameraState so it is a no-op.
        RestoreVanillaTPS();
        LOG_INFO("CameraStateManager: logical camera state deactivated.");
    }

    LogicalCameraHandle GetActiveLogicalCameraState()
    {
        return s_activeHandle.load(std::memory_order_relaxed);
    }

    void OnPreLoadGame()
    {
        // Unconditionally clear all handles — the game is about to reload.
        s_activeHandle.store(kInvalidLogicalCameraState, std::memory_order_relaxed);
        s_activePriority.store(0, std::memory_order_relaxed);

        {
            std::lock_guard lk(s_camMutex);
            s_preemptionStack.clear();
        }

        s_savedTPS = {};  // next session will re-capture at ActivateLogicalCameraState

        // Restore vanilla TPS if BSBCameraState is currently installed.
        RestoreVanillaTPS();

        LOG_INFO("CameraStateManager: handles cleared on pre-load-game.");
    }

}  // namespace BehaviorSwitchboard::CameraStateManager
