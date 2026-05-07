#pragma once
#include <functional>
#include <stdint.h>

// Engine Relay uses CommonLibSSE — always select the CommonLib backend.
#if !defined(SMOOTHCAM_API_COMMONLIB)
#   define SMOOTHCAM_API_COMMONLIB
#endif

#define SMOOTHCAM_API_LOGGER SKSE::log::info

/*
* For modders: Copy this file into your own project if you wish to use this API
*
* Original header requires SMOOTHCAM_API_SKSE or SMOOTHCAM_API_COMMONLIB to be
* defined.  Engine Relay unconditionally uses the CommonLib path; the define is
* set above so consumers of this copy do not need to set it themselves.
*/
namespace SmoothCamAPI {
	constexpr const auto SmoothCamPluginName = "SmoothCam";

	using PluginHandle = SKSE::PluginHandle;
	using Actor = RE::Actor;
	using TESObjectREFR = RE::TESObjectREFR;
	using NiCamera = RE::NiCamera;
	using NiPoint3 = RE::NiPoint3;

	// Available SmoothCam interface versions
	enum class InterfaceVersion : uint8_t {
		V1,
		V2,
		V3
	};

	// Error types that may be returned by the SmoothCam API
	enum class APIResult : uint8_t {
		// Your API call was successful
		OK,

		// You tried to release a resource that was not allocated to you
		// Do not attempt to manipulate the requested resource if you receive this response
		NotOwner,

		// SmoothCam currently must keep control of this resource for proper functionality
		// Do not attempt to manipulate the requested resource if you receive this response
		MustKeep,

		// You have already been given control of this resource
		AlreadyGiven,

		// Another mod has been given control of this resource at the present time
		// Do not attempt to manipulate the requested resource if you receive this response
		AlreadyTaken,

		// You sent a command on a thread that could cause a data race were it to be processed
		// Do not attempt to manipulate the requested resource if you receive this response
		BadThread,
	};

	// SmoothCam's modder interface
	class IVSmoothCam1 {
		public:
		[[nodiscard]] virtual unsigned long GetSmoothCamThreadId() const noexcept = 0;
		[[nodiscard]] virtual APIResult RequestCameraControl(PluginHandle myPluginHandle) noexcept = 0;
		[[nodiscard]] virtual APIResult RequestCrosshairControl(PluginHandle myPluginHandle,
			bool restoreDefaults = true) noexcept = 0;
		[[nodiscard]] virtual APIResult RequestStealthMeterControl(PluginHandle myPluginHandle,
			bool restoreDefaults = true) noexcept = 0;
		virtual PluginHandle GetCameraOwner() const noexcept = 0;
		virtual PluginHandle GetCrosshairOwner() const noexcept = 0;
		virtual PluginHandle GetStealthMeterOwner() const noexcept = 0;
		virtual APIResult ReleaseCameraControl(PluginHandle myPluginHandle) noexcept = 0;
		virtual APIResult ReleaseCrosshairControl(PluginHandle myPluginHandle) noexcept = 0;
		virtual APIResult ReleaseStealthMeterControl(PluginHandle myPluginHandle) noexcept = 0;
	};

	class IVSmoothCam2 : public IVSmoothCam1 {
		public:
		virtual NiPoint3 GetLastCameraPosition() const noexcept = 0;
		virtual APIResult RequestInterpolatorUpdates(PluginHandle myPluginHandle, bool allowUpdates) noexcept = 0;
		virtual APIResult SendToGoalPosition(PluginHandle myPluginHandle, bool shouldMoveToGoal,
			bool moveNow = false, const Actor* ref = nullptr) noexcept = 0;
		virtual void GetGoalPosition(TESObjectREFR* ref, NiPoint3& world, NiPoint3& local) const noexcept = 0;
		virtual bool IsCameraEnabled() const noexcept = 0;
	};

	class IVSmoothCam3 : public IVSmoothCam2 {
		public:
		virtual void EnableUnlockedHorseAim(bool enable) noexcept = 0;
	};

	struct PluginCommand {
		enum class Type : uint8_t { RequestInterface };
		uint32_t header = 0x9007CA50;
		Type type;
		void* commandStructure = nullptr;
	};

	struct InterfaceRequest {
		InterfaceVersion interfaceVersion;
	};

	struct PluginResponse {
		enum class Type : uint8_t { Error, InterfaceProvider };
		Type type;
		void* responseData = nullptr;
	};

	struct InterfaceContainer {
		void* interfaceInstance = nullptr;
		InterfaceVersion interfaceVersion;
	};

	using InterfaceLoaderCallback = std::function<void(
		void* interfaceInstance, InterfaceVersion interfaceVersion
	)>;

	/// <summary>
	/// Initiate a request for the SmoothCam API interface via SKSE's messaging system.
	/// Recommended: Send your request during kPostPostLoad.
	/// </summary>
	[[nodiscard]]
	inline bool RequestInterface(const SKSE::MessagingInterface* skseMessaging,
		InterfaceVersion version = InterfaceVersion::V2) noexcept
	{
		InterfaceRequest req = {};
		req.interfaceVersion = version;

		PluginCommand cmd = {};
		cmd.type = PluginCommand::Type::RequestInterface;
		cmd.commandStructure = &req;

		return skseMessaging->Dispatch(
			0,
			&cmd, sizeof(PluginCommand),
			SmoothCamPluginName
		);
	}

	/// <summary>
	/// Register the callback for obtaining the SmoothCam API interface.
	/// Call once during kPostLoad.
	/// </summary>
	[[nodiscard]]
	inline bool RegisterInterfaceLoaderCallback(const SKSE::MessagingInterface* skseMessaging,
		InterfaceLoaderCallback&& callback) noexcept
	{
		static InterfaceLoaderCallback storedCallback = callback;

		return skseMessaging->RegisterListener(
			SmoothCamPluginName,
			[](SKSE::MessagingInterface::Message* msg) {
				if (msg->sender && strcmp(msg->sender, SmoothCamPluginName) != 0) return;
				if (msg->type != 0) return;
				if (msg->dataLen != sizeof(PluginResponse)) return;

				const auto resp = reinterpret_cast<PluginResponse*>(msg->data);
				switch (resp->type) {
					case PluginResponse::Type::InterfaceProvider: {
						auto interfaceContainer = reinterpret_cast<InterfaceContainer*>(resp->responseData);
						storedCallback(
							interfaceContainer->interfaceInstance,
							interfaceContainer->interfaceVersion
						);
						break;
					}
					case PluginResponse::Type::Error:
						SMOOTHCAM_API_LOGGER("SmoothCam API: Error obtaining interface");
						break;
					default: return;
				}
			}
		);
	}
}
