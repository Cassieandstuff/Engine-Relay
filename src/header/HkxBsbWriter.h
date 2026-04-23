#pragma once

#include "PCH.h"
#include <BehaviorSwitchboard/BSB_API.h>
#include <filesystem>
#include <vector>

namespace BehaviorSwitchboard::HkxBsbWriter {

    // =========================================================================
    // HkxBsbWriter — Native C++ Havok packfile binary writer
    //
    // Generates a valid Skyrim SE BehaviorSwitchboard.hkx directly in memory
    // without any subprocess or external tool.  Supports 0..N registrations.
    //
    // Output format:
    //   • 64-bit little-endian Havok packfile (FileVersion=8, PointerSize=8)
    //   • Three sections: __classnames__, __types__ (empty), __data__
    //   • Object graph matches the HKX2E serializer's output exactly
    //
    // For 0 registrations the output is functionally identical to the deployed
    // fallback behaviorswitchboard.hkx.  For N>0 registrations the state
    // machine gains N wildcard transitions and N BehaviorReferenceGenerators.
    // =========================================================================

    /// Serialize a complete switchboard HKX into memory.
    ///
    /// @param registrations  Registrations targeting this switchboard graph
    ///                       (may be empty — produces the baseline idle graph).
    /// @param graphName      The hkbBehaviorGraph.name to embed in the file.
    ///                       Must match what BSB_Activate checks (e.g.
    ///                       "BehaviorSwitchboard.hkb" or
    ///                       "DragonBehaviorSwitchboard.hkb").
    /// @return               The complete HKX byte stream.
    std::vector<std::uint8_t> WriteToMemory(
        const std::vector<Registration>& registrations,
        std::string_view                 graphName = "BehaviorSwitchboard.hkb");

    /// Write a switchboard HKX to outPath (debug/testing helper).
    /// Calls WriteToMemory() and flushes to disk.
    /// @param outPath  Destination file path (parent dir must exist).
    /// @return true on success.
    bool Write(const std::vector<Registration>& registrations,
               const std::filesystem::path&     outPath,
               std::string_view                 graphName = "BehaviorSwitchboard.hkb");

}  // namespace BehaviorSwitchboard::HkxBsbWriter
