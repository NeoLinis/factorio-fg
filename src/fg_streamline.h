// Streamline / DLSS Frame Generation glue for the D3D12 bridge (step 2b).
//
// Loads NVIDIA Streamline (sl.interposer.dll and plugins from <game>\bin\x64\fg\) with manual hooking, and,
// if present, the dlssg_for_sm86 proxy (<game>\bin\x64\version.dll) that makes DLSS-G run on RTX 20/30.
// The bridge calls into this module at fixed points of its init, per-frame and shutdown paths.

#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace fg_sl
{
	using log_fn = void (*)(const char *message);

	// Process-wide: finds and loads the DLLs once. Returns true if Streamline is usable.
	bool load(log_fn log);

	// slInit (manual hooking). Must run before the bridge's swap chain is created. Returns false if unavailable.
	bool begin();
	// After the native D3D12 device exists: slSetD3DDevice, support check, Reflex, static inputs.
	// Returns false (and logs why) if DLSS-G cannot run; the bridge then continues without it.
	bool attach(ID3D12Device *native_device, const LUID &adapter, uint32_t width, uint32_t height);
	bool active();

	// Interface upgrades (manual hooking). Return false on failure; on success the pointer is replaced by the proxy,
	// which owns the reference that was passed in.
	bool upgrade(IDXGIFactory4 *&factory);
	bool upgrade(ID3D12Device *&device);

	// Per frame, on the presenting thread.
	void frame_start();                                   // first draw of a Factorio frame
	// 'motion_vectors': pixels, current -> previous frame, in D3D12_RESOURCE_STATE_UNORDERED_ACCESS; null = zero motion.
	// 'depth_layers': R32_FLOAT in D3D12_RESOURCE_STATE_UNORDERED_ACCESS; null = flat depth.
	// 'ui' (with its state): the UI layer (ui_layer.h), recomposed by DLSS-G over generated frames; 'clean' (null: the
	// HUD-less frame) is the frame DLSS-G interpolates then, the character painted over.
	void before_present(ID3D12GraphicsCommandList *list,  // open list that copies the frame into the back buffer
		ID3D12Resource *hudless, ID3D12Resource *ui, D3D12_RESOURCE_STATES ui_state, ID3D12Resource *clean, D3D12_RESOURCE_STATES clean_state,
		ID3D12Resource *motion_vectors, ID3D12Resource *depth_layers, uint32_t width, uint32_t height, int multiplier);
	void present_start();
	void present_end();
	void reset_history();                                 // after a resize or anything that breaks frame continuity
	void set_off();                                       // DLSS-G mode off (before resize / shutdown)

	// Before the bridge releases its swap chain and devices.
	void detach();
	// slShutdown. Call after the Streamline proxies were released, before the native device is released.
	void end();

	const char *status();
	bool ui_supported(); // false once DLSS-G refused UI recomposition (no UI layer from then on)
	// Frames shown per Factorio frame at the last state poll (1 = no generated frames), 0 if unknown.
	uint32_t frames_per_present();
	const char *depth_status(); // which depth was tagged and whether Streamline accepted it
	// Largest multiplier the system reports (4 until known).
	uint32_t max_multiplier();
	// False once slShutdown ran: Streamline cannot be initialised again in the same process.
	bool restartable();
}
