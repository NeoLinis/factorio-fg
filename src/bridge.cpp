// D3D11 -> D3D12 present bridge (step 2a). See bridge.h.
//
// Synchronisation between the two APIs uses one shared fence in a ping-pong:
//   D3D11: wait(2k)   -> copy final/HUD-less into the shared textures -> signal(2k+1)
//   D3D12: wait(2k+1) -> copy shared final into the swap chain        -> signal(2k+2)
// so D3D11 never overwrites a shared texture D3D12 is still reading, and D3D12 never reads a half-written one.

#include "bridge.h"
#include "fg.h"
#include "ui_layer.h"
#include "motion.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
	// Minimal COM holder (avoids a WRL dependency).
	template <class T>
	struct com
	{
		T *p = nullptr;
		com() = default;
		com(const com &) = delete;
		com &operator=(const com &) = delete;
		~com() { reset(); }
		void reset() { if (p != nullptr) { p->Release(); p = nullptr; } }
		T **put() { reset(); return &p; }
		T *get() const { return p; }
		T *operator->() const { return p; }
		explicit operator bool() const { return p != nullptr; }
	};

	constexpr UINT BACK_BUFFERS = 3;
	constexpr wchar_t WINDOW_CLASS[] = L"FactorioFgBridgeWindow";

	struct state
	{
		bridge::log_fn log = nullptr;

		HWND parent = nullptr;
		HWND child = nullptr;
		DWORD window_thread = 0;
		bool visible = true;

		uint32_t width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // swap chain / shared texture format (typed, non-sRGB)
		bool tearing_supported = false;

		com<ID3D11Device5> dev11;
		com<ID3D11Fence> shared_fence11;
		ID3D11DeviceContext *cached_ctx = nullptr; // not owned, only used to detect a different context
		com<ID3D11DeviceContext4> ctx11;

		com<IDXGIFactory4> factory;      // plain factory: adapter lookup, window association
		com<IDXGIFactory4> sc_factory;   // creates the swap chain: Streamline proxy when frame generation is on
		com<ID3D12Device> dev12;          // native device, used for everything Streamline does not intercept
		com<ID3D12Device> dev12_proxy;    // Streamline proxy device (only for CreateCommandQueue)
		bool fg = false;                  // a frame generation backend (DLSS-G or FSR) attached, see fg.h
		com<ID3D12CommandQueue> queue;
		com<ID3D12CommandAllocator> allocators[BACK_BUFFERS];
		uint64_t allocator_fence[BACK_BUFFERS] = {};
		com<ID3D12GraphicsCommandList> list;
		com<ID3D12Fence> frame_fence;
		uint64_t frame_fence_value = 0;
		HANDLE frame_event = nullptr;
		HANDLE cut_event = nullptr; // the scene-cut wait's own (it may be signalled after its short wait gave up)
		com<ID3D12Fence> shared_fence12;
		uint64_t shared_value = 0;

		com<IDXGISwapChain3> swapchain;
		com<ID3D12Resource> back_buffers[BACK_BUFFERS];

		com<ID3D12Resource> final12, hudless12, mask12;
		com<ID3D11Texture2D> final11, hudless11, mask11; // mask: the exact character mask (always R8G8B8A8_UNORM)
		// World layer snapshots, see bridge::layer. Two sets: the add-on copies this frame's snapshots straight into
		// set 'layer_write' (no extra copy at present) while D3D12 may still read the other set from the last frame.
		com<ID3D12Resource> layer12[2][bridge::LAYER_COUNT];
		com<ID3D11Texture2D> layer11[2][bridge::LAYER_COUNT];
		uint32_t layer_write = 0;

		uint64_t frames = 0;
		char status[256] = "off";

		// scene cuts and timing
		float diff_average = 0;  // running average of the frame difference at the camera shift
		uint32_t diff_samples = 0;
		float last_diff = 0;
		uint32_t cuts = 0;
		float wait_ms = 0, cpu_ms = 0; // smoothed: GPU wait for the cut check, total CPU time in present()
		float last_wait_ms = 0, last_cpu_ms = 0; // the last frame's
		// scene-cut check not finished within the short wait: evaluated at the next frame (fence value, 0 = none)
		uint64_t cut_pending = 0;
		float last_alloc_wait_ms = 0, last_present_call_ms = 0; // the last frame: waiting for a free command allocator, inside Present
		uint32_t cuts_late = 0, checks_late = 0;
	};

	state *s = nullptr;
	char g_status_off[] = "off";

	void log_line(const char *fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (s != nullptr && s->log != nullptr)
			s->log(buf);
	}

	void set_status(const char *fmt, ...)
	{
		if (s == nullptr)
			return;
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(s->status, sizeof(s->status), fmt, args);
		va_end(args);
	}

	DXGI_FORMAT swapchain_format(DXGI_FORMAT f)
	{
		// Flip-model swap chains do not accept sRGB formats.
		switch (f)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		default: return f;
		}
	}

	// ------------------------------------------------------------ window

	LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		switch (msg)
		{
		case WM_NCHITTEST:
			return HTTRANSPARENT; // every mouse event goes to Factorio's window underneath
		case WM_ERASEBKGND:
			return 1;
		case WM_CLOSE:
			DestroyWindow(hwnd);
			return 0;
		default:
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}
	}

	HINSTANCE this_module()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&window_proc), &module);
		return module;
	}

	bool create_child_window()
	{
		static bool registered = false;
		if (!registered)
		{
			WNDCLASSEXW wc = {};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = window_proc;
			wc.hInstance = this_module();
			wc.lpszClassName = WINDOW_CLASS;
			if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				return false;
			registered = true;
		}

		RECT rc = {};
		GetClientRect(s->parent, &rc);
		// WS_DISABLED + HTTRANSPARENT: the window never takes focus or input, Factorio keeps all of it.
		s->child = CreateWindowExW(WS_EX_NOPARENTNOTIFY, WINDOW_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
			0, 0, rc.right - rc.left, rc.bottom - rc.top, s->parent, nullptr, this_module(), nullptr);
		if (s->child == nullptr)
			return false;
		s->window_thread = GetCurrentThreadId();
		SetWindowPos(s->child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		return true;
	}

	void pump_child_messages()
	{
		if (s->child == nullptr || GetCurrentThreadId() != s->window_thread)
			return;
		MSG msg;
		while (PeekMessageW(&msg, s->child, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
	}

	void follow_parent_size()
	{
		RECT parent_rc = {}, child_rc = {};
		GetClientRect(s->parent, &parent_rc);
		GetClientRect(s->child, &child_rc);
		if (parent_rc.right != child_rc.right || parent_rc.bottom != child_rc.bottom)
			SetWindowPos(s->child, HWND_TOP, 0, 0, parent_rc.right, parent_rc.bottom, SWP_NOACTIVATE | (s->visible ? SWP_SHOWWINDOW : 0));
	}

	// ------------------------------------------------------------ D3D12 helpers

	void wait_gpu_idle()
	{
		if (!s->queue || !s->frame_fence)
			return;
		const uint64_t value = ++s->frame_fence_value;
		s->queue->Signal(s->frame_fence.get(), value);
		if (s->frame_fence->GetCompletedValue() < value)
		{
			s->frame_fence->SetEventOnCompletion(value, s->frame_event);
			WaitForSingleObject(s->frame_event, 2000);
		}
	}

	bool create_shared_texture(uint32_t w, uint32_t h, DXGI_FORMAT format, com<ID3D12Resource> &out12, com<ID3D11Texture2D> &out11)
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
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (FAILED(s->dev12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
				IID_PPV_ARGS(out12.put()))))
			return false;

		HANDLE handle = nullptr;
		if (FAILED(s->dev12->CreateSharedHandle(out12.get(), nullptr, GENERIC_ALL, nullptr, &handle)))
			return false;
		const HRESULT hr = s->dev11->OpenSharedResource1(handle, IID_PPV_ARGS(out11.put()));
		CloseHandle(handle);
		return SUCCEEDED(hr);
	}

	uint64_t g_layer_generation = 1; // changes whenever the layer textures are recreated or released

	bool create_size_dependent(uint32_t w, uint32_t h)
	{
		for (auto &b : s->back_buffers)
			b.reset();
		s->final11.reset(); s->final12.reset();
		s->hudless11.reset(); s->hudless12.reset();
		s->mask11.reset(); s->mask12.reset();
		++g_layer_generation; // pointers handed out by layer_target() are about to go
		for (uint32_t k = 0; k < 2; ++k)
			for (uint32_t i = 0; i < bridge::LAYER_COUNT; ++i)
			{
				s->layer11[k][i].reset();
				s->layer12[k][i].reset();
			}

		const UINT flags = s->tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
		if (s->swapchain)
		{
			if (s->fg)
				fg::set_off(); // DLSS-G must be off while the swap chain changes; re-applied next frame
			if (FAILED(s->swapchain->ResizeBuffers(BACK_BUFFERS, w, h, s->format, flags)))
				return false;
			if (s->fg)
				fg::resized(w, h);
			if (s->fg)
				fg::reset_history();
			motion::resize(w, h);
			ui_layer::resize(w, h);
		}
		else
		{
			DXGI_SWAP_CHAIN_DESC1 desc = {};
			desc.Width = w;
			desc.Height = h;
			desc.Format = s->format;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = BACK_BUFFERS;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			desc.Flags = flags;
			com<IDXGISwapChain1> sc1;
			if (s->fg && fg::creates_swapchain())
			{
				// FSR: its frame generation swap chain (a proxy that inserts the generated frames and paces them)
				IDXGISwapChain4 *sc4 = nullptr;
				if (!fg::create_swapchain(s->child, desc, s->sc_factory.get(), s->queue.get(), &sc4))
					return false;
				sc1.p = sc4;
			}
			else if (FAILED(s->sc_factory->CreateSwapChainForHwnd(s->queue.get(), s->child, &desc, nullptr, nullptr, sc1.put())))
				return false;
			s->sc_factory->MakeWindowAssociation(s->child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
			if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(s->swapchain.put()))))
				return false;
		}
		for (UINT i = 0; i < BACK_BUFFERS; ++i)
			if (FAILED(s->swapchain->GetBuffer(i, IID_PPV_ARGS(s->back_buffers[i].put()))))
				return false;

		if (!create_shared_texture(w, h, s->format, s->final12, s->final11) ||
			!create_shared_texture(w, h, s->format, s->hudless12, s->hudless11))
			return false;
		if (!create_shared_texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s->mask12, s->mask11))
		{
			s->mask11.reset(); // optional: without it the character is guessed as in 1.1
			s->mask12.reset();
		}
		for (uint32_t k = 0; k < 2; ++k)
			for (uint32_t i = 0; i < bridge::LAYER_COUNT; ++i)
				if (!create_shared_texture(w, h, s->format, s->layer12[k][i], s->layer11[k][i]))
					return false;

		s->width = w;
		s->height = h;
		return true;
	}

	void transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, D3D12_RESOURCE_BARRIER &out)
	{
		out = {};
		out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		out.Transition.pResource = res;
		out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		out.Transition.StateBefore = before;
		out.Transition.StateAfter = after;
	}

	bool init_impl(ID3D11Device *device11, uint32_t width, uint32_t height)
	{
		if (FAILED(device11->QueryInterface(IID_PPV_ARGS(s->dev11.put()))))
		{
			log_line("bridge: ID3D11Device5 not available (needs Windows 10 1703+)");
			return false;
		}

		com<IDXGIDevice> dxgi_device;
		com<IDXGIAdapter> adapter11;
		if (FAILED(device11->QueryInterface(IID_PPV_ARGS(dxgi_device.put()))) || FAILED(dxgi_device->GetAdapter(adapter11.put())))
			return false;
		DXGI_ADAPTER_DESC adapter_desc = {};
		adapter11->GetDesc(&adapter_desc);

		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(s->factory.put()))))
			return false;
		com<IDXGIAdapter1> adapter12;
		if (FAILED(s->factory->EnumAdapterByLuid(adapter_desc.AdapterLuid, IID_PPV_ARGS(adapter12.put()))))
			return false;
		if (FAILED(D3D12CreateDevice(adapter12.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(s->dev12.put()))))
		{
			log_line("bridge: D3D12CreateDevice failed");
			return false;
		}

		// ---- Frame generation backend (optional): DLSS-G or FSR, see fg.h. Everything below falls back to plain
		// D3D12 presentation if neither is available.
		{
			IDXGIFactory4 *proxy_factory = nullptr;
			ID3D12Device *proxy_device = nullptr;
			const fg::backend b = fg::start(s->dev12.get(), adapter_desc.AdapterLuid, adapter_desc.VendorId, width, height, s->format, s->log,
				&proxy_factory, &proxy_device);
			s->fg = b != fg::backend::none;
			if (proxy_factory != nullptr)
				s->sc_factory.p = proxy_factory; // DLSS-G: Streamline's proxies create the queue and the swap chain
			if (proxy_device != nullptr)
				s->dev12_proxy.p = proxy_device;
		}
		if (!s->sc_factory)
		{
			s->factory->AddRef();
			s->sc_factory.p = s->factory.get();
		}
		if (s->fg)
		{
			motion::init(s->dev12.get(), width, height, s->format, s->log);
			if (ui_layer::init(s->dev12.get(), s->format, s->log))
				ui_layer::resize(width, height);
		}

		com<IDXGIFactory5> factory5;
		if (SUCCEEDED(s->factory->QueryInterface(IID_PPV_ARGS(factory5.put()))))
		{
			BOOL tearing = FALSE;
			if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
				s->tearing_supported = tearing != FALSE;
		}

		D3D12_COMMAND_QUEUE_DESC queue_desc = {};
		queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		// Streamline intercepts CreateCommandQueue, so the queue comes from the proxy device when frame generation is on.
		ID3D12Device *const queue_device = s->dev12_proxy ? s->dev12_proxy.get() : s->dev12.get();
		if (FAILED(queue_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(s->queue.put()))))
			return false;
		for (auto &a : s->allocators)
			if (FAILED(s->dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a.put()))))
				return false;
		if (FAILED(s->dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocators[0].get(), nullptr, IID_PPV_ARGS(s->list.put()))))
			return false;
		s->list->Close();

		if (FAILED(s->dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(s->frame_fence.put()))))
			return false;
		s->frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		s->cut_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

		// Fence shared with D3D11
		if (FAILED(s->dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(s->shared_fence12.put()))))
			return false;
		HANDLE fence_handle = nullptr;
		if (FAILED(s->dev12->CreateSharedHandle(s->shared_fence12.get(), nullptr, GENERIC_ALL, nullptr, &fence_handle)))
			return false;
		const HRESULT hr = s->dev11->OpenSharedFence(fence_handle, IID_PPV_ARGS(s->shared_fence11.put()));
		CloseHandle(fence_handle);
		if (FAILED(hr))
		{
			log_line("bridge: OpenSharedFence failed (0x%08X)", static_cast<unsigned>(hr));
			return false;
		}
		{
			UINT64 frequency = 0;
			if (SUCCEEDED(s->queue->GetTimestampFrequency(&frequency)))
				motion::set_timestamp_frequency(frequency);
		}

		if (!create_child_window())
		{
			log_line("bridge: could not create the child window (error %lu)", GetLastError());
			return false;
		}
		if (!create_size_dependent(width, height))
		{
			log_line("bridge: could not create swap chain / shared textures");
			return false;
		}

		log_line("bridge: D3D12 device on '%ls', %ux%u format %d, %u buffers, tearing %s, frame generation %s",
			adapter_desc.Description, width, height, static_cast<int>(s->format), BACK_BUFFERS, s->tearing_supported ? "yes" : "no",
			s->fg ? fg::name(fg::current()) : "unavailable");
		return true;
	}
}

namespace
{
	// Scene-cut check, first part: the motion passes up to the camera's coarse search go to the GPU on their own, so
	// the CPU waits only for those (and for Factorio's frame), not for the whole motion estimation.
	struct cut_split
	{
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *allocator = nullptr;
		ID3D12Fence *shared_fence = nullptr;
		uint64_t shared_value = 0;
		ID3D12Fence *frame_fence = nullptr;
		uint64_t *frame_fence_value = nullptr;
		uint64_t check = 0;
		bool submitted = false;
	};

	void submit_cut_part(void *context)
	{
		cut_split *c = static_cast<cut_split *>(context);
		c->list->Close();
		c->queue->Wait(c->shared_fence, c->shared_value);
		ID3D12CommandList *const l[] = { c->list };
		c->queue->ExecuteCommandLists(1, l);
		c->check = ++*c->frame_fence_value;
		c->queue->Signal(c->frame_fence, c->check);
		c->list->Reset(c->allocator, nullptr); // same allocator: it may grow, the first part is in flight
		c->submitted = true;
	}
}

namespace bridge
{
	bool init(ID3D11Device *device11, HWND game_window, uint32_t width, uint32_t height, DXGI_FORMAT format, log_fn log)
	{
		shutdown();
		s = new state();
		s->log = log;
		s->parent = game_window;
		s->format = swapchain_format(format);

		if (game_window == nullptr || width == 0 || height == 0 || !init_impl(device11, width, height))
		{
			log_line("bridge: init failed, staying on Factorio's own swap chain");
			shutdown();
			return false;
		}
		set_status("active, %ux%u", width, height);
		return true;
	}

	void shutdown()
	{
		if (s == nullptr)
			return;
		// Unblock D3D11 if it is waiting on a value D3D12 will now never signal (fail-safe after a sync timeout).
		if (s->shared_fence12)
			s->shared_fence12->Signal(UINT64_MAX / 2);
		if (s->fg)
			fg::set_off();
		wait_gpu_idle();

		// Release the swap chain before its window goes away, and all Streamline objects before slShutdown.
		for (auto &b : s->back_buffers)
			b.reset();
		if (s->fg)
			fg::before_swapchain_release();
		s->swapchain.reset();
		motion::shutdown();
		ui_layer::shutdown();
		if (s->fg)
			fg::detach();
		s->sc_factory.reset();
		s->dev12_proxy.reset();
		s->queue.reset();
		if (s->fg)
		{
			fg::end();
			s->fg = false;
		}

		if (s->child != nullptr)
		{
			if (GetCurrentThreadId() == s->window_thread)
				DestroyWindow(s->child);
			else
				PostMessageW(s->child, WM_CLOSE, 0, 0);
		}
		if (s->frame_event != nullptr)
			CloseHandle(s->frame_event);
		if (s->cut_event != nullptr)
			CloseHandle(s->cut_event);

		++g_layer_generation;
		delete s; // com<> members release in reverse declaration order
		s = nullptr;
	}

	bool active()
	{
		return s != nullptr;
	}

	bool g_overlay_hidden = false; // the ReShade overlay is open (drawn on Factorio's own swap chain, under our window)
	bool g_suspended = false;      // frame generation switched off: everything stays alive, nothing is presented

	void apply_visibility()
	{
		if (s == nullptr || s->child == nullptr)
			return;
		const bool visible = !g_overlay_hidden && !g_suspended;
		s->visible = visible;
		ShowWindowAsync(s->child, visible ? SW_SHOWNA : SW_HIDE); // async: never blocks on another thread's queue
	}

	void set_visible(bool visible)
	{
		g_overlay_hidden = !visible;
		apply_visibility();
	}

	void suspend(bool suspended)
	{
		if (s == nullptr || g_suspended == suspended)
			return;
		g_suspended = suspended;
		if (suspended)
		{
			if (s->fg)
				fg::set_off(); // DLSS-G idle; re-applied on the first frame after resuming
			log_line("bridge: paused (frame generation off, Factorio presents on its own)");
		}
		else
		{
			if (s->fg)
				fg::reset_history();
			motion::invalidate_history();
			log_line("bridge: resumed");
		}
		apply_visibility();
	}

	bool suspended()
	{
		return s != nullptr && g_suspended;
	}

	const char *status()
	{
		return s != nullptr ? s->status : g_status_off;
	}

	uint32_t max_multiplier()
	{
		return s != nullptr && s->fg ? fg::max_multiplier() : 4u;
	}

	const char *backend_name()
	{
		return s != nullptr && s->fg ? fg::name(fg::current()) : "none";
	}

	const char *fg_status()
	{
		return fg::status();
	}

	const char *motion_status()
	{
		return motion::status();
	}

	void motion_global(float &dx, float &dy, float &confidence)
	{
		motion::last_global(dx, dy, confidence);
	}

	bool layers_used()
	{
		return s != nullptr && motion::layers_used();
	}

	const char *depth_status()
	{
		return s != nullptr && s->fg ? fg::depth_status() : "frame generation is off";
	}

	float motion_zoom()
	{
		return motion::last_zoom();
	}

	void motion_shares(float &camera, float &still, float &own)
	{
		motion::block_shares(camera, still, own);
	}

	const char *camera_line()
	{
		static char buf[200];
		const motion::camera_check c = motion::camera_compare();
		const char *src = c.used_last ? "Factorio's sprites (exact)" : (c.wanted ? "the picture (sprites not exact now)" : "the picture");
		if (c.frames == 0)
			std::snprintf(buf, sizeof(buf), "%s | picture vs sprites: not compared yet (move the camera)", src);
		else
			std::snprintf(buf, sizeof(buf), "%s | picture vs sprites: %.2f px off on average, %.0f%% of frames over 0.25 px (%u frames)",
				src, c.mean_diff, c.share_off * 100.0f, c.frames);
		return buf;
	}

	bool camera_trace_start(uint32_t frames)
	{
		return motion::trace_start(frames);
	}

	int camera_trace_left()
	{
		return motion::trace_left();
	}

	bool camera_trace_take(std::string &out)
	{
		return motion::trace_take(out);
	}

	const char *fit_line()
	{
		static char buf[240];
		motion::fit_info f;
		if (!motion::motion_fit(f))
		{
			std::snprintf(buf, sizeof(buf), "Motion fit: -");
			return buf;
		}
		// per layer: mean luma difference (x100) after moving the previous frame by our vectors, and by the camera alone
		static const char *const names[6] = { "ground", "floor", "objects", "icons", "flying", "all" };
		int n = std::snprintf(buf, sizeof(buf), "Motion fit (lower = better; camera alone):");
		for (int i = 0; i < 6 && n > 0 && n < static_cast<int>(sizeof(buf)); ++i)
			if (f.share[i] >= 0.002f)
				n += std::snprintf(buf + n, sizeof(buf) - n, " %s %.3f (%.3f)", names[i], f.ours[i] * 100.0f, f.camera[i] * 100.0f);
		return buf;
	}

	uint32_t frames_per_present()
	{
		if (s == nullptr)
			return 0;
		if (!s->fg)
			return 1;
		const uint32_t n = fg::frames_per_present();
		return n != 0 ? n : 1;
	}

	timings get_timings()
	{
		timings t;
		if (s == nullptr)
			return t;
		t.motion_gpu_ms = motion::gpu_ms();
		motion::gpu_breakdown(t.motion_camera_ms, t.motion_block_ms, t.motion_rest_ms);
		motion::search_counts(t.blocks_searched, t.blocks_wide, t.movers_found);
		t.cut_wait_ms = s->wait_ms;
		t.present_cpu_ms = s->cpu_ms;
		t.last_cut_wait_ms = s->last_wait_ms;
		t.cut_checks_late = s->checks_late;
		t.last_alloc_wait_ms = s->last_alloc_wait_ms;
		t.last_present_call_ms = s->last_present_call_ms;
		t.cuts_late = s->cuts_late;
		t.last_present_cpu_ms = s->last_cpu_ms;
		t.difference = s->last_diff;
		t.difference_average = s->diff_average;
		t.scene_cuts = s->cuts;
		return t;
	}

	void frame_start()
	{
		if (s != nullptr && s->fg && !g_suspended)
			fg::frame_start();
	}

	ID3D11Texture2D *layer_target(uint32_t layer)
	{
		if (s == nullptr || g_suspended || layer >= LAYER_COUNT)
			return nullptr;
		return s->layer11[s->layer_write][layer].get();
	}

	uint64_t layer_generation()
	{
		return g_layer_generation;
	}

	bool shadow_darkness(shadow_info &out)
	{
		out = {};
		if (s == nullptr)
			return false;
		motion::shadow_info mi = {};
		const bool used = motion::shadow_darkness(mi);
		for (int i = 0; i < 4; ++i)
		{
			out.k[i] = mi.k[i];
			out.frames[i] = mi.frames[i];
			out.samples[i] = mi.samples[i];
		}
		return used;
	}

	bool exact_mask_used()
	{
		return s != nullptr && motion::exact_mask_used();
	}

	void present(ID3D11DeviceContext *context, ID3D11Texture2D *final_frame, ID3D11Texture2D *hudless,
		ID3D11Texture2D *const *layers_in, uint64_t layers_generation, ID3D11Texture2D *character_mask, const present_options &options)
	{
		if (s == nullptr || context == nullptr || final_frame == nullptr || g_suspended)
			return;
		LARGE_INTEGER t_start, t_freq;
		QueryPerformanceCounter(&t_start);
		QueryPerformanceFrequency(&t_freq);

		pump_child_messages();
		follow_parent_size();

		D3D11_TEXTURE2D_DESC final_desc = {};
		final_frame->GetDesc(&final_desc);
		if (final_desc.Width != s->width || final_desc.Height != s->height)
		{
			wait_gpu_idle();
			if (!create_size_dependent(final_desc.Width, final_desc.Height))
			{
				log_line("bridge: resize to %ux%u failed, shutting down", final_desc.Width, final_desc.Height);
				shutdown();
				return;
			}
			log_line("bridge: resized to %ux%u", final_desc.Width, final_desc.Height);
		}

		if (context != s->cached_ctx)
		{
			s->ctx11.reset();
			if (FAILED(context->QueryInterface(IID_PPV_ARGS(s->ctx11.put()))))
				return;
			s->cached_ctx = context;
		}

		// ---- D3D11: wait until D3D12 is done with the shared textures, copy, signal
		s->ctx11->Wait(s->shared_fence11.get(), s->shared_value);
		context->CopyResource(s->final11.get(), final_frame);
		if (hudless != nullptr)
			context->CopyResource(s->hudless11.get(), hudless);
		bool have_mask = false;
		if (character_mask != nullptr && s->mask11)
		{
			D3D11_TEXTURE2D_DESC md = {};
			character_mask->GetDesc(&md);
			if (md.Width == s->width && md.Height == s->height && md.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
			{
				context->CopyResource(s->mask11.get(), character_mask);
				have_mask = true;
			}
		}
		// Snapshots taken straight into this frame's set need no copy; others (taken into the add-on's own textures)
		// are copied in. Pointers from before a resize or restart are not touched.
		const uint32_t set = s->layer_write;
		bool layers = layers_in != nullptr && layers_generation == g_layer_generation;
		for (uint32_t i = 0; layers && i < LAYER_COUNT; ++i)
			layers = layers_in[i] != nullptr;
		if (layers)
			for (uint32_t i = 0; i < LAYER_COUNT; ++i)
				if (layers_in[i] != s->layer11[set][i].get())
					context->CopyResource(s->layer11[set][i].get(), layers_in[i]);
		s->layer_write = 1 - set; // the add-on writes the other set next frame
		const uint64_t written = ++s->shared_value;
		s->ctx11->Signal(s->shared_fence11.get(), written);
		context->Flush();

		// ---- D3D12: wait for the copy, blit into the swap chain, present
		const UINT index = s->swapchain->GetCurrentBackBufferIndex();
		LARGE_INTEGER a0, a1;
		QueryPerformanceCounter(&a0);
		if (s->frame_fence->GetCompletedValue() < s->allocator_fence[index])
		{
			s->frame_fence->SetEventOnCompletion(s->allocator_fence[index], s->frame_event);
			if (WaitForSingleObject(s->frame_event, 1000) == WAIT_TIMEOUT)
			{
				// The D3D12 queue stopped making progress (most likely stuck on the shared fence). Do not let that
				// freeze the game: give up and fall back to Factorio's own swap chain.
				log_line("bridge: GPU sync timeout (shared fence %llu, completed %llu), shutting down",
					static_cast<unsigned long long>(s->shared_value),
					static_cast<unsigned long long>(s->shared_fence12->GetCompletedValue()));
				shutdown();
				return;
			}
		}
		QueryPerformanceCounter(&a1);
		s->last_alloc_wait_ms = static_cast<float>(static_cast<double>(a1.QuadPart - a0.QuadPart) * 1000.0 / static_cast<double>(t_freq.QuadPart));
		s->allocators[index]->Reset();
		s->list->Reset(s->allocators[index].get(), nullptr);

		ID3D12Resource *const back = s->back_buffers[index].get();
		D3D12_RESOURCE_BARRIER barriers[2];
		transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, barriers[0]);
		transition(s->final12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, barriers[1]);
		s->list->ResourceBarrier(2, barriers);
		s->list->CopyResource(back, s->final12.get());
		transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT, barriers[0]);
		transition(s->final12.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON, barriers[1]);
		s->list->ResourceBarrier(2, barriers);
		ID3D12CommandList *const lists[] = { s->list.get() };
		bool waited_for_shared = false;
		float wait_ms = 0;
		if (s->fg)
		{
			ID3D12Resource *mvec = nullptr;
			ID3D12Resource *depth = nullptr;
			ID3D12Resource *hold = nullptr; // character pixels held still (FSR shows them from the real frame)
			bool back_is_final = true; // false while the motion debug picture replaces the game picture
			if (hudless != nullptr && motion::active())
			{
				motion::settings ms;
				ms.enabled = options.motion_vectors;
				ms.flip_sign = options.motion_flip;
				ms.penalty_zero = options.motion_penalty_zero;
				ms.mismatch = options.motion_mismatch;
				ms.hysteresis = options.motion_hysteresis;
				{
					// mouse position in frame pixels: a zoom centre candidate
					POINT cursor;
					RECT client;
					if (GetCursorPos(&cursor) && ScreenToClient(s->parent, &cursor) && GetClientRect(s->parent, &client) &&
						client.right > 0 && client.bottom > 0)
					{
						ms.cursor_x = static_cast<float>(cursor.x) * static_cast<float>(s->width) / static_cast<float>(client.right);
						ms.cursor_y = static_cast<float>(cursor.y) * static_cast<float>(s->height) / static_cast<float>(client.bottom);
					}
				}
				ms.strictness = options.motion_strictness;
				ms.debug_view = options.motion_debug_view;
				ms.far_penalty = options.motion_far_penalty;
				ms.still_zone = options.motion_still_zone;
				ms.mover_search = options.motion_mover_search;
				ms.world_layers = options.motion_world_layers;
				ms.debug_layer_view = options.motion_debug_layer_view;
				ms.icons_first = options.motion_icons_first;
				ms.wide_search = options.motion_wide_search;
				ms.floor_camera = options.motion_floor_camera;
				ms.sprite_camera = options.motion_sprite_camera;
				ms.icon_depth_flat = options.motion_icon_depth_flat;
				ms.fit_measure = options.motion_fit_measure;
				ms.sprite_camera_support = options.sprite_camera_support;
				ms.sprite_camera_tight = options.sprite_camera_tight;
				ms.icon_motion = options.motion_icon_motion;
				ms.icon_shift_valid = options.icon_shift_valid;
				ms.icon_dx = options.icon_dx;
				ms.icon_dy = options.icon_dy;
				std::memcpy(ms.stage_diag, options.stage_diag, sizeof(ms.stage_diag));
				ms.sprite_camera_valid = options.sprite_camera_valid;
				ms.sprite_camera_dx = options.sprite_camera_dx;
				ms.sprite_camera_dy = options.sprite_camera_dy;
				ms.spread_depth = fg::current() == fg::backend::fsr;
				ms.hold_as_ground = options.fg_hold_character && options.fg_inpaint_character && !options.fg_show_ui_layer &&
					options.fg_multiplier > 1 && !options.motion_debug_view;
				ms.shadow_overlay = options.fg_shadow_overlay && options.fg_multiplier > 1 && !options.motion_debug_view &&
					fg::ui_layer_usable();
				motion::layer_set ls;
				if (layers)
				{
					ls.ground = s->layer12[set][LAYER_GROUND].get();
					ls.floor = s->layer12[set][LAYER_FLOOR].get();
					ls.entities = s->layer12[set][LAYER_ENTITIES].get();
					ls.postcloud = s->layer12[set][LAYER_POSTCLOUD].get();
					ls.world = s->layer12[set][LAYER_WORLD].get();
					ls.airsplit = s->layer12[set][LAYER_AIRSPLIT].get();
				}
				ls.character = have_mask ? s->mask12.get() : nullptr;
				const bool cut_check = options.scene_cut_detection && options.fg_multiplier > 1;
				// The scene-cut metric of a frame (motion::current_difference: the frame recorded last). A cut found
				// late (the check did not finish within the short wait at its own present) still clears the histories
				// before this frame's motion is recorded: only the frames generated across the cut itself are lost.
				const auto judge_cut = [&](bool late) {
					float diff = 0;
					if (!motion::current_difference(diff))
						return;
					s->last_diff = diff;
					const bool cut = s->diff_samples >= 8 && diff > options.scene_cut_min_difference &&
						diff > s->diff_average * options.scene_cut_factor;
					if (cut)
					{
						++s->cuts;
						if (late)
							++s->cuts_late;
						fg::reset_history();
						motion::invalidate_history();
						log_line("bridge: scene cut (difference %.3f, average %.3f)%s", diff, s->diff_average, late ? " - found a frame late" : "");
					}
					else
					{
						s->diff_average = s->diff_samples == 0 ? diff : s->diff_average * 0.9f + diff * 0.1f;
						++s->diff_samples;
					}
				};
				if (s->cut_pending != 0)
				{
					// a frame later the GPU is normally done; if not, waiting now would stall just the same: drop it
					if (s->frame_fence->GetCompletedValue() >= s->cut_pending)
						judge_cut(true);
					s->cut_pending = 0;
				}
				cut_split split;
				split.list = s->list.get();
				split.queue = s->queue.get();
				split.allocator = s->allocators[index].get();
				split.shared_fence = s->shared_fence12.get();
				split.shared_value = written;
				split.frame_fence = s->frame_fence.get();
				split.frame_fence_value = &s->frame_fence_value;
				mvec = motion::record(s->list.get(), s->hudless12.get(), ls, ms, cut_check ? submit_cut_part : nullptr, &split);
				if (split.submitted)
					waited_for_shared = true;
				if (mvec != nullptr)
				{
					depth = motion::depth_texture(); // from the world layers; flat when they are not available
					hold = motion::hold_texture();
				}
				if (ID3D12Resource *debug = motion::debug_texture())
				{
					// show the motion debug picture instead of the game picture
					D3D12_RESOURCE_BARRIER db[2];
					transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, db[0]);
					transition(debug, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, db[1]);
					s->list->ResourceBarrier(2, db);
					s->list->CopyResource(back, debug);
					back_is_final = false;
					transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT, db[0]);
					transition(debug, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, db[1]);
					s->list->ResourceBarrier(2, db);
				}

				if (split.submitted)
				{
					// Scene cut check: DLSS-G must be told before this Present. The first part (up to the camera's coarse
					// search) is already on the GPU; submit the rest too so the GPU keeps working, then wait for the first
					// part only and look at how different this frame is from the last one at the best camera shift.
					s->list->Close();
					s->queue->ExecuteCommandLists(1, lists);
					s->list->Reset(s->allocators[index].get(), nullptr);
					const uint64_t check = split.check;
					LARGE_INTEGER w0, w1;
					QueryPerformanceCounter(&w0);
					bool done = s->frame_fence->GetCompletedValue() >= check;
					if (!done)
					{
						// At most 2 ms: the GPU may still be busy with earlier frames (frame generation, its pacing), and
						// waiting for that stalled Factorio's thread by 30-60 ms at times (stutter log, beta 32). Not done
						// by then: judged at the next frame.
						if (s->cut_event != nullptr)
						{
							ResetEvent(s->cut_event); // a late signal of an earlier check
							s->frame_fence->SetEventOnCompletion(check, s->cut_event);
							WaitForSingleObject(s->cut_event, 2);
						}
						done = s->frame_fence->GetCompletedValue() >= check;
					}
					QueryPerformanceCounter(&w1);
					wait_ms = static_cast<float>(static_cast<double>(w1.QuadPart - w0.QuadPart) * 1000.0 / static_cast<double>(t_freq.QuadPart));
					if (done)
						judge_cut(false);
					else
					{
						s->cut_pending = check;
						++s->checks_late;
					}
				}
			}
			const int multiplier = back_is_final ? options.fg_multiplier : 1; // paused under the motion debug view
			// The UI layer (GUI and held character from the real frame) and the clean frame, for either technology.
			ID3D12Resource *ui = nullptr, *clean = nullptr;
			if (multiplier > 1 && hudless != nullptr)
			{
				ui_layer::inputs in;
				in.final_frame = s->final12.get();
				in.hudless = s->hudless12.get();
				in.hold = hold; // also when the character is generated: the shadow's darkening
				in.hold_character = options.fg_hold_character;
				in.exact = motion::exact_mask_used();
				in.shadow = options.fg_shadow_overlay && motion::exact_mask_used();
				in.floor = layers ? s->layer12[set][LAYER_FLOOR].get() : nullptr;
				in.objects = layers ? s->layer12[set][LAYER_ENTITIES].get() : nullptr;
				in.inpaint = options.fg_inpaint_character && !options.fg_show_ui_layer;
				if (ui_layer::record(s->list.get(), in))
				{
					ui = ui_layer::ui_texture();
					clean = ui_layer::clean_texture();
				}
			}
			fg::set_debug_view(options.fg_debug_view);
			fg::set_only_generated(options.fg_only_generated);
			fg::before_present(s->list.get(), back, hudless != nullptr ? s->hudless12.get() : nullptr, ui, clean, mvec, depth, s->width, s->height,
				multiplier);
			if (ui != nullptr && options.fg_show_ui_layer && s->format == DXGI_FORMAT_R8G8B8A8_UNORM)
			{
				// diagnostic: the UI layer itself in the back buffer (black where there is none)
				D3D12_RESOURCE_BARRIER sb[2];
				transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, sb[0]);
				transition(ui, ui_layer::ui_state(), D3D12_RESOURCE_STATE_COPY_SOURCE, sb[1]);
				s->list->ResourceBarrier(2, sb);
				s->list->CopyResource(back, ui);
				transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT, sb[0]);
				transition(ui, D3D12_RESOURCE_STATE_COPY_SOURCE, ui_layer::ui_state(), sb[1]);
				s->list->ResourceBarrier(2, sb);
			}
		}
		s->list->Close();

		if (!waited_for_shared)
			s->queue->Wait(s->shared_fence12.get(), written);
		s->queue->ExecuteCommandLists(1, lists);
		// FSR generates the extra frame inside Present on this queue, reading the HUD-less copy: D3D11 may only
		// overwrite the shared textures once that has run, so the fence follows Present there.
		const bool signal_late = s->fg && fg::signal_after_present();
		if (!signal_late)
			s->queue->Signal(s->shared_fence12.get(), ++s->shared_value);
		s->queue->Signal(s->frame_fence.get(), ++s->frame_fence_value);
		s->allocator_fence[index] = s->frame_fence_value;

		const bool tear = options.allow_tearing && s->tearing_supported;
		LARGE_INTEGER p0, p1;
		QueryPerformanceCounter(&p0);
		if (s->fg)
			fg::present_start();
		HRESULT hr = s->swapchain->Present(tear ? 0 : 1, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
		if (hr == DXGI_ERROR_INVALID_CALL && tear)
		{
			// a swap chain that refuses tearing in this window mode (FSR's): present with sync from now on
			log_line("bridge: present with tearing refused, using sync interval 1");
			s->tearing_supported = false;
			hr = s->swapchain->Present(1, 0);
		}
		if (s->fg)
			fg::present_end();
		QueryPerformanceCounter(&p1);
		s->last_present_call_ms = static_cast<float>(static_cast<double>(p1.QuadPart - p0.QuadPart) * 1000.0 / static_cast<double>(t_freq.QuadPart));
		if (signal_late)
			s->queue->Signal(s->shared_fence12.get(), ++s->shared_value);
		if (FAILED(hr))
		{
			log_line("bridge: Present failed (0x%08X), shutting down", static_cast<unsigned>(hr));
			shutdown();
			return;
		}

		++s->frames;
		LARGE_INTEGER t_end;
		QueryPerformanceCounter(&t_end);
		const float cpu_ms = static_cast<float>(static_cast<double>(t_end.QuadPart - t_start.QuadPart) * 1000.0 / static_cast<double>(t_freq.QuadPart));
		s->cpu_ms = s->cpu_ms * 0.9f + cpu_ms * 0.1f;
		s->wait_ms = s->wait_ms * 0.9f + wait_ms * 0.1f;
		s->last_cpu_ms = cpu_ms;
		s->last_wait_ms = wait_ms;
		set_status("active, %ux%u, %llu frames, %s", s->width, s->height, static_cast<unsigned long long>(s->frames),
			tear ? "tearing/VRR" : "vsync");
	}
}
