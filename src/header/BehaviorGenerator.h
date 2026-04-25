#pragma once

#include "PCH.h"
#include <EngineRelay/ER_API.h>
#include <vector>

namespace EngineRelay::BehaviorGenerator {

    // =========================================================================
    // BehaviorGenerator — EngineRelay.hkx builder
    //
    // Uses the native C++ HKX binary writer (HkxErWriter) to produce a valid
    // Skyrim SE/AE Havok packfile directly in memory.
    // No subprocess, no HKBuild.exe, no .NET runtime required.
    //
    // GenerateBytes() is called lazily inside
    // BehaviorFileInterceptor::Hook_DoCreateStream on the first interception of
    // enginerelay.hkx.  With 0 registrations, returns an empty vector
    // and the deployed fallback is used as-is.
    // =========================================================================

    /// Serialize a switchboard HKX for the supplied registrations.
    ///
    /// @param registrations  All registrations targeting this switchboard graph.
    ///                       If empty, returns {} (no bytes generated).
    /// @param targetGraphName  The graph name to embed (e.g. "EngineRelay.hkb"
    ///                         or "DragonEngineRelay.hkb"). Must match what
    ///                         ER_Activate's IsSwitchboardGraph() check accepts.
    /// @return               Complete HKX bytes on success, empty vector if no
    ///                       registrations are present.
    std::vector<std::uint8_t> GenerateBytes(
        const std::vector<Registration>& registrations,
        std::string_view                 targetGraphName = "EngineRelay.hkb");

}  // namespace EngineRelay::BehaviorGenerator
