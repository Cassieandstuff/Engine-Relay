#pragma once
#include "PCH.h"

namespace EngineRelay {

    // ── Bridge_TDM ────────────────────────────────────────────────────────────
    //
    // Engine Relay's interface to True Directional Movement.
    // Optional dependency — all methods are silent no-ops if TDM is not loaded.
    // ER holds the SKSE plugin handle for TDM API calls; individual consumer
    // mods never interact with TDM directly.
    //
    // Two ownership modes, chosen at acquire time:
    //
    //   Yaw-only  (physics state, no custom camera):
    //     TDM directional movement stays active (camera can still orbit).
    //     ER drives character facing by calling SetPlayerYaw each tick.
    //
    //   Full      (physics state + custom camera):
    //     TDM directional movement is disabled so the character faces exactly
    //     where ER's camera points, not where TDM would steer it.
    //     ER still drives yaw via SetPlayerYaw for smooth rotation.
    //
    // Lifecycle:
    //   Init()              — call once during kPostLoad
    //   AcquireYawOnly()    — physics-only registration activated
    //   AcquireFullControl()— physics+camera registration activated
    //   SetPlayerYaw()      — called each physics tick from the proxy update loop
    //   SetYawMultiplier()  — called when frame.yawMultiplier changes
    //   Release()           — registration deactivated

    class Bridge_TDM {
    public:
        static Bridge_TDM& GetSingleton() noexcept {
            static Bridge_TDM instance;
            return instance;
        }

        // Load the TDM API via GetProcAddress. Call once during kPostLoad.
        // Logs whether TDM was found. Safe to call if TDM is absent.
        void Init();

        // True if TDM was found and the API interface was obtained.
        [[nodiscard]] bool IsAvailable() const noexcept { return m_interface != nullptr; }

        // Acquire yaw control only — directional movement stays active.
        // Used when the registration has physics but no custom camera.
        // @param multiplier  TDM rotation speed (PI * mult rad/s; 0 = instant).
        //                    Typical: 0.15 for hover, 0.5 for direct flight.
        void AcquireYawOnly(float multiplier = 0.15f);

        // Acquire yaw control AND disable directional movement.
        // Used when the registration has both physics and a custom camera.
        // @param multiplier  TDM rotation speed (same scale as AcquireYawOnly).
        void AcquireFullControl(float multiplier = 0.5f);

        // Push the desired player facing yaw to TDM.  Call each physics tick.
        // No-op if yaw control has not been acquired or TDM is absent.
        void SetPlayerYaw(float yaw);

        // Change the yaw rotation speed multiplier mid-flight.
        // TDM has no SetYawSpeed, so this re-acquires yaw control internally.
        // No-op if the multiplier hasn't changed (within 0.001 tolerance).
        void SetYawMultiplier(float multiplier);

        // Release all acquired TDM resources (yaw control + DM disable if held).
        void Release();

        [[nodiscard]] bool  IsAcquired()                const noexcept { return m_acquired; }
        [[nodiscard]] bool  IsDirectionalMovementDisabled() const noexcept { return m_dmDisabled; }
        [[nodiscard]] float GetYawMultiplier()          const noexcept { return m_yawMultiplier; }

    private:
        Bridge_TDM() = default;
        Bridge_TDM(const Bridge_TDM&) = delete;
        Bridge_TDM& operator=(const Bridge_TDM&) = delete;

        void* m_interface   { nullptr };
        bool  m_acquired    { false };
        bool  m_dmDisabled  { false };
        float m_yawMultiplier{ 0.f };
    };

}  // namespace EngineRelay
