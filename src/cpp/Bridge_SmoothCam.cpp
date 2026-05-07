#include "PCH.h"
#include "Bridge_SmoothCam.h"
#include <SmoothCam/SmoothCamAPI.h>

namespace EngineRelay {

    void Bridge_SmoothCam::RegisterListener()
    {
        auto* msg = SKSE::GetMessagingInterface();
        if (!msg) return;

        const bool ok = SmoothCamAPI::RegisterInterfaceLoaderCallback(msg,
            [](void* interfacePtr, SmoothCamAPI::InterfaceVersion version) {
                auto& self = Bridge_SmoothCam::GetSingleton();
                self.m_interface = interfacePtr;
                self.m_hasV2 = (static_cast<int>(version) >=
                                static_cast<int>(SmoothCamAPI::InterfaceVersion::V2));
                LOG_INFO("Bridge_SmoothCam: SmoothCam API v{} obtained.",
                    static_cast<int>(version) + 1);
            });

        if (ok) {
            LOG_INFO("Bridge_SmoothCam: listener registered.");
        } else {
            LOG_INFO("Bridge_SmoothCam: SmoothCam not present — camera suppression unavailable.");
        }
    }

    void Bridge_SmoothCam::RequestInterface()
    {
        auto* msg = SKSE::GetMessagingInterface();
        if (!msg) return;

        // Request V2 for SendToGoalPosition support.  Falls back gracefully if
        // only V1 is available — the callback stores whichever version responds.
        (void)SmoothCamAPI::RequestInterface(msg, SmoothCamAPI::InterfaceVersion::V2);
    }

    bool Bridge_SmoothCam::IsCameraEnabled() const noexcept
    {
        if (!m_interface || !m_hasV2) return true;  // assume enabled if we can't check
        return static_cast<SmoothCamAPI::IVSmoothCam2*>(m_interface)->IsCameraEnabled();
    }

    bool Bridge_SmoothCam::RequestCameraControl()
    {
        if (!m_interface) return true;  // SmoothCam absent — nothing to suppress
        if (m_cameraOwned) return true;

        const auto result = static_cast<SmoothCamAPI::IVSmoothCam1*>(m_interface)
            ->RequestCameraControl(SKSE::GetPluginHandle());

        if (result == SmoothCamAPI::APIResult::OK ||
            result == SmoothCamAPI::APIResult::AlreadyGiven) {
            m_cameraOwned = true;
            LOG_INFO("Bridge_SmoothCam: camera control acquired.");
            return true;
        }

        LOG_WARN("Bridge_SmoothCam::RequestCameraControl: failed (result={}) — "
                 "SmoothCam may conflict with ER camera state.",
            static_cast<int>(result));
        return false;
    }

    void Bridge_SmoothCam::ReleaseCameraControl(bool sendToGoal)
    {
        if (!m_interface || !m_cameraOwned) return;

        auto* api    = static_cast<SmoothCamAPI::IVSmoothCam1*>(m_interface);
        auto  handle = SKSE::GetPluginHandle();

        // Ask SmoothCam to glide back to its computed goal rather than snapping.
        // Only available on V2+; skip silently on V1.
        if (sendToGoal && m_hasV2) {
            static_cast<SmoothCamAPI::IVSmoothCam2*>(m_interface)
                ->SendToGoalPosition(handle, true);
        }

        api->ReleaseCameraControl(handle);
        m_cameraOwned = false;
        LOG_INFO("Bridge_SmoothCam: camera control released (sendToGoal={}).", sendToGoal);
    }

}  // namespace EngineRelay
