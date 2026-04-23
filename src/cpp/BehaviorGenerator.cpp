#include "PCH.h"
#include "BehaviorGenerator.h"
#include "HkxBsbWriter.h"

namespace BehaviorSwitchboard::BehaviorGenerator {

std::vector<std::uint8_t> GenerateBytes(const std::vector<Registration>& registrations,
                                         std::string_view                 targetGraphName)
{
    if (registrations.empty()) {
        LOG_INFO("BehaviorGenerator: no registrations for '{}' — "
                 "deployed HKX will be used as-is.", targetGraphName);
        return {};
    }

    LOG_INFO("BehaviorGenerator: generating '{}' for {} registration(s).",
             targetGraphName, registrations.size());
    for (const auto& r : registrations)
        LOG_INFO("BehaviorGenerator:   '{}' → '{}' (event='{}')",
            r.modName, r.behaviorPath, r.eventName);

    auto bytes = HkxBsbWriter::WriteToMemory(registrations, targetGraphName);

    LOG_INFO("BehaviorGenerator: generated {} bytes in memory.", bytes.size());
    return bytes;
}

}  // namespace BehaviorSwitchboard::BehaviorGenerator
