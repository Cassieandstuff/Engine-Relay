#pragma once

#include "PCH.h"
#include <BehaviorSwitchboard/BSB_API.h>
#include <vector>

namespace BehaviorSwitchboard::BehaviorGenerator {

    // =========================================================================
    // BehaviorGenerator — BehaviorSwitchboard.hkx builder
    //
    // Uses the native C++ HKX binary writer (HkxBsbWriter) to produce a valid
    // Skyrim SE/AE Havok packfile directly in memory.
    // No subprocess, no HKBuild.exe, no .NET runtime required.
    //
    // GenerateBytes() is called lazily inside
    // BehaviorFileInterceptor::Hook_DoCreateStream on the first interception of
    // behaviorswitchboard.hkx.  With 0 registrations, returns an empty vector
    // and the deployed fallback is used as-is.
    // =========================================================================

    /// Serialize a switchboard HKX for the supplied registrations.
    ///
    /// @param registrations  All registrations targeting this switchboard graph.
    ///                       If empty, returns {} (no bytes generated).
    /// @param targetGraphName  The graph name to embed (e.g. "BehaviorSwitchboard.hkb"
    ///                         or "DragonBehaviorSwitchboard.hkb"). Must match what
    ///                         BSB_Activate's IsSwitchboardGraph() check accepts.
    /// @return               Complete HKX bytes on success, empty vector if no
    ///                       registrations are present.
    std::vector<std::uint8_t> GenerateBytes(
        const std::vector<Registration>& registrations,
        std::string_view                 targetGraphName = "BehaviorSwitchboard.hkb");

}  // namespace BehaviorSwitchboard::BehaviorGenerator
