#include "PCH.h"
#include "Bridge_TDM.h"
#include <TrueDirectionalMovement/TrueDirectionalMovementAPI.h>

namespace EngineRelay {

    void Bridge_TDM::Init()
    {
        // V2 gives us yaw control. V3 additionally exposes GetActualMovementInput
        // which ER may expose to mods in a future frame field.  Request V2 for now.
        m_interface = TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V2);
        if (m_interface) {
            LOG_INFO("Bridge_TDM: TDM API v2 acquired.");
        } else {
            LOG_INFO("Bridge_TDM: TDM not detected — directional movement control unavailable.");
        }
    }

    void Bridge_TDM::AcquireYawOnly(float multiplier)
    {
        if (!m_interface) return;
        if (m_acquired) return;

        auto* api    = static_cast<TDM_API::IVTDM2*>(m_interface);
        auto  handle = SKSE::GetPluginHandle();
        const float mult = (multiplier > 0.f) ? multiplier : 0.15f;

        const auto result = api->RequestYawControl(handle, mult);
        if (result != TDM_API::APIResult::OK && result != TDM_API::APIResult::AlreadyGiven) {
            LOG_WARN("Bridge_TDM::AcquireYawOnly: RequestYawControl failed (result={}).",
                static_cast<int>(result));
            return;
        }

        m_acquired     = true;
        m_dmDisabled   = false;
        m_yawMultiplier = mult;
        LOG_INFO("Bridge_TDM: yaw-only control acquired (mult={:.2f}).", mult);
    }

    void Bridge_TDM::AcquireFullControl(float multiplier)
    {
        if (!m_interface) return;
        if (m_acquired) return;

        auto* api    = static_cast<TDM_API::IVTDM2*>(m_interface);
        auto* api1   = static_cast<TDM_API::IVTDM1*>(m_interface);
        auto  handle = SKSE::GetPluginHandle();
        const float mult = (multiplier > 0.f) ? multiplier : 0.5f;

        const auto yawResult = api->RequestYawControl(handle, mult);
        if (yawResult != TDM_API::APIResult::OK && yawResult != TDM_API::APIResult::AlreadyGiven) {
            LOG_WARN("Bridge_TDM::AcquireFullControl: RequestYawControl failed (result={}).",
                static_cast<int>(yawResult));
            return;
        }

        const auto dmResult = api1->RequestDisableDirectionalMovement(handle);
        if (dmResult == TDM_API::APIResult::OK || dmResult == TDM_API::APIResult::AlreadyGiven) {
            m_dmDisabled = true;
        } else {
            LOG_WARN("Bridge_TDM::AcquireFullControl: RequestDisableDirectionalMovement "
                     "failed (result={}) — yaw control held, DM not suppressed.",
                static_cast<int>(dmResult));
        }

        m_acquired      = true;
        m_yawMultiplier = mult;
        LOG_INFO("Bridge_TDM: full control acquired (mult={:.2f}, DM disabled={}).",
            mult, m_dmDisabled);
    }

    void Bridge_TDM::SetPlayerYaw(float yaw)
    {
        if (!m_interface || !m_acquired) return;
        static_cast<TDM_API::IVTDM2*>(m_interface)->SetPlayerYaw(SKSE::GetPluginHandle(), yaw);
    }

    void Bridge_TDM::SetYawMultiplier(float multiplier)
    {
        if (!m_interface || !m_acquired) return;
        if (std::abs(multiplier - m_yawMultiplier) < 0.001f) return;

        auto* api    = static_cast<TDM_API::IVTDM2*>(m_interface);
        auto  handle = SKSE::GetPluginHandle();

        // TDM has no SetYawSpeed — must release and reacquire.
        api->ReleaseYawControl(handle);
        const auto result = api->RequestYawControl(handle, multiplier);
        if (result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven) {
            m_yawMultiplier = multiplier;
            LOG_DEBUG("Bridge_TDM: yaw multiplier updated to {:.2f}.", multiplier);
        } else {
            LOG_WARN("Bridge_TDM::SetYawMultiplier: reacquire failed (result={}) — "
                     "attempting recovery with previous multiplier {:.2f}.",
                static_cast<int>(result), m_yawMultiplier);
            api->RequestYawControl(handle, m_yawMultiplier);
        }
    }

    void Bridge_TDM::Release()
    {
        if (!m_interface || !m_acquired) return;

        auto* api    = static_cast<TDM_API::IVTDM2*>(m_interface);
        auto* api1   = static_cast<TDM_API::IVTDM1*>(m_interface);
        auto  handle = SKSE::GetPluginHandle();

        if (m_dmDisabled) {
            api1->ReleaseDisableDirectionalMovement(handle);
            m_dmDisabled = false;
        }
        api->ReleaseYawControl(handle);

        m_acquired      = false;
        m_yawMultiplier = 0.f;
        LOG_INFO("Bridge_TDM: released all TDM resources.");
    }

}  // namespace EngineRelay
