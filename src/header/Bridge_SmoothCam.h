#pragma once
#include "PCH.h"

namespace EngineRelay {

    // ── Bridge_SmoothCam ──────────────────────────────────────────────────────
    //
    // Engine Relay's interface to SmoothCam.
    // Optional dependency — all methods are silent no-ops if SmoothCam is not
    // loaded or if the API response has not yet been received.
    //
    // SmoothCam's API uses an async request/callback pattern:
    //   kPostLoad      → RegisterListener()   (register the response callback)
    //   kPostPostLoad  → RequestInterface()   (send the version request)
    //   callback fires → interface pointer stored, IsAvailable() becomes true
    //
    // ER suppresses SmoothCam exactly when an ERCameraState is active:
    //
    //   Camera activate → RequestCameraControl()
    //   Camera deactivate (no successor camera) → ReleaseCameraControl(sendToGoal)
    //   Camera deactivate (successor camera arriving) → hold, new Activate calls
    //                                                    Request again immediately
    //
    // The V2 interface is requested.  If only V1 is available (older SmoothCam)
    // RequestCameraControl / ReleaseCameraControl still work; SendToGoal is
    // skipped gracefully.

    class Bridge_SmoothCam {
    public:
        static Bridge_SmoothCam& GetSingleton() noexcept {
            static Bridge_SmoothCam instance;
            return instance;
        }

        // Register the SKSE listener that will receive the SmoothCam interface.
        // Call once during kPostLoad.
        void RegisterListener();

        // Send the interface version request to SmoothCam.
        // Call once during kPostPostLoad (after SmoothCam has loaded).
        void RequestInterface();

        // True if the SmoothCam API interface was successfully obtained.
        [[nodiscard]] bool IsAvailable() const noexcept { return m_interface != nullptr; }

        // True if ER currently holds SmoothCam camera ownership.
        [[nodiscard]] bool IsCameraOwned() const noexcept { return m_cameraOwned; }

        // True if SmoothCam is enabled by the user (MCM / hotkey).
        // Returns true if SmoothCam is absent (safe default — nothing to suppress).
        [[nodiscard]] bool IsCameraEnabled() const noexcept;

        // Request SmoothCam camera ownership for the duration of an ER camera state.
        // No-op if already owned or SmoothCam is absent.
        // @returns true if ownership was acquired (or was already held).
        bool RequestCameraControl();

        // Release SmoothCam camera ownership when the ER camera state ends.
        // @param sendToGoal  If true, instructs SmoothCam to smoothly return to its
        //                    goal position after regaining control.  Pass false when
        //                    transitioning directly to another ER camera state.
        void ReleaseCameraControl(bool sendToGoal = true);

    private:
        Bridge_SmoothCam() = default;
        Bridge_SmoothCam(const Bridge_SmoothCam&) = delete;
        Bridge_SmoothCam& operator=(const Bridge_SmoothCam&) = delete;

        void* m_interface   { nullptr };
        bool  m_cameraOwned { false };
        // V2 allows SendToGoalPosition; V1 does not.
        bool  m_hasV2       { false };
    };

}  // namespace EngineRelay
