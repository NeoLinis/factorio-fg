// Streamline / DLSS Frame Generation glue (step 2b). See fg_streamline.h.
//
// Frame timeline (all on Factorio's presenting thread):
//   present_end(N-1) -> new token N, PCL SimulationStart(N)      ~ Factorio starts simulating frame N
//   frame_start()    -> SimulationEnd(N), RenderSubmitStart(N)   ~ Factorio's first draw call of frame N
//   before_present() -> RenderSubmitEnd(N), constants, tags, DLSS-G options for frame N
//   present_start/end around the bridge's Present             -> PresentStart(N) / PresentEnd(N)
//
// Inputs to DLSS-G: the final frame (bridge swap chain back buffer, implicit), the HUD-less copy from the probe,
// a flat depth buffer and zero motion vectors. Camera motion vectors come in step 3; until then DLSS-G relies on
// its own optical flow, so expect smearing on fast pans.

#include "fg_streamline.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#if defined(__MINGW32__)
// MinGW's wintrust.h predates WINTRUST_SIGNATURE_SETTINGS, which sl_security.h needs. MinGW builds are only used
// as a compile check here; release builds are MSVC and always verify NVIDIA's signature.
namespace sl { namespace security { inline bool verifyEmbeddedSignature(const wchar_t *) { return true; } } }
#else
#include <sl_security.h>
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace
{
	fg::log_fn g_log = nullptr;
	bool g_load_attempted = false;
	HMODULE g_interposer = nullptr;
	std::wstring g_sl_dir;

	bool g_initialized = false; // slInit succeeded and slShutdown not called yet
	bool g_attached = false;    // device set and DLSS-G usable

	PFun_slInit *p_slInit = nullptr;
	PFun_slShutdown *p_slShutdown = nullptr;
	PFun_slSetD3DDevice *p_slSetD3DDevice = nullptr;
	PFun_slUpgradeInterface *p_slUpgradeInterface = nullptr;
	PFun_slIsFeatureSupported *p_slIsFeatureSupported = nullptr;
	PFun_slIsFeatureLoaded *p_slIsFeatureLoaded = nullptr;
	PFun_slGetFeatureFunction *p_slGetFeatureFunction = nullptr;
	PFun_slGetNewFrameToken *p_slGetNewFrameToken = nullptr;
	PFun_slSetConstants *p_slSetConstants = nullptr;
	PFun_slSetTagForFrame *p_slSetTagForFrame = nullptr;

	PFun_slDLSSGSetOptions *p_slDLSSGSetOptions = nullptr;
	PFun_slDLSSGGetState *p_slDLSSGGetState = nullptr;
	PFun_slReflexSetOptions *p_slReflexSetOptions = nullptr;
	PFun_slPCLSetMarker *p_slPCLSetMarker = nullptr;

	ID3D12Resource *g_depth = nullptr; // R32_FLOAT, zero (committed resources are zero-initialised)
	char g_depth_status[160] = "not tagged yet";
	uint64_t g_depth_frames = 0;       // frames the layered depth was tagged successfully
	ID3D12Resource *g_mvec = nullptr;  // R16G16_FLOAT, zero motion

	sl::FrameToken *g_token = nullptr;
	uint32_t g_frame_index = 0;
	bool g_render_started = false;
	bool g_reset = true;

	int g_applied_multiplier = -1;
	uint32_t g_frames_max = 0;      // DLSSGState::numFramesToGenerateMax
	uint32_t g_frames_presented = 0;
	uint32_t g_dlssg_status = 0;
	uint32_t g_state_poll = 0;

	char g_status[256] = "not loaded";

	void log_line(const char *fmt, ...)
	{
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (g_log != nullptr)
			g_log(buf);
	}

	void set_status(const char *fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(g_status, sizeof(g_status), fmt, args);
		va_end(args);
	}

	void sl_log_callback(sl::LogType type, const char *msg)
	{
		// Info is very chatty; forward warnings and errors only. Streamline also writes its own log file in fg\.
		if (type == sl::LogType::eInfo || msg == nullptr)
			return;
		std::string line(msg);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
			line.pop_back();
		log_line("streamline: %s", line.c_str());
	}

	std::wstring module_dir()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&module_dir), &module);
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		std::wstring dir(path);
		const size_t slash = dir.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
	}

	bool file_exists(const std::wstring &path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	std::string narrow(const std::wstring &w)
	{
		std::string out;
		for (const wchar_t c : w)
			out.push_back(c < 128 ? static_cast<char>(c) : '?');
		return out;
	}

	// dlssg_for_sm86 ships a version.dll proxy that must be in the process before Streamline checks the GPU.
	void load_sm86_proxy(const std::wstring &game_dir)
	{
		const std::wstring proxy = game_dir + L"version.dll";
		if (!file_exists(proxy))
		{
			log_line("fg: no version.dll next to factorio.exe; on RTX 20/30 DLSS-G needs the dlssg_for_sm86 package");
			return;
		}
		if (GetModuleHandleW(proxy.c_str()) != nullptr)
		{
			log_line("fg: dlssg_for_sm86 proxy already loaded by Factorio");
			return;
		}
		if (LoadLibraryW(proxy.c_str()) != nullptr)
			log_line("fg: loaded dlssg_for_sm86 proxy from %s", narrow(proxy).c_str());
		else
			log_line("fg: could not load %s (error %lu)", narrow(proxy).c_str(), GetLastError());
	}

	template <class T>
	bool get_proc(T *&out, const char *name)
	{
		out = reinterpret_cast<T *>(reinterpret_cast<void *>(GetProcAddress(g_interposer, name)));
		if (out == nullptr)
			log_line("fg: sl.interposer.dll has no export %s", name);
		return out != nullptr;
	}

	template <class T>
	bool get_feature_proc(T *&out, sl::Feature feature, const char *name)
	{
		void *fn = nullptr;
		if (p_slGetFeatureFunction(feature, name, fn) != sl::Result::eOk || fn == nullptr)
		{
			log_line("fg: Streamline feature function %s not available", name);
			out = nullptr;
			return false;
		}
		out = reinterpret_cast<T *>(fn);
		return true;
	}

	void marker(sl::PCLMarker m)
	{
		if (p_slPCLSetMarker != nullptr && g_token != nullptr)
			p_slPCLSetMarker(m, *g_token);
	}

	void begin_frame_token()
	{
		if (g_token != nullptr || p_slGetNewFrameToken == nullptr)
			return;
		uint32_t index = g_frame_index;
		if (p_slGetNewFrameToken(g_token, &index) != sl::Result::eOk)
		{
			g_token = nullptr;
			return;
		}
		marker(sl::PCLMarker::eSimulationStart);
	}

	ID3D12Resource *create_zero_texture(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format)
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		ID3D12Resource *res = nullptr;
		// Committed resources are zero-initialised unless D3D12_HEAP_FLAG_CREATE_NOT_ZEROED is passed.
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
				IID_PPV_ARGS(&res))))
			return nullptr;
		return res;
	}

	void release_static_inputs()
	{
		if (g_depth != nullptr) { g_depth->Release(); g_depth = nullptr; }
		if (g_mvec != nullptr) { g_mvec->Release(); g_mvec = nullptr; }
	}

	const char *status_text(uint32_t status)
	{
		if (status == 0) return "ok";
		if (status & static_cast<uint32_t>(sl::DLSSGStatus::eFailResolutionTooLow)) return "resolution too low";
		if (status & static_cast<uint32_t>(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)) return "Reflex not detected";
		if (status & static_cast<uint32_t>(sl::DLSSGStatus::eFailHDRFormatNotSupported)) return "HDR format not supported";
		if (status & static_cast<uint32_t>(sl::DLSSGStatus::eFailCommonConstantsInvalid)) return "constants invalid";
		if (status & static_cast<uint32_t>(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)) return "GetCurrentBackBufferIndex not called";
		return "error";
	}
}

namespace fg
{
	bool load(log_fn log)
	{
		g_log = log;
		if (g_load_attempted)
			return g_interposer != nullptr;
		g_load_attempted = true;

		const std::wstring game_dir = module_dir();
		load_sm86_proxy(game_dir);

		g_sl_dir = game_dir + L"fg\\";
		const std::wstring interposer = g_sl_dir + L"sl.interposer.dll";
		if (!file_exists(interposer))
		{
			log_line("fg: Streamline not found (%s); frame generation unavailable", narrow(interposer).c_str());
			set_status("Streamline not found in bin\\x64\\fg\\");
			return false;
		}
		if (!sl::security::verifyEmbeddedSignature(interposer.c_str()))
		{
			log_line("fg: sl.interposer.dll is not signed by NVIDIA; refusing to load it");
			set_status("sl.interposer.dll signature check failed");
			return false;
		}
		g_interposer = LoadLibraryW(interposer.c_str());
		if (g_interposer == nullptr)
		{
			log_line("fg: LoadLibrary(sl.interposer.dll) failed (error %lu)", GetLastError());
			set_status("could not load sl.interposer.dll");
			return false;
		}

		const bool ok = get_proc(p_slInit, "slInit") && get_proc(p_slShutdown, "slShutdown") &&
			get_proc(p_slSetD3DDevice, "slSetD3DDevice") && get_proc(p_slUpgradeInterface, "slUpgradeInterface") &&
			get_proc(p_slIsFeatureSupported, "slIsFeatureSupported") && get_proc(p_slIsFeatureLoaded, "slIsFeatureLoaded") &&
			get_proc(p_slGetFeatureFunction, "slGetFeatureFunction") && get_proc(p_slGetNewFrameToken, "slGetNewFrameToken") &&
			get_proc(p_slSetConstants, "slSetConstants") && get_proc(p_slSetTagForFrame, "slSetTagForFrame");
		if (!ok)
		{
			FreeLibrary(g_interposer);
			g_interposer = nullptr;
			set_status("sl.interposer.dll too old or incomplete");
			return false;
		}
		log_line("fg: Streamline loaded from %s", narrow(g_sl_dir).c_str());
		set_status("loaded");
		return true;
	}

	bool begin()
	{
		if (g_interposer == nullptr)
			return false;
		if (g_initialized)
			return true;

		static const wchar_t *plugin_paths[1];
		plugin_paths[0] = g_sl_dir.c_str();
		static const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };

		sl::Preferences pref{};
		pref.showConsole = false;
		pref.logLevel = sl::LogLevel::eDefault;
		pref.pathsToPlugins = plugin_paths;
		pref.numPathsToPlugins = 1;
		pref.pathToLogsAndData = g_sl_dir.c_str();
		pref.logMessageCallback = sl_log_callback;
		pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
			sl::PreferenceFlags::eUseFrameBasedResourceTagging;
		pref.featuresToLoad = features;
		pref.numFeaturesToLoad = static_cast<uint32_t>(sizeof(features) / sizeof(features[0]));
		pref.applicationId = 231313132; // Streamline sample application id
		pref.engine = sl::EngineType::eCustom;
		pref.engineVersion = "factorio-fg";
		pref.projectId = "5c9f3c38-6d0f-4a8e-9f55-2f6c1a7b4e21";
		pref.renderAPI = sl::RenderAPI::eD3D12;

		const sl::Result result = p_slInit(pref, sl::kSDKVersion);
		if (result != sl::Result::eOk)
		{
			log_line("fg: slInit failed (result %d)%s", static_cast<int>(result),
				result == sl::Result::eErrorOSDisabledHWS ? " - turn on Hardware-accelerated GPU scheduling in Windows graphics settings" : "");
			set_status("slInit failed (%d)", static_cast<int>(result));
			return false;
		}
		g_initialized = true;
		set_status("initialised");
		return true;
	}

	bool attach(ID3D12Device *native_device, const LUID &adapter, uint32_t width, uint32_t height)
	{
		if (!g_initialized)
			return false;

		sl::Result result = p_slSetD3DDevice(native_device);
		if (result != sl::Result::eOk)
		{
			log_line("fg: slSetD3DDevice failed (result %d)", static_cast<int>(result));
			set_status("slSetD3DDevice failed (%d)", static_cast<int>(result));
			return false;
		}

		LUID luid = adapter;
		sl::AdapterInfo adapter_info{};
		adapter_info.deviceLUID = reinterpret_cast<uint8_t *>(&luid);
		adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);
		result = p_slIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info);
		if (result != sl::Result::eOk)
		{
			log_line("fg: DLSS-G not supported on this system (result %d). On RTX 20/30 check that dlssg_for_sm86 is installed; "
				"Hardware-accelerated GPU scheduling must be on.", static_cast<int>(result));
			set_status("DLSS-G not supported (%d)", static_cast<int>(result));
			return false;
		}

		if (!get_feature_proc(p_slDLSSGSetOptions, sl::kFeatureDLSS_G, "slDLSSGSetOptions") ||
			!get_feature_proc(p_slDLSSGGetState, sl::kFeatureDLSS_G, "slDLSSGGetState") ||
			!get_feature_proc(p_slReflexSetOptions, sl::kFeatureReflex, "slReflexSetOptions") ||
			!get_feature_proc(p_slPCLSetMarker, sl::kFeaturePCL, "slPCLSetMarker"))
		{
			set_status("Streamline feature functions missing");
			return false;
		}

		// DLSS-G requires Reflex to be on.
		sl::ReflexOptions reflex{};
		reflex.mode = sl::ReflexMode::eLowLatency;
		if (p_slReflexSetOptions(reflex) != sl::Result::eOk)
			log_line("fg: slReflexSetOptions failed");

		release_static_inputs();
		g_depth = create_zero_texture(native_device, width, height, DXGI_FORMAT_R32_FLOAT);
		g_mvec = create_zero_texture(native_device, width, height, DXGI_FORMAT_R16G16_FLOAT);
		if (g_depth == nullptr || g_mvec == nullptr)
		{
			log_line("fg: could not create depth / motion vector inputs");
			release_static_inputs();
			set_status("could not create inputs");
			return false;
		}

		g_attached = true;
		g_applied_multiplier = -1;
		g_reset = true;
		g_token = nullptr;
		g_render_started = false;
		set_status("attached");
		log_line("fg: DLSS-G ready (Streamline %d.%d.%d)", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
		return true;
	}

	bool active()
	{
		return g_attached;
	}

	bool upgrade(IDXGIFactory4 *&factory)
	{
		if (!g_initialized || factory == nullptr)
			return false;
		void *iface = factory;
		if (p_slUpgradeInterface(&iface) != sl::Result::eOk || iface == nullptr)
			return false;
		factory = static_cast<IDXGIFactory4 *>(iface);
		return true;
	}

	bool upgrade(ID3D12Device *&device)
	{
		if (!g_initialized || device == nullptr)
			return false;
		void *iface = device;
		if (p_slUpgradeInterface(&iface) != sl::Result::eOk || iface == nullptr)
			return false;
		device = static_cast<ID3D12Device *>(iface);
		return true;
	}

	void frame_start()
	{
		if (!g_attached || g_render_started)
			return;
		begin_frame_token();
		marker(sl::PCLMarker::eSimulationEnd);
		marker(sl::PCLMarker::eRenderSubmitStart);
		g_render_started = true;
	}

	void before_present(ID3D12GraphicsCommandList *list, ID3D12Resource *hudless, ID3D12Resource *motion_vectors, ID3D12Resource *depth_layers,
		uint32_t width, uint32_t height, int multiplier)
	{
		if (!g_attached)
			return;
		if (!g_render_started)
			frame_start(); // first frame, or a frame without draw calls
		if (g_token == nullptr)
			return;
		marker(sl::PCLMarker::eRenderSubmitEnd);

		// Camera constants. Factorio is 2D and orthographic; with zero motion vectors and a flat depth buffer,
		// identity matrices are the honest description. Real camera motion arrives with step 3.
		sl::float4x4 identity;
		identity.setRow(0, sl::float4(1, 0, 0, 0));
		identity.setRow(1, sl::float4(0, 1, 0, 0));
		identity.setRow(2, sl::float4(0, 0, 1, 0));
		identity.setRow(3, sl::float4(0, 0, 0, 1));
		sl::Constants constants{};
		constants.cameraViewToClip = identity;
		constants.clipToCameraView = identity;
		constants.clipToLensClip = identity;
		constants.clipToPrevClip = identity;
		constants.prevClipToClip = identity;
		constants.jitterOffset = sl::float2(0, 0);
		// Motion vectors are in pixels; this scale brings them to the [-1, 1] range Streamline expects.
		constants.mvecScale = sl::float2(width != 0 ? 1.0f / static_cast<float>(width) : 1.0f, height != 0 ? 1.0f / static_cast<float>(height) : 1.0f);
		constants.cameraPinholeOffset = sl::float2(0, 0);
		constants.cameraPos = sl::float3(0, 0, 0);
		constants.cameraUp = sl::float3(0, 1, 0);
		constants.cameraRight = sl::float3(1, 0, 0);
		constants.cameraFwd = sl::float3(0, 0, 1);
		constants.cameraNear = 0.1f;
		constants.cameraFar = 1000.0f;
		constants.cameraFOV = 1.0f;
		constants.cameraAspectRatio = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
		constants.depthInverted = sl::Boolean::eFalse;
		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.reset = g_reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		constants.orthographicProjection = sl::Boolean::eTrue;
		constants.motionVectorsDilated = sl::Boolean::eFalse;
		constants.motionVectorsJittered = sl::Boolean::eFalse;
		const sl::ViewportHandle viewport(0);
		if (p_slSetConstants(constants, *g_token, viewport) != sl::Result::eOk)
			log_line("fg: slSetConstants failed");
		g_reset = false;

		// Inputs. The HUD-less texture is shared with D3D11 and overwritten next frame, so Streamline copies it now,
		// on this command list ("only valid now"). Depth and motion vectors never change.
		sl::Extent extent{};
		extent.width = width;
		extent.height = height;
		// Depth: the motion pass's two layers (rewritten every frame: copied now), or the flat zero texture.
		sl::Resource depth = depth_layers != nullptr
			? sl::Resource(sl::ResourceType::eTex2d, depth_layers, static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
			: sl::Resource(sl::ResourceType::eTex2d, g_depth, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
		// Real motion vectors are rewritten every frame, so Streamline copies them now; the zero texture never changes.
		sl::Resource mvec = motion_vectors != nullptr
			? sl::Resource(sl::ResourceType::eTex2d, motion_vectors, static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
			: sl::Resource(sl::ResourceType::eTex2d, g_mvec, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
		sl::Resource hud(sl::ResourceType::eTex2d, hudless, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
		const sl::ResourceTag tags[] = {
			sl::ResourceTag(&depth, sl::kBufferTypeDepth,
				depth_layers != nullptr ? sl::ResourceLifecycle::eOnlyValidNow : sl::ResourceLifecycle::eValidUntilPresent, &extent),
			sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors,
				motion_vectors != nullptr ? sl::ResourceLifecycle::eOnlyValidNow : sl::ResourceLifecycle::eValidUntilPresent, &extent),
			sl::ResourceTag(&hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &extent),
		};
		const sl::Result tag_result = p_slSetTagForFrame(*g_token, viewport, tags, hudless != nullptr ? 3u : 2u, list);
		if (tag_result != sl::Result::eOk)
			log_line("fg: slSetTagForFrame failed (%d)", static_cast<int>(tag_result));
		if (depth_layers != nullptr && tag_result == sl::Result::eOk)
			++g_depth_frames;
		std::snprintf(g_depth_status, sizeof(g_depth_status), "%s, tag %s, layered depth accepted by Streamline in %llu frames",
			depth_layers != nullptr ? "layered depth tagged this frame" : "flat depth tagged",
			tag_result == sl::Result::eOk ? "OK" : "FAILED (see ReShade.log)", static_cast<unsigned long long>(g_depth_frames));

		// Poll the state (first frame, then twice a second): the multiplier limit and status for the overlay.
		if ((g_state_poll++ % 30) == 0)
		{
			sl::DLSSGState state{};
			if (p_slDLSSGGetState(viewport, state, nullptr) == sl::Result::eOk)
			{
				g_frames_max = state.numFramesToGenerateMax;
				g_frames_presented = state.numFramesActuallyPresented;
				g_dlssg_status = static_cast<uint32_t>(state.status);
			}
		}


		// Options: only when the requested multiplier changes. Must be set on the presenting thread before Present.
		multiplier = std::clamp(multiplier, 1, 6);
		// Limit to what this system supports; until the limit is known, stay at 2x.
		multiplier = std::min<int>(multiplier, g_frames_max != 0 ? static_cast<int>(g_frames_max) + 1 : 2);
		if (multiplier != g_applied_multiplier)
		{
			sl::DLSSGOptions options{};
			options.mode = multiplier > 1 ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
			options.numFramesToGenerate = static_cast<uint32_t>(std::max(1, multiplier - 1));
			options.numBackBuffers = 3;
			options.colorWidth = width;
			options.colorHeight = height;
			options.mvecDepthWidth = width;
			options.mvecDepthHeight = height;
			if (p_slDLSSGSetOptions(viewport, options) == sl::Result::eOk)
			{
				g_applied_multiplier = multiplier;
				log_line("fg: DLSS-G %s", multiplier > 1 ? (std::to_string(multiplier) + "x").c_str() : "off");
			}
			else
				log_line("fg: slDLSSGSetOptions failed");
		}

		set_status("DLSS-G %s | status %s | max %ux | last present showed %u frame(s)",
			g_applied_multiplier > 1 ? (std::to_string(g_applied_multiplier) + "x").c_str() : "off",
			status_text(g_dlssg_status), g_frames_max + 1, g_frames_presented);
	}

	void present_start()
	{
		if (g_attached)
			marker(sl::PCLMarker::ePresentStart);
	}

	void present_end()
	{
		if (!g_attached)
			return;
		marker(sl::PCLMarker::ePresentEnd);
		// Factorio starts simulating the next frame now.
		g_token = nullptr;
		g_render_started = false;
		++g_frame_index;
		begin_frame_token();
	}

	void reset_history()
	{
		g_reset = true;
	}

	void set_off()
	{
		if (!g_attached || p_slDLSSGSetOptions == nullptr)
			return;
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		p_slDLSSGSetOptions(sl::ViewportHandle(0), options);
		g_applied_multiplier = -1; // re-applied on the next frame
	}

	void detach()
	{
		if (!g_attached)
			return;
		set_off();
		release_static_inputs();
		g_attached = false;
		g_token = nullptr;
		g_render_started = false;
		set_status("detached");
	}

	void end()
	{
		if (!g_initialized)
			return;
		const sl::Result result = p_slShutdown();
		if (result != sl::Result::eOk)
			log_line("fg: slShutdown returned %d", static_cast<int>(result));
		g_initialized = false;
		p_slDLSSGSetOptions = nullptr;
		p_slDLSSGGetState = nullptr;
		p_slReflexSetOptions = nullptr;
		p_slPCLSetMarker = nullptr;
		set_status("shut down");
	}

	const char *status()
	{
		return g_status;
	}

	const char *depth_status()
	{
		return g_depth_status;
	}

	uint32_t frames_per_present()
	{
		return g_attached ? g_frames_presented : 0;
	}
}
