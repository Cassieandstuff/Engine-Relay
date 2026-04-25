#pragma once

#include "PCH.h"

namespace EngineRelay {
namespace BehaviorFileInterceptor {

// =============================================================================
// BehaviorFileInterceptor — BSResource::LooseFileLocation::DoCreateStream hook
//
// Patches vtable slot 3 (DoCreateStream) on BSResource::LooseFileLocation.
// On the first call where the filename is "enginerelay.hkx":
//
//   1. BehaviorGenerator::GenerateBytes() is called via std::call_once to
//      produce the complete HKX bytes in memory (no disk I/O, no temp files).
//   2. A ERMemoryStream wrapping those bytes is returned directly as the
//      stream — Havok reads from it as if it were a normal file stream.
//
// If there are no registrations or generation fails, the hook falls through to
// the deployed fallback so the game always has a valid file.
// =============================================================================

/// Install the DoCreateStream vtable hook.  Call once at SKSEPluginLoad.
void Install();

}  // namespace BehaviorFileInterceptor
}  // namespace EngineRelay
