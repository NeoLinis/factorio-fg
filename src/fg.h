// Frame generation backend for the D3D12 bridge: NVIDIA DLSS-G (Streamline, fg_streamline) or AMD FSR Frame
// Generation (FSR API, fg_fsr). The bridge only talks to this interface; which backend runs is decided when the
// bridge starts, from the user's choice and what is installed and supported.

#pragma once

#include <d3d12.h>
#include <dxgi1_5.h>
#include <cstdint>

namespace fg
{
	using log_fn = void (*)(const char *message);

	enum class backend { none, dlssg, fsr };
	enum class choice { automatic = 0, dlssg = 1, fsr = 2 };

	// The user's choice for the next start of the bridge (automatic: DLSS-G on NVIDIA when installed, else FSR).
	void request(choice c);
	choice requested();
	backend current();                 // the backend of the running bridge, none if frame generation is unavailable
	const char *name(backend b);       // "DLSS-G", "FSR", "none"
	// Streamline cannot be started again once it was shut down in this process (switching away from DLSS-G).
	bool dlssg_restartable();

	// Bridge start, after the native D3D12 device exists and before its queue and swap chain are created. Tries the
	// requested backend, then the other one. For DLSS-G, '*proxy_factory' / '*proxy_device' receive Streamline's
	// proxies (references owned by the caller), which must be used for the queue and the swap chain.
	backend start(ID3D12Device *device, const LUID &adapter, uint32_t vendor_id, uint32_t width, uint32_t height, DXGI_FORMAT format,
		log_fn log, IDXGIFactory4 **proxy_factory, ID3D12Device **proxy_device);
	bool active();

	// FSR creates the swap chain itself.
	bool creates_swapchain();
	bool create_swapchain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 &desc, IDXGIFactory *factory, ID3D12CommandQueue *queue, IDXGISwapChain4 **out);
	// After ResizeBuffers.
	void resized(uint32_t width, uint32_t height);
	// Right before the bridge releases its swap chain (FSR destroys its contexts first).
	void before_swapchain_release();
	// FSR runs frame generation inside Present on the bridge's queue: the fence that releases the shared textures to
	// D3D11 has to be signalled after Present.
	bool signal_after_present();

	// Per frame, on the presenting thread.
	void frame_start();
	// 'ui': the UI layer (ui_layer.h: GUI and held character from the real frame), null when not available; 'clean':
	// the frame to interpolate instead of the HUD-less one (character painted over), or null. 'back_buffer' (PRESENT
	// state) holds the game frame (FSR replaces it with the frame it interpolates).
	void before_present(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *hudless, ID3D12Resource *ui,
		ID3D12Resource *clean, ID3D12Resource *motion_vectors, ID3D12Resource *depth_layers, uint32_t width, uint32_t height, int multiplier);
	void present_start();
	void present_end();
	void reset_history();
	void set_off();
	// FSR only: its own debug view
	void set_debug_view(bool on);
	void set_only_generated(bool on);

	void detach();
	void end();

	const char *status();
	// The UI layer will be laid over the generated frames (DLSS-G has not refused UI recomposition).
	bool ui_layer_usable();
	const char *depth_status();
	uint32_t frames_per_present();
	uint32_t max_multiplier();          // DLSS-G: what the system reports (up to 4); FSR: 2
}
