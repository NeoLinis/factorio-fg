// AMD FSR Frame Generation for the D3D12 bridge, through the FSR API (FidelityFX SDK 2.3).
//
// Loads amd_fidelityfx_loader_dx12.dll (and through it amd_fidelityfx_framegeneration_dx12.dll) from bin\x64. The API
// picks the version for the GPU: ML frame generation 4.x on Radeon RX 9000, the analytical 3.1.x everywhere else
// (other AMD GPUs, NVIDIA, Intel; Shader Model 6.2). FSR creates the bridge's swap chain itself (a proxy swap chain
// that inserts the generated frame and paces presentation); frame generation runs inside its Present.

#pragma once

#include <d3d12.h>
#include <dxgi1_5.h>
#include <cstdint>

namespace fg_fsr
{
	using log_fn = void (*)(const char *message);

	// Process-wide: finds and loads the loader DLL once. True if the FSR API is usable.
	bool load(log_fn log);
	// Remembers the device and frame format; the contexts are created with the swap chain.
	bool attach(ID3D12Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format);
	bool active();

	// Creates the FSR frame generation swap chain for the window and the frame generation context. 'out' receives a
	// reference the caller owns.
	bool create_swapchain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 &desc, IDXGIFactory *factory, ID3D12CommandQueue *queue, IDXGISwapChain4 **out);
	// Destroys the frame generation and swap chain contexts. Call before the swap chain's last reference is released.
	void release_contexts();

	// Per frame, on the presenting thread, into the list that is executed before Present. 'motion_vectors' pixels,
	// current -> previous (UAV state), 'depth_layers' R32_FLOAT (UAV state), 'hudless' (COMMON state, unchanged until
	// Present has returned and the queue passed it). 'ui': the UI layer (ui_layer.h, its read state), with 'clean' (or
	// null: 'hudless') the frame FSR interpolates, copied into 'back_buffer' (PRESENT state); the swap chain lays the UI
	// layer over every presented frame. 'ui' null: the back buffer keeps what it holds and FSR detects the UI itself.
	void before_present(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *hudless, ID3D12Resource *ui,
		ID3D12Resource *clean, ID3D12Resource *motion_vectors, ID3D12Resource *depth_layers, uint32_t width, uint32_t height, int multiplier);
	// After the swap chain's buffers were resized: the frame generation context is recreated for the new size.
	bool resize(uint32_t width, uint32_t height);
	void reset_history();
	void set_off();
	void detach();
	// FSR's own debug view (only generated frames are shown, with its internal surfaces drawn over them)
	void set_debug_view(bool on);
	// Diagnostic: present only the generated frames (each one twice as long), to look at them in screenshots.
	void set_only_generated(bool on);
	bool ui_separated();

	const char *status();
	const char *depth_status();
	const char *version_name();       // "4.0.1", "3.1.6", ... once the context exists
	uint32_t frames_per_present();    // 2 while generating, 1 otherwise, 0 if inactive
}
