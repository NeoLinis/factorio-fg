// Exact character mask from Factorio's own sprites (1.2).
//
// Factorio positions every sprite on the CPU and draws it as a screen-pixel quad from one dynamic vertex buffer
// (layout: float2 position, float2 texture coordinate, RGBA8 colour, uint; stride 24; a static quad index buffer).
// The sprite probe (sprite_probe.h) showed what the character is made of:
//   - on the world target, objects stage (running) or air stage (flying): the body (colour FFFFFFFF), the player-colour
//     mask (the player's colour at half alpha) and, flying, an additive glow (colour alpha 0);
//   - its shadow on a separate screen-sized target drawn before the world's tiles (black sprites, blended in later).
// Animation frames are trimmed to their visible pixels, so their size and corner change a little from frame to frame,
// but they stay put on screen while the camera moves.
//
// Each frame this module records the sprite draws of the world (objects and air stages) and of the other screen-sized
// targets and copies their vertices before Factorio reuses the buffer: at its next no-overwrite map (the data is still
// there), right at the draw when the ring is nearly full (the next map may discard it: zoomed far out that happens
// several times a frame), or at present. Then it keeps the sprites near the screen centre. The character is found without guessing:
//   - its player-colour mask: the sprite near the centre tinted with a colour at about half alpha (the player's colour);
//   - its body: drawn right before the mask, over the same place; flying, an additive glow right after it;
//   - its shadow: a black sprite on the shadow target next to the body that follows the camera (picked last frame at
//     the same place while the camera moved), carried on while standing, and remembered by its texture region.
// The body, mask and shadow are drawn with their own textures into the mask: R = the character (texture alpha), G = its
// shadow (red channel for textures without alpha). Optionally the mask is drawn over the frame as a red / blue overlay.
//
// All functions are called with the add-on's lock held. D3D11 work uses the native device and immediate context; the
// state of the context is saved and restored around it.

#pragma once

#include <reshade_api.hpp>
#include <d3d11.h>
#include <cstdint>
#include "sprite_probe.h"

namespace sprite_mask
{
	using log_fn = void (*)(const char *message);

	void set_log(log_fn log);
	void set_enabled(bool on);
	bool enabled();

	void on_init_resource(const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial, reshade::api::resource res);
	void on_destroy_resource(reshade::api::resource res);
	void on_init_pipeline(uint32_t count, const reshade::api::pipeline_subobject *subobjects, reshade::api::pipeline pipe);
	void on_destroy_pipeline(reshade::api::pipeline pipe);
	void on_bind_input_layout(reshade::api::pipeline pipe);
	void on_bind_vertex_buffers(uint32_t first, uint32_t count, const reshade::api::resource *buffers, const uint64_t *offsets, const uint32_t *strides);
	void on_bind_index_buffer(reshade::api::resource buffer, uint64_t offset, uint32_t index_size);
	void on_bind_ps_view(uint32_t slot, uint64_t view);
	void on_draw(const sprite_probe::draw_info &d);
	// Factorio maps a buffer (after the map): pending draws' vertices are copied before a discard can replace them.
	void on_map(reshade::api::resource res, reshade::api::map_access access, void *data);

	// At present, once Factorio has drawn the frame: builds this frame's mask; with 'overlay', draws it over 'back_buffer'.
	void frame_end(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, uint32_t width, uint32_t height, bool overlay);
	void shutdown();

	ID3D11Texture2D *mask();   // R8G8B8A8_UNORM, the screen's size: R the character, G its shadow, B the character flying
	                           // (and its shadow while it flies), A coverage (a quarter of it on a shadow held from an
	                           // earlier frame at an estimated place)
	                           // (null before the first frame)
	bool valid();              // the last frame_end() produced a mask
	// 1.3: the camera shift of the last frame_end() from the world sprites (this frame's top-left corners minus last
	// frame's, full-res px, for sprites whose texture region and size appear once in both). 'exact': most of them agree
	// within 0.05 px (Factorio's own float positions), so this is the camera's shift itself. Unknown while zooming
	// (sizes change) or with too few sprites.
	struct camera_info
	{
		bool known = false, exact = false; float dx = 0, dy = 0; int support = 0, tight = 0;
		// 1.3 beta 5: per world stage (1 floor, 2 objects, 3 air: alt-mode icons, bots), how its sprites moved relative to
		// the camera: the most common residual (0.25 px steps), its share, the share that moved exactly with the camera,
		// and how many sprites were matched
		struct stage_shift { int n = 0; float dx = 0, dy = 0, share = 0, share_zero = 0; };
		stage_shift stage[4];
	};
	camera_info camera();
	// 1.3 beta 6: measure the per-stage shifts (camera_info::stage) - for the icon option and the camera trace only.
	void set_stage_shifts(bool on);
	const char *status();
	// CPU cost of the last finished frame on Factorio's render thread (for the stutter readout).
	struct frame_costs
	{
		float draw_ms = 0;     // while Factorio drew: recording draws, copying vertices (incl. our own maps)
		float end_ms = 0;      // at present: the rest of the copies, reading the sprites, the pick, drawing the mask
		float dump_ms = 0;     // writing a sprite_mask_N.txt
		uint32_t draws = 0, early = 0, lost = 0, own_maps = 0, sprites = 0;
		float vertex_kb = 0;
		bool copy_all = false, dumped = false;
	};
	frame_costs last_costs();
	// One line for the on-screen readout: this frame's character / shadow state and how often the shadow was held or
	// missing in the last ~10 s.
	void readout(char *buf, size_t size);

	// Diagnostics: where to write sprite_mask_N.txt (written automatically when the character or its shadow is lost, at most 18 per
	// session), and a request to write one for the next frame.
	void set_dump_dir(const wchar_t *dir);
	void dump_now();
}
