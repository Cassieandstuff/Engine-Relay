#include "PCH.h"
#include "WildcardGate.h"
#include "HavokTypes.h"

#include <unordered_set>

namespace EngineRelay::WildcardGate {

    // =======================================================================
    // ERCondition — custom hkbCondition implementation
    //
    // We can't use hkbExpressionCondition because it requires expression
    // linking (compilation) during hkbBehaviorGraph::link(). Our conditions
    // are injected AFTER linking, so the internal compiled state is never
    // populated and isTrue() crashes dereferencing nullptr.
    //
    // Instead, we build a custom condition object with our OWN vtable that
    // overrides isTrue() to directly read the ER_Active variable from the
    // behavior graph's runtime variable state. No expression parsing needed.
    //
    // Layout matches hkReferencedObject (0x10) + our custom fields:
    //   + varIndex (int32)  offset 0x10 — cached index of ER_Active
    //   + pad14            offset 0x14
    //
    // Size: 0x18
    // =======================================================================

    struct ERCondition
    {
        void*           vtable;           // 0x00
        std::uint16_t   memSizeAndFlags;  // 0x08
        std::uint16_t   referenceCount;   // 0x0A
        std::uint32_t   pad0C;            // 0x0C
        std::int32_t    varIndex;         // 0x10  — ER_Active variable index
        std::uint32_t   pad14;            // 0x14
    };
    static_assert(sizeof(ERCondition) == 0x18);

    // =======================================================================
    // Custom vtable for ERCondition
    //
    // hkbCondition inherits hkReferencedObject. Vtable layout (MSVC x64):
    //   [0] destructor (deleting)
    //   [1] GetClassType() → returns nullptr (no RTTI class)
    //   [2] CalcContentStatistics() → no-op
    //   [3] isTrue(const hkbContext&) → OUR implementation
    //
    // We copy slots 0-2 from hkbExpressionCondition (they're fine as-is —
    // destructor just frees, GetClassType returns class ptr, stats is no-op).
    // Slot 3 is replaced with our ERCondition_isTrue.
    // =======================================================================

    static std::uintptr_t s_conditionVtable[8] = {};  // custom vtable (padded)

    /// Safe no-op destructor for ERCondition (vtable slot 0).
    ///
    /// We must NOT use hkbExpressionCondition's destructor here. That destructor
    /// expects the hkbExpressionCondition layout and will attempt to destroy
    /// m_internalExpression at offset 0x18 — exactly where our struct ends.
    /// If Havok decrements our refcount to zero during graph teardown it calls
    /// vtable[0], which would read past our 0x18-byte allocation and corrupt
    /// memory (or crash) trying to destroy a string field that doesn't exist.
    ///
    /// Our condition objects are allocated with std::calloc and live for the game
    /// session. We intentionally skip destruction here. Havok must not free them
    /// via its allocator either since they were allocated with std::calloc.
    ///
    /// Signature matches MSVC x64 scalar deleting destructor: (this*, uint flags)
    static void __fastcall ERCondition_dtor(ERCondition* /*a_this*/, unsigned int /*a_flags*/)
    {
        // Intentional no-op.
    }

    /// Custom isTrue() — reads ER_Active directly from the graph's variable state.
    /// Returns true when ER_Active == 0 (normal gameplay: wildcards fire).
    /// Returns false when ER_Active != 0 (in sub-behavior: wildcards suppressed).
    ///
    /// Signature: bool __fastcall isTrue(this*, const hkbContext*)
    static bool __fastcall ERCondition_isTrue(ERCondition* a_this, const RE::hkbContext* a_context)
    {
        if (!a_context || !a_context->behavior) return true;  // default: let wildcards fire

        // hkbBehaviorGraph::variableValueSet is at offset 0xD8 (hkRefVariant = void*).
        auto* graph = a_context->behavior;
        auto* valueSet = *reinterpret_cast<ERVariableValueSet**>(
            reinterpret_cast<std::byte*>(graph) + 0xD8);
        if (!valueSet || !valueSet->wordValues_data) return true;

        std::int32_t idx = a_this->varIndex;
        if (idx < 0 || idx >= valueSet->wordValues_size) return true;

        // ER_Active == 0 → condition is TRUE → wildcard fires normally.
        // ER_Active != 0 → condition is FALSE → wildcard suppressed.
        return (valueSet->wordValues_data[idx].value == 0);
    }

    // =======================================================================
    // Constants
    // =======================================================================

    static constexpr const char* kVarName = "ER_Active";

    // =======================================================================
    // State — dedup and allocation tracking
    // =======================================================================

    static std::mutex                                     s_mutex;
    static std::unordered_set<RE::hkbBehaviorGraphData*> s_gatedGraphs;

    // =======================================================================
    // Cached vtable addresses for generator type identification
    // =======================================================================

    struct VtableCache {
        std::uintptr_t modifierGen    = 0;
        std::uintptr_t blenderGen     = 0;
        std::uintptr_t poseMatching   = 0;  // inherits blenderGen layout
        std::uintptr_t cyclicBlend    = 0;
        std::uintptr_t manualSelector = 0;
        std::uintptr_t iStateTagging  = 0;
        std::uintptr_t offsetAnim     = 0;
        std::uintptr_t boneSwitch     = 0;
        bool initialized = false;

        void Init() {
            if (initialized) return;
            modifierGen    = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbModifierGenerator[0] }.address();
            blenderGen     = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbBlenderGenerator[0] }.address();
            poseMatching   = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbPoseMatchingGenerator[0] }.address();
            cyclicBlend    = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSCyclicBlendTransitionGenerator[0] }.address();
            manualSelector = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbManualSelectorGenerator[0] }.address();
            iStateTagging  = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSiStateTaggingGenerator[0] }.address();
            offsetAnim     = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSOffsetAnimationGenerator[0] }.address();
            boneSwitch     = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSBoneSwitchGenerator[0] }.address();
            initialized = true;
        }
    };
    static VtableCache s_vtables;

    static std::uintptr_t GetVtable(void* obj) {
        return *reinterpret_cast<std::uintptr_t*>(obj);
    }

    // =======================================================================
    // Helpers — allocation
    // =======================================================================

    template <typename T>
    static T* Alloc()
    {
        auto* obj = static_cast<T*>(std::calloc(1, sizeof(T)));
        return obj;
    }

    // =======================================================================
    // Build custom condition
    // =======================================================================

    static ERCondition* BuildCondition(std::int32_t varIndex)
    {
        auto* cond = Alloc<ERCondition>();
        if (!cond) return nullptr;

        cond->vtable          = reinterpret_cast<void*>(&s_conditionVtable[0]);
        cond->memSizeAndFlags = 0x18;
        cond->referenceCount  = 1;
        cond->varIndex        = varIndex;

        return cond;
    }

    // =======================================================================
    // Variable lookup (READ-ONLY)
    //
    // Finds "ER_Active" in the graph's variable data and returns its index.
    // Returns -1 if not found (graph doesn't have it statically declared).
    //
    // IMPORTANT: Does NOT inject variables at runtime. Growing graphData arrays
    // after the behavior system allocates runtime state arrays causes the
    // expression evaluator to desync (NULL word values → crash). Non-humanoid
    // graphs that lack ER_Active are simply skipped — they don't need gating
    // because ER sub-behaviors only target humanoid actors.
    // =======================================================================

    static std::int32_t FindERActiveVariable(RE::hkbBehaviorGraphData* graphData)
    {
        if (!graphData || !graphData->stringData) return -1;

        auto* strData = graphData->stringData.get();

        for (std::int32_t i = 0; i < static_cast<std::int32_t>(strData->variableNames.size()); ++i) {
            const char* n = strData->variableNames[i].data();
            if (n && std::strcmp(n, kVarName) == 0) {
                LOG_DEBUG("WildcardGate: '{}' found at variable index {}.", kVarName, i);
                return i;
            }
        }

        return -1;
    }

    // =======================================================================
    // State machine walking and transition gating
    // =======================================================================

    /// Gate global wildcard transitions on a single state machine.
    /// Returns the number of transitions gated.
    static int32_t GateWildcards(RE::hkbStateMachine* sm, ERCondition* conditionObj)
    {
        if (!sm->wildcardTransitions) return 0;

        auto* tia = reinterpret_cast<ERTransitionInfoArray*>(sm->wildcardTransitions.get());
        if (!tia->transitions_data || tia->transitions_size <= 0) return 0;

        int32_t gated = 0;

        for (int32_t i = 0; i < tia->transitions_size; ++i) {
            auto& t = tia->transitions_data[i];

            // Only gate global wildcards. Local wildcards are fine — they only
            // fire within their own state machine's scope.
            if ((t.flags & kTransFlag_IsGlobalWildcard) == 0)
                continue;

            // Skip if already conditioned (don't double-gate).
            if (t.condition != nullptr)
                continue;

            // Assign our condition and clear FLAG_DISABLE_CONDITION so Havok
            // actually evaluates it. Vanilla transitions with no condition have
            // this flag set (meaning "nothing to evaluate") — we must clear it
            // now that we've added something to evaluate.
            //
            // ER's own entry/exit transitions (TF_EnterFlight, TF_ExitFlight)
            // are safe because of ordering: the entry event fires while ER_Active
            // is still 0 (condition TRUE), and Deactivate() resets ER_Active to 0
            // before firing any exit event.
            t.condition = conditionObj;
            t.flags &= ~kTransFlag_DisableCondition;
            ++gated;
        }

        return gated;
    }

    /// Recursively walk all generators to find hkbStateMachines and gate them.
    /// Returns total transitions gated.
    static int32_t WalkAndGate(RE::hkbGenerator* gen, ERCondition* cond,
        std::unordered_set<void*>& visited)
    {
        if (!gen || !visited.insert(gen).second)
            return 0;

        int32_t total = 0;

        // Check if this generator IS a state machine (skyrim_cast uses RTTI).
        if (auto* sm = skyrim_cast<RE::hkbStateMachine*>(gen)) {
            total += GateWildcards(sm, cond);

            // Walk into states.
            for (int32_t i = 0; i < sm->states.size(); ++i) {
                auto* state = sm->states[i];
                if (!state) continue;
                auto* realState = reinterpret_cast<ERStateInfo*>(state);

                // Gate per-state transitions too (state-level global wildcards).
                if (realState->transitions) {
                    auto* stTIA = realState->transitions;
                    for (int32_t j = 0; j < stTIA->transitions_size; ++j) {
                        auto& t = stTIA->transitions_data[j];
                        if ((t.flags & kTransFlag_IsGlobalWildcard) != 0 &&
                            t.condition == nullptr) {
                            t.condition = cond;
                            t.flags &= ~kTransFlag_DisableCondition;
                            ++total;
                        }
                    }
                }

                // Recurse into the state's generator.
                if (realState->generator) {
                    total += WalkAndGate(realState->generator, cond, visited);
                }
            }
            return total;
        }

        // For non-SM generators, identify by vtable and walk children.
        auto vtbl = GetVtable(gen);

        // hkbModifierGenerator: modifier(0x48) + generator(0x50)
        if (vtbl == s_vtables.modifierGen) {
            auto* mg = reinterpret_cast<ERModifierGen*>(gen);
            total += WalkAndGate(mg->generator, cond, visited);
            return total;
        }

        // hkbBlenderGenerator / hkbPoseMatchingGenerator: children array at offset 0x68
        // hkbPoseMatchingGenerator inherits hkbBlenderGenerator and extends it at
        // offset 0xA0+. The children array layout is identical — check both vtables.
        if (vtbl == s_vtables.blenderGen || vtbl == s_vtables.poseMatching) {
            auto* bg = reinterpret_cast<ERBlenderGen*>(gen);
            for (int32_t i = 0; i < bg->children_size; ++i) {
                auto* child = reinterpret_cast<ERBlenderGenChild*>(bg->children_data[i]);
                if (child && child->generator) {
                    total += WalkAndGate(child->generator, cond, visited);
                }
            }
            return total;
        }

        // BSCyclicBlendTransitionGenerator: pBlenderGenerator at offset 0x48
        if (vtbl == s_vtables.cyclicBlend) {
            auto* cbg = reinterpret_cast<ERCyclicBlendGen*>(gen);
            total += WalkAndGate(cbg->pBlenderGenerator, cond, visited);
            return total;
        }

        // hkbManualSelectorGenerator: generators array at offset 0x48
        // Used extensively for weapon type selection (1H/2H/magic/etc.)
        if (vtbl == s_vtables.manualSelector) {
            auto* msg = reinterpret_cast<ERManualSelectorGen*>(gen);
            for (int32_t i = 0; i < msg->generators_size; ++i) {
                if (msg->generators_data[i]) {
                    total += WalkAndGate(msg->generators_data[i], cond, visited);
                }
            }
            return total;
        }

        // BSiStateTaggingGenerator: pDefaultGenerator at offset 0x50
        if (vtbl == s_vtables.iStateTagging) {
            auto* stg = reinterpret_cast<ERiStateTaggingGen*>(gen);
            if (stg->pDefaultGenerator) {
                total += WalkAndGate(stg->pDefaultGenerator, cond, visited);
            }
            return total;
        }

        // BSOffsetAnimationGenerator: pDefaultGenerator(0x50) + pOffsetClipGenerator(0x60)
        if (vtbl == s_vtables.offsetAnim) {
            auto* oag = reinterpret_cast<EROffsetAnimGen*>(gen);
            if (oag->pDefaultGenerator) {
                total += WalkAndGate(oag->pDefaultGenerator, cond, visited);
            }
            if (oag->pOffsetClipGenerator) {
                total += WalkAndGate(oag->pOffsetClipGenerator, cond, visited);
            }
            return total;
        }

        // BSBoneSwitchGenerator: pDefaultGenerator(0x50) + ChildrenA array(0x58)
        if (vtbl == s_vtables.boneSwitch) {
            auto* bsg = reinterpret_cast<ERBoneSwitchGen*>(gen);
            if (bsg->pDefaultGenerator) {
                total += WalkAndGate(bsg->pDefaultGenerator, cond, visited);
            }
            for (int32_t i = 0; i < bsg->childrenA_size; ++i) {
                auto* boneData = reinterpret_cast<ERBoneSwitchBoneData*>(bsg->childrenA_data[i]);
                if (boneData && boneData->pGenerator) {
                    total += WalkAndGate(boneData->pGenerator, cond, visited);
                }
            }
            return total;
        }

        // Unknown generator type — can't walk further. This is fine;
        // most generators (clip, reference, etc.) don't contain child SMs.
        return total;
    }

    // =======================================================================
    // InitHavok hook — RE::Character vtable slot 0x66
    //
    // Fires after the actor's behavior system is fully loaded and linked.
    // We walk the root behavior graph and gate global wildcard transitions.
    // =======================================================================

    using InitHavokFn = void(*)(RE::Character*);
    static InitHavokFn s_originalInitHavok = nullptr;

    static void Hook_InitHavok(RE::Character* a_character)
    {
        // Call original — behavior system loads here.
        s_originalInitHavok(a_character);

        // Get animation graph manager.
        RE::BSAnimationGraphManagerPtr graphMgr;
        a_character->GetAnimationGraphManager(graphMgr);
        if (!graphMgr) return;

        // Get active graph.
        if (graphMgr->graphs.empty()) return;
        auto& activeGraph = graphMgr->graphs[graphMgr->activeGraph];
        if (!activeGraph) return;

        // Get behavior graph from character instance.
        auto* character = &activeGraph->characterInstance;
        auto* behaviorGraph = character->behaviorGraph.get();
        if (!behaviorGraph) return;

        auto* graphData = behaviorGraph->data.get();
        if (!graphData) return;

        // Dedup: only gate each unique graph data once.
        {
            std::lock_guard lock(s_mutex);
            if (!s_gatedGraphs.insert(graphData).second) return;
        }

        // Acquire the graph manager's update lock for thread safety.
        RE::BSSpinLockGuard lockGuard(graphMgr->updateLock);

        // ── 1. Find ER_Active variable index (read-only) ──
        // Only graphs with ER_Active statically declared (humanoid 0_master) get
        // gated. Non-humanoid graphs (Draugr, Dragon, etc.) are skipped — they
        // don't need gating because ER sub-behaviors only target humanoid actors.
        std::int32_t varIndex = FindERActiveVariable(graphData);
        if (varIndex < 0) {
            LOG_DEBUG("WildcardGate: '{}' not found in project '{}' — skipping (non-humanoid graph).",
                kVarName, activeGraph->projectName.c_str());
            return;
        }

        // ── 2. Build the custom condition object ──
        // Uses our own vtable with ERCondition_isTrue() that reads ER_Active
        // directly from the graph's runtime variable state — no expression
        // compilation/linking required.
        auto* condition = BuildCondition(varIndex);
        if (!condition) {
            LOG_ERROR("WildcardGate: failed to allocate condition object.");
            return;
        }

        // ── 3. Walk all state machines and gate global wildcards ──
        auto* rootGen = behaviorGraph->rootGenerator.get();
        if (!rootGen) return;

        std::unordered_set<void*> visited;
        int32_t gatedCount = WalkAndGate(rootGen, condition, visited);

        if (gatedCount > 0) {
            LOG_INFO("WildcardGate: gated {} global wildcard transition(s) in project '{}'.",
                gatedCount, activeGraph->projectName.c_str());
        } else {
            LOG_DEBUG("WildcardGate: no global wildcard transitions found in project '{}'.",
                activeGraph->projectName.c_str());
        }
    }

    // =======================================================================
    // Public API
    // =======================================================================

    void Install()
    {
        s_vtables.Init();

        // ── Build custom condition vtable ──
        // Copy slots 0-3 from hkbExpressionCondition as a baseline, then replace:
        //   [0] destructor  → ERCondition_dtor   (safe no-op; prevents dtor from
        //                     accessing m_internalExpression at offset 0x18 which
        //                     our smaller struct doesn't have)
        //   [3] isTrue      → ERCondition_isTrue (reads ER_Active from variable state)
        REL::Relocation<std::uintptr_t> exprVtbl{ RE::VTABLE_hkbExpressionCondition[0] };
        auto* srcVtable = reinterpret_cast<std::uintptr_t*>(exprVtbl.address());
        for (int i = 0; i < 4; ++i) {
            s_conditionVtable[i] = srcVtable[i];
        }
        s_conditionVtable[0] = reinterpret_cast<std::uintptr_t>(&ERCondition_dtor);
        s_conditionVtable[3] = reinterpret_cast<std::uintptr_t>(&ERCondition_isTrue);

        LOG_INFO("WildcardGate: custom condition vtable built (dtor+isTrue replaced).");

        // ── Install InitHavok hook ──
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
        s_originalInitHavok = reinterpret_cast<InitHavokFn>(
            vtbl.write_vfunc(0x66, &Hook_InitHavok));

        LOG_INFO("WildcardGate: InitHavok hook installed (Character vtable slot 0x66).");
    }

    void ClearGatedSet()
    {
        std::lock_guard lock(s_mutex);
        s_gatedGraphs.clear();
        LOG_INFO("WildcardGate: gated set cleared — will re-gate on next InitHavok.");
    }

}
