#include "PCH.h"
#include "BehaviorPatcher.h"
#include "HavokTypes.h"

#include <unordered_map>
#include <unordered_set>

namespace BehaviorSwitchboard::BehaviorPatcher {

    // -----------------------------------------------------------------------
    // Registration storage
    // -----------------------------------------------------------------------

    static std::mutex                  s_regMutex;
    static std::vector<Registration>   s_registrations;

    void AddRegistration(const Registration& reg)
    {
        std::lock_guard lock(s_regMutex);
        s_registrations.push_back(reg);
    }

    const std::vector<Registration>& GetRegistrations()
    {
        return s_registrations;
    }

    // -----------------------------------------------------------------------
    // Version-based patch guard
    // -----------------------------------------------------------------------

    // Maps each shared hkbBehaviorGraphData* to the count of registrations
    // already injected into it.  Multiple NPC graph instances share the same
    // graphData; injecting any one mutates the shared data for all.
    //
    // Using a version count (instead of a plain "seen" flag) means that
    // late-arriving registrations added after the first Activate are picked up
    // on the next Activate call rather than being silently skipped for the
    // remainder of the session.
    static std::mutex                                               s_patchedMutex;
    static std::unordered_map<RE::hkbBehaviorGraphData*, uint32_t> s_patchedVersions;

    // ANIM-DIAG dedup: only dump the animation list once per character string
    // data pointer per session.  Multiple NPC instances sharing the same
    // hkbCharacterData share the same stringData, so the list is identical
    // for every activation after the first.
    static std::mutex                                                   s_animDiagMutex;
    static std::unordered_set<RE::hkbCharacterStringData*>              s_animDiagReported;

    // GRAPH-DUMP dedup: dump the full root SM state once per unique graphData*.
    // Keyed on graphData so that all NPC instances sharing the same archetype
    // only produce one dump (they'd be identical after the first injection).
    static std::mutex                                                   s_graphDumpMutex;
    static std::unordered_set<RE::hkbBehaviorGraphData*>                s_graphDumpReported;

    // Maps hkbCharacter* → captured switchboard graph for that character.
    // Populated when a switchboard graph activates (option-B path).
    // Used by the sub-behavior node ID fix-up to find the correct parent graph
    // (the switchboard that owns the static ID reservation) rather than falling
    // back to the character's root graph which has no reserved range.
    static std::mutex s_switchboardGraphsMutex;
    static std::unordered_map<RE::hkbCharacter*, RE::hkbBehaviorGraph*>
        s_switchboardGraphs;

    // -----------------------------------------------------------------------
    // Pending BSB event — bridges SendEvent → BSB_Activate (option-B path)
    //
    // BSB_API::SendEvent stores the triggering event name here immediately
    // before calling actor->NotifyAnimationGraph().  Since BSB_Activate fires
    // synchronously within that call (proven by log ordering), BSB_Activate
    // can read and clear it before the original Activate returns, then set
    // BSB's SM startStateID to the matching pre-baked state so the character
    // enters the correct state instead of A-posing in BSB_IdleState.
    // -----------------------------------------------------------------------
    static std::mutex  s_pendingEventMutex;
    static std::string s_pendingBSBEvent;

    static std::string GetAndClearPendingBSBEvent()
    {
        std::lock_guard lock(s_pendingEventMutex);
        return std::exchange(s_pendingBSBEvent, std::string{});
    }

    void SetPendingBSBEvent(const std::string& eventName)
    {
        std::lock_guard lock(s_pendingEventMutex);
        s_pendingBSBEvent = eventName;
        LOG_DEBUG("BehaviorPatcher: pending BSB event set to '{}'.", eventName);
    }

    /// Returns the start index into s_registrations for pending injections,
    /// or -1 if this graphData is already fully up to date.
    /// Atomically advances the stored version to currentRegCount.
    static int32_t GetPendingStart(RE::hkbBehaviorGraphData* data, uint32_t currentRegCount)
    {
        if (!data) return -1;
        std::lock_guard lock(s_patchedMutex);
        auto [it, inserted] = s_patchedVersions.emplace(data, currentRegCount);
        if (inserted) return 0;                       // first time: inject all
        if (it->second >= currentRegCount) return -1; // already up to date
        const uint32_t prev = it->second;
        it->second = currentRegCount;
        return static_cast<int32_t>(prev);            // new regs start here
    }
    void ClearPatchedSet()
    {
        {
            std::lock_guard lock(s_patchedMutex);
            s_patchedVersions.clear();
        }
        {
            std::lock_guard lock(s_switchboardGraphsMutex);
            s_switchboardGraphs.clear();
        }
        {
            std::lock_guard lock(s_animDiagMutex);
            s_animDiagReported.clear();
        }
        {
            std::lock_guard lock(s_graphDumpMutex);
            s_graphDumpReported.clear();
        }
        LOG_INFO("BehaviorPatcher: patch guard cleared — graphs will be re-injected on next Activate.");
    }

    // -----------------------------------------------------------------------
    // Havok memory helpers
    // -----------------------------------------------------------------------

    template <typename T>
    static T* AllocHavokObject()
    {
        auto* obj = static_cast<T*>(std::calloc(1, sizeof(T)));
        if (!obj) {
            LOG_ERROR("BehaviorPatcher: failed to allocate {} bytes", sizeof(T));
        }
        return obj;
    }

    static std::vector<void*> s_allocations;

    template <typename T>
    static T* AllocAndTrack()
    {
        auto* obj = AllocHavokObject<T>();
        if (obj) s_allocations.push_back(obj);
        return obj;
    }

    // -----------------------------------------------------------------------
    // Generic hkArray grow helper
    // -----------------------------------------------------------------------

    /// Grow a raw hkArray<T> by one element. If there's spare capacity, we
    /// just bump the size. Otherwise we reallocate into a new buffer, copy
    /// old data, and track the allocation for lifetime management.
    ///
    /// arrayBase must point to the start of the hkArray in memory:
    ///   T*      data         (offset 0x00)
    ///   int32   size         (offset 0x08)
    ///   int32   capAndFlags  (offset 0x0C)
    ///
    /// Returns a reference to the newly appended (zero-initialized) element,
    /// or nullptr on allocation failure.
    template <typename T>
    static T* GrowHkArray(void* arrayBase)
    {
        auto* base = reinterpret_cast<std::byte*>(arrayBase);
        auto& dataPtr     = *reinterpret_cast<T**>(base + 0x00);
        auto& arraySize   = *reinterpret_cast<std::int32_t*>(base + 0x08);
        auto& capAndFlags = *reinterpret_cast<std::int32_t*>(base + 0x0C);
        std::int32_t cap  = capAndFlags & 0x3FFFFFFF;

        if (dataPtr && arraySize < cap) {
            // Spare capacity — zero the new slot and bump size.
            std::memset(&dataPtr[arraySize], 0, sizeof(T));
            return &dataPtr[arraySize++];
        }

        // Need to reallocate.
        std::int32_t oldSize = arraySize;
        std::int32_t newCap  = oldSize + 1;

        auto* newData = static_cast<T*>(std::calloc(newCap, sizeof(T)));
        if (!newData) {
            LOG_ERROR("BehaviorPatcher: GrowHkArray failed to allocate {} bytes",
                newCap * sizeof(T));
            return nullptr;
        }
        s_allocations.push_back(newData);

        if (dataPtr && oldSize > 0) {
            std::memcpy(newData, dataPtr, oldSize * sizeof(T));
        }

        dataPtr     = newData;
        arraySize   = oldSize + 1;
        capAndFlags = newCap | static_cast<std::int32_t>(0x80000000u);

        return &newData[oldSize];
    }

    /// Read the element count of a raw hkArray at the given base address.
    static int32_t HkArraySize(const void* arrayBase)
    {
        return *reinterpret_cast<const std::int32_t*>(
            static_cast<const std::byte*>(arrayBase) + 0x08);
    }

    /// Decrement the size field of a raw hkArray by one without releasing memory.
    /// Used to roll back a GrowHkArray when a subsequent step fails so that
    /// parallel arrays stay in sync.
    template <typename T>
    static void ShrinkHkArray(void* arrayBase)
    {
        auto& arraySize = *reinterpret_cast<std::int32_t*>(
            static_cast<std::byte*>(arrayBase) + 0x08);
        if (arraySize > 0) --arraySize;
    }

    // -----------------------------------------------------------------------
    // Helpers — append to Havok arrays
    // -----------------------------------------------------------------------

    /// Append an event name to the behavior graph's event table.
    /// Idempotent: returns the existing index if the event is already present.
    /// Rolls back the name append if the parallel eventInfos grow fails.
    /// Returns the index of the event, or -1 on failure.
    static int32_t AppendEvent(RE::hkbBehaviorGraphData* graphData, const char* eventName)
    {
        if (!graphData || !graphData->stringData) return -1;
        auto* strData = graphData->stringData.get();

        // Idempotency: reuse existing index if already present.
        for (int32_t i = 0; i < static_cast<int32_t>(strData->eventNames.size()); ++i) {
            const char* n = strData->eventNames[i].data();
            if (n && std::strcmp(n, eventName) == 0) {
                LOG_DEBUG("BehaviorPatcher: event '{}' already at index {} — reusing.", eventName, i);
                return i;
            }
        }

        const int32_t idx = static_cast<int32_t>(strData->eventNames.size());
        strData->eventNames.push_back(RE::hkStringPtr(eventName));

        // Grow eventInfos in parallel with the string table.
        auto* info = GrowHkArray<BSBEventInfo>(&graphData->eventInfos);
        if (!info) {
            ShrinkHkArray<RE::hkStringPtr>(&strData->eventNames);  // keep arrays in sync
            LOG_WARN("BehaviorPatcher: failed to grow eventInfos for '{}' — rolled back.", eventName);
            return -1;
        }
        info->flags = 0;  // not silent

        // Invariant: parallel array sizes must match.
        const int32_t infoCount = HkArraySize(&graphData->eventInfos);
        if (static_cast<int32_t>(strData->eventNames.size()) != infoCount) {
            LOG_ERROR("BehaviorPatcher: INVARIANT — eventNames({}) != eventInfos({}) after appending '{}'.",
                strData->eventNames.size(), infoCount, eventName);
        }

        LOG_INFO("BehaviorPatcher: registered event '{}' at index {}", eventName, idx);
        return idx;
    }

    // -----------------------------------------------------------------------
    // Object construction
    // -----------------------------------------------------------------------

    static RE::hkbStateMachine* GetRootStateMachine(RE::hkbBehaviorGraph* graph)
    {
        if (!graph || !graph->rootGenerator) return nullptr;
        return skyrim_cast<RE::hkbStateMachine*>(graph->rootGenerator.get());
    }

    static int32_t GetNextStateId(RE::hkbStateMachine* sm)
    {
        int32_t maxId = 0;
        for (int32_t i = 0; i < sm->states.size(); ++i) {
            auto* stateInfo = sm->states[i];
            if (!stateInfo) continue;
            auto* realState = reinterpret_cast<BSBStateInfo*>(stateInfo);
            if (realState->stateId > maxId) {
                maxId = realState->stateId;
            }
        }
        return maxId + 1;
    }

    static BSBBehaviorReferenceGenerator* BuildBehaviorRef(
        const char* behaviorPath, const char* nodeName,
        std::uint16_t nodeId)
    {
        auto* ref = AllocAndTrack<BSBBehaviorReferenceGenerator>();
        if (!ref) return nullptr;

        REL::Relocation<void*> behavRefVtable{ RE::VTABLE_hkbBehaviorReferenceGenerator[0] };
        ref->vtable = reinterpret_cast<void*>(behavRefVtable.address());

        ref->memSizeAndFlags = 0x58;
        ref->referenceCount  = 1;
        ref->userData        = 0;
        ref->name            = nodeName;
        ref->id              = nodeId;
        ref->cloneState      = 0;
        ref->behaviorName    = behaviorPath;
        ref->behavior        = nullptr;

        ref->variableBindingSet       = nullptr;
        ref->cachedBindables_data     = 0;
        ref->cachedBindables_size     = 0;
        ref->cachedBindables_capAndFlags = 0x80000000;
        ref->areBindablesCached       = false;

        return ref;
    }

    static BSBTransitionInfoArray* BuildWildcardTransitions(
        int32_t eventId, int32_t toStateId)
    {
        auto* tia = AllocAndTrack<BSBTransitionInfoArray>();
        if (!tia) return nullptr;

        REL::Relocation<void*> tiaVtable{ RE::VTABLE_hkbStateMachine__TransitionInfoArray[0] };
        tia->vtable = reinterpret_cast<void*>(tiaVtable.address());
        tia->memSizeAndFlags = 0x20;
        tia->referenceCount  = 1;

        auto* trans = AllocAndTrack<BSBTransitionInfo>();
        if (!trans) return nullptr;

        trans->triggerInterval  = { -1, -1, 0.0f, 0.0f };
        trans->initiateInterval = { -1, -1, 0.0f, 0.0f };
        trans->transition       = nullptr;
        trans->condition        = nullptr;
        trans->eventId          = eventId;
        trans->toStateId        = toStateId;
        trans->fromNestedStateId = 0;
        trans->toNestedStateId   = 0;
        trans->priority          = 0;
        trans->flags             = kTransFlag_Wildcard;

        tia->transitions_data        = trans;
        tia->transitions_size        = 1;
        tia->transitions_capAndFlags = 1 | 0x80000000;

        return tia;
    }

    static BSBStateInfo* BuildStateInfo(
        RE::hkbGenerator* generator, const char* stateName, int32_t stateId)
    {
        auto* state = AllocAndTrack<BSBStateInfo>();
        if (!state) return nullptr;

        REL::Relocation<void*> stateInfoVtable{ RE::VTABLE_hkbStateMachine__StateInfo[0] };
        state->vtable = reinterpret_cast<void*>(stateInfoVtable.address());
        state->memSizeAndFlags = 0x78;
        state->referenceCount  = 1;

        state->variableBindingSet       = nullptr;
        state->cachedBindables_data     = 0;
        state->cachedBindables_size     = 0;
        state->cachedBindables_capAndFlags = 0x80000000;
        state->areBindablesCached       = false;

        state->listeners_data        = nullptr;
        state->listeners_size        = 0;
        state->listeners_capAndFlags = 0x80000000;
        state->enterNotifyEvents     = nullptr;
        state->exitNotifyEvents      = nullptr;
        state->transitions           = nullptr;
        state->generator             = generator;
        state->name                  = stateName;
        state->stateId               = stateId;
        state->probability           = 1.0f;
        state->enable                = true;

        return state;
    }

    // -----------------------------------------------------------------------
    // Grow root SM states array (manual hkArray realloc)
    // -----------------------------------------------------------------------

    static void AppendStateToSM(RE::hkbStateMachine* rootSM, BSBStateInfo* stateInfo)
    {
        auto* arrayBase = reinterpret_cast<std::byte*>(&rootSM->states);
        auto& dataPtr     = *reinterpret_cast<RE::hkbStateMachine::StateInfo***>(arrayBase + 0x00);
        auto& arraySize   = *reinterpret_cast<std::int32_t*>(arrayBase + 0x08);
        auto& capAndFlags = *reinterpret_cast<std::int32_t*>(arrayBase + 0x0C);

        std::int32_t oldSize = arraySize;
        std::int32_t newSize = oldSize + 1;

        auto** newArray = static_cast<RE::hkbStateMachine::StateInfo**>(
            std::calloc(newSize, sizeof(RE::hkbStateMachine::StateInfo*)));
        s_allocations.push_back(newArray);

        if (dataPtr && oldSize > 0) {
            std::memcpy(newArray, dataPtr, oldSize * sizeof(RE::hkbStateMachine::StateInfo*));
        }

        newArray[oldSize] = reinterpret_cast<RE::hkbStateMachine::StateInfo*>(stateInfo);

        dataPtr     = newArray;
        arraySize   = newSize;
        capAndFlags = newSize | static_cast<std::int32_t>(0x80000000u);
    }

    // -----------------------------------------------------------------------
    // Append wildcard transition to existing root SM
    // -----------------------------------------------------------------------

    static void AppendWildcardTransition(RE::hkbStateMachine* rootSM,
        int32_t eventIdx, int32_t stateId)
    {
        if (!rootSM->wildcardTransitions) {
            auto* newWildcards = BuildWildcardTransitions(eventIdx, stateId);
            rootSM->wildcardTransitions.reset(
                reinterpret_cast<RE::hkbStateMachine::TransitionInfoArray*>(newWildcards));
            return;
        }

        // Dedup: skip if an identical (eventId, toStateId) pair already exists.
        auto* existingTIA = reinterpret_cast<BSBTransitionInfoArray*>(
            rootSM->wildcardTransitions.get());
        for (int32_t i = 0; i < existingTIA->transitions_size; ++i) {
            const auto& t = existingTIA->transitions_data[i];
            if (t.eventId == eventIdx && t.toStateId == stateId) {
                LOG_DEBUG("BehaviorPatcher: wildcard (event={}, state={}) already exists — skipping.",
                    eventIdx, stateId);
                return;
            }
        }

        int32_t oldSize = existingTIA->transitions_size;
        int32_t newCap = oldSize + 1;
        auto* newData = static_cast<BSBTransitionInfo*>(
            std::calloc(newCap, sizeof(BSBTransitionInfo)));
        s_allocations.push_back(newData);

        if (existingTIA->transitions_data && oldSize > 0) {
            std::memcpy(newData, existingTIA->transitions_data,
                oldSize * sizeof(BSBTransitionInfo));
        }

        auto& newTrans = newData[oldSize];
        newTrans.triggerInterval  = { -1, -1, 0.0f, 0.0f };
        newTrans.initiateInterval = { -1, -1, 0.0f, 0.0f };
        newTrans.transition       = nullptr;
        newTrans.condition        = nullptr;
        newTrans.eventId          = eventIdx;
        newTrans.toStateId        = stateId;
        newTrans.fromNestedStateId = 0;
        newTrans.toNestedStateId   = 0;
        newTrans.priority          = 0;
        newTrans.flags             = kTransFlag_Wildcard;

        existingTIA->transitions_data = newData;
        existingTIA->transitions_size = oldSize + 1;
        existingTIA->transitions_capAndFlags = newCap | 0x80000000;
    }

    // -----------------------------------------------------------------------
    // Graph / project path matching
    // -----------------------------------------------------------------------

    /// Check if a registration targets this behavior graph.
    /// Exact match on the graph's hkb filename.
    static bool RegistrationMatchesGraph(const Registration& reg,
        std::string_view graphName)
    {
        return !reg.graphName.empty() && reg.graphName == graphName;
    }

    /// Check if a registration should have its events/variables/animations
    /// injected for the given Havok project path.
    ///
    /// If reg.projectPath is empty, the registration matches ALL projects
    /// (safe backward-compatible default — all injection helpers are idempotent).
    /// Otherwise the comparison is case-insensitive.
    static bool RegistrationMatchesProject(const Registration& reg,
        const char* projectPath)
    {
        if (reg.projectPath.empty()) return true;  // match all projects
        if (!projectPath)            return false;

        const std::string_view regPath(reg.projectPath);
        const std::string_view projPath(projectPath);
        if (regPath.size() != projPath.size()) return false;

        return std::equal(regPath.begin(), regPath.end(),
                          projPath.begin(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    }

    // -----------------------------------------------------------------------
    // Pre-injection validation
    // -----------------------------------------------------------------------

    /// Verifies that all Havok structures required by the structural part of a
    /// registration are present and that state/node IDs won't overflow before
    /// any mutation takes place.  Event/variable/animation injection is handled
    /// by the CreateSymbolIdMap hook and is NOT checked here.
    static bool ValidateRegistrationPrereqs(const Registration& reg,
        RE::hkbBehaviorGraphData* graphData,
        RE::hkbStateMachine* rootSM,
        int32_t nextStateId, std::uint16_t nextNodeId)
    {
        if (!graphData || !graphData->stringData) {
            LOG_WARN("BehaviorPatcher: '{}' — graphData or stringData null.", reg.modName);
            return false;
        }
        if (!rootSM) {
            LOG_WARN("BehaviorPatcher: '{}' — rootSM null.", reg.modName);
            return false;
        }
        if (nextStateId > 60000 || static_cast<int32_t>(nextNodeId) > 60000) {
            LOG_WARN("BehaviorPatcher: '{}' — ID overflow risk (stateId={}, nodeId={}) — skipping.",
                reg.modName, nextStateId, static_cast<int32_t>(nextNodeId));
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Structural injection — called BEFORE original Activate
    //
    // Option-A path: used for non-switchboard root graphs when a registration
    // explicitly targets them.  Currently a no-op (all registrations target a
    // switchboard graph and go through Option-B), but kept for future use by
    // the Behavior Plugins system.
    //
    // InjectRegistrations is structural-only:
    //   - BehaviorReferenceGenerator
    //   - StateInfo
    //   - wildcard transition
    //   - nextUniqueID bump
    //
    // AppendEvent is called here to retrieve (or register as fallback) the event
    // index needed for the wildcard transition.  With Option-B the event is
    // already in the pre-baked HKX; AppendEvent will find it idempotently.
    // -----------------------------------------------------------------------

    /// Inject the structural parts of all pending registrations
    /// (indices [startIndex, regCount)) that target graphName.
    static void InjectRegistrations(RE::hkbBehaviorGraph* graph, const RE::hkbContext& ctx,
        std::string_view graphName, uint32_t startIndex)
    {
        std::lock_guard lock(s_regMutex);
        const uint32_t regCount = static_cast<uint32_t>(s_registrations.size());
        if (startIndex >= regCount) return;

        auto* graphData = graph->data.get();
        if (!graphData || !graphData->stringData) {
            LOG_WARN("BehaviorPatcher: graphData or stringData null — skipping.");
            return;
        }

        // Get root state machine.
        auto* rootSM = GetRootStateMachine(graph);
        if (!rootSM) {
            LOG_WARN("BehaviorPatcher: root generator is not an hkbStateMachine.");
            return;
        }

        if (rootSM->states.size() <= 0 || rootSM->states.size() > 10000) {
            LOG_WARN("BehaviorPatcher: states array size {} looks invalid.",
                rootSM->states.size());
            return;
        }

        int32_t nextStateId    = GetNextStateId(rootSM);
        // Use the static node ID range for injected generators.
        // Havok sizes nodePartitionInfo to exactly numStaticNodes slots (indexed by node->id).
        // Using nextUniqueID (the dynamic range, >= numStaticNodes) leaves the BRG without
        // a partition slot, which causes Havok to read OOB during Activate when it scans all
        // SM states — corrupting the partition assignment of the locomotion BlenderGenerator
        // in female characters, and preventing Generate() from ever being called on the BRG
        // when the player enters the TF state (so trueflight's BlenderGenerators never run).
        std::uint16_t nextNodeId = graph->numStaticNodes;

        LOG_INFO("BehaviorPatcher: structural injection into graph '{}' — "
                 "pending=[{}..{}), {} existing states, nextStateId={}, nextNodeId={} "
                 "(numStaticNodes={}, nextUniqueID={}).",
            graph->name.data() ? graph->name.data() : "unknown",
            startIndex, regCount, rootSM->states.size(), nextStateId, nextNodeId,
            graph->numStaticNodes, graph->nextUniqueID);

        // ── Diagnostic: wildcard transitions before injection ──
        if (rootSM->wildcardTransitions) {
            auto* existingTIA = reinterpret_cast<BSBTransitionInfoArray*>(
                rootSM->wildcardTransitions.get());
            LOG_INFO("BehaviorPatcher: wildcardTransitions={}, existing count={}.",
                static_cast<void*>(rootSM->wildcardTransitions.get()),
                existingTIA->transitions_size);
        } else {
            LOG_INFO("BehaviorPatcher: wildcardTransitions=null (will create new).");
        }

        int32_t injectedCount = 0;

        for (uint32_t i = startIndex; i < regCount; ++i) {
            const auto& reg = s_registrations[i];

            if (!RegistrationMatchesGraph(reg, graphName)) {
                LOG_DEBUG("BehaviorPatcher: skipping '{}' — graphName='{}' doesn't match '{}'.",
                    reg.modName, reg.graphName, graphName);
                continue;
            }

            // ── Diagnostic: log the BFR behaviorName we are about to set ──
            LOG_INFO("BehaviorPatcher: attempting structural injection for '{}' — "
                     "behaviorPath='{}'.",
                reg.modName, reg.behaviorPath);

            // Pre-validate structural prereqs.
            if (!ValidateRegistrationPrereqs(reg, graphData, rootSM,
                    nextStateId, nextNodeId)) {
                continue;
            }

            // ── 1. Look up (or register) the event index needed for the wildcard ──
            // AppendEvent is idempotent: returns the existing index if already present
            // in the pre-baked HKX, or registers it fresh if not.
            const int32_t eventIdx = AppendEvent(graphData, reg.eventName.c_str());
            if (eventIdx < 0) {
                LOG_WARN("BehaviorPatcher: event '{}' not found and cannot be registered "
                         "for '{}' — skipping.",
                    reg.eventName, reg.modName);
                continue;
            }

            // ── 2. Build BehaviorReferenceGenerator ──
            // Allocate persistent name buffers: hkStringPtr stores a raw const char* with
            // no ownership, so the pointed-to memory must outlive the behavior graph.
            // Local std::string .c_str() pointers dangle after the loop iteration ends.
            const std::string nodeNameStr = "BSB_" + reg.modName;
            auto* nodeNameBuf = static_cast<char*>(
                std::calloc(nodeNameStr.size() + 1, 1));
            if (!nodeNameBuf) {
                LOG_ERROR("BehaviorPatcher: failed to allocate node name for '{}' — skipping.", reg.modName);
                continue;
            }
            std::memcpy(nodeNameBuf, nodeNameStr.c_str(), nodeNameStr.size() + 1);
            s_allocations.push_back(nodeNameBuf);

            auto* behaviorRef = BuildBehaviorRef(
                reg.behaviorPath.c_str(), nodeNameBuf, nextNodeId++);
            if (!behaviorRef) {
                LOG_ERROR("BehaviorPatcher: BuildBehaviorRef failed for '{}' — skipping.", reg.modName);
                continue;
            }

            // ── 3. Build StateInfo ──
            const std::string stateNameStr = "BSB_State_" + reg.modName;
            auto* stateNameBuf = static_cast<char*>(
                std::calloc(stateNameStr.size() + 1, 1));
            if (!stateNameBuf) {
                LOG_ERROR("BehaviorPatcher: failed to allocate state name for '{}' — skipping.", reg.modName);
                continue;
            }
            std::memcpy(stateNameBuf, stateNameStr.c_str(), stateNameStr.size() + 1);
            s_allocations.push_back(stateNameBuf);

            auto* stateInfo = BuildStateInfo(
                reinterpret_cast<RE::hkbGenerator*>(behaviorRef),
                stateNameBuf, nextStateId);
            if (!stateInfo) {
                LOG_ERROR("BehaviorPatcher: BuildStateInfo failed for '{}' — skipping.", reg.modName);
                continue;
            }

            // ── 4. Append state to root SM ──
            AppendStateToSM(rootSM, stateInfo);

            // ── 5. Add wildcard transition ──
            AppendWildcardTransition(rootSM, eventIdx, nextStateId);

            LOG_INFO("BehaviorPatcher: [INJECTED] '{}' — state='{}' (id={}), "
                     "event='{}' (idx={}), behavior='{}'.",
                reg.modName, stateNameStr, nextStateId,
                reg.eventName, eventIdx, reg.behaviorPath);

            nextStateId++;
            injectedCount++;
        }

        // Reserve static partition slots for the injected BRG nodes AND for all
        // generators inside the sub-behaviors they reference.
        //
        // Sub-behaviors (e.g. trueflight.hkx) are NOT part of the character project
        // at startup, so their generators never go through CreateSymbolIdMap and every
        // hkbNode.id stays at the packfile default 0xFFFF.  Havok does NOT reassign
        // those IDs at BRG activation time — they remain 0xFFFF when the flight state
        // is entered.  Indexing nodePartitionInfo[0xFFFF] is always out-of-bounds,
        // silently corrupting heap memory until a vtable is clobbered and the game
        // crashes (~90 s of flight).
        //
        // Two-part fix:
        //   1. HERE (at InjectRegistrations time, before CreateSymbolIdMap):
        //      Extend numStaticNodes by kBSBNodeReservation slots per injected
        //      sub-behavior so the partition table is large enough to accommodate
        //      all sub-behavior nodes, and leave nextUniqueID pointing at the base
        //      of that reserved range.
        //   2. In BSB_Activate (when the sub-behavior graph is first activated):
        //      Walk the sub-graph's generator tree and assign IDs from
        //      [nextUniqueID, numStaticNodes) — the range reserved here.
        //      This happens BEFORE s_originalActivate builds the partition table.
        //
        // kBSBNodeReservation = 512 per registration: generous ceiling for any single
        // sub-behavior file.  Cost: sizeof(nodePartitionInfo entry) × 512 bytes extra.
        static constexpr std::uint16_t kBSBNodeReservation = 512;
        const std::uint32_t totalStatic =
            static_cast<std::uint32_t>(nextNodeId) +
            static_cast<std::uint32_t>(injectedCount) * kBSBNodeReservation;
        graph->numStaticNodes = static_cast<std::int16_t>(
            std::min(totalStatic, static_cast<std::uint32_t>(32767u)));
        // nextUniqueID = nextNodeId (right after the last BRG ID) so that the
        // BSB_Activate walk starts assigning from the base of the reserved range.
        graph->nextUniqueID = static_cast<std::int16_t>(nextNodeId);
        LOG_INFO("BehaviorPatcher: reserved {} static partition slots ({} sub-behavior(s), "
                 "{} slots each) — numStaticNodes={}, nextUniqueID={}.",
            static_cast<std::uint32_t>(injectedCount) * kBSBNodeReservation,
            injectedCount, kBSBNodeReservation,
            graph->numStaticNodes, graph->nextUniqueID);

        LOG_INFO("BehaviorPatcher: structural injection complete — {}/{} pending "
                 "registrations applied, {} total states in graph.",
            injectedCount, regCount - startIndex, rootSM->states.size());
    }

    // -----------------------------------------------------------------------
    // Full graph dump — called once per unique graphData* after Activate
    // -----------------------------------------------------------------------

    /// Dumps the complete root-SM state to the log.
    /// Fires once per unique graphData* per session (deduplicated by caller).
    /// charName is the character name at the time of first activation —
    /// used purely for context so male vs female entries can be distinguished.
    static void DumpGraphState(RE::hkbBehaviorGraph* graph, const char* charName)
    {
        auto* graphData = graph->data.get();
        auto* strData   = graphData ? graphData->stringData.get() : nullptr;

        LOG_INFO("GRAPH-DUMP ===== graph='{}' char='{}' numStaticNodes={} nextUniqueID={} "
                 "isLinked={} isActive={} =====",
            graph->name.data() ? graph->name.data() : "?",
            charName,
            graph->numStaticNodes,
            graph->nextUniqueID,
            graph->isLinked,
            graph->isActive);

        // ── Root SM metadata ──
        auto* rootSM = GetRootStateMachine(graph);
        if (!rootSM) {
            LOG_INFO("GRAPH-DUMP   rootSM: <null or non-SM root generator>");
        } else {
            LOG_INFO("GRAPH-DUMP   rootSM: startStateID={} currentStateID={} stateCount={}",
                rootSM->startStateID,
                rootSM->currentStateID,
                rootSM->states.size());

            // ── All SM states ──
            LOG_INFO("GRAPH-DUMP   states({}):", rootSM->states.size());
            for (int32_t i = 0; i < rootSM->states.size(); ++i) {
                auto* si = reinterpret_cast<BSBStateInfo*>(rootSM->states[i]);
                if (!si) {
                    LOG_INFO("GRAPH-DUMP     [{}] <null>", i);
                    continue;
                }
                const char* sname = si->name.data() ? si->name.data() : "<null>";
                // hkbNode::name is at offset 0x38 inside the generator object.
                const char* gname = "<null>";
                if (si->generator) {
                    const auto* namePtr = reinterpret_cast<const RE::hkStringPtr*>(
                        reinterpret_cast<const std::byte*>(si->generator) + 0x38);
                    if (namePtr->data()) gname = namePtr->data();
                }
                LOG_INFO("GRAPH-DUMP     [{}] stateId={} '{}' gen='{}'",
                    i, si->stateId, sname, gname);
            }

            // ── Wildcard transitions ──
            if (!rootSM->wildcardTransitions) {
                LOG_INFO("GRAPH-DUMP   wildcardTransitions: <none>");
            } else {
                auto* tia = reinterpret_cast<BSBTransitionInfoArray*>(
                    rootSM->wildcardTransitions.get());
                LOG_INFO("GRAPH-DUMP   wildcardTransitions({}):", tia->transitions_size);
                for (int32_t i = 0; i < tia->transitions_size; ++i) {
                    const auto& t = tia->transitions_data[i];
                    const char* evtName = "<unknown>";
                    if (strData && t.eventId >= 0 &&
                        t.eventId < static_cast<int32_t>(strData->eventNames.size())) {
                        const char* n = strData->eventNames[t.eventId].data();
                        if (n) evtName = n;
                    }
                    LOG_INFO("GRAPH-DUMP     [{}] event={}('{}') → stateId={} flags=0x{:04X}",
                        i, t.eventId, evtName, t.toStateId,
                        static_cast<uint16_t>(t.flags));
                }
            }
        }

        // ── Events — count + any BSB/TF events ──
        if (strData) {
            const int32_t evtCount = static_cast<int32_t>(strData->eventNames.size());
            LOG_INFO("GRAPH-DUMP   events: total={}", evtCount);
            for (int32_t i = 0; i < evtCount; ++i) {
                const char* n = strData->eventNames[i].data();
                if (n && (std::strncmp(n, "BSB_", 4) == 0 ||
                          std::strncmp(n, "TF_",  3) == 0)) {
                    LOG_INFO("GRAPH-DUMP     event[{}] '{}'", i, n);
                }
            }

            // ── Variables — count + any BSB/TF variables ──
            const int32_t varCount = static_cast<int32_t>(strData->variableNames.size());
            LOG_INFO("GRAPH-DUMP   variables: total={}", varCount);
            for (int32_t i = 0; i < varCount; ++i) {
                const char* n = strData->variableNames[i].data();
                if (n && (std::strncmp(n, "BSB_", 4) == 0 ||
                          std::strncmp(n, "TF_",  3) == 0)) {
                    LOG_INFO("GRAPH-DUMP     var[{}] '{}'", i, n);
                }
            }
        }

        LOG_INFO("GRAPH-DUMP ===== end =====");
    }

    // -----------------------------------------------------------------------
    // Sub-behavior node ID fix-up
    //
    // Sub-behaviors compiled standalone (e.g. TrueFlightBehavior.hkb) are
    // never processed by CreateSymbolIdMap.  Every hkbNode inside them has
    // id = 0xFFFF (the packfile default).  When Havok builds the partition
    // table during Activate it indexes nodePartitionInfo[node->id], so
    // id=0xFFFF (65535) is always out-of-bounds, corrupting the heap and
    // crashing ~90 s after the first flight.
    //
    // BSB reserves a static ID range in the parent graph via numStaticNodes
    // at InjectRegistrations time.  The assumption that Havok would fill those
    // IDs from nextUniqueID proved wrong — Havok leaves them at 0xFFFF.
    //
    // Fix: walk the sub-graph's generator tree before s_originalActivate and
    // assign sequential IDs from the parent graph's reserved range
    // [nextUniqueID, numStaticNodes).  Then set the sub-graph's own
    // numStaticNodes so its internal nodePartitionInfo is correctly sized.
    // -----------------------------------------------------------------------

    static void WalkAndAssignNodeIDs(
        RE::hkbGenerator*            gen,
        RE::hkbBehaviorGraph*        parentGraph,
        std::unordered_set<void*>&   visited)
    {
        if (!gen || !visited.insert(gen).second) return;

        // Assign a valid static ID if the node still has the packfile default.
        auto* node = reinterpret_cast<RE::hkbNode*>(gen);
        if (node->id == 0xFFFF &&
            parentGraph->nextUniqueID < parentGraph->numStaticNodes) {
            node->id = static_cast<std::uint16_t>(parentGraph->nextUniqueID++);
            LOG_DEBUG("BehaviorPatcher: sub-behavior node '{}' → id={}",
                node->name.data() ? node->name.data() : "?", node->id);
        }

        // Resolve vtable addresses once (lazy, thread-safe via static init).
        static const auto vtSM =
            REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbStateMachine[0] }.address();
        static const auto vtBlend =
            REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbBlenderGenerator[0] }.address();
        static const auto vtCyclic =
            REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSCyclicBlendTransitionGenerator[0] }.address();
        static const auto vtModGen =
            REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbModifierGenerator[0] }.address();
        static const auto vtBRG =
            REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbBehaviorReferenceGenerator[0] }.address();

        const auto vtable = *reinterpret_cast<std::uintptr_t*>(gen);

        if (vtable == vtSM) {
            // State machine — recurse into each state's generator.
            auto* sm = reinterpret_cast<RE::hkbStateMachine*>(gen);
            for (auto* statePtr : sm->states) {
                if (statePtr) {
                    auto* si = reinterpret_cast<BSBStateInfo*>(statePtr);
                    WalkAndAssignNodeIDs(si->generator, parentGraph, visited);
                }
            }
        } else if (vtable == vtCyclic) {
            // BSCyclicBlendTransitionGenerator — has a single blender child.
            auto* cb = reinterpret_cast<BSBCyclicBlendGen*>(gen);
            WalkAndAssignNodeIDs(cb->pBlenderGenerator, parentGraph, visited);
        } else if (vtable == vtBlend) {
            // hkbBlenderGenerator — recurse into each child's generator.
            auto* bg = reinterpret_cast<BSBBlenderGen*>(gen);
            if (bg->children_data) {
                for (std::int32_t i = 0; i < bg->children_size; ++i) {
                    auto* child = reinterpret_cast<BSBBlenderGenChild*>(bg->children_data[i]);
                    if (child) {
                        WalkAndAssignNodeIDs(child->generator, parentGraph, visited);
                    }
                }
            }
        } else if (vtable == vtModGen) {
            // hkbModifierGenerator — wrap around a child generator.
            auto* mg = reinterpret_cast<BSBModifierGen*>(gen);
            WalkAndAssignNodeIDs(mg->generator, parentGraph, visited);
        } else if (vtable == vtBRG) {
            // Nested BRG — its own sub-behavior will be fixed when it activates.
        }
        // All other types (hkbClipGenerator, etc.) are leaf generators;
        // the ID assignment above is all they need.
    }

    // -----------------------------------------------------------------------
    // Vtable hook on hkbBehaviorGraph::Activate
    // -----------------------------------------------------------------------

    /// Returns true if graphName identifies a BSB-managed switchboard graph.
    /// Add a new entry here whenever a new creature switchboard is introduced.
    static bool IsSwitchboardGraph(std::string_view graphName)
    {
        return graphName == "BehaviorSwitchboard.hkb" ||
               graphName == "DragonBehaviorSwitchboard.hkb";
    }

    using ActivateFn = void(*)(RE::hkbBehaviorGraph*, const RE::hkbContext&);
    static ActivateFn s_originalActivate = nullptr;

    static void BSB_Activate(RE::hkbBehaviorGraph* a_this, const RE::hkbContext& a_context)
    {
        auto* character = a_context.character;
        const char* charName = (character && character->name.data())
            ? character->name.data() : "unknown";
        const char* graphName = a_this->name.data() ? a_this->name.data() : "null";

        // Guard: only inject if this is the CHARACTER's root behavior graph OR if
        // at least one registration explicitly names this graph (e.g. a mod targeting
        // "BehaviorSwitchboard.hkb" instead of "0_Master.hkb").
        //
        // When character is null the graph belongs to a pure object (forge, sharpening
        // wheel, autoplay trigger, etc.) — treat it the same as an unmatched sub-behavior
        // and let the hasMatchingReg check below decide.  Using !character as a short-cut
        // for isRootGraph would cause every object behavior graph to enter the injection
        // path, pollute s_patchedVersions, and emit spurious GRAPH-DUMPs.
        const bool isRootGraph = character && character->behaviorGraph.get() == a_this;
        bool hasMatchingReg = false;
        if (!isRootGraph && graphName) {
            std::lock_guard lock(s_regMutex);
            for (const auto& reg : s_registrations) {
                if (!reg.graphName.empty() && reg.graphName == std::string_view(graphName)) {
                    hasMatchingReg = true;
                    break;
                }
            }
        }

        if (!isRootGraph && !hasMatchingReg) {
            // ── Sub-behavior node ID fix-up ──────────────────────────────────
            // Sub-behaviors (e.g. TrueFlightBehavior.hkb) are compiled standalone
            // and never go through CreateSymbolIdMap, leaving every hkbNode.id at
            // the packfile default 0xFFFF.  Walk the generator tree now, BEFORE
            // s_originalActivate builds the partition table, and assign valid IDs
            // from the parent graph's reserved static range.
            if (character) {
                // Prefer the character's captured switchboard graph — it holds the
                // static node ID reservation made during the option-B path.
                // Fall back to character->behaviorGraph (root) only if the switchboard
                // hasn't activated yet for this character.
                RE::hkbBehaviorGraph* parentGraph = nullptr;
                {
                    std::lock_guard lock(s_switchboardGraphsMutex);
                    const auto it = s_switchboardGraphs.find(character);
                    if (it != s_switchboardGraphs.end() && it->second != a_this)
                        parentGraph = it->second;
                }
                if (!parentGraph)
                    parentGraph = character->behaviorGraph.get();
                if (parentGraph && parentGraph != a_this &&
                    parentGraph->nextUniqueID < parentGraph->numStaticNodes) {
                    LOG_INFO("BehaviorPatcher: sub-behavior '{}' — fixing node IDs "
                             "(parent='{}' nextUniqueID={} numStaticNodes={}).",
                        graphName ? graphName : "?",
                        parentGraph->name.data() ? parentGraph->name.data() : "?",
                        parentGraph->nextUniqueID, parentGraph->numStaticNodes);

                    // Assign ID to the behavior graph node itself.
                    auto* bgNode = reinterpret_cast<RE::hkbNode*>(a_this);
                    if (bgNode->id == 0xFFFF &&
                        parentGraph->nextUniqueID < parentGraph->numStaticNodes) {
                        bgNode->id = static_cast<std::uint16_t>(parentGraph->nextUniqueID++);
                    }

                    // Walk and fix all generator nodes in the sub-graph.
                    std::unordered_set<void*> visited;
                    WalkAndAssignNodeIDs(
                        a_this->rootGenerator.get(), parentGraph, visited);

                    // Size the sub-graph's own partition table to match.
                    a_this->numStaticNodes = parentGraph->nextUniqueID;
                    a_this->nextUniqueID   = a_this->numStaticNodes;

                    LOG_INFO("BehaviorPatcher: sub-behavior '{}' fix-up complete — "
                             "{} generator nodes visited, parentGraph->nextUniqueID={}.",
                        graphName ? graphName : "?",
                        visited.size(), parentGraph->nextUniqueID);
                }
            }
            LOG_DEBUG("BehaviorPatcher: skipping injection for sub-behavior '{}' / '{}'.",
                graphName, charName);
        } else {
            // Version-based guard: inject only registrations not yet applied to this
            // graphData.  Multiple NPC instances share the same graphData — modifying
            // any one mutates the shared data for all.  Using version counts (rather
            // than a plain "seen" flag) ensures late-arriving registrations are picked
            // up on the next Activate without re-injecting already-applied entries.
            auto* graphData = a_this->data.get();
            // Switchboard graphs (BehaviorSwitchboard.hkb, DragonBehaviorSwitchboard.hkb,
            // etc.) use the option-B path: skip runtime injection and instead manage
            // numStaticNodes, startStateID, and sub-behavior node IDs directly.
            const bool isBSBGraph = graphName && IsSwitchboardGraph(graphName);
            uint32_t regCount = 0;
            uint32_t subBehaviorCount = 0;
            {
                std::lock_guard lock(s_regMutex);
                regCount = static_cast<uint32_t>(s_registrations.size());
                if (isBSBGraph) {
                    for (const auto& reg : s_registrations) {
                        if (!reg.graphName.empty() &&
                            reg.graphName == std::string_view(graphName))
                            ++subBehaviorCount;
                    }
                }
            }
            if (isBSBGraph && character) {
                std::lock_guard lock(s_switchboardGraphsMutex);
                s_switchboardGraphs[character] = a_this;
                LOG_INFO("BehaviorPatcher: switchboard graph '{}' captured for character {:p}.",
                    graphName, (void*)character);
            }

            // Option B: BehaviorSwitchboard.hkb uses a pre-patched HKX (served by
            // BehaviorFileInterceptor) that already contains all BRGs + wildcard
            // transitions compiled from YAML.  Skip runtime InjectRegistrations to
            // avoid duplicating nodes that LinkProject has already resolved.
            //
            // We still bump numStaticNodes to reserve ID space for sub-behavior
            // WalkAndAssignNodeIDs (trueflight.hkx nodes draw from this pool).
            if (isBSBGraph) {
                if (subBehaviorCount > 0) {
                    // baseCount = number of compiled nodes in the pre-baked HKX.
                    // After bumping, the reserved ID range for sub-behavior nodes is
                    // [baseCount, numStaticNodes).  WalkAndAssignNodeIDs draws from
                    // this range using nextUniqueID as the current cursor.
                    const uint32_t baseCount = static_cast<uint32_t>(a_this->numStaticNodes);
                    uint32_t reserved = baseCount + 512u * subBehaviorCount;
                    a_this->numStaticNodes = static_cast<std::int16_t>(
                        std::min(reserved, static_cast<uint32_t>(32767u)));
                    // nextUniqueID = start of reserved range (= baseCount),
                    // NOT numStaticNodes (end of range) — that would leave 0 slots.
                    a_this->nextUniqueID = static_cast<std::int16_t>(baseCount);
                    LOG_INFO("BehaviorPatcher: option-B reserve — baseCount={} "
                             "numStaticNodes={} nextUniqueID={} ({} sub-behavior(s)).",
                        baseCount, a_this->numStaticNodes,
                        a_this->nextUniqueID, subBehaviorCount);
                }
                LOG_INFO("BehaviorPatcher: '{}' — skipping runtime injection (option-B pre-patched HKX).",
                    graphName);

                // ── Set startStateID to the pending event's pre-baked state ──────────────
                // The pre-baked HKX assigns state IDs in registration order:
                //   stateId=0  → BSB_IdleState   (base/idle placeholder)
                //   stateId=1  → first BSB-targeting registration
                //   stateId=2  → second BSB-targeting registration
                //   ...
                // SetPendingBSBEvent was called just before NotifyAnimationGraph so
                // we know which event triggered this activation.  Find its 1-based index
                // among BSB registrations and pin startStateID to that state so Havok
                // starts the SM in the correct state rather than A-posing in IdleState.
                {
                    const std::string pendingEvt = GetAndClearPendingBSBEvent();
                    if (!pendingEvt.empty()) {
                        auto* rootSM = GetRootStateMachine(a_this);
                        if (rootSM) {
                            int32_t matchStateId = 0;
                            {
                                std::lock_guard lock(s_regMutex);
                                int32_t bsbIdx = 0;
                                for (const auto& reg : s_registrations) {
                                    if (!reg.graphName.empty() &&
                                        reg.graphName == std::string_view(graphName)) {
                                        ++bsbIdx;
                                        if (reg.eventName == pendingEvt) {
                                            matchStateId = bsbIdx;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (matchStateId > 0) {
                                rootSM->startStateID = matchStateId;
                                LOG_INFO("BehaviorPatcher: option-B — startStateID set to {} "
                                         "for pending event '{}'.",
                                    matchStateId, pendingEvt);
                            } else {
                                LOG_WARN("BehaviorPatcher: option-B — no BSB registration "
                                         "matched pending event '{}' — startStateID stays {}.",
                                    pendingEvt, rootSM->startStateID);
                            }
                        } else {
                            LOG_WARN("BehaviorPatcher: option-B — rootSM null, cannot "
                                     "set startStateID for event '{}'.", pendingEvt);
                        }
                    } else {
                        LOG_DEBUG("BehaviorPatcher: option-B — no pending BSB event "
                                  "(startStateID stays at default).");
                    }
                }

            } else {
                const int32_t pendingStart = GetPendingStart(graphData, regCount);
                if (pendingStart >= 0) {
                    LOG_INFO("BehaviorPatcher: Activate hook fired for '{}' "
                             "(graph={}, name='{}', pending=[{}..{})).",
                        charName, (void*)a_this, graphName, pendingStart, regCount);
                    InjectRegistrations(a_this, a_context,
                        graphName ? std::string_view(graphName) : std::string_view{},
                        static_cast<uint32_t>(pendingStart));
                } else {
                    LOG_DEBUG("BehaviorPatcher: graphData fully patched for '{}' "
                              "(graph={}, name='{}').",
                        charName, (void*)a_this, graphName);
                }
            }
        }

        // Call the original Activate — Havok builds stateIDToIndexMap,
        // eventIDMap, variableIDMap etc. with our data already in place.
        s_originalActivate(a_this, a_context);

        // -----------------------------------------------------------------
        // Diagnostic: dump TrueFlight animation indices from the character's
        // animation list.  Only logs once per unique charStrData pointer so
        // that multiple NPC instances sharing the same graphData don't flood
        // the log with identical entries.
        // -----------------------------------------------------------------
        if (character && character->behaviorGraph.get() == a_this) {
            auto* charSetup   = character->setup.get();
            auto* charData    = charSetup ? charSetup->data.get() : nullptr;
            auto* charStrData = charData ? charData->stringData.get() : nullptr;
            if (charStrData) {
                std::lock_guard diagLock(s_animDiagMutex);
                const bool firstTime = s_animDiagReported.insert(charStrData).second;
                if (firstTime) {
                    int32_t animCount = static_cast<int32_t>(charStrData->animationNames.size());
                    LOG_DEBUG("BehaviorPatcher: ANIM-DIAG graph='{}' — {} total animations.",
                        graphName, animCount);

                    // Dump all TrueFlight animations and their indices.
                    for (int32_t i = 0; i < animCount; ++i) {
                        const char* name = charStrData->animationNames[i].data();
                        if (name && (std::strstr(name, "TrueFlight") ||
                                     std::strstr(name, "Hover") ||
                                     std::strstr(name, "Flight")))
                        {
                            LOG_DEBUG("BehaviorPatcher: ANIM-DIAG  [{}] {}", i, name);
                        }
                    }
                }
            }
        }

        // Post-Activate diagnostic: verify the graph was linked and scan for BSB events.
        if (a_this->isLinked) {
            auto* gd = a_this->data.get();
            auto* sd = gd ? gd->stringData.get() : nullptr;
            if (sd) {
                const int32_t eventCount = static_cast<int32_t>(sd->eventNames.size());
                std::lock_guard lock(s_regMutex);
                for (const auto& reg : s_registrations) {
                    for (int32_t i = 0; i < eventCount; ++i) {
                        if (sd->eventNames[i].data() &&
                            std::strcmp(sd->eventNames[i].data(), reg.eventName.c_str()) == 0) {
                            LOG_INFO("BehaviorPatcher: POST-ACTIVATE '{}' event '{}' "
                                     "confirmed at stringData index {} (of {}). "
                                     "isLinked={}, isActive={}.",
                                graphName, reg.eventName, i, eventCount,
                                a_this->isLinked, a_this->isActive);
                            break;
                        }
                    }
                }
            }
        } else {
            LOG_DEBUG("BehaviorPatcher: POST-ACTIVATE graph '{}' is NOT linked!",
                graphName);
        }

        // -----------------------------------------------------------------
        // GRAPH-DUMP: dump the full root-SM state once per unique graphData*
        // so we can diff male vs female graphs and spot injected-state
        // collisions, wrong wildcard flags, or corrupt startStateID.
        // -----------------------------------------------------------------
        {
            auto* graphData = a_this->data.get();
            if (graphData) {
                std::lock_guard dumpLock(s_graphDumpMutex);
                const bool firstTime = s_graphDumpReported.insert(graphData).second;
                if (firstTime) {
                    DumpGraphState(a_this, charName);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Install
    // -----------------------------------------------------------------------

    static bool s_installed = false;

    void Install()
    {
        if (s_installed) return;
        s_installed = true;

        // ── 1. Vtable hook on hkbBehaviorGraph::Activate (virtual index 04) ──
        // Fires before Havok builds stateIDToIndexMap. BSB injects the structural
        // content here (BehaviorReferenceGenerator, StateInfo, wildcard transitions).
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_hkbBehaviorGraph[0] };
        s_originalActivate = reinterpret_cast<ActivateFn>(vtbl.write_vfunc(0x04, &BSB_Activate));

        LOG_INFO("BehaviorPatcher: vtable hook installed on hkbBehaviorGraph::Activate.");

    }

}
