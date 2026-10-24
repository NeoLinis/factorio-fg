// D3D11 -> D3D12 present bridge (step 2a).
//
// Factorio renders with D3D11; DLSS Frame Generation only runs on D3D12. The bridge owns a D3D12 device on the
// same GPU, a click-through child window covering Factorio's client area, and a D3D12 flip-model swap chain on
// that child window. Every Factorio present, the final frame and the HUD-less copy are copied into textures
// shared between the two APIs (synchronised with a shared fence) and presented from D3D12.
// Step 2b plugs Streamline DLSS-G into the D3D12 side.

#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>

namespace bridge
{
	using log_fn = void (*)(const char *message);

	struct present_options
	{
		bool allow_tearing = true; // present with sync interval 0 + tearing when supported (VRR displays)
		int fg_multiplier = 2;     // DLSS-G output multiplier: 1 = off, 2..4 (clamped to what the system supports)
		bool motion_vectors = true; // image-based motion vectors for DLSS-G (otherwise zero motion)
		bool motion_flip = false;   // flip their sign (diagnostic)
		float motion_penalty_zero = 0.1f; // see motion::settings
		float motion_mismatch = 1.0f;
		float motion_hysteresis = 0.05f;
		float motion_strictness = 0.5f;  // see motion::settings
		bool motion_debug_view = false;
		bool fg_debug_view = false;  // FSR's own debug view
		bool fg_show_ui_layer = false; // FSR: show the extracted GUI layer (diagnostic)
		bool fg_hold_character = true; // FSR: the held character from the newest real frame (no blended poses)
		bool fg_inpaint_character = false; // FSR: and painted over in what FSR interpolates (ground only)
		bool fg_only_generated = false;    // FSR: present only the generated frames (diagnostic)
		bool fg_shadow_overlay = true;     // 1.2, exact mask: the character's shadow laid over as a darkening (the ground
		                                   // under it moves with the camera) once its darkness is measured
		float motion_far_penalty = 0.1f;
		float motion_still_zone = 0.25f;
		bool motion_mover_search = true;
		bool motion_world_layers = true;
		uint32_t motion_debug_layer_view = 0;
		bool motion_icons_first = true;
		bool motion_wide_search = true;
		bool motion_floor_camera = true;      // 1.2: belts and other floor move with the world (see motion::settings)
		bool motion_sprite_camera = true;     // 1.3: use the camera shift from the sprites when it is exact
		bool motion_icon_depth_flat = true;   // 1.3 beta 3: alt-mode icons at the depth of the building under them
		bool motion_fit_measure = true;       // 1.3 beta 4: run the motion fit pass
		int sprite_camera_support = 0, sprite_camera_tight = 0; // diagnostics (camera trace)
		bool motion_icon_motion = true;       // 1.3 beta 5: alt-mode icons moved by their own sprites' shift
		bool icon_shift_valid = false;
		float icon_dx = 0, icon_dy = 0;
		float stage_diag[12] = {};
		bool sprite_camera_valid = false;     // ... this frame's: known exactly
		float sprite_camera_dx = 0, sprite_camera_dy = 0; // ... world sprites' shift since the last frame, full-res px
		bool scene_cut_detection = true;      // tell DLSS-G to start over when the picture changes completely
		float scene_cut_factor = 4.0f;        // cut when the frame difference exceeds this many times its running average
		float scene_cut_min_difference = 0.08f; // ...and this absolute mean luma difference (0..1); 0.04 until beta 34 cut
		                                        // on zooming, explosions and fast turns (0.04-0.09) many times a minute
	};

	struct timings
	{
		float motion_gpu_ms = 0;  // motion estimation passes on the GPU
		float motion_camera_ms = 0, motion_block_ms = 0, motion_rest_ms = 0; // ... per stage
		uint32_t blocks_searched = 0, blocks_wide = 0, movers_found = 0; // mover search work last frame
		float cut_wait_ms = 0;    // CPU wait for the scene-cut check (includes Factorio's own GPU work still in flight)
		float present_cpu_ms = 0; // total CPU time of bridge::present
		float last_cut_wait_ms = 0, last_present_cpu_ms = 0; // the same for the last frame alone (not smoothed)
		uint32_t cut_checks_late = 0, cuts_late = 0; // scene-cut checks judged a frame late (GPU busy), cuts found so
		float last_alloc_wait_ms = 0;    // the last frame: waiting for the GPU to free a command allocator (3 frames back)
		float last_present_call_ms = 0;  // ... inside the swap chain's Present (frame generation, its pacing)
		float difference = 0, difference_average = 0; // scene-cut metric, last and running average
		uint32_t scene_cuts = 0;
	};

	// Creates everything. Safe to call again after shutdown(). Returns false and logs on failure.
	bool init(ID3D11Device *device11, HWND game_window, uint32_t width, uint32_t height, DXGI_FORMAT format, log_fn log);
	void shutdown();
	bool active();

	// Called on the thread that presents, with Factorio's immediate context, right before Factorio's own Present.
	// 'final_frame' is Factorio's back buffer (with GUI), 'hudless' the probe's HUD-less copy (may be null).
	// World layer snapshots: array of LAYER_COUNT textures indexed by 'layer' (all must be set), or null.
	enum layer : uint32_t { LAYER_GROUND, LAYER_FLOOR, LAYER_ENTITIES, LAYER_POSTCLOUD, LAYER_WORLD, LAYER_AIRSPLIT, LAYER_COUNT };
	// 'layers_generation': layer_generation() when the snapshots were taken (they are ignored if it changed since).
	// 'character_mask' (1.2, may be null): the exact character mask of this frame (R8G8B8A8_UNORM, the frame's size:
	// R character, G its shadow, B flying). With it the motion pass does not guess the character.
	void present(ID3D11DeviceContext *context, ID3D11Texture2D *final_frame, ID3D11Texture2D *hudless,
		ID3D11Texture2D *const *layers, uint64_t layers_generation, ID3D11Texture2D *character_mask, const present_options &options);
	bool exact_mask_used(); // the last frame's motion pass used the exact character mask
	// The character's shadow darkness (motion::shadow_darkness): true if it is laid over as a darkening.
	struct shadow_info { float k[4]; uint32_t frames[4], samples[4]; }; // as motion::shadow_info
	bool shadow_darkness(shadow_info &out);
	// The texture to copy this frame's snapshot of 'layer' into directly (D3D11, on Factorio's device), or null when
	// the bridge is not running. Saves a full-screen copy per layer at present. Valid while layer_generation() is
	// unchanged; the next frame gets the other of two sets.
	ID3D11Texture2D *layer_target(uint32_t layer);
	uint64_t layer_generation();

	// Hides the bridge window, e.g. while the ReShade overlay is open (the overlay is drawn on Factorio's own swap chain).
	void set_visible(bool visible);
	// Frame generation off/on without tearing anything down (Streamline cannot be started twice in one process):
	// paused = window hidden, DLSS-G off, present() does nothing.
	void suspend(bool suspended);
	bool suspended();

	// Marks the start of a Factorio frame (its first draw call), for Reflex/PCL timing.
	void frame_start();

	// Short human readable status for the overlay.
	const char *status();
	// Frame generation status (why it is unavailable, or the live DLSS-G state).
	const char *fg_status();
	// Frame generation in use: "DLSS-G", "FSR" or "none", and the largest multiplier it supports.
	const char *backend_name();
	uint32_t max_multiplier();
	// Frames shown per Factorio frame (1 without frame generation), 0 if the bridge is not presenting.
	uint32_t frames_per_present();
	// Motion estimation status (camera shift and match quality).
	const char *motion_status();
	// Timing and scene-cut numbers (smoothed) for the overlay.
	timings get_timings();
	// Live numbers for the on-screen readout.
	void motion_global(float &dx, float &dy, float &confidence);
	float motion_zoom(); // 1 = no zoom, > 1 zooming in
	const char *depth_status(); // depth tagging status for the overlay
	bool layers_used();         // the last frame used the world layers
	void motion_shares(float &camera, float &still, float &own); // share of blocks per kind, 0..1
	// 1.3: where the camera shift came from and how far the picture fit is from the sprites' (readout line).
	const char *camera_line();
	// 1.3: motion fit per layer (readout line): the previous frame moved by the motion vectors vs the new one.
	const char *fit_line();
	// 1.3 beta 4: camera trace (see motion::trace_start / trace_take).
	bool camera_trace_start(uint32_t frames); // false if the motion pass is not running
	bool camera_trace_take(std::string &out);
	int camera_trace_left();                  // frames still to record
}
