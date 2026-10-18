// Image-based motion vectors for DLSS-G (step 3b).
//
// Factorio computes sprite positions on the CPU, so there is no camera matrix to read. Instead motion is estimated
// from consecutive HUD-less frames on the GPU:
//   1. luma at 1/2, 1/4 and 1/8 resolution
//   2. the camera: pan, plus zoom about the screen centre or the mouse (prev = Z + a * (p - Z) + t), in levels: at 1/8 resolution every shift within +-128 px around zero and around
//      last frame's camera motion (panning speed changes little between frames, so fast pans of several hundred px
//      per frame are found), then +-12 px around that at 1/4 resolution, then +-3 px at full resolution with a sub-pixel fit
//   3. per 16x16 block: the camera shift by default; zero, last frame's motion of this block and its neighbours,
//      and a +-1 half-res pixel refinement only if they match clearly better and stay inside the previous frame.
//      Blocks with no real match (revealed or new content) take the camera. Screen-locked things (the character)
//      pick zero. Changing kind (camera / still / own motion) from last frame costs extra (hysteresis: less shimmer).
//   4. 3x3 vector median over blocks
//   5. per 2x2 full-res pixels near block borders, the better of the own and neighbouring blocks' motion;
//      written as a full-resolution R16G16_FLOAT texture in pixels, pointing from the current pixel to its
//      position in the previous frame (Streamline's convention).

#pragma once

#include <d3d12.h>
#include <cstdint>

namespace motion
{
	using log_fn = void (*)(const char *message);

	struct settings
	{
		bool enabled = true;
		bool flip_sign = false;
		float penalty_zero = 0.1f; // how much better "no motion" must match than the camera (SAD units)
		float mismatch = 1.0f;     // block SAD above this counts as "no match": use the camera
		float hysteresis = 0.05f;  // extra cost for a block to change kind from last frame
		float cursor_x = -1, cursor_y = -1; // mouse in full-res px (zoom centre candidate), negative if unknown
		float strictness = 0.5f;   // new own motion must cut the camera's SAD by this fraction (filters animations)
		bool debug_view = false;   // write the motion debug picture (debug_texture())
		float far_penalty = 0.1f;  // own motion: extra SAD cost per half-res px away from the camera (repeating patterns)
		float still_zone = 0.25f;  // "still" (zero motion) only within this radius of the screen centre, in screen heights
		bool mover_search = true;   // search each block for its own motion (trains, cars, bots), not only follow it
		bool world_layers = true;   // use the ground / entity snapshots when available (bare ground = camera, exactly)
		uint32_t debug_layer_view = 0; // debug view: 0 motion kinds, 1 world layers (= depth), 2 ground, 3 floor, 4 entity, 5 world,
		                               // 6 air split snapshot
		bool icons_first = true;    // alt-mode icons are drawn before the air split (flying robots after it)
		bool wide_search = true;    // look up to +-192 px for fast movers where the normal mover search finds nothing
		bool spread_depth = false;  // depth for FSR: same layer order, values spread far apart in distance
		bool hold_as_ground = false; // the held character is painted over in the frame generation input: give its pixels
		                             // the camera's motion and the ground's depth (what is there now is the world)
	};

	// Starts compiling the shaders on a worker thread (or loads them from the cache). Optional; init() does it too.
	void prewarm();
	// Shader compile progress (any thread): compiling (or not started), ready, failed.
	enum class shader_state { compiling, ready, failed };
	shader_state shaders(int &compiled, int &total);
	bool init(ID3D12Device *device, uint32_t width, uint32_t height, DXGI_FORMAT color_format, log_fn log);
	bool resize(uint32_t width, uint32_t height);
	void shutdown();
	bool active();

	// Records the motion passes. 'hudless' must be in D3D12_RESOURCE_STATE_COMMON and is returned in that state.
	// Returns the motion vector texture (D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pixels), or nullptr if inactive.
	// World layer snapshots of Factorio's world picture (D3D12_RESOURCE_STATE_COMMON, returned so); any null = not
	// available this frame.
	struct layer_set
	{
		ID3D12Resource *ground = nullptr;    // after the tile pass
		ID3D12Resource *floor = nullptr;     // after the floor stage (belts, pipes, shadows)
		ID3D12Resource *entities = nullptr;  // before the cloud pass (all objects)
		ID3D12Resource *postcloud = nullptr; // right after the cloud pass
		ID3D12Resource *world = nullptr;     // complete, before the composite pass (air stage: bots, icons)
		ID3D12Resource *airsplit = nullptr;  // in the air stage, before the first draw of the split shader
	};
	// 'split' (optional): called once the scene-cut metric is on its way to the CPU (current_difference() valid after
	// that part completes); it must close, submit and reset 'list'. Recording then continues on 'list'.
	using split_fn = void (*)(void *context);
	ID3D12Resource *record(ID3D12GraphicsCommandList *list, ID3D12Resource *hudless, const layer_set &layers, const settings &options,
		split_fn split = nullptr, void *split_context = nullptr);
	// GPU time per stage (smoothed): camera solve (with luma and downsampling), block search, smoothing / expand / rest.
	void gpu_breakdown(float &camera, float &blocks, float &rest);
	// Blocks given the mover search / the wide search last frame, and movers found.
	void search_counts(uint32_t &searched, uint32_t &wide, uint32_t &movers);
	bool layers_used(); // the last frame used the world layers

	// After the GPU has finished the frame just recorded: mean luma difference (0..1) between it and the previous
	// frame at the best camera shift. A jump means a scene cut. False if not available.
	bool current_difference(float &mean_difference);
	// After a scene cut: this frame's camera and block motion must not seed the next frame.
	void invalidate_history();
	// GPU time of the motion passes (a few frames old), in ms. Needs the queue's timestamp frequency.
	void set_timestamp_frequency(uint64_t ticks_per_second);
	float gpu_ms();

	// Camera shift of a recent frame in full-res pixels and the match confidence (0..1), for the overlay.
	void last_global(float &dx, float &dy, float &confidence);
	// Zoom of a recent frame: how much bigger things got since the previous frame (1 = no zoom, > 1 zooming in).
	float last_zoom();
	// Depth from the world layers (R32_FLOAT, UNORDERED_ACCESS state, 0.25 placed on the ground / 0.75 bare ground;
	// flat 0.75 without layers), or nullptr.
	ID3D12Resource *depth_texture();
	// R8_UNORM, full res, UAV state: 1 where the character is held still while the camera moves (FSR keeps it sharp)
	ID3D12Resource *hold_texture();
	// Motion debug picture of the frame just recorded (UNORDERED_ACCESS state, colour format), or nullptr.
	ID3D12Resource *debug_texture();
	// Share of blocks (0..1) moving with the camera / still / with their own motion, a few frames old.
	void block_shares(float &camera, float &still, float &own);
	const char *status();
}
