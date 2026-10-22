// AMD FSR Frame Generation backend. See fg_fsr.h.

#include "fg_fsr.h"

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include "ui_layer.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace
{
	fg_fsr::log_fn g_log = nullptr;
	bool g_load_attempted = false;
	HMODULE g_loader = nullptr;

	PfnFfxCreateContext p_create = nullptr;
	PfnFfxDestroyContext p_destroy = nullptr;
	PfnFfxConfigure p_configure = nullptr;
	PfnFfxQuery p_query = nullptr;
	PfnFfxDispatch p_dispatch = nullptr;

	ID3D12Device *g_device = nullptr; // not owned (the bridge's device outlives the contexts)
	uint32_t g_width = 0, g_height = 0;
	DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	bool g_attached = false;

	// Context descriptions: the FSR API requires them to stay alive until the context is destroyed.
	ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 g_sc_desc = {};
	ffxCreateContextDescFrameGenerationSwapChainVersionDX12 g_sc_version = {};
	DXGI_SWAP_CHAIN_DESC1 g_sc_desc1 = {};
	IDXGISwapChain4 *g_swapchain = nullptr; // the proxy (not owned here: the bridge holds the reference)
	ffxContext g_sc_ctx = nullptr;

	ffxCreateContextDescFrameGeneration g_fg_desc = {};
	ffxCreateContextDescFrameGenerationVersion g_fg_version = {};
	ffxCreateBackendDX12Desc g_backend = {};
	ffxContext g_fg = nullptr;

	ID3D12Resource *g_zero_mv = nullptr;    // R16G16_FLOAT, zero motion (motion estimation not running)
	ID3D12Resource *g_flat_depth = nullptr; // R32_FLOAT, zero

	// UI separation: FSR interpolates the HUD-less (or clean) frame and its swap chain lays the UI layer
	// (ui_layer.h) over every presented frame, real and generated.
	bool g_ui_registered = false;
	bool g_ui_mode = false;                 // last frame used UI separation
	bool g_debug_view = false;
	bool g_only_generated = false;          // diagnostic: present only the generated frames

	uint64_t g_frame_id = 0;
	bool g_reset = true;
	bool g_enabled = false;
	LARGE_INTEGER g_last_time = {};
	char g_version[64] = "";
	char g_status[200] = "not loaded";
	char g_depth_status[160] = "not passed yet";

	void log_line(const char *fmt, ...)
	{
		if (g_log == nullptr)
			return;
		char buf[512];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		g_log(buf);
	}

	void set_status(const char *fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(g_status, sizeof(g_status), fmt, args);
		va_end(args);
	}

	std::wstring game_dir()
	{
		wchar_t buf[MAX_PATH * 2] = {};
		const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
		std::wstring path(buf, n);
		const size_t slash = path.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
	}

	std::string narrow(const wchar_t *w)
	{
		std::string out;
		for (; w != nullptr && *w != 0; ++w)
			out += (*w >= 32 && *w < 127) ? static_cast<char>(*w) : '?';
		return out;
	}

	void message_callback(uint32_t type, const wchar_t *message)
	{
		log_line("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", narrow(message).c_str());
	}

	ffxReturnCode_t dispatch_callback(ffxDispatchDescFrameGeneration *params, void *context)
	{
		return p_dispatch(static_cast<ffxContext *>(context), &params->header);
	}

	// The swap chain composes this UI over every presented frame; null: none (the presented frame has its UI).
	void register_ui(ID3D12Resource *ui)
	{
		if (g_sc_ctx == nullptr || (ui == nullptr && !g_ui_registered))
			return;
		ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 d = {};
		d.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
		if (ui != nullptr)
		{
			d.uiResource = ffxApiGetResourceDX12(ui, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			// the swap chain keeps its own copy: the texture can be rewritten for the next frame right away
			d.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
		}
		const ffxReturnCode_t rc = p_configure(&g_sc_ctx, &d.header);
		if (rc != FFX_API_RETURN_OK)
			log_line("fsr: registering the UI resource failed (%u)", static_cast<unsigned>(rc));
		g_ui_registered = ui != nullptr && rc == FFX_API_RETURN_OK;
	}

	void barrier(D3D12_RESOURCE_BARRIER &b, ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
	}

	ID3D12Resource *create_texture(DXGI_FORMAT format)
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = g_width;
		desc.Height = g_height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		ID3D12Resource *res = nullptr;
		if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
				IID_PPV_ARGS(&res))))
			return nullptr;
		return res;
	}

	void release(ID3D12Resource *&r)
	{
		if (r != nullptr)
			r->Release();
		r = nullptr;
	}

	void configure(bool enabled, ID3D12Resource *hudless)
	{
		ffxConfigureDescFrameGeneration cfg = {};
		cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		cfg.swapChain = g_swapchain;
		cfg.frameGenerationCallback = enabled ? dispatch_callback : nullptr;
		cfg.frameGenerationCallbackUserContext = enabled ? &g_fg : nullptr;
		cfg.frameGenerationEnabled = enabled;
		cfg.allowAsyncWorkloads = false; // on the bridge's queue: its fence after Present then covers the inputs
		if (enabled && hudless != nullptr)
			cfg.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMMON);
		cfg.flags = g_debug_view ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
		cfg.onlyPresentGenerated = enabled && g_only_generated;
		cfg.generationRect.left = 0;
		cfg.generationRect.top = 0;
		cfg.generationRect.width = static_cast<int32_t>(g_width);
		cfg.generationRect.height = static_cast<int32_t>(g_height);
		cfg.frameID = g_frame_id;
		const ffxReturnCode_t rc = p_configure(&g_fg, &cfg.header);
		if (rc != FFX_API_RETURN_OK)
			log_line("fsr: configure failed (%u)", static_cast<unsigned>(rc));
	}
	// The frame generation context, for the current size (recreated on resize: its display size is fixed).
	bool create_fg_context()
	{
		g_fg_desc = {};
		g_fg_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		g_fg_desc.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
		g_fg_desc.displaySize.width = g_width;
		g_fg_desc.displaySize.height = g_height;
		g_fg_desc.maxRenderSize.width = g_width;
		g_fg_desc.maxRenderSize.height = g_height;
		g_fg_desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(g_format);
		g_fg_version = {};
		g_fg_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
		g_fg_version.version = FFX_FRAMEGENERATION_VERSION;
		g_backend = {};
		g_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		g_backend.device = g_device;
		g_fg_desc.header.pNext = &g_fg_version.header;
		g_fg_version.header.pNext = &g_backend.header;
		const ffxReturnCode_t rc = p_create(&g_fg, &g_fg_desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK)
		{
			log_line("fsr: creating the frame generation context failed (%u)", static_cast<unsigned>(rc));
			set_status("frame generation context failed (%u)", static_cast<unsigned>(rc));
			g_fg = nullptr;
			return false;
		}

		ffxConfigureDescGlobalDebug1 debug = {};
		debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
		debug.fpMessage = message_callback;
		debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
		p_configure(&g_fg, &debug.header);

		ffxQueryGetProviderVersion version = {};
		version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (p_query(&g_fg, &version.header) == FFX_API_RETURN_OK && version.versionName != nullptr)
			std::snprintf(g_version, sizeof(g_version), "%s", version.versionName);
		log_line("fsr: frame generation %s ready, %ux%u", g_version[0] != 0 ? g_version : "(version unknown)", g_width, g_height);
		set_status("FSR FG %s ready", g_version);

		release(g_zero_mv);
		release(g_flat_depth);
		g_zero_mv = create_texture(DXGI_FORMAT_R16G16_FLOAT);
		g_flat_depth = create_texture(DXGI_FORMAT_R32_FLOAT);

		g_frame_id = 0;
		g_reset = true;
		g_enabled = false;
		QueryPerformanceCounter(&g_last_time);
		return true;
	}
}

namespace fg_fsr
{
	bool load(log_fn log)
	{
		g_log = log;
		if (g_load_attempted)
			return g_loader != nullptr;
		g_load_attempted = true;

		const std::wstring dir = game_dir();
		const std::wstring loader = dir + L"amd_fidelityfx_loader_dx12.dll";
		if (GetFileAttributesW(loader.c_str()) == INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesW((dir + L"amd_fidelityfx_framegeneration_dx12.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			log_line("fsr: amd_fidelityfx_loader_dx12.dll / amd_fidelityfx_framegeneration_dx12.dll not found in bin\\x64; FSR frame generation unavailable");
			set_status("AMD FSR DLLs not found in bin\\x64");
			return false;
		}
		g_loader = LoadLibraryW(loader.c_str());
		if (g_loader == nullptr)
		{
			log_line("fsr: LoadLibrary(amd_fidelityfx_loader_dx12.dll) failed (error %lu)", GetLastError());
			set_status("could not load amd_fidelityfx_loader_dx12.dll");
			return false;
		}
		p_create = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void *>(GetProcAddress(g_loader, "ffxCreateContext")));
		p_destroy = reinterpret_cast<PfnFfxDestroyContext>(reinterpret_cast<void *>(GetProcAddress(g_loader, "ffxDestroyContext")));
		p_configure = reinterpret_cast<PfnFfxConfigure>(reinterpret_cast<void *>(GetProcAddress(g_loader, "ffxConfigure")));
		p_query = reinterpret_cast<PfnFfxQuery>(reinterpret_cast<void *>(GetProcAddress(g_loader, "ffxQuery")));
		p_dispatch = reinterpret_cast<PfnFfxDispatch>(reinterpret_cast<void *>(GetProcAddress(g_loader, "ffxDispatch")));
		if (p_create == nullptr || p_destroy == nullptr || p_configure == nullptr || p_query == nullptr || p_dispatch == nullptr)
		{
			FreeLibrary(g_loader);
			g_loader = nullptr;
			set_status("amd_fidelityfx_loader_dx12.dll incomplete");
			return false;
		}
		log_line("fsr: FSR API loaded from %s", narrow(dir.c_str()).c_str());
		set_status("loaded");
		return true;
	}

	bool attach(ID3D12Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format)
	{
		if (g_loader == nullptr || device == nullptr)
			return false;
		g_device = device;
		g_width = width;
		g_height = height;
		g_format = format;
		g_attached = true;
		g_reset = true;
		set_status("attached, waiting for the swap chain");
		return true;
	}

	bool active()
	{
		return g_attached && g_fg != nullptr;
	}

	bool create_swapchain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 &desc, IDXGIFactory *factory, ID3D12CommandQueue *queue, IDXGISwapChain4 **out)
	{
		*out = nullptr;
		if (!g_attached)
			return false;
		release_contexts();

		g_sc_desc1 = desc;
		g_swapchain = nullptr;
		g_sc_desc = {};
		g_sc_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
		g_sc_desc.swapchain = &g_swapchain;
		g_sc_desc.hwnd = hwnd;
		g_sc_desc.desc = &g_sc_desc1;
		g_sc_desc.fullscreenDesc = nullptr;
		g_sc_desc.dxgiFactory = factory;
		g_sc_desc.gameQueue = queue;
		g_sc_version = {};
		g_sc_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
		g_sc_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
		g_sc_desc.header.pNext = &g_sc_version.header;
		ffxReturnCode_t rc = p_create(&g_sc_ctx, &g_sc_desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK || g_swapchain == nullptr)
		{
			log_line("fsr: creating the frame generation swap chain failed (%u)", static_cast<unsigned>(rc));
			set_status("swap chain creation failed (%u)", static_cast<unsigned>(rc));
			g_sc_ctx = nullptr;
			g_swapchain = nullptr;
			return false;
		}

		if (!create_fg_context())
			log_line("fsr: continuing without frame generation");
		*out = g_swapchain; // the creation reference goes to the caller; g_swapchain does not own one
		return true;
	}

	void release_contexts()
	{
		if (g_fg != nullptr)
		{
			// disable first: waits for the proxy swap chain's work that uses frame generation resources
			if (g_swapchain != nullptr)
				configure(false, nullptr);
			p_destroy(&g_fg, nullptr);
			g_fg = nullptr;
		}
		register_ui(nullptr);
		if (g_sc_ctx != nullptr)
		{
			p_destroy(&g_sc_ctx, nullptr); // the swap chain itself goes with its last reference
			g_sc_ctx = nullptr;
		}
		g_swapchain = nullptr;
		g_enabled = false;
		g_ui_registered = false;
		g_ui_mode = false;
		release(g_zero_mv);
		release(g_flat_depth);
	}

	void before_present(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *hudless, ID3D12Resource *ui,
		ID3D12Resource *clean, ID3D12Resource *motion_vectors, ID3D12Resource *depth_layers, uint32_t width, uint32_t height, int multiplier)
	{
		if (g_fg == nullptr || g_swapchain == nullptr || list == nullptr)
			return;
		if (width != g_width || height != g_height)
			return; // a resize is in progress: the contexts are recreated with the swap chain
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		const float delta_ms = static_cast<float>(static_cast<double>(now.QuadPart - g_last_time.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart));
		g_last_time = now;

		const bool enabled = multiplier > 1;
		if (enabled && !g_enabled)
			g_reset = true;
		g_enabled = enabled;
		++g_frame_id;
		if (!enabled)
		{
			register_ui(nullptr); // the presented frame carries its own UI
			g_ui_mode = false;
			configure(false, nullptr);
			set_status("FSR FG %s | off", g_version);
			return;
		}
		// UI separation: the back buffer gets the HUD-less frame (or the clean one, the character painted over), the
		// swap chain adds the UI layer to every presented frame. Without it (no HUD-less copy, motion debug view, UI pass
		// unavailable) FSR detects the UI itself.
		const bool ui_mode = ui != nullptr && hudless != nullptr && back_buffer != nullptr;
		if (ui_mode)
		{
			D3D12_RESOURCE_BARRIER b[2];
			UINT n = 0;
			barrier(b[n++], back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
			if (clean == nullptr)
				barrier(b[n++], hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->ResourceBarrier(n, b);
			list->CopyResource(back_buffer, clean != nullptr ? clean : hudless); // the clean frame is in a copy-source read state
			n = 0;
			barrier(b[n++], back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
			if (clean == nullptr)
				barrier(b[n++], hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			list->ResourceBarrier(n, b);
		}
		static uint32_t mode_logs = 0;
		if (ui_mode != g_ui_mode && mode_logs < 20 && ++mode_logs)
			log_line("fsr: UI %s", ui_mode ? "separated (composed by the swap chain)" : "detected by FSR from the HUD-less frame");
		g_ui_mode = ui_mode;
		register_ui(ui_mode ? ui : nullptr);
		configure(true, ui_mode ? nullptr : hudless);

		ffxDispatchDescFrameGenerationPrepareV2 prep = {};
		prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prep.frameID = g_frame_id;
		prep.flags = g_debug_view ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
		prep.commandList = list;
		prep.renderSize.width = width;
		prep.renderSize.height = height;
		prep.jitterOffset.x = 0;
		prep.jitterOffset.y = 0;
		prep.motionVectorScale.x = 1.0f; // pixels, current -> previous
		prep.motionVectorScale.y = 1.0f;
		prep.frameTimeDelta = delta_ms;
		prep.reset = g_reset;
		// Factorio is a flat top-down view: a fixed camera looking straight down, depth from the world layers (0..1).
		prep.cameraNear = 0.1f;
		prep.cameraFar = 100.0f;
		prep.cameraFovAngleVertical = 1.0f;
		prep.viewSpaceToMetersFactor = 1.0f;
		ID3D12Resource *const depth = depth_layers != nullptr ? depth_layers : g_flat_depth;
		ID3D12Resource *const mvec = motion_vectors != nullptr ? motion_vectors : g_zero_mv;
		prep.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		prep.motionVectors = ffxApiGetResourceDX12(mvec, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		prep.cameraPosition[0] = 0.0f; prep.cameraPosition[1] = 0.0f; prep.cameraPosition[2] = 10.0f;
		prep.cameraUp[0] = 0.0f; prep.cameraUp[1] = 1.0f; prep.cameraUp[2] = 0.0f;
		prep.cameraRight[0] = 1.0f; prep.cameraRight[1] = 0.0f; prep.cameraRight[2] = 0.0f;
		prep.cameraForward[0] = 0.0f; prep.cameraForward[1] = 0.0f; prep.cameraForward[2] = -1.0f;
		const ffxReturnCode_t rc = p_dispatch(&g_fg, &prep.header);
		if (rc != FFX_API_RETURN_OK)
			log_line("fsr: prepare dispatch failed (%u)", static_cast<unsigned>(rc));
		g_reset = false;
		std::snprintf(g_depth_status, sizeof(g_depth_status), "%s", depth_layers != nullptr ? "world layers passed to FSR" : "flat (no layers this frame)");
		if (g_ui_mode)
			set_status("FSR FG %s | 2x | UI separated, GUI %.1f%% of the screen%s%s", g_version, ui_layer::coverage() * 100.0f,
				clean != nullptr && ui_layer::inpainted() ? " | character painted over" : "", g_only_generated ? " | GENERATED FRAMES ONLY" : "");
		else
			set_status("FSR FG %s | 2x | UI %s", g_version, hudless != nullptr ? "detected by FSR" : "not separated (no HUD-less frame)");
	}

	bool resize(uint32_t width, uint32_t height)
	{
		if (!g_attached || g_swapchain == nullptr)
			return false;
		register_ui(nullptr); // the UI texture is recreated for the new size
		g_ui_mode = false;
		if (g_fg != nullptr)
		{
			configure(false, nullptr);
			p_destroy(&g_fg, nullptr);
			g_fg = nullptr;
		}
		g_width = width;
		g_height = height;
		return create_fg_context();
	}

	void reset_history()
	{
		g_reset = true;
	}

	void set_off()
	{
		if (g_fg != nullptr && g_swapchain != nullptr)
			configure(false, nullptr);
		register_ui(nullptr);
		g_ui_mode = false;
		g_enabled = false;
		g_reset = true;
	}

	void set_debug_view(bool on)
	{
		g_debug_view = on;
	}

	void set_only_generated(bool on)
	{
		g_only_generated = on;
	}

	bool ui_separated()
	{
		return active() && g_ui_mode;
	}

	void detach()
	{
		release_contexts();
		g_attached = false;
		g_device = nullptr;
		set_status("detached");
	}

	const char *status()
	{
		return g_status;
	}

	const char *depth_status()
	{
		return g_depth_status;
	}

	const char *version_name()
	{
		return g_version;
	}

	uint32_t frames_per_present()
	{
		if (!active())
			return 0;
		return g_enabled ? 2u : 1u;
	}
}
