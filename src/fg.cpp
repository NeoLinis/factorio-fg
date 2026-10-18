// Frame generation backend selection and dispatch. See fg.h.

#include "fg.h"
#include "fg_fsr.h"
#include "fg_streamline.h"
#include "ui_layer.h"

#include <cstdarg>
#include <cstdio>

namespace
{
	fg::choice g_choice = fg::choice::automatic;
	fg::backend g_current = fg::backend::none;
	fg::log_fn g_log = nullptr;
	char g_status[160] = "frame generation is off";

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

	bool start_dlssg(ID3D12Device *device, const LUID &adapter, uint32_t width, uint32_t height, IDXGIFactory4 **proxy_factory,
		ID3D12Device **proxy_device)
	{
		if (!fg_sl::restartable())
		{
			log_line("fg: DLSS-G was shut down earlier in this session; restart Factorio to use it again");
			return false;
		}
		if (!fg_sl::load(g_log) || !fg_sl::begin())
			return false;
		if (!fg_sl::attach(device, adapter, width, height))
		{
			fg_sl::end();
			return false;
		}
		IDXGIFactory4 *factory = nullptr;
		ID3D12Device *proxy = device;
		proxy->AddRef();
		if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && fg_sl::upgrade(factory) && fg_sl::upgrade(proxy))
		{
			*proxy_factory = factory;
			*proxy_device = proxy;
			return true;
		}
		log_line("fg: Streamline interface upgrade failed");
		if (factory != nullptr)
			factory->Release();
		proxy->Release();
		fg_sl::detach();
		fg_sl::end();
		return false;
	}

	bool start_fsr(ID3D12Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format)
	{
		return fg_fsr::load(g_log) && fg_fsr::attach(device, width, height, format);
	}
}

namespace fg
{
	void request(choice c)
	{
		g_choice = c;
	}

	choice requested()
	{
		return g_choice;
	}

	backend current()
	{
		return g_current;
	}

	const char *name(backend b)
	{
		return b == backend::dlssg ? "DLSS-G" : (b == backend::fsr ? "FSR" : "none");
	}

	bool dlssg_restartable()
	{
		return fg_sl::restartable();
	}

	backend start(ID3D12Device *device, const LUID &adapter, uint32_t vendor_id, uint32_t width, uint32_t height, DXGI_FORMAT format,
		log_fn log, IDXGIFactory4 **proxy_factory, ID3D12Device **proxy_device)
	{
		g_log = log;
		*proxy_factory = nullptr;
		*proxy_device = nullptr;
		g_current = backend::none;
		// Order to try: the user's choice first, then the other one. Automatic: DLSS-G on NVIDIA, FSR elsewhere.
		const bool nvidia = vendor_id == 0x10DE;
		backend order[2];
		if (g_choice == choice::fsr || (g_choice == choice::automatic && !nvidia))
		{
			order[0] = backend::fsr;
			order[1] = backend::dlssg;
		}
		else
		{
			order[0] = backend::dlssg;
			order[1] = backend::fsr;
		}
		for (const backend b : order)
		{
			if (b == backend::dlssg && !nvidia)
				continue; // DLSS-G needs an NVIDIA RTX GPU
			const bool ok = b == backend::dlssg ? start_dlssg(device, adapter, width, height, proxy_factory, proxy_device)
				: start_fsr(device, width, height, format);
			if (ok)
			{
				g_current = b;
				if (b != order[0])
					log_line("fg: %s not available, using %s", name(order[0]), name(b));
				log_line("fg: frame generation backend %s", name(b));
				return b;
			}
		}
		log_line("fg: no frame generation backend available (DLSS-G: Streamline in bin\\x64\\fg\\ on NVIDIA RTX; FSR: AMD FSR DLLs in bin\\x64)");
		std::snprintf(g_status, sizeof(g_status), "no frame generation: neither DLSS-G nor FSR could start (see ReShade.log)");
		return backend::none;
	}

	bool active()
	{
		return g_current == backend::dlssg ? fg_sl::active() : (g_current == backend::fsr ? fg_fsr::active() : false);
	}

	bool creates_swapchain()
	{
		return g_current == backend::fsr;
	}

	bool create_swapchain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 &desc, IDXGIFactory *factory, ID3D12CommandQueue *queue, IDXGISwapChain4 **out)
	{
		return g_current == backend::fsr && fg_fsr::create_swapchain(hwnd, desc, factory, queue, out);
	}

	void resized(uint32_t width, uint32_t height)
	{
		if (g_current == backend::fsr)
			fg_fsr::resize(width, height);
	}

	void before_swapchain_release()
	{
		if (g_current == backend::fsr)
			fg_fsr::release_contexts();
	}

	bool signal_after_present()
	{
		return g_current == backend::fsr;
	}

	void frame_start()
	{
		if (g_current == backend::dlssg)
			fg_sl::frame_start();
	}

	void before_present(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *hudless, ID3D12Resource *ui,
		ID3D12Resource *clean, ID3D12Resource *motion_vectors, ID3D12Resource *depth_layers, uint32_t width, uint32_t height, int multiplier)
	{
		if (g_current == backend::dlssg)
			fg_sl::before_present(list, hudless, ui, ui_layer::ui_state(), clean, ui_layer::clean_state(), motion_vectors, depth_layers, width, height,
				multiplier);
		else if (g_current == backend::fsr)
			fg_fsr::before_present(list, back_buffer, hudless, ui, clean, motion_vectors, depth_layers, width, height, multiplier);
	}

	void present_start()
	{
		if (g_current == backend::dlssg)
			fg_sl::present_start();
	}

	void present_end()
	{
		if (g_current == backend::dlssg)
			fg_sl::present_end();
	}

	void reset_history()
	{
		if (g_current == backend::dlssg)
			fg_sl::reset_history();
		else if (g_current == backend::fsr)
			fg_fsr::reset_history();
	}

	void set_off()
	{
		if (g_current == backend::dlssg)
			fg_sl::set_off();
		else if (g_current == backend::fsr)
			fg_fsr::set_off();
	}

	void set_debug_view(bool on)
	{
		fg_fsr::set_debug_view(on);
	}

	void set_only_generated(bool on)
	{
		fg_fsr::set_only_generated(on);
	}

	void detach()
	{
		if (g_current == backend::dlssg)
			fg_sl::detach();
		else if (g_current == backend::fsr)
			fg_fsr::detach();
	}

	void end()
	{
		if (g_current == backend::dlssg)
			fg_sl::end();
		g_current = backend::none;
		std::snprintf(g_status, sizeof(g_status), "frame generation is off");
	}

	const char *status()
	{
		return g_current == backend::dlssg ? fg_sl::status() : (g_current == backend::fsr ? fg_fsr::status() : g_status);
	}

	const char *depth_status()
	{
		return g_current == backend::dlssg ? fg_sl::depth_status() : (g_current == backend::fsr ? fg_fsr::depth_status() : "frame generation is off");
	}

	uint32_t frames_per_present()
	{
		return g_current == backend::dlssg ? fg_sl::frames_per_present() : (g_current == backend::fsr ? fg_fsr::frames_per_present() : 0);
	}

	uint32_t max_multiplier()
	{
		return g_current == backend::dlssg ? fg_sl::max_multiplier() : (g_current == backend::fsr ? 2u : 1u);
	}
}
