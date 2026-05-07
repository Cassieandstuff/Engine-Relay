#pragma once

#include "PCH.h"

namespace EngineRelay::WildcardGate {

    /// Install the InitHavok vtable hook on RE::Character.
    /// Fires after an actor's behavior system is fully loaded and linked.
    /// Gates global wildcard transitions with an hkbExpressionCondition
    /// ("ER_Active==0") so that vanilla wildcards don't yank actors out of
    /// ER sub-behaviors.
    void Install();

    /// Clear the dedup set so wildcard gating re-applies after save load.
    /// Behavior graphs are rebuilt on load, so the injected conditions and
    /// variables are lost.
    void ClearGatedSet();

}
