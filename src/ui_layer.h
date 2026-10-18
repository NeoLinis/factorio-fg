// The UI layer for frame generation, shared by both technologies (DLSS-G and FSR).
//
// Generated frames should never move the GUI or the character with the world. This pass writes:
// - the UI layer (R8G8B8A8_UNORM, alpha 1 where it applies): Factorio's GUI (where the final frame differs from the
//   HUD-less copy) and the character held still while the camera moves (the motion pass's hold mask, grown by 2 px),
//   both from the real frame. FSR's swap chain lays it over every presented frame; DLSS-G recomposes it
//   (UI color and alpha, with enableUserInterfaceRecomposition).
// - optionally the "clean" frame: the HUD-less frame with the held character painted over by what is under it
//   (world snapshots taken before it is drawn), so frame generation blends only the world around the character.

#pragma once

#include <d3d12.h>
#include <cstdint>

namespace ui_layer
{
	using log_fn = void (*)(const char *message);

	struct inputs
	{
		ID3D12Resource *final_frame = nullptr; // Factorio's frame with the GUI (COMMON state)
		ID3D12Resource *hudless = nullptr;     // the HUD-less copy (COMMON)
		ID3D12Resource *hold = nullptr;        // R8_UNORM hold mask (UNORDERED_ACCESS), null: no character hold
		ID3D12Resource *floor = nullptr;       // floor snapshot (COMMON), for painting the character over on the ground
		ID3D12Resource *objects = nullptr;     // objects snapshot (COMMON), ... in flight
		bool inpaint = false;                  // paint the held character over in the clean frame
	};

	// Once per device; 'format' is the frame format (typed, non-sRGB).
	bool init(ID3D12Device *device, DXGI_FORMAT format, log_fn log);
	bool resize(uint32_t width, uint32_t height);
	void shutdown();

	// Records the pass into 'list'. The inputs are returned in the states they came in. Afterwards ui_texture() is in
	// ui_state(), and clean_texture() (null if not painted this frame) in clean_state().
	bool record(ID3D12GraphicsCommandList *list, const inputs &in);
	ID3D12Resource *ui_texture();
	D3D12_RESOURCE_STATES ui_state();       // NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE
	ID3D12Resource *clean_texture();
	D3D12_RESOURCE_STATES clean_state();    // ... | COPY_SOURCE

	float coverage();     // share of the screen in the UI layer (two frames old), -1 if unknown
	bool inpainted();     // the last record() painted the character over
}
