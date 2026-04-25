#include "PCH.h"
#include "BehaviorFileInterceptor.h"
#include "BehaviorPatcher.h"
#include "CameraStateManager.h"
#include "CharacterStateAllocator.h"
#include "ConfigLoader.h"
#include <EngineRelay/ER_API.h>

// ============================================================================
// Engine Relay — SKSE Plugin Entry
//
// Non-destructive behavior graph augmentation. Hooks hkbBehaviorGraph::Activate
// to inject sub-behavior references into the graph before Havok builds its
// internal index maps.
//
// Mod authors register sub-behaviors via the C++ API (ER_API.h) or by
// dropping config files into Data/SKSE/Plugins/EngineRelay/.
// ============================================================================

namespace EngineRelay {

    // ── Action Gate storage ──
    /// Per-mod gate declarations.  Populated by RegisterActionGates() and by
    /// Register() when a Registration carries a non-empty actionGates field.
    static std::mutex                                               s_gateMutex;
    static std::unordered_map<std::string, std::vector<ActionGate>> s_gateRegistrations;

    // ── Public API implementation ──

    bool Register(const Registration& reg)
    {
        // ── Required fields ──
        if (reg.modName.empty() || reg.behaviorPath.empty() || reg.eventName.empty() ||
            reg.graphName.empty()) {
            LOG_WARN("ER_API: Register() called with missing fields "
                     "(modName, behaviorPath, eventName, graphName all required).");
            return false;
        }

        // ── Event name must not contain spaces (Havok event names are single tokens) ──
        if (reg.eventName.find(' ') != std::string::npos) {
            LOG_WARN("ER_API: '{}' — eventName '{}' contains spaces (invalid Havok event name).",
                reg.modName, reg.eventName);
            return false;
        }

        // ── Behavior path should use backslashes ──
        if (reg.behaviorPath.find('/') != std::string::npos) {
            LOG_WARN("ER_API: '{}' — behaviorPath '{}' contains forward slashes; "
                     "Havok paths must use backslashes.",
                reg.modName, reg.behaviorPath);
        }

        // ── Validate variables ──
        for (const auto& var : reg.variables) {
            if (var.name.empty()) {
                LOG_WARN("ER_API: '{}' — variable with empty name.", reg.modName);
                return false;
            }
            if (var.name.find(' ') != std::string::npos) {
                LOG_WARN("ER_API: '{}' — variable name '{}' contains spaces (invalid).",
                    reg.modName, var.name);
                return false;
            }
            const auto t = static_cast<std::int8_t>(var.type);
            if (t < 0 || t > static_cast<std::int8_t>(VariableType::Float)) {
                LOG_WARN("ER_API: '{}' — variable '{}' has unrecognised type {}.",
                    reg.modName, var.name, static_cast<int>(t));
                return false;
            }
        }

        // ── Duplicate modName ──
        if (IsRegistered(reg.modName)) {
            LOG_WARN("ER_API: '{}' is already registered.", reg.modName);
            return false;
        }

        // ── Cross-registration duplicate event/variable checks for the same graph ──
        for (const auto& existing : BehaviorPatcher::GetRegistrations()) {
            if (existing.graphName != reg.graphName) continue;
            if (existing.eventName == reg.eventName) {
                LOG_WARN("ER_API: event '{}' already registered by '{}' for graph '{}' — "
                         "event names must be unique across registrations.",
                    reg.eventName, existing.modName, reg.graphName);
                return false;
            }
            for (const auto& newVar : reg.variables) {
                for (const auto& existVar : existing.variables) {
                    if (newVar.name == existVar.name) {
                        LOG_WARN("ER_API: variable '{}' already registered by '{}' for graph '{}' — "
                                 "variable names must be unique across registrations.",
                            newVar.name, existing.modName, reg.graphName);
                        return false;
                    }
                }
            }
        }

        BehaviorPatcher::AddRegistration(reg);
        // Auto-register gate declarations so ActivateGates/DeactivateGates can
        // find them by modName even without a separate RegisterActionGates call.
        if (!reg.actionGates.empty()) {
            std::lock_guard lock(s_gateMutex);
            s_gateRegistrations.emplace(reg.modName, reg.actionGates);
        }
        LOG_INFO("ER_API: registered '{}' — behavior='{}', event='{}', "
                 "{} animation(s), {} variable(s), {} gate(s).",
            reg.modName, reg.behaviorPath, reg.eventName,
            reg.animations.size(), reg.variables.size(), reg.actionGates.size());
        return true;
    }

    bool IsRegistered(const std::string& modName)
    {
        for (const auto& reg : BehaviorPatcher::GetRegistrations()) {
            if (reg.modName == modName) return true;
        }
        return false;
    }

    size_t GetRegistrationCount()
    {
        return BehaviorPatcher::GetRegistrations().size();
    }

    // ── Direct event delivery via NotifyAnimationGraph ──

    /// For creature switchboard graphs that use a single pre-baked generic
    /// entry wildcard (option-B), returns the event that must be sent to the
    /// actor's ROOT behavior graph to trigger the BSB container transition.
    ///
    /// Returns an empty string_view for human EngineRelay.hkb, where
    /// ER injects per-mod wildcards into 0_master at runtime and the
    /// mod-specific event fires directly.
    ///
    /// To add a new creature type: append an entry matching the graphName
    /// used in the Registration and the generic event baked into that
    /// creature's master behavior (e.g. "BSB_WolfEnter" in wolfbehavior.hkx).
    static std::string_view GetCreatureGenericEntryEvent(std::string_view graphName)
    {
        if (graphName == "DragonEngineRelay.hkb")
            return "ER_DragonEnter";
        // Human EngineRelay.hkb — fire the mod-specific event directly.
        return {};
    }

    bool SendEvent(RE::Actor* actor, const std::string& modName)
    {
        if (!actor) {
            LOG_WARN("ER_API::SendEvent: null actor.");
            return false;
        }

        // Find the registration.
        const Registration* found = nullptr;
        for (const auto& reg : BehaviorPatcher::GetRegistrations()) {
            if (reg.modName == modName) { found = &reg; break; }
        }
        if (!found) {
            LOG_WARN("ER_API::SendEvent: '{}' not registered.", modName);
            return false;
        }

        // Store the mod-specific event as the pending ER event BEFORE calling
        // NotifyAnimationGraph.  ER_Activate fires synchronously inside that
        // call and reads this value to set the switchboard SM's startStateID,
        // routing Havok to the correct pre-baked state instead of IdleState.
        BehaviorPatcher::SetPendingEREvent(found->eventName);

        // Determine which event to fire on the actor's ROOT behavior graph.
        //
        // Human actors (EngineRelay.hkb):
        //   ER injects a per-mod wildcard transition into 0_master at runtime,
        //   so NotifyAnimationGraph with the mod-specific event resolves it.
        //
        // Creature actors (DragonEngineRelay.hkb, etc.):
        //   The creature's master behavior (e.g. dragonbehavior.hkx) contains a
        //   single pre-baked generic wildcard (e.g. ER_DragonEnter).  We fire
        //   that generic event; the pending event above handles state routing
        //   inside the loaded switchboard graph.
        const std::string_view genericEvt = GetCreatureGenericEntryEvent(found->graphName);
        const char* triggerEvt = genericEvt.empty()
            ? found->eventName.c_str()
            : genericEvt.data();

        bool result = actor->NotifyAnimationGraph(RE::BSFixedString(triggerEvt));
        LOG_INFO("ER_API::SendEvent: NotifyAnimationGraph('{}') on '{}' returned {} "
                 "(pending ER event: '{}').",
            triggerEvt, actor->GetName(), result, found->eventName);
        return result;
    }

    // ── Character State Allocator API implementation ──

    RE::hkpCharacterStateType RegisterCharacterState(
        const std::string& modName,
        const ERPhysicsCallbacks& callbacks)
    {
        return CharacterStateAllocator::Register(modName, callbacks);
    }

    bool IsCharacterStateRegistered(const std::string& modName)
    {
        return CharacterStateAllocator::IsRegistered(modName);
    }

    RE::hkpCharacterStateType GetCharacterStateSlot(const std::string& modName)
    {
        return CharacterStateAllocator::GetSlot(modName);
    }

    void InstallCharacterStates(RE::Actor* actor)
    {
        if (!actor) {
            LOG_WARN("ER_API::InstallCharacterStates: null actor.");
            return;
        }
        auto* ctrl = actor->GetCharController();
        if (!ctrl) {
            LOG_WARN("ER_API::InstallCharacterStates: actor '{}' has no character controller.",
                actor->GetName());
            return;
        }
        CharacterStateAllocator::InstallStates(ctrl);
    }

    // ── Logical State API implementation ──────────────────────────────────────

    ERStateHandle RegisterLogicalState(const std::string& modName,
                                             const ERPhysicsCallbacks& callbacks,
                                             std::uint32_t priority)
    {
        return CharacterStateAllocator::RegisterLogicalState(modName, callbacks, priority);
    }

    bool IsLogicalStateRegistered(const std::string& modName)
    {
        return CharacterStateAllocator::IsLogicalStateRegistered(modName);
    }

    ERStateHandle GetERStateHandle(const std::string& modName)
    {
        return CharacterStateAllocator::GetERStateHandle(modName);
    }

    RE::hkpCharacterStateType GetLogicalStateHostSlot()
    {
        return CharacterStateAllocator::GetHostSlot();
    }

    bool InstallLogicalStateHost(RE::Actor* actor)
    {
        if (!actor) {
            LOG_WARN("ER_API::InstallLogicalStateHost: null actor.");
            return false;
        }
        auto* ctrl = actor->GetCharController();
        if (!ctrl) {
            LOG_WARN("ER_API::InstallLogicalStateHost: actor '{}' has no character controller.",
                actor->GetName());
            return false;
        }
        return CharacterStateAllocator::InstallLogicalStateHost(ctrl);
    }

    bool ActivateLogicalState(RE::Actor* actor, ERStateHandle handle)
    {
        if (!actor) {
            LOG_WARN("ER_API::ActivateLogicalState: null actor.");
            return false;
        }
        auto* ctrl = actor->GetCharController();
        if (!ctrl) {
            LOG_WARN("ER_API::ActivateLogicalState: actor '{}' has no character controller.",
                actor->GetName());
            return false;
        }
        return CharacterStateAllocator::ActivateLogicalState(ctrl, handle);
    }

    void DeactivateLogicalState(RE::Actor* actor)
    {
        if (!actor) {
            LOG_WARN("ER_API::DeactivateLogicalState: null actor.");
            return;
        }
        auto* ctrl = actor->GetCharController();
        if (!ctrl) {
            LOG_WARN("ER_API::DeactivateLogicalState: actor '{}' has no character controller.",
                actor->GetName());
            return;
        }
        CharacterStateAllocator::DeactivateLogicalState(ctrl);
    }

    ERStateHandle GetActiveLogicalState(RE::Actor* actor)
    {
        if (!actor) return kInvalidERState;
        auto* ctrl = actor->GetCharController();
        if (!ctrl) return kInvalidERState;
        return CharacterStateAllocator::GetActiveLogicalState(ctrl);
    }

    // ── Action Gate API implementation ──

    bool RegisterActionGates(const std::string& modName, const std::vector<ActionGate>& gates)
    {
        if (modName.empty()) {
            LOG_WARN("ER_API::RegisterActionGates: empty modName.");
            return false;
        }
        std::lock_guard lock(s_gateMutex);
        auto [it, inserted] = s_gateRegistrations.emplace(modName, gates);
        if (!inserted) {
            LOG_WARN("ER_API::RegisterActionGates: '{}' already has gates registered — ignored.",
                modName);
            return false;
        }
        LOG_INFO("ER_API::RegisterActionGates: '{}' registered {} gate(s).",
            modName, gates.size());
        return true;
    }

    void SetActionGate(RE::Actor* actor, ActionGate gate, bool suppress)
    {
        if (!actor) {
            LOG_WARN("ER_API::SetActionGate: null actor.");
            return;
        }
        const auto idx = static_cast<std::size_t>(gate);
        if (idx >= std::size(kGateVarNames)) {
            LOG_WARN("ER_API::SetActionGate: unrecognised ActionGate value {}.",
                static_cast<int>(gate));
            return;
        }
        actor->SetGraphVariableBool(
            RE::BSFixedString(kGateVarNames[idx].data()), suppress);
        LOG_INFO("ER_API::SetActionGate: '{}' {} on '{}'.",
            kGateVarNames[idx],
            suppress ? "suppressed" : "cleared",
            actor->GetName());
    }

    void ClearAllActionGates(RE::Actor* actor)
    {
        if (!actor) {
            LOG_WARN("ER_API::ClearAllActionGates: null actor.");
            return;
        }
        for (const auto& name : kGateVarNames) {
            actor->SetGraphVariableBool(RE::BSFixedString(name.data()), false);
        }
        LOG_INFO("ER_API::ClearAllActionGates: all gates cleared on '{}'.",
            actor->GetName());
    }

    void ActivateGates(RE::Actor* actor, const std::string& modName)
    {
        if (!actor) {
            LOG_WARN("ER_API::ActivateGates: null actor.");
            return;
        }
        std::lock_guard lock(s_gateMutex);
        auto it = s_gateRegistrations.find(modName);
        if (it == s_gateRegistrations.end()) {
            LOG_WARN("ER_API::ActivateGates: '{}' has no registered gates.", modName);
            return;
        }
        for (const auto gate : it->second) {
            const auto idx = static_cast<std::size_t>(gate);
            if (idx < std::size(kGateVarNames)) {
                actor->SetGraphVariableBool(
                    RE::BSFixedString(kGateVarNames[idx].data()), true);
            }
        }
        LOG_INFO("ER_API::ActivateGates: {} gate(s) activated for '{}' on '{}'.",
            it->second.size(), modName, actor->GetName());
    }

    void DeactivateGates(RE::Actor* actor, const std::string& modName)
    {
        if (!actor) {
            LOG_WARN("ER_API::DeactivateGates: null actor.");
            return;
        }
        std::lock_guard lock(s_gateMutex);
        auto it = s_gateRegistrations.find(modName);
        if (it == s_gateRegistrations.end()) {
            LOG_WARN("ER_API::DeactivateGates: '{}' has no registered gates.", modName);
            return;
        }
        for (const auto gate : it->second) {
            const auto idx = static_cast<std::size_t>(gate);
            if (idx < std::size(kGateVarNames)) {
                actor->SetGraphVariableBool(
                    RE::BSFixedString(kGateVarNames[idx].data()), false);
            }
        }
        LOG_INFO("ER_API::DeactivateGates: {} gate(s) deactivated for '{}' on '{}'.",
            it->second.size(), modName, actor->GetName());
    }

    // ── Unified ER registration ─────────────────────────────────────────────

    struct EREntry {
        std::string                        modName;
        std::uint32_t                      priority{ 0 };
        bool                               hasBehavior{ false };
        std::optional<ERStateHandle>  physicsHandle;
        std::optional<ERCameraHandle> cameraHandle;
    };

    static std::mutex            s_bsbMutex;
    static std::vector<EREntry> s_bsbEntries;

    ERHandle Register(const ERRegistration& reg)
    {
        if (reg.modName.empty()) {
            LOG_WARN("ER_API::Register(ERRegistration): empty modName.");
            return kInvalidERHandle;
        }
        if (!reg.behavior && !reg.physicsState && !reg.cameraState) {
            LOG_WARN("ER_API::Register(ERRegistration) '{}': no components specified — "
                     "at least one of behavior/physicsState/cameraState must be set.",
                reg.modName);
            return kInvalidERHandle;
        }

        EREntry entry;
        entry.modName  = reg.modName;
        entry.priority = reg.priority;

        // ── Register behavior component ───────────────────────────────────────
        if (reg.behavior) {
            Registration bReg;
            bReg.modName      = reg.modName;
            bReg.behaviorPath = reg.behavior->behaviorPath;
            bReg.eventName    = reg.behavior->eventName;
            bReg.graphName    = reg.behavior->graphName;
            bReg.projectPath  = reg.behavior->projectPath;
            bReg.animations   = reg.behavior->animations;
            bReg.variables    = reg.behavior->variables;
            bReg.actionGates  = reg.behavior->actionGates;
            if (!Register(bReg)) {
                // Register(Registration) already logged the reason.
                return kInvalidERHandle;
            }
            entry.hasBehavior = true;
        }

        // ── Register physics component ────────────────────────────────────────
        if (reg.physicsState) {
            const auto h = CharacterStateAllocator::RegisterLogicalState(
                reg.modName, *reg.physicsState, reg.priority);
            if (h == kInvalidERState) {
                LOG_WARN("ER_API::Register(ERRegistration) '{}': "
                         "physics component registration failed.", reg.modName);
                return kInvalidERHandle;
            }
            entry.physicsHandle = h;
        }

        // ── Register camera component ─────────────────────────────────────────
        if (reg.cameraState) {
            const auto h = CameraStateManager::RegisterLogicalCameraState(
                reg.modName, *reg.cameraState, reg.priority);
            if (h == kInvalidERCameraState) {
                LOG_WARN("ER_API::Register(ERRegistration) '{}': "
                         "camera component registration failed.", reg.modName);
                return kInvalidERHandle;
            }
            entry.cameraHandle = h;
        }

        // ── Store entry ───────────────────────────────────────────────────────
        std::lock_guard lock(s_bsbMutex);
        // Duplicate check under lock (all component registrations are done; if
        // modName already exists we log and return invalid — components already
        // registered are left in their respective sub-systems, which will reject
        // any future duplicate on their own modName guard).
        for (const auto& e : s_bsbEntries) {
            if (e.modName == reg.modName) {
                LOG_WARN("ER_API::Register(ERRegistration) '{}': "
                         "modName already registered via unified API.", reg.modName);
                return kInvalidERHandle;
            }
        }
        const ERHandle handle       = static_cast<ERHandle>(s_bsbEntries.size());
        const bool      hasBehavior  = entry.hasBehavior;
        const bool      hasPhysics   = entry.physicsHandle.has_value();
        const bool      hasCamera    = entry.cameraHandle.has_value();
        s_bsbEntries.push_back(std::move(entry));

        LOG_INFO("ER_API::Register(ERRegistration): '{}' → handle={} "
                 "(behavior={}, physics={}, camera={}, priority={}).",
            reg.modName, handle,
            hasBehavior ? "yes" : "no",
            hasPhysics  ? "yes" : "no",
            hasCamera   ? "yes" : "no",
            reg.priority);
        return handle;
    }

    bool Activate(RE::Actor* actor, ERHandle handle)
    {
        if (!actor) {
            LOG_WARN("ER_API::Activate: null actor.");
            return false;
        }

        const EREntry* entry = nullptr;
        {
            std::lock_guard lock(s_bsbMutex);
            if (handle >= s_bsbEntries.size()) {
                LOG_WARN("ER_API::Activate: handle {} out of range.", handle);
                return false;
            }
            entry = &s_bsbEntries[handle];
        }

        // ── Dry-run priority checks (touch nothing) ───────────────────────────
        RE::bhkCharacterController* ctrl = nullptr;
        if (entry->physicsHandle) {
            ctrl = actor->GetCharController();
            if (!ctrl) {
                LOG_WARN("ER_API::Activate: '{}' — actor '{}' has no character controller.",
                    entry->modName, actor->GetName());
                return false;
            }
            if (!CharacterStateAllocator::CanActivateLogicalState(ctrl, entry->priority)) {
                LOG_WARN("ER_API::Activate: '{}' — physics blocked by higher-priority state.",
                    entry->modName);
                return false;
            }
        }
        if (entry->cameraHandle) {
            if (!CameraStateManager::CanActivateLogicalCameraState(entry->priority)) {
                LOG_WARN("ER_API::Activate: '{}' — camera blocked by higher-priority state.",
                    entry->modName);
                return false;
            }
        }

        // ── All checks passed — activate ──────────────────────────────────────
        if (entry->physicsHandle) {
            if (!ctrl) ctrl = actor->GetCharController();
            InstallLogicalStateHost(actor);
            CharacterStateAllocator::ActivateLogicalState(ctrl, *entry->physicsHandle);
        }
        if (entry->cameraHandle) {
            CameraStateManager::ActivateLogicalCameraState(*entry->cameraHandle);
        }
        if (entry->hasBehavior) {
            SendEvent(actor, entry->modName);
        }

        LOG_INFO("ER_API::Activate: '{}' activated on '{}'.",
            entry->modName, actor->GetName());
        return true;
    }

    void Deactivate(RE::Actor* actor, ERHandle handle)
    {
        if (!actor) {
            LOG_WARN("ER_API::Deactivate: null actor.");
            return;
        }

        const EREntry* entry = nullptr;
        {
            std::lock_guard lock(s_bsbMutex);
            if (handle >= s_bsbEntries.size()) {
                LOG_WARN("ER_API::Deactivate: handle {} out of range.", handle);
                return;
            }
            entry = &s_bsbEntries[handle];
        }

        if (entry->physicsHandle) {
            DeactivateLogicalState(actor);
        }
        if (entry->cameraHandle) {
            CameraStateManager::DeactivateLogicalCameraState();
        }
        if (entry->hasBehavior) {
            actor->NotifyAnimationGraph(RE::BSFixedString("IdleForceDefaultState"));
        }

        LOG_INFO("ER_API::Deactivate: '{}' deactivated on '{}'.",
            entry->modName, actor->GetName());
    }

    void EnterPhysicsState(RE::Actor* actor, ERHandle handle)
    {
        if (!actor) {
            LOG_WARN("ER_API::EnterPhysicsState: null actor.");
            return;
        }

        const EREntry* entry = nullptr;
        {
            std::lock_guard lock(s_bsbMutex);
            if (handle >= s_bsbEntries.size()) {
                LOG_WARN("ER_API::EnterPhysicsState: handle {} out of range.", handle);
                return;
            }
            entry = &s_bsbEntries[handle];
        }

        if (!entry->physicsHandle) {
            LOG_WARN("ER_API::EnterPhysicsState: '{}' has no physics component.", entry->modName);
            return;
        }

        auto* ctrl = actor->GetCharController();
        if (!ctrl) {
            LOG_WARN("ER_API::EnterPhysicsState: actor '{}' has no character controller.",
                actor->GetName());
            return;
        }

        const auto hostSlot = CharacterStateAllocator::GetHostSlot();

        // No-op if already in the BSB host state — covers the hover→flight transition
        // where physics is already active and only the camera changes.
        if (ctrl->context.currentState == hostSlot) {
            LOG_INFO("ER_API::EnterPhysicsState: '{}' — actor '{}' already in host state, no-op.",
                entry->modName, actor->GetName());
            return;
        }

        // Ensure the host state is installed before requesting the transition.
        CharacterStateAllocator::InstallLogicalStateHost(ctrl);
        ctrl->wantState = hostSlot;

        LOG_INFO("ER_API::EnterPhysicsState: '{}' — wantState → host on '{}'.",
            entry->modName, actor->GetName());
    }

    void ExitPhysicsState(RE::Actor* actor, ERHandle handle)
    {
        if (!actor) {
            LOG_WARN("ER_API::ExitPhysicsState: null actor.");
            return;
        }

        const EREntry* entry = nullptr;
        {
            std::lock_guard lock(s_bsbMutex);
            if (handle >= s_bsbEntries.size()) {
                LOG_WARN("ER_API::ExitPhysicsState: handle {} out of range.", handle);
                return;
            }
            entry = &s_bsbEntries[handle];
        }

        if (!entry->physicsHandle) {
            LOG_WARN("ER_API::ExitPhysicsState: '{}' has no physics component.", entry->modName);
            return;
        }

        auto* ctrl = actor->GetCharController();
        if (!ctrl) {
            LOG_WARN("ER_API::ExitPhysicsState: actor '{}' has no character controller.",
                actor->GetName());
            return;
        }

        const auto hostSlot = CharacterStateAllocator::GetHostSlot();

        // No-op if not currently in the BSB host state.
        if (ctrl->context.currentState != hostSlot) {
            LOG_INFO("ER_API::ExitPhysicsState: '{}' — actor '{}' not in host state, no-op.",
                entry->modName, actor->GetName());
            return;
        }

        ctrl->wantState = RE::hkpCharacterStateType::kOnGround;

        LOG_INFO("ER_API::ExitPhysicsState: '{}' — wantState → kOnGround on '{}'.",
            entry->modName, actor->GetName());
    }

    // ── Logical Camera State API implementation ───────────────────────────────

    ERCameraHandle RegisterLogicalCameraState(const std::string& modName,
                                                    const ERCameraCallbacks& callbacks,
                                                    std::uint32_t priority)
    {
        return CameraStateManager::RegisterLogicalCameraState(modName, callbacks, priority);
    }

    bool IsLogicalCameraStateRegistered(const std::string& modName)
    {
        return CameraStateManager::IsLogicalCameraStateRegistered(modName);
    }

    ERCameraHandle GetERCameraHandle(const std::string& modName)
    {
        return CameraStateManager::GetERCameraHandle(modName);
    }

    bool ActivateLogicalCameraState(ERCameraHandle handle)
    {
        return CameraStateManager::ActivateLogicalCameraState(handle);
    }

    void DeactivateLogicalCameraState()
    {
        CameraStateManager::DeactivateLogicalCameraState();
    }

    ERCameraHandle GetActiveLogicalCameraState()
    {
        return CameraStateManager::GetActiveLogicalCameraState();
    }

    // ── Debug console command: ER_DumpVars ──────────────────────────────────
    //
    // Hijacks the unused vanilla console command "GetPlayerGrabbedRef" and
    // repurposes it as a BSB diagnostic tool.
    //
    // Usage (in the Skyrim console):
    //   ER_DumpVars                — dumps all BSB-registered variable names
    //                                 and their current values from the PLAYER's
    //                                 behavior graph.
    //   player.ER_DumpVars        — same, explicit target.
    //   <npc_ref>.ER_DumpVars     — dump from an NPC's behavior graph.
    //
    // Short name alias: "ERVars"
    // -------------------------------------------------------------------------

    static bool ER_DumpGraphVars(
        const RE::SCRIPT_PARAMETER*,
        RE::SCRIPT_FUNCTION::ScriptData*,
        RE::TESObjectREFR*           a_thisObj,
        RE::TESObjectREFR*,
        RE::Script*,
        RE::ScriptLocals*,
        double&,
        std::uint32_t&)
    {
        auto* console = RE::ConsoleLog::GetSingleton();

        // Resolve the target actor: use the passed ref if it's an actor, fall
        // back to the player.
        RE::Actor* actor = nullptr;
        if (a_thisObj) actor = a_thisObj->As<RE::Actor>();
        if (!actor)    actor = RE::PlayerCharacter::GetSingleton();

        if (!actor) {
            if (console) console->Print("ER_DumpVars: no actor target.");
            LOG_WARN("ER_DumpVars: no actor target.");
            return true;
        }

        // Walk the behavior graph's variable table.
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!actor->GetAnimationGraphManager(manager) || !manager || manager->graphs.empty()) {
            if (console) console->Print("ER_DumpVars: no animation graph manager for '%s'.",
                actor->GetName());
            LOG_WARN("ER_DumpVars: no animation graph manager for '{}'.", actor->GetName());
            return true;
        }

        auto* bsAnimGraph = manager->graphs[0].get();
        auto* graph       = bsAnimGraph ? bsAnimGraph->behaviorGraph : nullptr;
        auto* graphData   = graph ? graph->data.get() : nullptr;
        auto* strData     = graphData ? graphData->stringData.get() : nullptr;

        if (!strData) {
            if (console) console->Print("ER_DumpVars: no behavior graph / stringData for '%s'.",
                actor->GetName());
            LOG_WARN("ER_DumpVars: no stringData for '{}'.", actor->GetName());
            return true;
        }

        const auto& regs = BehaviorPatcher::GetRegistrations();
        if (regs.empty()) {
            if (console) console->Print("ER_DumpVars: no ER registrations.");
            return true;
        }

        if (console) console->Print("ER_DumpVars: variables for '%s':", actor->GetName());
        LOG_INFO("ER_DumpVars: variable dump for '{}'.", actor->GetName());

        for (const auto& reg : regs) {
            for (const auto& var : reg.variables) {
                // Look up the variable index in the graph's string table.
                int32_t idx = -1;
                for (int32_t i = 0; i < static_cast<int32_t>(strData->variableNames.size()); ++i) {
                    const char* n = strData->variableNames[i].data();
                    if (n && std::strcmp(n, var.name.c_str()) == 0) { idx = i; break; }
                }

                if (idx < 0) {
                    if (console) console->Print("  [%s] %s = <not registered>",
                        reg.modName.c_str(), var.name.c_str());
                    LOG_INFO("ER_DumpVars:   [{}] {} = <not registered>",
                        reg.modName, var.name);
                    continue;
                }

                // Read current value via the engine API.
                switch (var.type) {
                    case VariableType::Bool: {
                        bool v = false;
                        actor->GetGraphVariableBool(RE::BSFixedString(var.name.c_str()), v);
                        if (console) console->Print("  [%s] %s (Bool) = %s",
                            reg.modName.c_str(), var.name.c_str(), v ? "true" : "false");
                        LOG_INFO("ER_DumpVars:   [{}] {} (Bool) = {}", reg.modName, var.name, v);
                        break;
                    }
                    case VariableType::Int8:
                    case VariableType::Int16:
                    case VariableType::Int32: {
                        int v = 0;
                        actor->GetGraphVariableInt(RE::BSFixedString(var.name.c_str()), v);
                        if (console) console->Print("  [%s] %s (Int) = %d",
                            reg.modName.c_str(), var.name.c_str(), v);
                        LOG_INFO("ER_DumpVars:   [{}] {} (Int) = {}", reg.modName, var.name, v);
                        break;
                    }
                    case VariableType::Float: {
                        float v = 0.0f;
                        actor->GetGraphVariableFloat(RE::BSFixedString(var.name.c_str()), v);
                        if (console) console->Print("  [%s] %s (Float) = %.4f",
                            reg.modName.c_str(), var.name.c_str(), static_cast<double>(v));
                        LOG_INFO("ER_DumpVars:   [{}] {} (Float) = {:.4f}", reg.modName, var.name, v);
                        break;
                    }
                }
            }
        }

        return true;
    }

    static void InstallConsoleCommands()
    {
        // Repurpose the unused vanilla console command "GetPlayerGrabbedRef"
        // as the BSB diagnostic variable dump.  We keep the original numParams
        // = 0 so no parameter parsing is needed.
        auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand("GetPlayerGrabbedRef");
        if (!cmd) {
            LOG_WARN("ER: could not locate 'GetPlayerGrabbedRef' console command "
                     "— ER_DumpVars will not be available.");
            return;
        }

        static const char* kLongName  = "ER_DumpVars";
        static const char* kShortName = "ERVars";
        static const char* kHelp      =
            "ER_DumpVars: dump all Engine Relay graph variables "
            "for the target actor (default: player).";

        cmd->functionName    = kLongName;
        cmd->shortName       = kShortName;
        cmd->helpString      = kHelp;
        cmd->executeFunction = ER_DumpGraphVars;
        cmd->numParams       = 0;
        cmd->params          = nullptr;

        LOG_INFO("ER: console command '{}' (short: '{}') registered on "
                 "GetPlayerGrabbedRef slot.", kLongName, kShortName);
    }

    // ── ERInterface — SKSE inter-plugin API ─────────────────────────────────
    //
    // Plain C-ABI function-pointer table broadcast to any plugin that calls
    // RegisterListener("EngineRelay", ...) before kPostLoad fires.
    // Wrappers translate const char* → std::string and bare pointer args →
    // the internal API.  No STL types cross the DLL boundary.

    namespace ERInterfaceImpl {

        static ERCameraHandle Wrap_RegisterLogicalCameraState(
            const char* modName, const ERCameraCallbacks* cbs, std::uint32_t priority)
        {
            if (!modName || !cbs) return kInvalidERCameraState;
            return CameraStateManager::RegisterLogicalCameraState(modName, *cbs, priority);
        }

        static bool Wrap_ActivateLogicalCameraState(ERCameraHandle handle)
        {
            return CameraStateManager::ActivateLogicalCameraState(handle);
        }

        static void Wrap_DeactivateLogicalCameraState()
        {
            CameraStateManager::DeactivateLogicalCameraState();
        }

        static ERCameraHandle Wrap_GetActiveLogicalCameraState()
        {
            return CameraStateManager::GetActiveLogicalCameraState();
        }

        static ERStateHandle Wrap_RegisterLogicalState(
            const char* modName, const ERPhysicsCallbacks* cbs, std::uint32_t priority)
        {
            if (!modName || !cbs) return kInvalidERState;
            return CharacterStateAllocator::RegisterLogicalState(modName, *cbs, priority);
        }

        static bool Wrap_InstallLogicalStateHost(RE::Actor* actor)
        {
            if (!actor) return false;
            auto* ctrl = actor->GetCharController();
            if (!ctrl) return false;
            return CharacterStateAllocator::InstallLogicalStateHost(ctrl);
        }

        static bool Wrap_ActivateLogicalState(RE::Actor* actor, ERStateHandle handle)
        {
            if (!actor) return false;
            auto* ctrl = actor->GetCharController();
            if (!ctrl) return false;
            return CharacterStateAllocator::ActivateLogicalState(ctrl, handle);
        }

        static void Wrap_DeactivateLogicalState(RE::Actor* actor)
        {
            if (!actor) return;
            auto* ctrl = actor->GetCharController();
            if (!ctrl) return;
            CharacterStateAllocator::DeactivateLogicalState(ctrl);
        }

        static ERStateHandle Wrap_GetActiveLogicalState(RE::Actor* actor)
        {
            if (!actor) return kInvalidERState;
            auto* ctrl = actor->GetCharController();
            if (!ctrl) return kInvalidERState;
            return CharacterStateAllocator::GetActiveLogicalState(ctrl);
        }

        static void Wrap_EnterPhysicsHost(RE::Actor* actor)
        {
            if (!actor) return;
            auto* ctrl = actor->GetCharController();
            if (!ctrl) return;
            const auto hostSlot = CharacterStateAllocator::GetHostSlot();
            if (ctrl->context.currentState == hostSlot) return;
            CharacterStateAllocator::InstallLogicalStateHost(ctrl);
            ctrl->wantState = hostSlot;
        }

        static void Wrap_ExitPhysicsHost(RE::Actor* actor)
        {
            if (!actor) return;
            auto* ctrl = actor->GetCharController();
            if (!ctrl) return;
            const auto hostSlot = CharacterStateAllocator::GetHostSlot();
            if (ctrl->context.currentState != hostSlot) return;
            ctrl->wantState = RE::hkpCharacterStateType::kOnGround;
        }

        static const ERInterface g_interface = {
            ERInterface::kVersion,
            &Wrap_RegisterLogicalCameraState,
            &Wrap_ActivateLogicalCameraState,
            &Wrap_DeactivateLogicalCameraState,
            &Wrap_GetActiveLogicalCameraState,
            &Wrap_RegisterLogicalState,
            &Wrap_InstallLogicalStateHost,
            &Wrap_ActivateLogicalState,
            &Wrap_DeactivateLogicalState,
            &Wrap_GetActiveLogicalState,
            &Wrap_EnterPhysicsHost,
            &Wrap_ExitPhysicsHost,
        };

    }  // namespace ERInterfaceImpl

    // ── SKSE messaging ──

    static void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        switch (msg->type) {
            case SKSE::MessagingInterface::kPostLoad:
            {
                // Load file-based configs now — other plugins can also
                // call Register() during kPostLoad via the API.
                auto configs = ConfigLoader::LoadConfigs();
                for (auto& cfg : configs) {
                    Register(std::move(cfg));
                }
                LOG_INFO("Switchboard: {} total registrations after config load.",
                    GetRegistrationCount());

                // Broadcast the inter-plugin interface to all listeners that
                // called RegisterListener("EngineRelay", ...) during
                // their own SKSEPluginLoad.  Dispatch is synchronous — all
                // callbacks fire before this returns, so recipients' handles
                // are set before their own kPostLoad handlers run.
                if (auto* m = SKSE::GetMessagingInterface()) {
                    m->Dispatch(ERInterface::kMessage,
                        const_cast<ERInterface*>(&ERInterfaceImpl::g_interface),
                        sizeof(ERInterface),
                        nullptr);
                    LOG_INFO("ER: interface v{} dispatched.", ERInterface::kVersion);
                }

                break;
            }

            case SKSE::MessagingInterface::kPreLoadGame:
            case SKSE::MessagingInterface::kNewGame:
                // The engine is about to reload behavior graphs.
                // Clear the patch guard so BSB re-injects on the next
                // hkbBehaviorGraph::Activate call.
                BehaviorPatcher::ClearPatchedSet();
                // Clear stale per-actor character-state entries so that
                // InstallLogicalStateHost allocates fresh objects after load.
                CharacterStateAllocator::ClearActorEntries();
                // Clear logical camera state handles and restore vanilla TPS.
                CameraStateManager::OnPreLoadGame();
                break;

            default:
                break;
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    PluginLogger::Init("EngineRelay", "Engine Relay");
    LOG_INFO("Engine Relay v0.1.0 loading.");

    EngineRelay::BehaviorFileInterceptor::Install();
    EngineRelay::BehaviorPatcher::Install();
    EngineRelay::CameraStateManager::Init();
    EngineRelay::InstallConsoleCommands();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        LOG_ERROR("Failed to get SKSE messaging interface.");
        return false;
    }
    messaging->RegisterListener(EngineRelay::OnMessage);

    LOG_INFO("Engine Relay loaded — waiting for registrations.");
    return true;
}
