// Factorio HUD-less probe (ReShade add-on)
//
// Step 1 of the Factorio DLSS-G injector project. Answers one question:
// can we reliably grab the frame right before Factorio starts drawing its GUI?
//
// What it does:
//   * Hashes every vertex/pixel shader and blend state Factorio creates.
//   * Records a per-draw trace of one frame (shader hashes, blend, render target).
//   * Lets you mark a draw as "GUI starts here" (pixel shader hash + occurrence),
//     or scrub a raw draw index, and copies the back buffer just before that draw
//     into a HUD-less texture every frame.
//   * Shows the HUD-less texture live in the ReShade overlay.
//   * F10 dumps final.bmp, hudless.bmp, ui_mask.bmp and trace.csv for the next frame.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <windows.h>
#include <d3d11.h>
#include "bridge.h"
#include "motion.h"
#include "layer_learn.h"
#include "fg.h"
#include "version.h"
#include "setup_check.h"
#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex> // std::lock_guard only
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

namespace
{
	constexpr const char *CONFIG_SECTION = "FACTORIO_HUDLESS_PROBE";
	constexpr uint32_t DUMP_KEY = VK_F10;

	enum class boundary_mode : int { off = 0, shader_rule = 1, draw_index = 2, back_buffer_draw = 3, after_anchor = 4, anchor_to_gui = 5 };

	struct draw_record
	{
		uint32_t index;
		uint32_t vs, ps, blend;
		uint64_t rt;          // render target resource handle (0 = none)
		bool rt_is_back_buffer;
		uint32_t rt_width, rt_height;
		format rt_format;
		uint32_t count, instances;
		bool indexed;
	};

	HMODULE g_module = nullptr;
	// Recursive lock (critical section) instead of std::mutex: no threading runtime dependency in the DLL, and
	// re-entrant, because ReShade calls back into this add-on from inside D3D12 calls the bridge makes
	// (e.g. the 'present' event for the bridge's own swap chain fires inside our Present call).
	struct app_mutex
	{
		CRITICAL_SECTION cs;
		app_mutex() { InitializeCriticalSectionAndSpinCount(&cs, 1000); }
		~app_mutex() { DeleteCriticalSection(&cs); }
		void lock() { EnterCriticalSection(&cs); }
		void unlock() { LeaveCriticalSection(&cs); }
	};
	app_mutex g_mutex;
	device *g_device = nullptr; // the one D3D11 device we follow; events from any other device are ignored
	bool g_config_loaded = false;

	void log_info(const char *fmt, ...)
	{
		char buf[512];
		va_list args; va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		reshade::log::message(reshade::log::level::info, buf);
	}


	// ---------------------------------------------------------------- ABI-safe D3D11 helpers
	//
	// ReShade is built with MSVC. MSVC and GCC/MinGW disagree on how a C++ member function returns a
	// struct by value (MSVC passes a hidden return pointer after 'this'), so calling ReShade virtuals such as
	// get_back_buffer(), get_resource_desc() or get_resource_from_view() from a MinGW build corrupts memory.
	// These helpers get the same information through D3D11/DXGI COM, which is ABI-safe on both compilers.
	// ReShade's D3D11 handles are the raw ID3D11Resource* / ID3D11View* pointers, so the values match.

	const GUID IID_tex2d = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

	resource native_back_buffer(swapchain *sc)
	{
		auto *const dxgi = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
		ID3D11Texture2D *tex = nullptr;
		if (dxgi == nullptr || FAILED(dxgi->GetBuffer(0, IID_tex2d, reinterpret_cast<void **>(&tex))) || tex == nullptr)
			return resource { 0 };
		tex->Release(); // the swap chain keeps its buffer alive
		return resource { reinterpret_cast<uint64_t>(static_cast<ID3D11Resource *>(tex)) };
	}

	resource native_resource_from_view(resource_view view)
	{
		if (view.handle == 0)
			return resource { 0 };
		ID3D11Resource *res = nullptr;
		reinterpret_cast<ID3D11View *>(view.handle)->GetResource(&res);
		if (res != nullptr)
			res->Release(); // the view keeps its resource alive
		return resource { reinterpret_cast<uint64_t>(res) };
	}

	resource_desc native_texture_desc(resource r)
	{
		resource_desc desc;
		if (r.handle == 0)
			return desc;
		auto *const res = reinterpret_cast<ID3D11Resource *>(r.handle);
		D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
		res->GetType(&dim);
		if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		{
			D3D11_TEXTURE2D_DESC td = {};
			static_cast<ID3D11Texture2D *>(res)->GetDesc(&td);
			desc.type = resource_type::texture_2d;
			desc.texture.width = td.Width;
			desc.texture.height = td.Height;
			desc.texture.depth_or_layers = static_cast<uint16_t>(td.ArraySize);
			desc.texture.levels = static_cast<uint16_t>(td.MipLevels);
			desc.texture.format = static_cast<format>(td.Format); // reshade::api::format mirrors DXGI_FORMAT
			desc.texture.samples = static_cast<uint16_t>(td.SampleDesc.Count);
		}
		return desc;
	}

	// Shader / state identification
	std::unordered_map<uint64_t, uint32_t> g_pipeline_hash; // pipeline handle -> FNV-1a hash
	uint32_t g_cur_vs = 0, g_cur_ps = 0, g_cur_blend = 0;
	resource g_cur_rt = { 0 };

	// Swap chain resources
	std::vector<uint64_t> g_back_buffers;
	resource g_hudless = { 0 };
	// World layers (step 8): three snapshots of the world picture (pre-lighting), from Factorio 2.0 traces:
	//   ground   after the 1st draw with 7C158CCA: the tile pass (checked in game on Nauvis and Vulcanus; the large draw
	//            after it adds decoratives - grass, rocks - and in remote view on Vulcanus even stack inserters)
	//   floor    after the full-screen pass BCD79626 (belts, pipes, inserters, floor entities, all shadows are in)
	//   entities right before the full-screen pass 703619E0 (clouds); alt-mode icons and lighting come after
	constexpr uint32_t LAYER_GROUND_AFTER_PS = 0x7C158CCA;
	constexpr int LAYER_GROUND_AFTER_OCC = 1;
	constexpr uint32_t LAYER_FLOOR_AFTER_PS = 0xBCD79626;
	constexpr uint32_t LAYER_ENTITIES_BEFORE_PS = 0x703619E0;
	// air stage: right after the cloud pass, and the finished world right before the composite pass (575CE4FC)
	constexpr uint32_t LAYER_WORLD_BEFORE_PS = 0x575CE4FC;
	resource g_layer_ground = { 0 }, g_layer_floor = { 0 }, g_layer_entities = { 0 }, g_layer_postcloud = { 0 }, g_layer_world = { 0 };
	// The air stage split in two: snapshot right before the first draw with this pixel shader (none that frame: the
	// complete world). Alt-mode icons on one side get the camera motion, flying robots on the other the motion search.
	constexpr uint32_t DEFAULT_AIR_SPLIT_PS = 0xD82AC825;
	resource g_layer_airsplit = { 0 };
	bool g_layer_airsplit_done = false;
	uint32_t g_air_split_ps = DEFAULT_AIR_SPLIT_PS;
	bool g_air_icons_first = true;     // icons are drawn before the split
	bool g_wide_search = true;         // wide mover search (fast trains)
	// pixel shaders drawn on the world target in the air stage, with counts: this frame (building) and last frame
	struct air_draw { uint32_t ps; int count; };
	constexpr int AIR_DRAW_SLOTS = 12;
	air_draw g_air_draws[AIR_DRAW_SLOTS] = {}, g_air_draws_last[AIR_DRAW_SLOTS] = {};
	int g_air_draw_count = 0, g_air_draw_count_last = 0;
	constexpr int AIR_SEEN_SLOTS = 16;
	air_draw g_air_seen[AIR_SEEN_SLOTS] = {}; // every air stage shader seen so far (settings list)
	int g_air_seen_count = 0;
	bool g_layer_floor_pending = false, g_layer_floor_done = false;
	bool g_layer_postcloud_pending = false, g_layer_postcloud_done = false, g_layer_world_done = false;
	resource g_layer_rt = { 0 };       // the world target the ground pass drew into
	// direct capture into the bridge's shared textures (see choose_layer_targets)
	uint32_t g_layer_fallback = 0, g_layer_fallback_last = 0; // bit 0 no floor pass, bit 1 no cloud pass (this / last frame)
	uint32_t g_layer_found_last = 0; // snapshots taken last frame: bit per bridge::layer
	bool g_layers_direct = false, g_layers_direct_last = false;
	uint64_t g_layers_gen = 0, g_layers_gen_last = 0;
	resource g_layer_dst[bridge::LAYER_COUNT] = {};
	bool g_layer_ground_pending = false, g_layer_ground_done = false, g_layer_entities_done = false;
	bool g_layers_last_frame = false;  // both snapshots were taken last frame
	bool g_world_layers = true;        // use them
	uint64_t g_layer_frames = 0;
	// Ground snapshot point: after occurrence N of this pixel shader on the world target. Must come after the tiles and
	// before belts, floor entities and the shadow layer (those can move: belt items, the character's shadow).
	uint32_t g_layer_ground_ps = LAYER_GROUND_AFTER_PS;
	int g_layer_ground_occ = LAYER_GROUND_AFTER_OCC;
	int g_layer_ground_seen = 0;
	// Snapshot points in use: the built-in ones above, or - when a Factorio update has changed those shaders - the
	// ones read off the frame's structure (layer_learn.h). Set at the start of each frame.
	uint32_t g_act_ground_ps = LAYER_GROUND_AFTER_PS, g_act_floor_ps = LAYER_FLOOR_AFTER_PS, g_act_cloud_ps = LAYER_ENTITIES_BEFORE_PS,
		g_act_world_ps = LAYER_WORLD_BEFORE_PS;
	int g_act_ground_occ = LAYER_GROUND_AFTER_OCC;
	bool g_act_has_floor = true, g_act_has_cloud = true;
	std::vector<layer_learn::draw> g_learn_draws;   // this frame's draws (every LEARN_EVERY-th frame)
	constexpr uint64_t LEARN_EVERY = 10;
	constexpr int LEARN_CONFIRM = 6;                 // the same result this many times in a row (about a second)
	constexpr uint64_t BUILTIN_GONE_FRAMES = 300;    // built-in shaders not seen for this long: use the learned points
	layer_learn::points g_learn_candidate, g_learned; // last analysis / confirmed
	int g_learn_streak = 0;
	bool g_builtin_ground_seen = false, g_builtin_world_seen = false; // this frame
	uint64_t g_builtin_last_seen = 0; // game frame when both were last seen
	bool g_use_learned = false;
	int g_preview_source = 0; // 0 HUD-less, 1 ground snapshot, 2 entity snapshot, 3 floor snapshot, 4 complete world
	resource_view g_hudless_srv = { 0 };

	// Overlay preview. Factorio leaves the back buffer's alpha at 0, which ImGui would draw fully transparent,
	// so the preview is a half-size copy with alpha forced to 255. It goes through a CPU ring of staging
	// textures (mapped 3 updates after the copy, so it never stalls) and only runs while the preview is open.
	constexpr uint32_t PREVIEW_RING = 3;
	resource g_preview = { 0 };
	resource_view g_preview_srv = { 0 };
	resource g_preview_staging[PREVIEW_RING] = {};
	bool g_preview_staging_valid[PREVIEW_RING] = {};
	uint32_t g_preview_slot = 0, g_preview_w = 0, g_preview_h = 0;
	uint64_t g_preview_requested_frame = 0;
	int g_preview_alpha_max = -1;
	bool g_preview_crop = false; // false: whole frame at half res; true: centre crop at 1:1
	std::vector<uint8_t> g_preview_pixels;
	resource_desc g_bb_desc;

	// Boundary rule (persisted to ReShade.ini)
	// Factorio 2.x default (found with this probe): copy right after the full-screen post pass that runs
	// into the final composite target just before the GUI. Shader hashes change if Wube recompiles shaders,
	// which is what the hash-free fallback below is for.
	constexpr uint32_t FACTORIO_DEFAULT_ANCHOR_PS = 0x575CE4FC;
	boundary_mode g_mode = boundary_mode::after_anchor;
	uint32_t g_rule_ps = 0;
	int g_rule_occurrence = 1;
	int g_rule_index = 0;
	int g_rule_bb_index = 1;
	uint32_t g_anchor_ps = FACTORIO_DEFAULT_ANCHOR_PS; // "after anchor" rule: copy right after occurrence N of this pixel shader
	int g_anchor_occurrence = 1;
	int g_anchor_extra = 1;       // also include this many draws into the anchor's target after the anchor (space dust)
	// The extra draws only apply when a space-background pass ran earlier in the frame: in space the first draw after
	// the post pass is the dust layer, on planets the same slot is the minimap background.
	constexpr uint32_t SPACE_MARKER_PS[] = { 0x80D6CA13, 0x383F8F67 };
	bool g_extra_needs_marker = true;
	bool g_marker_seen = false, g_last_marker_seen = false;
	uint32_t g_gui_ps = 0, g_gui_vs = 0, g_gui_blend = 0; // "anchor -> GUI" rule: first draw of this shader after the anchor
	bool g_gui_strict = false;                             // also require matching VS and blend state

	// Per-frame state
	uint32_t g_draw_index = 0;
	int g_rule_ps_seen = 0;
	bool g_captured_this_frame = false;
	int g_matched_at = -1;
	int g_first_bb_draw = -1;
	uint32_t g_bb_draw_index = 0;
	bool g_capture_from_offscreen = false;
	// Hash-free fallback: if the anchor never shows up, use the state after the frame's last full-screen pass
	// (3 vertices, not indexed) into an offscreen target. In Factorio that pass is the last thing before the GUI.
	bool g_auto_fallback = true;

	// Step 2a: D3D12 present bridge (opt-in)
	bool g_bridge_enabled = false;
	bool g_bridge_tearing = true;
#ifdef FG_RELEASE
	int g_fg_multiplier = 0;       // 0 = auto, 1 = frame generation off, 2..4
#else
	int g_fg_multiplier = 2;
#endif
	int g_auto_target = 0;         // auto: output fps to aim for, 0 = the display's refresh rate
	int g_auto_multiplier = 2;     // auto: current choice
	int g_auto_candidate = 2, g_auto_candidate_windows = 0;
	int g_display_hz = 0;          // refresh rate of the display Factorio is on
	HWND g_game_window = nullptr;
	bool g_motion_vectors = true;
	bool g_motion_flip = false;
	constexpr float DEFAULT_MOTION_PENALTY_ZERO = 0.1f;
	constexpr float DEFAULT_MOTION_MISMATCH = 1.0f;
	constexpr float DEFAULT_MOTION_HYSTERESIS = 0.05f;
	float g_motion_penalty_zero = DEFAULT_MOTION_PENALTY_ZERO;
	float g_motion_mismatch = DEFAULT_MOTION_MISMATCH;
	float g_motion_hysteresis = DEFAULT_MOTION_HYSTERESIS;
	constexpr float DEFAULT_MOTION_STRICTNESS = 0.5f;
	float g_motion_strictness = DEFAULT_MOTION_STRICTNESS;
	constexpr float DEFAULT_MOTION_FAR_PENALTY = 0.1f;
	float g_motion_far_penalty = DEFAULT_MOTION_FAR_PENALTY;
	constexpr float DEFAULT_MOTION_STILL_ZONE = 0.25f;
	float g_motion_still_zone = DEFAULT_MOTION_STILL_ZONE;
	bool g_still_zone_active = true; // false in map view
	bool g_mover_search = true;      // search blocks for their own motion
	int g_debug_mode = 0;            // debug view: 0 motion kinds, 1 world layers (= depth), 2 ground, 3 floor, 4 entity, 5 complete world, 6 air split snapshot; not saved
#ifdef FG_RELEASE
	bool g_dev_tools = false;        // developer tools (boundary editor, frame trace, camera probe) hidden in release builds
#else
	bool g_dev_tools = true;
#endif
	bool g_anchor_ever_matched = false;
	uint64_t g_game_frames = 0;      // Factorio frames since start
	bool g_motion_debug = false;   // not saved: a diagnostic
	bool g_fsr_debug = false;      // FSR's own debug view; not saved
	bool g_fsr_ui_layer = false;   // FSR: show the extracted GUI layer; not saved
	bool g_fsr_only_generated = false; // FSR: present only the generated frames; not saved
	bool g_fsr_hold = true;        // FSR: the character from the newest real frame in generated frames
	bool g_fsr_inpaint = false;    // and painted over (with what is under it) in what frame generation blends (option)
#ifdef FG_RELEASE
	bool g_osd = false;              // readout drawn on the bridge picture (hotkey)
#else
	bool g_osd = true;
#endif
	// Hotkey that shows / hides the readout (ReShade-style: virtual key + modifiers). Default Ctrl+F11.
	uint32_t g_osd_key = VK_F11;
	bool g_osd_key_ctrl = true, g_osd_key_shift = false, g_osd_key_alt = false;
	bool g_osd_key_capture = false;
	bool g_scene_cut = true;          // reset DLSS-G when the picture changes completely
	float g_scene_cut_factor = 4.0f;  // "Set hotkey" pressed: the next key becomes the hotkey
	constexpr const char *BUILD_STAMP = __DATE__ " " __TIME__;
	// Frame-rate readout: Factorio presents per second, measured over half-second windows
	LARGE_INTEGER g_fps_window_start = {};
	uint32_t g_fps_window_frames = 0;
	float g_base_fps = 0.0f;
	// Frame generation technology: 0 automatic (DLSS-G on NVIDIA when installed, else FSR), 1 DLSS-G, 2 FSR. Applied when
	// the bridge starts; changing it restarts the bridge.
	bool g_capture_mode_migrated = false; // load_config turned a saved "Off" capture mode into the built-in rule
	int g_fg_choice = 0;
	int g_fg_choice_applied = -1; // the choice the running bridge was started with
	bool g_bridge_failed = false;   // init failed once; do not retry every frame until toggled again
	bool g_overlay_open = false;
	bool g_auto_pending = false, g_auto_captured = false;
	resource g_auto_rt = { 0 };
	int g_auto_at = -1, g_last_auto_at = -1;
	resource g_last_screen_rt = { 0 }; // most recent screen-sized offscreen target drawn into this frame
	int g_anchor_seen = 0;
	bool g_anchor_pending = false;  // anchor draw was issued; copy before the next draw (or at present)
	int g_anchor_skip = 0;          // extra draws still to include before copying
	std::vector<std::pair<int, uint32_t>> g_extra_building, g_extra_last; // (draw, index count) of included extra draws
	resource g_anchor_rt = { 0 };
	bool g_anchor_done = false;      // anchor draw has run this frame

	// Results of the last finished frame (for the overlay)
	uint32_t g_last_draw_count = 0;
	int g_last_matched_at = -1;
	int g_last_first_bb_draw = -1;
	uint32_t g_last_bb_draw_count = 0;
	bool g_last_capture_from_offscreen = false;
	uint64_t g_frame_counter = 0;
	uint32_t g_match_streak = 0, g_miss_streak = 0;

	// Trace capture
	bool g_trace_requested = false, g_dump_requested = false;
	bool g_trace_active = false, g_dump_active = false;
	std::vector<draw_record> g_trace_building, g_trace;
	std::vector<uint32_t> g_trace_ps_uses; // per trace row: how often its pixel shader ran that frame
	std::unordered_map<uint64_t, resource_desc> g_rt_desc_cache;
	int g_selected_row = -1;
	std::string g_last_status;

	uint32_t fnv1a(const void *data, size_t size, uint32_t h = 2166136261u)
	{
		const auto *p = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i)
			h = (h ^ p[i]) * 16777619u;
		return h;
	}

	bool is_back_buffer(resource r)
	{
		return r.handle != 0 && std::find(g_back_buffers.begin(), g_back_buffers.end(), r.handle) != g_back_buffers.end();
	}

	std::filesystem::path output_dir()
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(g_module, path, MAX_PATH);
		return std::filesystem::path(path).parent_path() / L"hudless_probe";
	}

	// ---------------------------------------------------------------- config

	// ---------------------------------------------------------------- auto multiplier

	int display_refresh_hz(HWND window)
	{
		HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
		MONITORINFOEXW info = {};
		info.cbSize = sizeof(info);
		if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
			return 0;
		DEVMODEW mode = {};
		mode.dmSize = sizeof(mode);
		if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
			return 0;
		return static_cast<int>(mode.dmDisplayFrequency);
	}

	// Called every fps window (0.5 s): largest multiplier whose output stays at or below the target, changed only
	// after the same answer for 2 s, so a short fps dip does not make DLSS-G reconfigure back and forth.
	void update_auto_multiplier()
	{
		if (g_game_window != nullptr)
			g_display_hz = display_refresh_hz(g_game_window);
		const int target = g_auto_target > 0 ? g_auto_target : (g_display_hz > 0 ? g_display_hz : 60);
		if (g_base_fps < 5.0f)
			return; // loading, paused rendering: keep the current choice
		const int wanted = std::clamp(static_cast<int>(static_cast<float>(target) / g_base_fps + 0.03f), 1,
			static_cast<int>(bridge::max_multiplier())); // FSR: 2x
		if (wanted == g_auto_multiplier)
		{
			g_auto_candidate_windows = 0;
			return;
		}
		if (wanted != g_auto_candidate)
		{
			g_auto_candidate = wanted;
			g_auto_candidate_windows = 0;
		}
		if (++g_auto_candidate_windows >= 4)
		{
			log_info("HUD-less probe: auto frame generation %dx -> %dx (Factorio %.1f fps, target %d fps)", g_auto_multiplier, wanted, g_base_fps, target);
			g_auto_multiplier = wanted;
			g_auto_candidate_windows = 0;
		}
	}

	int effective_multiplier()
	{
		const int wanted = g_fg_multiplier == 0 ? g_auto_multiplier : g_fg_multiplier;
		return std::min(wanted, static_cast<int>(bridge::max_multiplier())); // FSR: 2x
	}

	void load_config()
	{
		// Default: the built-in rule. (Up to 1.1.0 beta 4 a missing key meant Off, so the GUI was never kept out of the
		// HUD-less copy on fresh installs and that Off got saved with the next setting change: undo that once.)
		int mode = static_cast<int>(boundary_mode::after_anchor);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "Mode", mode);
		bool mode_checked = false;
		reshade::get_config_value(nullptr, CONFIG_SECTION, "CaptureModeChecked", mode_checked);
		if (!mode_checked && mode == static_cast<int>(boundary_mode::off))
		{
			mode = static_cast<int>(boundary_mode::after_anchor);
			g_capture_mode_migrated = true;
		}
		g_mode = static_cast<boundary_mode>(std::clamp(mode, 0, 5));
		char hex[32] = {}; size_t len = sizeof(hex) - 1;
		if (reshade::get_config_value(nullptr, CONFIG_SECTION, "RulePixelShader", hex, &len))
			g_rule_ps = static_cast<uint32_t>(std::strtoul(hex, nullptr, 16));
		reshade::get_config_value(nullptr, CONFIG_SECTION, "RuleOccurrence", g_rule_occurrence);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "RuleDrawIndex", g_rule_index);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "RuleBackBufferDraw", g_rule_bb_index);
		len = sizeof(hex) - 1;
		if (reshade::get_config_value(nullptr, CONFIG_SECTION, "AnchorPixelShader", hex, &len))
			g_anchor_ps = static_cast<uint32_t>(std::strtoul(hex, nullptr, 16));
		reshade::get_config_value(nullptr, CONFIG_SECTION, "AnchorOccurrence", g_anchor_occurrence);
		auto read_hex = [&](const char *key, uint32_t &out) {
			char buf[32] = {}; size_t n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, key, buf, &n))
				out = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
		};
		read_hex("GuiPixelShader", g_gui_ps);
		read_hex("GuiVertexShader", g_gui_vs);
		read_hex("GuiBlend", g_gui_blend);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "GuiStrict", g_gui_strict);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "AutoFallback", g_auto_fallback);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "AnchorExtraDraws", g_anchor_extra);
		g_anchor_extra = std::clamp(g_anchor_extra, 0, 16);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "AnchorExtraNeedsSpace", g_extra_needs_marker);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "BridgeEnabled", g_bridge_enabled);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "BridgeTearing", g_bridge_tearing);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "FrameGenMultiplier", g_fg_multiplier);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionVectors", g_motion_vectors);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionVectorsFlip", g_motion_flip);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "OnScreenReadout", g_osd);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "SceneCutDetection", g_scene_cut);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "DeveloperTools", g_dev_tools);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "MoverSearch", g_mover_search);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "WorldLayers", g_world_layers);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "FrameGenerationBackend", g_fg_choice);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "FsrHoldCharacter", g_fsr_hold);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "PaintOverCharacter", g_fsr_inpaint);
		g_fg_choice = std::clamp(g_fg_choice, 0, 2);
		{
			char hexbuf[16] = {}; size_t hn = sizeof(hexbuf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "LayerGroundPixelShader", hexbuf, &hn))
				g_layer_ground_ps = static_cast<uint32_t>(std::strtoul(hexbuf, nullptr, 16));
			reshade::get_config_value(nullptr, CONFIG_SECTION, "LayerGroundOccurrence", g_layer_ground_occ);
			hn = sizeof(hexbuf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "AirSplitPixelShader", hexbuf, &hn))
				g_air_split_ps = static_cast<uint32_t>(std::strtoul(hexbuf, nullptr, 16));
			reshade::get_config_value(nullptr, CONFIG_SECTION, "AltModeIconsBeforeSplit", g_air_icons_first);
			reshade::get_config_value(nullptr, CONFIG_SECTION, "WideMoverSearch", g_wide_search);
			g_layer_ground_occ = std::max(g_layer_ground_occ, 1);
			// Earlier defaults: BCD79626 (0.10.x, that is the floor stage) and 7C158CCA #2 (0.11.0, includes decoratives)
			if (g_layer_ground_ps == LAYER_FLOOR_AFTER_PS || (g_layer_ground_ps == LAYER_GROUND_AFTER_PS && g_layer_ground_occ == 2))
			{
				g_layer_ground_ps = LAYER_GROUND_AFTER_PS;
				g_layer_ground_occ = LAYER_GROUND_AFTER_OCC;
			}
		}
		{
			char buf[32] = {}; size_t n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "SceneCutFactor", buf, &n)) g_scene_cut_factor = std::clamp(static_cast<float>(std::atof(buf)), 1.5f, 20.0f);
		}
		reshade::get_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKey", g_osd_key);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKeyCtrl", g_osd_key_ctrl);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKeyShift", g_osd_key_shift);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKeyAlt", g_osd_key_alt);
		if (g_osd_key > 0xFE)
			g_osd_key = 0;
		{
			char buf[32] = {}; size_t n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionZeroPenalty", buf, &n)) g_motion_penalty_zero = std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 2.0f);
			n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionMismatch", buf, &n)) g_motion_mismatch = std::clamp(static_cast<float>(std::atof(buf)), 0.1f, 32.0f);
			n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionStability", buf, &n)) g_motion_hysteresis = std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 2.0f);
			n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionStrictness", buf, &n)) g_motion_strictness = std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 0.95f);
			n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionRepeatGuard", buf, &n)) g_motion_far_penalty = std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 0.3f);
			n = sizeof(buf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionStillZone", buf, &n)) g_motion_still_zone = std::clamp(static_cast<float>(std::atof(buf)), 0.05f, 1.2f);
		}
		g_fg_multiplier = std::clamp(g_fg_multiplier, 0, 4);
		reshade::get_config_value(nullptr, CONFIG_SECTION, "AutoTargetFps", g_auto_target);
		g_auto_target = std::clamp(g_auto_target, 0, 1000);
		g_anchor_occurrence = std::max(g_anchor_occurrence, 1);
		g_rule_occurrence = std::max(g_rule_occurrence, 1);

		// Version tracking: ReShade.log says when a different build than last time was loaded.
		char last[32] = {};
		size_t n = sizeof(last) - 1;
		if (!reshade::get_config_value(nullptr, CONFIG_SECTION, "AddonVersion", last, &n))
			log_info("HUD-less probe: first start of version " ADDON_VERSION_STRING);
		else if (std::strcmp(last, ADDON_VERSION_STRING) != 0)
			log_info("HUD-less probe: updated from version %s to " ADDON_VERSION_STRING, last);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AddonVersion", ADDON_VERSION_STRING);

		// Setup check, and Factorio version tracking: an update can change the shaders the capture rule relies on.
		setup::run(g_module);
		motion::prewarm(); // start the shader compile (or cache load) now, not when frame generation starts
		for (const setup::item &i : setup::items())
			log_info("HUD-less probe: setup: %s: %s%s", i.name.c_str(), i.result == setup::state::ok ? "" : (i.result == setup::state::warning ? "WARNING " : "PROBLEM "),
				i.detail.c_str());
		const std::string &factorio = setup::factorio_version();
		if (!factorio.empty())
		{
			char lastf[64] = {};
			size_t nf = sizeof(lastf) - 1;
			if (reshade::get_config_value(nullptr, CONFIG_SECTION, "LastFactorioVersion", lastf, &nf) && lastf[0] != 0 && factorio != lastf)
			{
				log_info("HUD-less probe: Factorio updated from %s to %s", lastf, factorio.c_str());
			}
			reshade::set_config_value(nullptr, CONFIG_SECTION, "LastFactorioVersion", factorio.c_str());
		}
	}

	// After a Factorio update the boundary's anchor shader may be gone: the capture then always uses the fallback.
	// Map view also has no anchor, so only warn when the anchor has not been seen at all for a while.
	const char *capture_warning()
	{
		if (g_mode == boundary_mode::off || g_anchor_ever_matched || g_game_frames < 60 * 30)
			return nullptr;
		return "The GUI capture rule has not matched since start (Factorio update?). Using the fallback; check the HUD-less capture tab.";
	}

	void save_config()
	{
		reshade::set_config_value(nullptr, CONFIG_SECTION, "Mode", static_cast<int>(g_mode));
		reshade::set_config_value(nullptr, CONFIG_SECTION, "CaptureModeChecked", true);
		char hex[16];
		std::snprintf(hex, sizeof(hex), "%08X", g_rule_ps);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "RulePixelShader", static_cast<const char *>(hex));
		reshade::set_config_value(nullptr, CONFIG_SECTION, "RuleOccurrence", g_rule_occurrence);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "RuleDrawIndex", g_rule_index);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "RuleBackBufferDraw", g_rule_bb_index);
		std::snprintf(hex, sizeof(hex), "%08X", g_anchor_ps);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AnchorPixelShader", static_cast<const char *>(hex));
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AnchorOccurrence", g_anchor_occurrence);
		auto write_hex = [&](const char *key, uint32_t v) {
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%08X", v);
			reshade::set_config_value(nullptr, CONFIG_SECTION, key, static_cast<const char *>(buf));
		};
		write_hex("GuiPixelShader", g_gui_ps);
		write_hex("GuiVertexShader", g_gui_vs);
		write_hex("GuiBlend", g_gui_blend);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "GuiStrict", g_gui_strict);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AutoFallback", g_auto_fallback);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AnchorExtraDraws", g_anchor_extra);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AnchorExtraNeedsSpace", g_extra_needs_marker);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "BridgeEnabled", g_bridge_enabled);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "BridgeTearing", g_bridge_tearing);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "FrameGenMultiplier", g_fg_multiplier);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "AutoTargetFps", g_auto_target);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionVectors", g_motion_vectors);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionVectorsFlip", g_motion_flip);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "OnScreenReadout", g_osd);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "SceneCutDetection", g_scene_cut);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "DeveloperTools", g_dev_tools);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "MoverSearch", g_mover_search);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "WorldLayers", g_world_layers);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "FrameGenerationBackend", g_fg_choice);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "FsrHoldCharacter", g_fsr_hold);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "PaintOverCharacter", g_fsr_inpaint);
		{
			char hexbuf[16];
			std::snprintf(hexbuf, sizeof(hexbuf), "%08X", g_layer_ground_ps);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "LayerGroundPixelShader", static_cast<const char *>(hexbuf));
			reshade::set_config_value(nullptr, CONFIG_SECTION, "LayerGroundOccurrence", g_layer_ground_occ);
			std::snprintf(hexbuf, sizeof(hexbuf), "%08X", g_air_split_ps);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "AirSplitPixelShader", static_cast<const char *>(hexbuf));
			reshade::set_config_value(nullptr, CONFIG_SECTION, "AltModeIconsBeforeSplit", g_air_icons_first);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "WideMoverSearch", g_wide_search);
		}
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.2f", g_scene_cut_factor);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "SceneCutFactor", static_cast<const char *>(buf));
		}
		reshade::set_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKey", g_osd_key);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKeyCtrl", g_osd_key_ctrl);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKeyShift", g_osd_key_shift);
		reshade::set_config_value(nullptr, CONFIG_SECTION, "OnScreenReadoutKeyAlt", g_osd_key_alt);
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.3f", g_motion_penalty_zero);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionZeroPenalty", static_cast<const char *>(buf));
			std::snprintf(buf, sizeof(buf), "%.3f", g_motion_mismatch);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionMismatch", static_cast<const char *>(buf));
			std::snprintf(buf, sizeof(buf), "%.3f", g_motion_hysteresis);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionStability", static_cast<const char *>(buf));
			std::snprintf(buf, sizeof(buf), "%.3f", g_motion_strictness);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionStrictness", static_cast<const char *>(buf));
			std::snprintf(buf, sizeof(buf), "%.3f", g_motion_far_penalty);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionRepeatGuard", static_cast<const char *>(buf));
			std::snprintf(buf, sizeof(buf), "%.3f", g_motion_still_zone);
			reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionStillZone", static_cast<const char *>(buf));
		}
	}

	// ---------------------------------------------------------------- pipelines

	void on_init_pipeline(device *dev, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
	{
		if (dev->get_api() != device_api::d3d11)
			return;
		uint32_t h = 2166136261u;
		bool any = false;
		for (uint32_t i = 0; i < count; ++i)
		{
			const pipeline_subobject &so = subobjects[i];
			switch (so.type)
			{
			case pipeline_subobject_type::vertex_shader:
			case pipeline_subobject_type::pixel_shader:
				if (so.data != nullptr)
				{
					const auto *desc = static_cast<const shader_desc *>(so.data);
					if (desc->code != nullptr && desc->code_size != 0)
					{
						h = fnv1a(desc->code, desc->code_size, h);
						any = true;
					}
				}
				break;
			case pipeline_subobject_type::blend_state:
				if (so.data != nullptr)
				{
					h = fnv1a(so.data, sizeof(blend_desc), h);
					any = true;
				}
				break;
			default:
				break;
			}
		}
		if (!any)
			return;
		std::lock_guard<app_mutex> lock(g_mutex);
		g_pipeline_hash[pipe.handle] = h;
	}

	void on_destroy_pipeline(device *, pipeline pipe)
	{
		std::lock_guard<app_mutex> lock(g_mutex);
		g_pipeline_hash.erase(pipe.handle);
	}

	void on_bind_pipeline(command_list *cmd_list, pipeline_stage stages, pipeline pipe)
	{
		std::lock_guard<app_mutex> lock(g_mutex);
		if (cmd_list->get_device() != g_device)
			return;
		uint32_t h = 0;
		if (pipe.handle != 0)
		{
			const auto it = g_pipeline_hash.find(pipe.handle);
			h = (it != g_pipeline_hash.end()) ? it->second : 0xFFFFFFFFu;
		}
		if ((stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader) g_cur_vs = h;
		if ((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader) g_cur_ps = h;
		if ((stages & pipeline_stage::output_merger) == pipeline_stage::output_merger) g_cur_blend = h;
	}

	void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view)
	{
		std::lock_guard<app_mutex> lock(g_mutex);
		if (cmd_list->get_device() != g_device)
			return;
		g_cur_rt = (count != 0 && rtvs[0].handle != 0) ? native_resource_from_view(rtvs[0]) : resource { 0 };
	}

	// ---------------------------------------------------------------- swap chain

	void destroy_swapchain_resources(device *dev)
	{
		if (g_preview_srv.handle != 0) dev->destroy_resource_view(g_preview_srv);
		if (g_preview.handle != 0) dev->destroy_resource(g_preview);
		for (uint32_t i = 0; i < PREVIEW_RING; ++i)
		{
			if (g_preview_staging[i].handle != 0) dev->destroy_resource(g_preview_staging[i]);
			g_preview_staging[i] = { 0 };
			g_preview_staging_valid[i] = false;
		}
		g_preview_srv = { 0 };
		g_preview = { 0 };
		if (g_hudless_srv.handle != 0) dev->destroy_resource_view(g_hudless_srv);
		if (g_hudless.handle != 0) dev->destroy_resource(g_hudless);
		if (g_layer_ground.handle != 0) dev->destroy_resource(g_layer_ground);
		if (g_layer_entities.handle != 0) dev->destroy_resource(g_layer_entities);
		if (g_layer_floor.handle != 0) dev->destroy_resource(g_layer_floor);
		if (g_layer_postcloud.handle != 0) dev->destroy_resource(g_layer_postcloud);
		if (g_layer_world.handle != 0) dev->destroy_resource(g_layer_world);
		if (g_layer_airsplit.handle != 0) dev->destroy_resource(g_layer_airsplit);
		g_layer_airsplit = { 0 };
		g_layer_ground = { 0 };
		g_layer_entities = { 0 };
		g_layer_floor = { 0 };
		g_layer_postcloud = { 0 };
		g_layer_world = { 0 };
		g_hudless_srv = { 0 };
		g_hudless = { 0 };
		g_back_buffers.clear();
		g_rt_desc_cache.clear();
	}

	void on_init_swapchain(swapchain *sc, bool)
	{
		device *const dev = sc->get_device();
		if (dev->get_api() != device_api::d3d11)
		{
			log_info("HUD-less probe: ignoring swap chain on non-D3D11 device (api %#x)", static_cast<unsigned>(dev->get_api()));
			return;
		}
		std::lock_guard<app_mutex> lock(g_mutex);
		if (g_device != nullptr && g_device != dev)
		{
			log_info("HUD-less probe: ignoring swap chain on a second D3D11 device");
			return;
		}
		if (!g_config_loaded)
		{
			load_config();
			g_config_loaded = true;
			if (g_capture_mode_migrated)
			{
				log_info("HUD-less probe: GUI capture rule was saved as Off (default bug before 1.1.0 beta 5); switched to the built-in rule");
				save_config();
			}
		}
		destroy_swapchain_resources(dev);
		g_device = dev;

		// D3D11 flip model only exposes buffer 0; it always refers to the current back buffer.
		const resource bb = native_back_buffer(sc);
		if (bb.handle == 0)
		{
			log_info("HUD-less probe: could not get the back buffer");
			return;
		}
		g_back_buffers.push_back(bb.handle);

		g_bb_desc = native_texture_desc(bb);
		const format typeless = format_to_typeless(g_bb_desc.texture.format);
		const resource_desc desc(g_bb_desc.texture.width, g_bb_desc.texture.height, 1, 1, typeless, 1,
			memory_heap::default_, resource_usage::copy_dest | resource_usage::shader_resource);
		if (!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_hudless))
		{
			g_last_status = "Failed to create HUD-less texture";
			log_info("HUD-less probe: failed to create HUD-less texture (format %u)", static_cast<unsigned>(typeless));
			return;
		}
		if (!dev->create_resource_view(g_hudless, resource_usage::shader_resource,
				resource_view_desc(format_to_default_typed(g_bb_desc.texture.format, 0)), &g_hudless_srv))
			g_hudless_srv = { 0 };
		if (!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_layer_ground) ||
			!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_layer_floor) ||
			!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_layer_postcloud) ||
			!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_layer_world) ||
			!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_layer_airsplit) ||
			!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &g_layer_entities))
			log_info("HUD-less probe: world layer textures could not be created (layers off)");

		g_preview_w = std::max(1u, g_bb_desc.texture.width / 2);
		g_preview_h = std::max(1u, g_bb_desc.texture.height / 2);
		bool preview_ok = dev->create_resource(resource_desc(g_preview_w, g_preview_h, 1, 1, format::r8g8b8a8_unorm, 1,
				memory_heap::default_, resource_usage::copy_dest | resource_usage::shader_resource),
			nullptr, resource_usage::shader_resource, &g_preview)
			&& dev->create_resource_view(g_preview, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm), &g_preview_srv);
		for (uint32_t i = 0; i < PREVIEW_RING && preview_ok; ++i)
			preview_ok = dev->create_resource(resource_desc(g_bb_desc.texture.width, g_bb_desc.texture.height, 1, 1, typeless, 1,
				memory_heap::readback, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &g_preview_staging[i]);
		if (!preview_ok)
			log_info("HUD-less probe: preview resources could not be created (preview disabled)");
		log_info("HUD-less probe: swap chain %ux%u format %u, %zu back buffer(s), HUD-less texture ready",
			g_bb_desc.texture.width, g_bb_desc.texture.height, static_cast<unsigned>(g_bb_desc.texture.format), g_back_buffers.size());
	}

	void on_destroy_swapchain(swapchain *sc, bool resize)
	{
		std::lock_guard<app_mutex> lock(g_mutex);
		if (sc->get_device() != g_device)
			return;
		destroy_swapchain_resources(g_device);
		if (!resize)
		{
			bridge::shutdown();
			g_device = nullptr;
		}
	}

	void bridge_log(const char *message)
	{
		reshade::log::message(reshade::log::level::info, message);
	}

	// Runs at Factorio's present, after the HUD-less copy for this frame is final.
	void bridge_frame(command_queue *queue, swapchain *sc)
	{
		if (!g_bridge_enabled)
		{
			if (bridge::active())
				bridge::suspend(true); // not shutdown: Streamline cannot be started again in the same session
			return;
		}
		if (bridge::active() && bridge::suspended())
			bridge::suspend(false);
		// Another technology chosen while the bridge runs: it applies at the next start of Factorio. (Restarting the
		// bridge in place hung DXGI on an RTX 3080, both ways; Streamline cannot start twice in a session anyway.)
		const resource bb = native_back_buffer(sc);
		if (bb.handle == 0)
			return;
		if (!bridge::active())
		{
			if (g_bridge_failed)
				return;
			fg::request(static_cast<fg::choice>(g_fg_choice));
			g_fg_choice_applied = g_fg_choice;
			auto *const dev11 = reinterpret_cast<ID3D11Device *>(g_device->get_native());
			const bool ok = bridge::init(dev11, static_cast<HWND>(sc->get_hwnd()), g_bb_desc.texture.width, g_bb_desc.texture.height,
				static_cast<DXGI_FORMAT>(g_bb_desc.texture.format), bridge_log);
			g_bridge_failed = !ok;
			if (!ok)
				return;
			bridge::set_visible(!g_overlay_open);
		}
		bridge::present_options options;
		options.allow_tearing = g_bridge_tearing;
		options.fg_multiplier = effective_multiplier();
		options.motion_vectors = g_motion_vectors;
		options.motion_flip = g_motion_flip;
		options.motion_penalty_zero = g_motion_penalty_zero;
		options.motion_mismatch = g_motion_mismatch;
		options.motion_hysteresis = g_motion_hysteresis;
		options.motion_strictness = g_motion_strictness;
		options.motion_debug_view = g_motion_debug;
		options.fg_debug_view = g_fsr_debug;
		options.fg_show_ui_layer = g_fsr_ui_layer;
		options.fg_only_generated = g_fsr_only_generated;
		options.fg_hold_character = g_fsr_hold;
		options.fg_inpaint_character = g_fsr_inpaint;
		options.motion_far_penalty = g_motion_far_penalty;
		// Map view (and anything else without the game view's post pass): nothing follows the camera, so nothing is
		// still. The boundary rule's anchor only matches in the game view.
		g_still_zone_active = g_last_matched_at >= 0;
		options.motion_still_zone = g_still_zone_active ? g_motion_still_zone : 0.0f;
		options.motion_mover_search = g_mover_search;
		options.motion_world_layers = g_world_layers;
		options.motion_debug_layer_view = static_cast<uint32_t>(g_debug_mode);
		options.motion_icons_first = g_air_icons_first;
		options.motion_wide_search = g_wide_search;
		options.scene_cut_detection = g_scene_cut;
		options.scene_cut_factor = g_scene_cut_factor;
		ID3D11Texture2D *layer_textures[bridge::LAYER_COUNT] = {};
		layer_textures[bridge::LAYER_GROUND] = reinterpret_cast<ID3D11Texture2D *>(g_layer_ground.handle);
		layer_textures[bridge::LAYER_FLOOR] = reinterpret_cast<ID3D11Texture2D *>(g_layer_floor.handle);
		layer_textures[bridge::LAYER_ENTITIES] = reinterpret_cast<ID3D11Texture2D *>(g_layer_entities.handle);
		layer_textures[bridge::LAYER_POSTCLOUD] = reinterpret_cast<ID3D11Texture2D *>(g_layer_postcloud.handle);
		layer_textures[bridge::LAYER_WORLD] = reinterpret_cast<ID3D11Texture2D *>(g_layer_world.handle);
		layer_textures[bridge::LAYER_AIRSPLIT] = reinterpret_cast<ID3D11Texture2D *>(g_layer_airsplit.handle);
		if (g_layers_direct_last)
			for (uint32_t i = 0; i < bridge::LAYER_COUNT; ++i)
				layer_textures[i] = reinterpret_cast<ID3D11Texture2D *>(g_layer_dst[i].handle);
		bridge::present(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()),
			reinterpret_cast<ID3D11Texture2D *>(bb.handle),
			g_hudless.handle != 0 ? reinterpret_cast<ID3D11Texture2D *>(g_hudless.handle) : nullptr,
			g_layers_last_frame ? layer_textures : nullptr, g_layers_direct_last ? g_layers_gen_last : bridge::layer_generation(),
			options);
		if (!bridge::active())
			g_bridge_failed = true; // the bridge shut itself down (timeout, device error); do not restart it every frame
	}

	// Copies a render target into one of our textures (kept in shader_resource state between copies).
	// With frame generation running, the snapshots go straight into the bridge's shared textures (one copy each
	// instead of two). Chosen once per frame, at the first snapshot; the preview needs the add-on's own textures.

	// End of frame: read the snapshot points off this frame's structure (every LEARN_EVERY-th frame, not in space), and
	// pick the points for the next frame: built-in while their shaders are seen, else the learned ones once confirmed.
	void update_snapshot_points()
	{
		if (!g_learn_draws.empty())
		{
			if (!g_last_marker_seen)
			{
				const layer_learn::points p = layer_learn::analyse(g_learn_draws);
				if (p.valid && p.same(g_learn_candidate))
					++g_learn_streak;
				else
				{
					g_learn_candidate = p;
					g_learn_streak = p.valid ? 1 : 0;
				}
				if (g_learn_streak >= LEARN_CONFIRM && !g_learn_candidate.same(g_learned))
				{
					g_learned = g_learn_candidate;
					log_info("HUD-less probe: snapshot points from the frame structure: ground %08X #%d, floor %s%08X, clouds %s%08X, composite %08X",
						g_learned.ground_ps, g_learned.ground_occ, g_learned.has_floor ? "" : "none ", g_learned.floor_ps,
						g_learned.has_cloud ? "" : "none ", g_learned.cloud_ps, g_learned.composite_ps);
				}
			}
			g_learn_draws.clear();
		}
		if (g_builtin_ground_seen && g_builtin_world_seen)
			g_builtin_last_seen = g_game_frames;
		g_builtin_ground_seen = g_builtin_world_seen = false;
		const bool use_learned = g_learned.valid && g_game_frames > g_builtin_last_seen + BUILTIN_GONE_FRAMES;
		if (use_learned != g_use_learned)
			log_info(use_learned ? "HUD-less probe: built-in snapshot shaders not seen for a while (Factorio update?): using the points found from the frame structure"
				: "HUD-less probe: built-in snapshot shaders seen again: using them");
		g_use_learned = use_learned;
		if (use_learned)
		{
			g_act_ground_ps = g_learned.ground_ps;
			g_act_ground_occ = g_learned.ground_occ;
			g_act_has_floor = g_learned.has_floor;
			g_act_floor_ps = g_learned.floor_ps;
			g_act_has_cloud = g_learned.has_cloud;
			g_act_cloud_ps = g_learned.cloud_ps;
			g_act_world_ps = g_learned.composite_ps;
		}
		else
		{
			g_act_ground_ps = g_layer_ground_ps;
			g_act_ground_occ = g_layer_ground_occ;
			g_act_has_floor = true;
			g_act_floor_ps = LAYER_FLOOR_AFTER_PS;
			g_act_has_cloud = true;
			g_act_cloud_ps = LAYER_ENTITIES_BEFORE_PS;
			g_act_world_ps = LAYER_WORLD_BEFORE_PS;
		}
	}

	void choose_layer_targets()
	{
		g_layers_direct = false;
		if (!g_bridge_enabled || g_preview_source != 0 || !bridge::active())
			return;
		g_layers_gen = bridge::layer_generation();
		bool ok = true;
		for (uint32_t i = 0; i < bridge::LAYER_COUNT; ++i)
		{
			ID3D11Texture2D *t = bridge::layer_target(i);
			ok = ok && t != nullptr;
			g_layer_dst[i] = { reinterpret_cast<uint64_t>(t) };
		}
		g_layers_direct = ok;
	}

	resource layer_dst(uint32_t layer, resource own)
	{
		return g_layers_direct ? g_layer_dst[layer] : own;
	}

	// Fallback when the floor pass (BCD79626) did not come: the floor snapshot = the ground snapshot.
	void floor_from_ground(command_list *cmd_list)
	{
		const resource src = layer_dst(bridge::LAYER_GROUND, g_layer_ground), dst = layer_dst(bridge::LAYER_FLOOR, g_layer_floor);
		const resource res[2] = { src, dst };
		const resource_usage before[2] = { resource_usage::shader_resource, resource_usage::shader_resource };
		const resource_usage during[2] = { resource_usage::copy_source, resource_usage::copy_dest };
		cmd_list->barrier(2, res, before, during);
		cmd_list->copy_resource(src, dst);
		cmd_list->barrier(2, res, during, before);
		g_layer_floor_done = true;
		g_layer_fallback |= 1u;
	}

	void copy_target(command_list *cmd_list, resource source, resource dest)
	{
		if (source.handle == 0 || dest.handle == 0)
			return;
		const resource res[2] = { source, dest };
		const resource_usage before[2] = { resource_usage::render_target, resource_usage::shader_resource };
		const resource_usage during[2] = { resource_usage::copy_source, resource_usage::copy_dest };
		cmd_list->barrier(2, res, before, during);
		cmd_list->copy_resource(source, dest);
		cmd_list->barrier(2, res, during, before);
	}

	// Copies the given back buffer into the HUD-less texture.
	void capture_hudless(command_list *cmd_list, resource bb, bool mark_captured = true)
	{
		if (g_hudless.handle == 0 || bb.handle == 0)
			return;
		const resource res[2] = { bb, g_hudless };
		const resource_usage before[2] = { resource_usage::render_target, resource_usage::shader_resource };
		const resource_usage during[2] = { resource_usage::copy_source, resource_usage::copy_dest };
		cmd_list->barrier(2, res, before, during);
		cmd_list->copy_resource(bb, g_hudless);
		cmd_list->barrier(2, res, during, before);
		if (mark_captured)
			g_captured_this_frame = true;
	}

	// ---------------------------------------------------------------- draws

	const resource_desc &cached_desc(resource r)
	{
		auto it = g_rt_desc_cache.find(r.handle);
		if (it == g_rt_desc_cache.end())
			it = g_rt_desc_cache.emplace(r.handle, native_texture_desc(r)).first;
		return it->second;
	}

	// A render target we can CopyResource into the HUD-less texture: same size and format family as the back buffer.
	bool is_screen_copyable(resource r)
	{
		if (r.handle == 0)
			return false;
		const resource_desc &d = cached_desc(r);
		return d.type == resource_type::texture_2d && d.texture.samples <= 1 && d.texture.depth_or_layers == 1 && d.texture.levels == 1 &&
			d.texture.width == g_bb_desc.texture.width && d.texture.height == g_bb_desc.texture.height &&
			format_to_typeless(d.texture.format) == format_to_typeless(g_bb_desc.texture.format);
	}

	// Copies a render target into the HUD-less texture. If Factorio composes the frame offscreen and only copies
	// it to the back buffer at the end, that screen-sized offscreen target is the one that matters.
	void capture_from_target(command_list *cmd_list, resource target, bool mark_captured = true)
	{
		const bool to_bb = is_back_buffer(target);
		bool offscreen = !to_bb && is_screen_copyable(target);
		resource source = target;
		if (!to_bb && !offscreen)
		{
			// The target is not screen-sized (e.g. a half-res lighting buffer). Use the screen-sized target the
			// frame is being composed in instead of the back buffer, which stays black until the final copy.
			offscreen = g_last_screen_rt.handle != 0;
			source = offscreen ? g_last_screen_rt : resource { g_back_buffers[0] };
		}
		capture_hudless(cmd_list, source, mark_captured);
		g_capture_from_offscreen = offscreen;
	}

	// ---------------------------------------------------------------- camera probe (step 3)
	//
	// Records the vertex-shader constant buffers Factorio binds, so the camera transform (pan, zoom) can be found.
	// Off by default: when on, every buffer Map/Unmap passes through here.

	constexpr int CB_FLOATS = 32; // first 128 bytes of each constant buffer
	struct cb_snapshot { float f[CB_FLOATS]; uint32_t bytes; };
	struct cam_entry { uint32_t draw, vs; uint32_t rt_w, rt_h; uint32_t slot; uint64_t cb; cb_snapshot data; };

	bool g_cam_probe = false;
	std::unordered_set<uint64_t> g_cb_handles;                              // buffers created as constant buffers
	std::unordered_map<uint64_t, std::pair<void *, uint64_t>> g_cb_mapped;   // CB currently mapped for write: pointer, size
	std::unordered_map<uint64_t, cb_snapshot> g_cb_latest;                   // last contents written to each CB
	uint64_t g_vs_cb[2] = {};                                               // VS constant buffer slots 0 and 1
	std::vector<cam_entry> g_cam_frame, g_cam_last;                         // distinct (VS, slot, CB) per frame
	bool g_cam_recording = false;
	uint64_t g_cam_record_end = 0;
	FILE *g_cam_file = nullptr;

	void store_cb(uint64_t handle, const void *data, uint64_t size)
	{
		cb_snapshot snap = {};
		snap.bytes = static_cast<uint32_t>(std::min<uint64_t>(size, sizeof(snap.f)));
		std::memcpy(snap.f, data, snap.bytes);
		g_cb_latest[handle] = snap;
	}

	void on_init_resource(device *dev, const resource_desc &desc, const subresource_data *initial_data, resource_usage, resource res)
	{
		if (dev->get_api() != device_api::d3d11 || desc.type != resource_type::buffer ||
			(desc.usage & resource_usage::constant_buffer) != resource_usage::constant_buffer)
			return;
		std::lock_guard<app_mutex> lock(g_mutex);
		g_cb_handles.insert(res.handle);
		if (initial_data != nullptr && initial_data->data != nullptr)
			store_cb(res.handle, initial_data->data, desc.buffer.size);
	}

	void on_destroy_resource(device *dev, resource res)
	{
		if (dev->get_api() != device_api::d3d11)
			return;
		std::lock_guard<app_mutex> lock(g_mutex);
		if (g_cb_handles.erase(res.handle) != 0)
		{
			g_cb_latest.erase(res.handle);
			g_cb_mapped.erase(res.handle);
		}
	}

	void on_map_buffer(device *dev, resource res, uint64_t, uint64_t size, map_access access, void **data)
	{
		if (!g_cam_probe || dev != g_device || access == map_access::read_only || data == nullptr || *data == nullptr)
			return;
		std::lock_guard<app_mutex> lock(g_mutex);
		if (g_cb_handles.count(res.handle) != 0)
			g_cb_mapped[res.handle] = { *data, size };
	}

	void on_unmap_buffer(device *dev, resource res)
	{
		if (!g_cam_probe || dev != g_device)
			return;
		std::lock_guard<app_mutex> lock(g_mutex);
		const auto it = g_cb_mapped.find(res.handle);
		if (it == g_cb_mapped.end())
			return;
		store_cb(res.handle, it->second.first, it->second.second == UINT64_MAX ? sizeof(cb_snapshot::f) : it->second.second);
		g_cb_mapped.erase(it);
	}

	bool on_update_buffer(device *dev, const void *data, resource dest, uint64_t dest_offset, uint64_t size)
	{
		if (!g_cam_probe || dev != g_device || data == nullptr || dest_offset != 0)
			return false;
		std::lock_guard<app_mutex> lock(g_mutex);
		if (g_cb_handles.count(dest.handle) != 0)
			store_cb(dest.handle, data, size);
		return false;
	}

	void on_push_descriptors(command_list *cmd_list, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update &update)
	{
		if (!g_cam_probe || update.type != descriptor_type::constant_buffer ||
			(stages & shader_stage::vertex) != shader_stage::vertex || cmd_list->get_device() != g_device)
			return;
		std::lock_guard<app_mutex> lock(g_mutex);
		const auto *ranges = static_cast<const buffer_range *>(update.descriptors);
		for (uint32_t i = 0; i < update.count; ++i)
		{
			const uint32_t slot = update.binding + i;
			if (slot < 2)
				g_vs_cb[slot] = ranges != nullptr ? ranges[i].buffer.handle : 0;
		}
	}

	// Called from on_any_draw with the lock held.
	void camera_probe_draw(uint32_t index)
	{
		if (!g_cam_probe || g_cam_frame.size() >= 64)
			return;
		for (uint32_t slot = 0; slot < 2; ++slot)
		{
			const uint64_t cb = g_vs_cb[slot];
			if (cb == 0)
				continue;
			bool seen = false;
			for (const cam_entry &e : g_cam_frame)
				if (e.vs == g_cur_vs && e.slot == slot && e.cb == cb)
				{
					seen = true;
					break;
				}
			if (seen)
				continue;
			cam_entry e = {};
			e.draw = index;
			e.vs = g_cur_vs;
			e.slot = slot;
			e.cb = cb;
			if (g_cur_rt.handle != 0)
			{
				const resource_desc &d = cached_desc(g_cur_rt);
				e.rt_w = d.texture.width;
				e.rt_h = d.texture.height;
			}
			const auto it = g_cb_latest.find(cb);
			if (it != g_cb_latest.end())
				e.data = it->second;
			g_cam_frame.push_back(e);
		}
	}

	// Called at present with the lock held.
	void camera_probe_frame_end()
	{
		if (g_cam_recording && g_cam_file != nullptr)
		{
			for (const cam_entry &e : g_cam_frame)
			{
				std::fprintf(g_cam_file, "%" PRIu64 ",%u,%08X,%u,%u,%u,%016" PRIX64 ",%u", g_frame_counter, e.draw, e.vs, e.rt_w, e.rt_h, e.slot, e.cb, e.data.bytes);
				for (int i = 0; i < CB_FLOATS; ++i)
					std::fprintf(g_cam_file, ",%.9g", e.data.f[i]);
				std::fputc('\n', g_cam_file);
			}
			if (g_frame_counter >= g_cam_record_end)
			{
				std::fclose(g_cam_file);
				g_cam_file = nullptr;
				g_cam_recording = false;
				g_last_status = "Camera recording saved to " + (output_dir() / L"camera.csv").u8string();
			}
		}
		g_cam_last.swap(g_cam_frame);
		g_cam_frame.clear();
	}

	void start_camera_recording()
	{
		std::error_code ec;
		std::filesystem::create_directories(output_dir(), ec);
		g_cam_file = _wfopen((output_dir() / L"camera.csv").c_str(), L"w");
		if (g_cam_file == nullptr)
		{
			g_last_status = "Could not create camera.csv";
			return;
		}
		std::fprintf(g_cam_file, "frame,draw,vs,rt_w,rt_h,slot,cb,bytes");
		for (int i = 0; i < CB_FLOATS; ++i)
			std::fprintf(g_cam_file, ",f%d", i);
		std::fputc('\n', g_cam_file);
		g_cam_recording = true;
		g_cam_record_end = g_frame_counter + 600; // 10 s at 60 fps
	}

	void on_any_draw(command_list *cmd_list, uint32_t count, uint32_t instances, bool indexed)
	{
		std::lock_guard<app_mutex> lock(g_mutex);
		if (g_device == nullptr || cmd_list->get_device() != g_device)
			return;
		const uint32_t index = g_draw_index++;
		if (index == 0)
			bridge::frame_start(); // Reflex/PCL: Factorio has finished simulating and starts submitting this frame
		const bool to_bb = is_back_buffer(g_cur_rt);
		if (to_bb && g_first_bb_draw < 0)
			g_first_bb_draw = static_cast<int>(index);
		const int bb_ordinal = to_bb ? static_cast<int>(g_bb_draw_index++) : -1;
		for (const uint32_t marker : SPACE_MARKER_PS)
			if (g_cur_ps == marker)
				g_marker_seen = true;
		const bool full_size = is_screen_copyable(g_cur_rt);
		if (!to_bb && full_size)
			g_last_screen_rt = g_cur_rt;
		camera_probe_draw(index);
		// structure of the frame, for finding the snapshot points without shader hashes
		if (g_cur_ps == g_layer_ground_ps)
			g_builtin_ground_seen = true;
		if (g_cur_ps == LAYER_WORLD_BEFORE_PS)
			g_builtin_world_seen = true;
		if (g_world_layers && g_game_frames % LEARN_EVERY == 0 && g_learn_draws.size() < 8192)
		{
			layer_learn::draw d;
			d.ps = g_cur_ps;
			d.rt = g_cur_rt.handle;
			d.count = count;
			d.instances = instances;
			d.indexed = indexed;
			d.full = full_size;
			d.to_bb = to_bb;
			g_learn_draws.push_back(d);
		}

		// World layers. "After" snapshots are taken once that draw has executed (before the next draw); the entity
		// snapshot right before the cloud pass draws. All from the same world target.
		if ((g_world_layers || g_preview_source != 0) && g_layer_ground.handle != 0 && g_layer_floor.handle != 0 && g_layer_entities.handle != 0 &&
			g_layer_postcloud.handle != 0 && g_layer_world.handle != 0 && g_layer_airsplit.handle != 0)
		{
			if (g_layer_postcloud_pending)
			{
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_POSTCLOUD, g_layer_postcloud));
				g_layer_postcloud_pending = false;
				g_layer_postcloud_done = true;
			}
			if (g_layer_ground_pending)
			{
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_GROUND, g_layer_ground));
				g_layer_ground_pending = false;
				g_layer_ground_done = true;
			}
			if (g_layer_floor_pending)
			{
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_FLOOR, g_layer_floor));
				g_layer_floor_pending = false;
				g_layer_floor_done = true;
			}
			// air stage draws (shown in the Motion vectors tab to pick the split)
			if (g_layer_postcloud_done && !g_layer_world_done && g_cur_rt == g_layer_rt && g_cur_ps != g_act_world_ps)
			{
				int k = 0;
				while (k < g_air_draw_count && g_air_draws[k].ps != g_cur_ps)
					++k;
				if (k < g_air_draw_count)
					++g_air_draws[k].count;
				else if (g_air_draw_count < AIR_DRAW_SLOTS)
					g_air_draws[g_air_draw_count++] = { g_cur_ps, 1 };
			}
			// occurrence counted over all draws with that shader, like the frame trace's "occurrence"
			if (!g_layer_ground_done && !g_layer_ground_pending && g_cur_ps == g_act_ground_ps && ++g_layer_ground_seen == g_act_ground_occ &&
				!to_bb && is_screen_copyable(g_cur_rt))
			{
				g_layer_ground_pending = true;
				g_layer_rt = g_cur_rt;
				choose_layer_targets();
			}
			else if (g_layer_ground_done && !g_layer_floor_done && !g_layer_floor_pending && g_act_has_floor && g_cur_ps == g_act_floor_ps && g_cur_rt == g_layer_rt)
				g_layer_floor_pending = true;
			else if (g_layer_ground_done && !g_layer_floor_pending && !g_layer_entities_done && g_act_has_cloud && g_cur_ps == g_act_cloud_ps && g_cur_rt == g_layer_rt)
			{
				if (!g_layer_floor_done)
					floor_from_ground(cmd_list); // no floor pass seen: everything placed counts as objects
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_ENTITIES, g_layer_entities));
				g_layer_entities_done = true;
				g_layer_postcloud_pending = true; // and once the cloud pass has run
			}
			else if (g_layer_ground_done && !g_layer_floor_pending && !g_layer_entities_done && g_cur_ps == g_act_world_ps)
			{
				// No cloud pass on this surface (Vulcanus has no clouds): the world is complete now. Objects and air
				// together become the object layer (no air layer this frame).
				if (!g_layer_floor_done)
					floor_from_ground(cmd_list);
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_ENTITIES, g_layer_entities));
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_POSTCLOUD, g_layer_postcloud));
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_AIRSPLIT, g_layer_airsplit));
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_WORLD, g_layer_world));
				g_layer_entities_done = g_layer_postcloud_done = g_layer_airsplit_done = g_layer_world_done = true;
				g_layer_fallback |= 2u;
			}
			else if (g_layer_postcloud_done && !g_layer_world_done && g_cur_ps == g_act_world_ps)
			{
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_WORLD, g_layer_world)); // the world target is complete: air stage included
				g_layer_world_done = true;
				if (!g_layer_airsplit_done)
				{
					copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_AIRSPLIT, g_layer_airsplit)); // no split draw this frame: all on one side
					g_layer_airsplit_done = true;
				}
			}
			else if (g_layer_postcloud_done && !g_layer_world_done && !g_layer_airsplit_done && g_cur_ps == g_air_split_ps && g_cur_rt == g_layer_rt)
			{
				copy_target(cmd_list, g_layer_rt, layer_dst(bridge::LAYER_AIRSPLIT, g_layer_airsplit));
				g_layer_airsplit_done = true;
			}
		}


		// "After anchor": the anchor draw has executed by now, so copy its target before this draw runs,
		// unless we still have to let a few more draws into the same target through (space dust layers).
		if (g_anchor_pending && !g_captured_this_frame)
		{
			if (g_anchor_skip > 0)
			{
				if (g_cur_rt == g_anchor_rt)
				{
					--g_anchor_skip; // this draw runs and is part of the HUD-less image
					g_extra_building.emplace_back(static_cast<int>(index), count);
				}
			}
			else
			{
				capture_from_target(cmd_list, g_anchor_rt);
				g_anchor_pending = false;
			}
		}

		// Fallback copy (does not mark the frame as captured, so a later anchor still wins).
		if (g_auto_pending && !g_captured_this_frame)
		{
			capture_from_target(cmd_list, g_auto_rt, false);
			g_auto_captured = true;
		}
		g_auto_pending = false;

		// Boundary check happens BEFORE this draw executes, so the copy holds everything drawn so far.
		if (!g_captured_this_frame && !g_back_buffers.empty())
		{
			bool hit = false;
			if (g_mode == boundary_mode::shader_rule && g_cur_ps == g_rule_ps && g_rule_ps != 0)
				hit = (++g_rule_ps_seen == g_rule_occurrence);
			else if (g_mode == boundary_mode::draw_index)
				hit = (static_cast<int>(index) == g_rule_index);
			else if (g_mode == boundary_mode::back_buffer_draw)
				hit = (bb_ordinal == g_rule_bb_index);
			else if (g_mode == boundary_mode::after_anchor && g_cur_ps == g_anchor_ps && g_anchor_ps != 0 &&
				++g_anchor_seen == g_anchor_occurrence)
			{
				g_anchor_pending = true;  // copy once this draw has executed
				g_anchor_skip = (g_extra_needs_marker && !g_marker_seen) ? 0 : g_anchor_extra;
				g_anchor_rt = g_cur_rt;
				g_matched_at = static_cast<int>(index);
			}
			else if (g_mode == boundary_mode::anchor_to_gui && g_anchor_done && g_gui_ps != 0 && g_cur_ps == g_gui_ps &&
				(!g_gui_strict || (g_cur_vs == g_gui_vs && g_cur_blend == g_gui_blend)))
				hit = true;  // first GUI-shader draw after the anchor: world, lighting and dust are all in by now
			if (hit)
			{
				capture_from_target(cmd_list, g_cur_rt);
				g_matched_at = static_cast<int>(index);
			}
		}

		// Mark the anchor as done after the rule check, so the anchor draw itself never counts as the GUI start.
		if (g_mode == boundary_mode::anchor_to_gui && !g_anchor_done && g_anchor_ps != 0 && g_cur_ps == g_anchor_ps &&
			++g_anchor_seen == g_anchor_occurrence)
			g_anchor_done = true;

		const bool anchor_mode = g_mode == boundary_mode::after_anchor || g_mode == boundary_mode::anchor_to_gui;
		if (anchor_mode && g_auto_fallback && !g_captured_this_frame && !to_bb && g_cur_rt.handle != 0 &&
			!indexed && count == 3 && instances <= 1)
		{
			g_auto_pending = true;
			g_auto_rt = g_cur_rt;
			g_auto_at = static_cast<int>(index);
		}

		if (g_trace_active)
		{
			draw_record r = {};
			r.index = index;
			r.vs = g_cur_vs; r.ps = g_cur_ps; r.blend = g_cur_blend;
			r.rt = g_cur_rt.handle;
			r.rt_is_back_buffer = to_bb;
			if (g_cur_rt.handle != 0)
			{
				const resource_desc &d = cached_desc(g_cur_rt);
				r.rt_width = d.texture.width;
				r.rt_height = d.texture.height;
				r.rt_format = d.texture.format;
			}
			r.count = count; r.instances = instances; r.indexed = indexed;
			g_trace_building.push_back(r);
		}
	}

	bool on_draw(command_list *cmd_list, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
	{
		on_any_draw(cmd_list, vertices, instances, false);
		return false;
	}

	bool on_draw_indexed(command_list *cmd_list, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
	{
		on_any_draw(cmd_list, indices, instances, true);
		return false;
	}

	bool on_draw_indirect(command_list *cmd_list, indirect_command type, resource, uint64_t, uint32_t draw_count, uint32_t)
	{
		if (type != indirect_command::dispatch)
			on_any_draw(cmd_list, draw_count, 0, type == indirect_command::draw_indexed);
		return false;
	}

	// ---------------------------------------------------------------- dumping

	// Converts one mapped texel to 8-bit RGB. Returns false for unsupported formats.
	bool read_rgb(format fmt, const uint8_t *px, uint8_t rgb[3])
	{
		switch (format_to_typeless(fmt))
		{
		case format::r8g8b8a8_typeless:
			rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2];
			return true;
		case format::b8g8r8a8_typeless:
		case format::b8g8r8x8_typeless:
			rgb[0] = px[2]; rgb[1] = px[1]; rgb[2] = px[0];
			return true;
		case format::r10g10b10a2_typeless:
		{
			uint32_t v; std::memcpy(&v, px, 4);
			rgb[0] = static_cast<uint8_t>(((v >> 0) & 0x3FF) >> 2);
			rgb[1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
			rgb[2] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
			return true;
		}
		default:
			return false;
		}
	}

	bool write_bmp(const std::filesystem::path &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &rgb)
	{
		const uint32_t row = (w * 3 + 3) & ~3u;
		const uint32_t image_size = row * h;
		uint8_t header[54] = { 'B', 'M' };
		auto put32 = [&](int off, uint32_t v) { std::memcpy(header + off, &v, 4); };
		auto put16 = [&](int off, uint16_t v) { std::memcpy(header + off, &v, 2); };
		put32(2, 54 + image_size); put32(10, 54); put32(14, 40);
		put32(18, w); put32(22, h); put16(26, 1); put16(28, 24); put32(34, image_size);

		FILE *f = _wfopen(path.c_str(), L"wb");
		if (f == nullptr)
			return false;
		std::fwrite(header, 1, sizeof(header), f);
		std::vector<uint8_t> line(row, 0);
		for (uint32_t y = 0; y < h; ++y)
		{
			const uint8_t *src = rgb.data() + static_cast<size_t>(h - 1 - y) * w * 3; // BMP is bottom-up
			for (uint32_t x = 0; x < w; ++x)
			{
				line[x * 3 + 0] = src[x * 3 + 2];
				line[x * 3 + 1] = src[x * 3 + 1];
				line[x * 3 + 2] = src[x * 3 + 0];
			}
			std::fwrite(line.data(), 1, row, f);
		}
		std::fclose(f);
		return true;
	}

	// Copies a GPU texture into a CPU readable one and converts it to packed RGB.
	bool read_back(command_queue *queue, resource src, std::vector<uint8_t> &rgb)
	{
		device *const dev = queue->get_device();
		const uint32_t w = g_bb_desc.texture.width, h = g_bb_desc.texture.height;
		const format fmt = format_to_default_typed(g_bb_desc.texture.format, 0);

		resource staging = { 0 };
		if (!dev->create_resource(resource_desc(w, h, 1, 1, format_to_typeless(fmt), 1, memory_heap::readback, resource_usage::copy_dest),
				nullptr, resource_usage::copy_dest, &staging))
			return false;

		command_list *const cmd = queue->get_immediate_command_list();
		const resource_usage src_state = (src == g_hudless) ? resource_usage::shader_resource : resource_usage::present;
		cmd->barrier(src, src_state, resource_usage::copy_source);
		cmd->copy_resource(src, staging);
		cmd->barrier(src, resource_usage::copy_source, src_state);
		queue->flush_immediate_command_list();
		queue->wait_idle();

		bool ok = false;
		subresource_data data = {};
		if (dev->map_texture_region(staging, 0, nullptr, map_access::read_only, &data))
		{
			rgb.resize(static_cast<size_t>(w) * h * 3);
			ok = true;
			for (uint32_t y = 0; y < h && ok; ++y)
			{
				const uint8_t *row = static_cast<const uint8_t *>(data.data) + static_cast<size_t>(y) * data.row_pitch;
				for (uint32_t x = 0; x < w && ok; ++x)
					ok = read_rgb(fmt, row + x * 4, &rgb[(static_cast<size_t>(y) * w + x) * 3]);
			}
			dev->unmap_texture_region(staging, 0);
		}
		dev->destroy_resource(staging);
		return ok;
	}

	void write_trace_csv(const std::filesystem::path &path, const std::vector<draw_record> &trace)
	{
		FILE *f = _wfopen(path.c_str(), L"w");
		if (f == nullptr)
			return;
		std::fprintf(f, "draw,vs,ps,blend,rt,rt_is_backbuffer,rt_width,rt_height,rt_format,count,instances,indexed\n");
		for (const draw_record &r : trace)
			std::fprintf(f, "%u,%08X,%08X,%08X,%016" PRIX64 ",%d,%u,%u,%u,%u,%u,%d\n",
				r.index, r.vs, r.ps, r.blend, r.rt, r.rt_is_back_buffer ? 1 : 0,
				r.rt_width, r.rt_height, static_cast<uint32_t>(r.rt_format), r.count, r.instances, r.indexed ? 1 : 0);
		std::fclose(f);
	}

	void do_dump(command_queue *queue, swapchain *sc)
	{
		std::error_code ec;
		const std::filesystem::path dir = output_dir() / std::to_wstring(g_frame_counter);
		std::filesystem::create_directories(dir, ec);

		std::vector<uint8_t> final_rgb, hudless_rgb;
		if (!read_back(queue, native_back_buffer(sc), final_rgb) || !read_back(queue, g_hudless, hudless_rgb))
		{
			g_last_status = "Dump failed: unsupported back buffer format " + std::to_string(static_cast<uint32_t>(g_bb_desc.texture.format));
			return;
		}

		const uint32_t w = g_bb_desc.texture.width, h = g_bb_desc.texture.height;
		std::vector<uint8_t> mask(final_rgb.size());
		size_t ui_pixels = 0;
		for (size_t i = 0; i < final_rgb.size(); i += 3)
		{
			const int d = std::abs(final_rgb[i] - hudless_rgb[i]) + std::abs(final_rgb[i + 1] - hudless_rgb[i + 1]) + std::abs(final_rgb[i + 2] - hudless_rgb[i + 2]);
			const uint8_t m = d > 6 ? 255 : 0;
			ui_pixels += (m != 0);
			mask[i] = mask[i + 1] = mask[i + 2] = m;
		}

		write_bmp(dir / L"final.bmp", w, h, final_rgb);
		write_bmp(dir / L"hudless.bmp", w, h, hudless_rgb);
		write_bmp(dir / L"ui_mask.bmp", w, h, mask);
		write_trace_csv(dir / L"trace.csv", g_trace);

		char msg[256];
		std::snprintf(msg, sizeof(msg), "Dumped frame %" PRIu64 " (UI covers %.1f%% of pixels, boundary at draw %d)",
			g_frame_counter, 100.0 * ui_pixels / (static_cast<double>(w) * h), g_last_matched_at);
		g_last_status = std::string(msg) + " to " + dir.u8string();
	}

	// ---------------------------------------------------------------- frame boundary

	void update_preview(command_queue *queue)
	{
		// Only while the preview is visible, every other frame.
		if (g_preview_srv.handle == 0 || g_preview_staging[PREVIEW_RING - 1].handle == 0 ||
			g_frame_counter > g_preview_requested_frame + 2 || (g_frame_counter & 1) != 0)
			return;

		device *const dev = queue->get_device();
		const uint32_t slot = g_preview_slot;
		g_preview_slot = (slot + 1) % PREVIEW_RING;
		const format fmt = format_to_default_typed(g_bb_desc.texture.format, 0);

		if (g_preview_staging_valid[slot])
		{
			subresource_data mapped = {};
			if (dev->map_texture_region(g_preview_staging[slot], 0, nullptr, map_access::read_only, &mapped))
			{
				g_preview_pixels.resize(static_cast<size_t>(g_preview_w) * g_preview_h * 4);
				int alpha_max = 0;
				bool ok = true;
				for (uint32_t y = 0; y < g_preview_h && ok; ++y)
				{
					const uint32_t sy = g_preview_crop ? y + g_preview_h / 2 : y * 2;
					const uint8_t *row = static_cast<const uint8_t *>(mapped.data) + static_cast<size_t>(sy) * mapped.row_pitch;
					uint8_t *dst = g_preview_pixels.data() + static_cast<size_t>(y) * g_preview_w * 4;
					for (uint32_t x = 0; x < g_preview_w && ok; ++x)
					{
						const uint32_t sx = g_preview_crop ? x + g_preview_w / 2 : x * 2;
						const uint8_t *px = row + static_cast<size_t>(sx) * 4;
						ok = read_rgb(fmt, px, dst + x * 4);
						dst[x * 4 + 3] = 255;
						alpha_max = std::max<int>(alpha_max, px[3]);
					}
				}
				dev->unmap_texture_region(g_preview_staging[slot], 0);
				if (ok)
				{
					subresource_data upload = {};
					upload.data = g_preview_pixels.data();
					upload.row_pitch = g_preview_w * 4;
					upload.slice_pitch = upload.row_pitch * g_preview_h;
					dev->update_texture_region(upload, g_preview, 0, nullptr);
					g_preview_alpha_max = alpha_max;
				}
			}
		}

		command_list *const cmd = queue->get_immediate_command_list();
		// Preview source: the HUD-less image, or one of the world layer snapshots (to pick the ground snapshot point)
		const resource src = (g_preview_source == 1 && g_layer_ground.handle != 0) ? g_layer_ground :
			(g_preview_source == 3 && g_layer_floor.handle != 0) ? g_layer_floor :
			(g_preview_source == 4 && g_layer_world.handle != 0) ? g_layer_world :
			(g_preview_source == 2 && g_layer_entities.handle != 0) ? g_layer_entities : g_hudless;
		cmd->barrier(src, resource_usage::shader_resource, resource_usage::copy_source);
		cmd->copy_resource(src, g_preview_staging[slot]);
		cmd->barrier(src, resource_usage::copy_source, resource_usage::shader_resource);
		g_preview_staging_valid[slot] = true;
	}

	// Returns true if this was Factorio's swap chain (and the frame was processed).
	bool present_locked(command_queue *queue, swapchain *sc)
	{
		std::lock_guard<app_mutex> lock(g_mutex);
		if (g_device == nullptr || sc->get_device() != g_device)
			return false;

		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		++g_fps_window_frames;
		const double elapsed = static_cast<double>(now.QuadPart - g_fps_window_start.QuadPart) / static_cast<double>(freq.QuadPart);
		if (g_fps_window_start.QuadPart == 0 || elapsed > 2.0)
		{
			g_fps_window_start = now;
			g_fps_window_frames = 0;
		}
		else if (elapsed >= 0.5)
		{
			g_base_fps = static_cast<float>(g_fps_window_frames / elapsed);
			g_fps_window_start = now;
			g_fps_window_frames = 0;
			g_game_window = static_cast<HWND>(sc->get_hwnd());
			update_auto_multiplier();
		}

		// No GUI boundary this frame (e.g. no GUI visible or rule off): HUD-less == final, which is what DLSS-G expects.
		if (g_anchor_pending && !g_captured_this_frame)
			capture_from_target(queue->get_immediate_command_list(), g_anchor_rt); // anchor was the frame's last draw
		if (g_auto_pending && !g_captured_this_frame)
		{
			capture_from_target(queue->get_immediate_command_list(), g_auto_rt, false);
			g_auto_captured = true;
		}
		if (!g_captured_this_frame && !g_auto_captured)
			capture_hudless(queue->get_immediate_command_list(), native_back_buffer(sc));

		g_last_draw_count = g_draw_index;
		g_last_matched_at = g_matched_at;
		g_anchor_ever_matched = g_anchor_ever_matched || g_matched_at >= 0;
		++g_game_frames;
		g_last_auto_at = (g_matched_at < 0 && g_auto_captured) ? g_auto_at : -1;
		g_last_marker_seen = g_marker_seen;
		g_last_first_bb_draw = g_first_bb_draw;
		g_last_bb_draw_count = g_bb_draw_index;
		g_last_capture_from_offscreen = g_capture_from_offscreen;
		if (g_mode != boundary_mode::off)
		{
			if (g_matched_at >= 0) { ++g_match_streak; g_miss_streak = 0; }
			else { ++g_miss_streak; g_match_streak = 0; }
		}

		if (g_trace_active)
		{
			g_trace.swap(g_trace_building);
			std::unordered_map<uint32_t, uint32_t> uses;
			for (const draw_record &r : g_trace)
				++uses[r.ps];
			g_trace_ps_uses.resize(g_trace.size());
			for (size_t i = 0; i < g_trace.size(); ++i)
				g_trace_ps_uses[i] = uses[g_trace[i].ps];
			g_trace_building.clear();
			g_selected_row = -1;
		}
		update_preview(queue);
		if (g_dump_active)
			do_dump(queue, sc);

		// Start next frame
		++g_frame_counter;
		g_draw_index = 0;
		g_rule_ps_seen = 0;
		g_captured_this_frame = false;
		g_matched_at = -1;
		g_first_bb_draw = -1;
		g_bb_draw_index = 0;
		g_capture_from_offscreen = false;
		g_last_screen_rt = { 0 };
		g_auto_pending = false;
		g_auto_captured = false;
		g_auto_at = -1;
		g_anchor_seen = 0;
		g_anchor_pending = false;
		g_anchor_skip = 0;
		g_marker_seen = false;
		g_layers_last_frame = g_layer_ground_done && g_layer_floor_done && g_layer_entities_done && g_layer_postcloud_done && g_layer_world_done &&
			g_layer_airsplit_done && !g_last_marker_seen; // not in space: the starfield must not be locked to the camera
		update_snapshot_points();
		g_layer_fallback_last = g_layer_fallback;
		g_layer_fallback = 0;
		g_layer_found_last = (g_layer_ground_done ? 1u : 0u) | (g_layer_floor_done ? 2u : 0u) | (g_layer_entities_done ? 4u : 0u) |
			(g_layer_postcloud_done ? 8u : 0u) | (g_layer_world_done ? 16u : 0u) | (g_layer_airsplit_done ? 32u : 0u);
		g_layers_direct_last = g_layers_direct; // for bridge_frame, right after this
		g_layers_gen_last = g_layers_gen;
		g_layers_direct = false;
		if (g_layers_last_frame)
			++g_layer_frames;
		g_layer_ground_pending = false;
		g_layer_ground_done = false;
		g_layer_floor_pending = false;
		g_layer_floor_done = false;
		g_layer_entities_done = false;
		g_layer_postcloud_pending = false;
		g_layer_postcloud_done = false;
		g_layer_world_done = false;
		g_layer_airsplit_done = false;
		for (int k = 0; k < g_air_draw_count; ++k)
			g_air_draws_last[k] = g_air_draws[k];
		g_air_draw_count_last = g_air_draw_count;
		g_air_draw_count = 0;
		g_layer_rt = { 0 };
		g_layer_ground_seen = 0;
		g_extra_last.swap(g_extra_building);
		g_extra_building.clear();
		g_anchor_rt = { 0 };
		g_anchor_done = false;
		g_trace_active = g_trace_requested || g_dump_requested;
		g_dump_active = g_dump_requested;
		g_trace_requested = g_dump_requested = false;
		camera_probe_frame_end();
		return true;
	}

	void on_present(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
	{
		// Device check first: this event also fires for the bridge's own D3D12 swap chain, from inside bridge::present.
		if (g_device == nullptr || sc->get_device() != g_device)
			return;
		if (!present_locked(queue, sc))
			return;
		// The bridge runs outside the lock: its D3D12 calls make ReShade raise events into this add-on.
		bridge_frame(queue, sc);
	}

	// On-screen readout. ReShade raises this for every runtime every frame, even with its overlay closed. The bridge's
	// D3D12 swap chain has its own runtime, and its frames are the ones on screen, so the readout is drawn there
	// (after frame generation, so it stays sharp). Runs on whichever thread presents that swap chain; it only reads.
	// Shader compile notice: shown top left on whatever is on screen until the motion shaders are ready (first start
	// after an update), and permanently if they failed - also with the readout off.
	bool shader_notice(char *buf, size_t size)
	{
		int compiled = 0, total = 0;
		const motion::shader_state st = motion::shaders(compiled, total);
		if (st == motion::shader_state::ready)
			return false;
		if (st == motion::shader_state::failed)
			std::snprintf(buf, size, "Factorio FG: motion shaders failed to compile (see ReShade.log) - frame generation runs without motion vectors");
		else
			std::snprintf(buf, size, "Factorio FG: compiling motion shaders (%d/%d, once after an update, up to a minute) - frame generation runs\n"
				"without motion vectors until then", compiled, total);
		return true;
	}

	void on_reshade_overlay(effect_runtime *runtime)
	{
		if (g_device == nullptr)
			return;
		const bool bridge_up = g_bridge_enabled && bridge::active();
		const bool on_bridge = runtime->get_device() != g_device;
		char notice[256];
		const bool has_notice = shader_notice(notice, sizeof(notice));
		if (has_notice && on_bridge == bridge_up && !(g_osd && bridge_up))
		{
			// the runtime whose picture is on screen: the bridge's when frame generation runs, else Factorio's
			ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always, ImVec2(0, 0));
			ImGui::SetNextWindowBgAlpha(0.65f);
			const ImGuiWindowFlags nflags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
			if (ImGui::Begin("##fg_notice", nullptr, nflags))
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", notice);
			ImGui::End();
		}
		if (!g_osd || !g_bridge_enabled || runtime->get_device() == g_device || !bridge::active())
			return;
		const uint32_t shown = bridge::frames_per_present();
		float dx = 0, dy = 0, conf = 0;
		bridge::motion_global(dx, dy, conf);
		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always, ImVec2(0, 0));
		ImGui::SetNextWindowBgAlpha(0.55f);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if (ImGui::Begin("##fg_osd", nullptr, flags))
		{
			ImGui::Text("%s %ux%s | Factorio %.0f fps -> %.0f fps | v" ADDON_VERSION_SHOWN, bridge::backend_name(), shown, g_fg_multiplier == 0 ? " auto" : "",
				g_base_fps, g_base_fps * (shown != 0 ? shown : 1));
			ImGui::Text("Camera %+5.1f %+5.1f px | zoom %.3f | match %3.0f%% | MV %s", dx, dy, bridge::motion_zoom(), conf * 100.0f, g_motion_vectors ? "on" : "off");
			if (has_notice)
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", notice);
			{
				float cam = 0, still = 0, own = 0;
				bridge::motion_shares(cam, still, own);
				ImGui::Text("Blocks: camera %3.0f%% | still %3.0f%% | own %3.0f%%%s%s", cam * 100.0f, still * 100.0f, own * 100.0f,
					g_still_zone_active ? "" : " | map: no still", g_motion_debug ? " | DEBUG VIEW (frame generation paused)" : "");
			}
			// where this frame's HUD-less copy came from: without the rule, the GUI is in it and moves with the world
			if (g_last_matched_at >= 0)
				ImGui::Text("GUI capture: rule matched (draw %d)", g_last_matched_at);
			else
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "GUI capture: %s - the GUI is not kept out of frame generation",
					g_mode == boundary_mode::off ? "OFF (HUD-less capture tab: Use Factorio default rule)" : (g_last_auto_at >= 0 ? "fallback" : "none"));
			ImGui::Text("%s", bridge::fg_status()); // UI layer, character held / painted over, diagnostics
			if (const char *warning = capture_warning())
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", warning);
			if (setup::problems() > 0)
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Setup: %d problem(s), see the Frame generation tab", setup::problems());
			const bridge::timings t = bridge::get_timings();
			ImGui::Text("GPU motion %.2f ms (camera %.2f, blocks %.2f, rest %.2f) | cut check wait %.2f ms | bridge CPU %.2f ms | cuts %u",
				t.motion_gpu_ms, t.motion_camera_ms, t.motion_block_ms, t.motion_rest_ms, t.cut_wait_ms, t.present_cpu_ms, t.scene_cuts);
			ImGui::Text("Searched blocks %u | wide %u | movers %u", t.blocks_searched, t.blocks_wide, t.movers_found);
		}
		ImGui::End();
	}

	bool on_open_overlay(effect_runtime *runtime, bool open, input_source)
	{
		// ReShade also attaches a runtime to the bridge's own D3D12 swap chain; only Factorio's runtime counts here.
		if (g_device == nullptr || runtime->get_device() != g_device)
			return false;
		// No lock: ReShade raises this from its own GUI code on the present thread; taking g_mutex here could
		// nest inside another callback. The bridge and this flag are only touched from that thread.
		g_overlay_open = open;
		bridge::set_visible(!open); // the ReShade overlay is drawn on Factorio's own swap chain, under the bridge window
		return false;
	}

	bool is_modifier(uint32_t vk)
	{
		return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN;
	}

	std::string key_name(uint32_t vk, bool ctrl, bool shift, bool alt)
	{
		if (vk == 0)
			return "none";
		std::string name;
		if (ctrl) name += "Ctrl + ";
		if (shift) name += "Shift + ";
		if (alt) name += "Alt + ";
		if (vk >= VK_F1 && vk <= VK_F24)
			return name + "F" + std::to_string(vk - VK_F1 + 1);
		if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
			return name + static_cast<char>(vk);
		LONG scan = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
		switch (vk)
		{
		case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
			scan |= 1 << 24; // extended key
			break;
		default:
			break;
		}
		char buf[64] = {};
		if (GetKeyNameTextA(scan, buf, static_cast<int>(sizeof(buf))) > 0)
			return name + buf;
		char hex[16];
		std::snprintf(hex, sizeof(hex), "key 0x%02X", vk);
		return name + hex;
	}

	void on_reshade_present(effect_runtime *runtime)
	{
		// Both runtimes raise this; keys are only taken from Factorio's (the bridge window never has focus).
		if (g_device == nullptr || runtime->get_device() != g_device)
			return;
		if (runtime->is_key_pressed(DUMP_KEY))
		{
			std::lock_guard<app_mutex> lock(g_mutex);
			g_dump_requested = true;
		}
		const bool ctrl = runtime->is_key_down(VK_CONTROL), shift = runtime->is_key_down(VK_SHIFT), alt = runtime->is_key_down(VK_MENU);
		if (g_osd_key_capture)
		{
			const uint32_t k = runtime->last_key_pressed();
			if (k != 0 && !is_modifier(k) && runtime->is_key_pressed(k))
			{
				std::lock_guard<app_mutex> lock(g_mutex);
				g_osd_key_capture = false;
				if (k == VK_ESCAPE)
					return; // cancelled
				if (k == VK_BACK)
					g_osd_key = 0; // cleared
				else
				{
					g_osd_key = k;
					g_osd_key_ctrl = ctrl;
					g_osd_key_shift = shift;
					g_osd_key_alt = alt;
				}
				save_config();
			}
			return;
		}
		if (g_osd_key != 0 && runtime->is_key_pressed(g_osd_key) && ctrl == g_osd_key_ctrl && shift == g_osd_key_shift && alt == g_osd_key_alt)
		{
			std::lock_guard<app_mutex> lock(g_mutex);
			g_osd = !g_osd;
			save_config();
		}
	}

	// ---------------------------------------------------------------- overlay

	void draw_overlay(effect_runtime *)
	{
		std::lock_guard<app_mutex> lock(g_mutex);

		if (g_back_buffers.empty())
		{
			ImGui::TextUnformatted("No swap chain yet.");
			return;
		}

		ImGui::TextDisabled("Version " ADDON_VERSION_STRING " (" ADDON_VERSION_STAGE "), built %s", BUILD_STAMP);
		if (!ImGui::BeginTabBar("##probe_tabs", 0))
			return;
		ImGui::PushTextWrapPos(0.0f); // hints wrap at the window edge instead of being cut off

		if (ImGui::BeginTabItem("Frame generation", nullptr, 0))
		{
			ImGui::PushItemWidth(-ImGui::GetFontSize() * 13.0f); // sliders leave room for their labels
			bool changed = ImGui::Checkbox("Frame generation on (presents the game through D3D12)", &g_bridge_enabled);
			if (changed)
				g_bridge_failed = false;
			// Technology
			{
				ImGui::TextUnformatted("Technology:");
				ImGui::SameLine();
				int choice = g_fg_choice;
				ImGui::RadioButton("Automatic##tech", &choice, 0); ImGui::SameLine();
				ImGui::RadioButton("NVIDIA DLSS-G##tech", &choice, 1); ImGui::SameLine();
				ImGui::RadioButton("AMD FSR##tech", &choice, 2);
				if (choice != g_fg_choice)
				{
					g_fg_choice = choice;
					g_bridge_failed = false; // try again with the new choice
					changed = true;
				}
				const char *running = bridge::active() ? bridge::backend_name() : "none (frame generation off)";
				ImGui::TextDisabled("  In use: %s. Automatic: DLSS-G on NVIDIA RTX when installed, else FSR (any DirectX 12 GPU, 2x).", running);
				if (bridge::active() && g_fg_choice_applied >= 0 && g_fg_choice != g_fg_choice_applied)
					ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "  Restart Factorio to switch: the technology is chosen when frame generation starts.");
			}
			// What the running technology supports (nothing greyed out while frame generation is not running)
			const bool fsr_running = bridge::active() && std::strcmp(bridge::backend_name(), "FSR") == 0;
			const int max_mult = bridge::active() ? static_cast<int>(bridge::max_multiplier()) : 4;
			ImGui::TextUnformatted("Multiplier:");
			ImGui::SameLine();
			changed |= ImGui::RadioButton("Off##fg", &g_fg_multiplier, 1); ImGui::SameLine();
			changed |= ImGui::RadioButton("2x", &g_fg_multiplier, 2); ImGui::SameLine();
			ImGui::BeginDisabled(max_mult < 3);
			changed |= ImGui::RadioButton("3x", &g_fg_multiplier, 3); ImGui::SameLine();
			ImGui::EndDisabled();
			ImGui::BeginDisabled(max_mult < 4);
			changed |= ImGui::RadioButton("4x", &g_fg_multiplier, 4); ImGui::SameLine();
			ImGui::EndDisabled();
			changed |= ImGui::RadioButton("Auto", &g_fg_multiplier, 0);
			ImGui::SameLine();
			ImGui::TextDisabled("(Off = bridge only)");
			if (g_fg_multiplier == 0)
			{
				const int target = g_auto_target > 0 ? g_auto_target : g_display_hz;
				ImGui::TextDisabled("  Auto: display %d Hz, aiming for %d fps; Factorio %.1f fps -> %dx", g_display_hz, target, g_base_fps, g_auto_multiplier);
				changed |= ImGui::SliderInt("Auto target fps (0 = display refresh rate)", &g_auto_target, 0, 360, "%d", 0);
			}

			const uint32_t shown = bridge::frames_per_present();
			if (g_bridge_enabled && shown != 0)
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Factorio %.1f fps  ->  output about %.0f fps (%u frame%s per Factorio frame)",
					g_base_fps, g_base_fps * shown, shown, shown == 1 ? "" : "s");
			else
				ImGui::Text("Factorio %.1f fps", g_base_fps);

			ImGui::Separator();
			ImGui::TextWrapped("%s: %s", bridge::backend_name(), bridge::fg_status());
			if (bridge::max_multiplier() < 4 && bridge::active())
				ImGui::TextDisabled("  %s generates one frame per Factorio frame: 2x at most.", bridge::backend_name());
			changed |= ImGui::Checkbox("Keep the character sharp", &g_fsr_hold);
			ImGui::TextDisabled("  While you move, generated frames show the character from the newest Factorio frame (laid over them like the GUI) instead of a blend of two animation poses.");
			ImGui::BeginDisabled(!g_fsr_hold);
			changed |= ImGui::Checkbox("  ...and remove it from what frame generation blends", &g_fsr_inpaint);
			ImGui::EndDisabled();
			ImGui::TextDisabled("    Frame generation gets what is under the character (from the world snapshots) instead of the character, moving with the world: no copies of it, and much less noise where the ground behind it is uncovered.");
			if (!fsr_running && bridge::active())
				ImGui::TextDisabled("    DLSS-G: uses its UI recomposition; if this DLSS-G does not offer it, the DLSS-G line above shows no \"UI layer\" and these have no effect.");
			ImGui::TextWrapped("Bridge: %s%s", bridge::status(), g_bridge_failed ? " (init failed, see ReShade.log)" : "");
			if (bridge::active())
			{
				const bridge::timings t = bridge::get_timings();
				ImGui::Text("GPU motion passes %.2f ms (camera %.2f, blocks %.2f, rest %.2f) | scene-cut check wait %.2f ms | bridge CPU %.2f ms",
					t.motion_gpu_ms, t.motion_camera_ms, t.motion_block_ms, t.motion_rest_ms, t.cut_wait_ms, t.present_cpu_ms);
			}
			ImGui::Separator();
			changed |= ImGui::Checkbox("Scene-cut detection (map open/close, surface change, teleport)", &g_scene_cut);
			if (g_scene_cut)
			{
				changed |= ImGui::SliderFloat("Scene-cut factor", &g_scene_cut_factor, 1.5f, 20.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
				ImGui::TextDisabled("  A cut is a frame this many times more different than usual. Lower: more sensitive.");
				const bridge::timings t = bridge::get_timings();
				ImGui::Text("  Frame difference %.3f (usual %.3f) | cuts detected: %u", t.difference, t.difference_average, t.scene_cuts);
			}
			ImGui::Separator();
			changed |= ImGui::Checkbox("Tearing / VRR present (sync interval 0)", &g_bridge_tearing);
			changed |= ImGui::Checkbox("On-screen readout (top left of the game picture)", &g_osd);
			ImGui::SameLine();
			if (g_osd_key_capture)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key (with Ctrl / Shift / Alt if wanted). Esc cancels, Backspace clears.");
			}
			else
			{
				ImGui::TextDisabled("toggle: %s", key_name(g_osd_key, g_osd_key_ctrl, g_osd_key_shift, g_osd_key_alt).c_str());
				ImGui::SameLine();
				if (ImGui::Button("Set hotkey"))
					g_osd_key_capture = true;
			}
			if (changed)
				save_config();
			ImGui::TextDisabled("The frame-generated picture is hidden while this overlay is open. Close the overlay (Home) to see it.");

			if (const char *warning = capture_warning())
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", warning);

			ImGui::Separator();
			const int problems = setup::problems();
			if (ImGui::CollapsingHeader(problems > 0 ? "Setup check (problems found)###setup" : "Setup check###setup", problems > 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			{
				for (const setup::item &i : setup::items())
				{
					const ImVec4 colour = i.result == setup::state::ok ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
						(i.result == setup::state::warning ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f) : ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
					ImGui::TextColored(colour, "%s", i.result == setup::state::ok ? "OK  " : (i.result == setup::state::warning ? "WARN" : "FIX "));
					ImGui::SameLine();
					ImGui::TextWrapped("%s: %s", i.name.c_str(), i.detail.c_str());
				}
				if (ImGui::Button("Check again"))
					setup::run(g_module);
			}
			if (ImGui::Checkbox("Show developer tools (capture rule editor, frame trace, camera probe)", &g_dev_tools))
				save_config();
			ImGui::PopItemWidth();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Motion vectors", nullptr, 0))
		{
			ImGui::PushItemWidth(-ImGui::GetFontSize() * 13.0f);
			bool changed = ImGui::Checkbox("Motion vectors (estimated from the image)", &g_motion_vectors);
			ImGui::SameLine();
			changed |= ImGui::Checkbox("Flip sign (diagnostic)", &g_motion_flip);
			ImGui::Text("Motion: %s", bridge::active() ? bridge::motion_status() : "frame generation is off");
			changed |= ImGui::Checkbox("World layers (ground / floor / objects / air)", &g_world_layers);
			ImGui::SameLine();
			if (!g_world_layers)
				ImGui::TextDisabled("(off)");
			else if (g_layers_last_frame)
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "(all 6 snapshots taken, %llu frames%s%s)", static_cast<unsigned long long>(g_layer_frames),
					(g_layer_fallback_last & 1u) ? "; no floor pass: floor = objects" : "", (g_layer_fallback_last & 2u) ? "; no cloud pass: no air layer" : "");
			else
			{
				const uint32_t f = g_layer_found_last;
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "(snapshots incomplete: ground %s, floor %s, objects %s, after clouds %s, world %s, air split %s)",
					(f & 1u) ? "ok" : "MISSING", (f & 2u) ? "ok" : "MISSING", (f & 4u) ? "ok" : "MISSING", (f & 8u) ? "ok" : "MISSING",
					(f & 16u) ? "ok" : "MISSING", (f & 32u) ? "ok" : "MISSING");
			}
			if (g_use_learned)
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "  Snapshot points found from the frame structure (the built-in shaders are not drawn: Factorio "
					"update?): ground PS %08X #%d, floor %08X, clouds %08X, composite %08X.", g_act_ground_ps, g_act_ground_occ,
					g_act_has_floor ? g_act_floor_ps : 0u, g_act_has_cloud ? g_act_cloud_ps : 0u, g_act_world_ps);
			else
			{
				const bool match = g_learned.valid && g_learned.ground_ps == g_layer_ground_ps && g_learned.ground_occ == g_layer_ground_occ &&
					(!g_learned.has_floor || g_learned.floor_ps == LAYER_FLOOR_AFTER_PS) && (!g_learned.has_cloud || g_learned.cloud_ps == LAYER_ENTITIES_BEFORE_PS) &&
					g_learned.composite_ps == LAYER_WORLD_BEFORE_PS;
				ImGui::TextDisabled("  Snapshot points: built-in (ground after PS %08X occurrence %d). Structure check: %s", g_layer_ground_ps, g_layer_ground_occ,
					!g_learned.valid ? "not run yet" : (match ? "finds the same points" : "finds different points (only used if the built-in ones disappear)"));
			}
			// Air split: icons on one side (camera motion), flying robots on the other (motion search). Collapsed, and
			// the shader choice in a combo box, so the air stage's changing draws do not move the settings below.
			{
				// shaders seen in the air stage (kept, so the list does not change from frame to frame)
				for (int k = 0; k < g_air_draw_count_last; ++k)
				{
					int j = 0;
					while (j < g_air_seen_count && g_air_seen[j].ps != g_air_draws_last[k].ps)
						++j;
					if (j < g_air_seen_count)
						g_air_seen[j].count = g_air_draws_last[k].count;
					else if (g_air_seen_count < AIR_SEEN_SLOTS)
						g_air_seen[g_air_seen_count++] = g_air_draws_last[k];
				}
				char header[96];
				std::snprintf(header, sizeof(header), "Air split: alt-mode icons / flying robots (split before PS %08X%s)###airsplit", g_air_split_ps,
					g_air_split_ps == DEFAULT_AIR_SPLIT_PS ? ", default" : "");
				if (ImGui::CollapsingHeader(header, 0))
				{
					ImGui::TextDisabled("Check in Debug view > World layers: alt-mode icons cyan, flying robots yellow. The wrong way round: flip the "
						"checkbox. Both one colour: split before another shader of the air stage.");
					changed |= ImGui::Checkbox("Alt-mode icons are drawn before the split", &g_air_icons_first);
					char preview[48];
					std::snprintf(preview, sizeof(preview), "%08X%s", g_air_split_ps, g_air_split_ps == DEFAULT_AIR_SPLIT_PS ? " (default)" : "");
					if (ImGui::BeginCombo("Split before shader##air", preview, 0))
					{
						if (ImGui::Selectable("D82AC825 (default)##airdef", g_air_split_ps == DEFAULT_AIR_SPLIT_PS, 0, ImVec2(0, 0)))
						{
							g_air_split_ps = DEFAULT_AIR_SPLIT_PS;
							changed = true;
						}
						for (int k = 0; k < g_air_seen_count; ++k)
						{
							bool now = false;
							for (int j = 0; j < g_air_draw_count_last; ++j)
								now = now || g_air_draws_last[j].ps == g_air_seen[k].ps;
							char label[64];
							std::snprintf(label, sizeof(label), "%08X  (%d draws last frame)##air%d", g_air_seen[k].ps, now ? g_air_seen[k].count : 0, k);
							if (ImGui::Selectable(label, g_air_split_ps == g_air_seen[k].ps, 0, ImVec2(0, 0)))
							{
								g_air_split_ps = g_air_seen[k].ps;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
				}
			}
			changed |= ImGui::Checkbox("Mover search", &g_mover_search);
			ImGui::TextDisabled("  Finds trains, cars, bots by searching each block; off = only follow motion found earlier, as before 0.9.");
			changed |= ImGui::Checkbox("Wide search for fast movers", &g_wide_search);
			ImGui::TextDisabled("  Up to 192 px per frame instead of 48, where the normal search finds nothing: fast trains.");
			ImGui::TextDisabled("  Depth for DLSS-G comes from the world layers. %s", bridge::depth_status());
			if (ImGui::CollapsingHeader("Fine tuning", 0))
			{
			changed |= ImGui::SliderFloat("Zero-motion penalty", &g_motion_penalty_zero, 0.0f, 2.0f, "%.2f", 0);
			ImGui::TextDisabled("  Higher: the character must match 'still' more clearly before it gets zero motion.");
			changed |= ImGui::SliderFloat("No-match threshold", &g_motion_mismatch, 0.1f, 32.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
			ImGui::TextDisabled("  Lower: more blocks fall back to camera motion.");
			changed |= ImGui::SliderFloat("Stability", &g_motion_hysteresis, 0.0f, 2.0f, "%.2f", 0);
			ImGui::TextDisabled("  Higher: blocks keep last frame's kind of motion (camera / still / own) more stubbornly. Less shimmer; too high and moving things lag behind.");
			changed |= ImGui::SliderFloat("Own-motion strictness", &g_motion_strictness, 0.0f, 0.95f, "%.2f", 0);
			ImGui::TextDisabled("  New own motion must match this much better than the camera (0.5 = half the difference). Higher: animations (water, trees, smoke, machines) stop getting bogus motion; too high and things that start moving are picked up a frame or two later.");
			changed |= ImGui::SliderFloat("Repeating-pattern guard", &g_motion_far_penalty, 0.0f, 0.3f, "%.3f", 0);
			ImGui::TextDisabled("  Own motion pays this per pixel it differs from the camera. On repeating patterns (module rows, belts, assembler grids) a shift by one repeat matches as well as the right answer; higher stops those ghost copies. Too high and fast trains or cars fall back to the camera.");
			changed |= ImGui::SliderFloat("Still zone", &g_motion_still_zone, 0.05f, 1.2f, "%.2f", 0);
			ImGui::TextDisabled("  Radius around the screen centre (in screen heights) where things may be 'still': the character or vehicle the camera follows. Outside it, identical things that moved exactly one spacing (module icons, belt items) can not be mistaken for still. Shown as a circle in the debug view.");
			if (ImGui::Button("Reset to defaults"))
			{
				g_motion_penalty_zero = DEFAULT_MOTION_PENALTY_ZERO;
				g_motion_mismatch = DEFAULT_MOTION_MISMATCH;
				g_motion_hysteresis = DEFAULT_MOTION_HYSTERESIS;
				g_motion_strictness = DEFAULT_MOTION_STRICTNESS;
				g_motion_far_penalty = DEFAULT_MOTION_FAR_PENALTY;
				g_motion_still_zone = DEFAULT_MOTION_STILL_ZONE;
				changed = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(%.2f / %.2f / %.2f / %.2f / %.3f / %.2f)", DEFAULT_MOTION_PENALTY_ZERO, DEFAULT_MOTION_MISMATCH, DEFAULT_MOTION_HYSTERESIS,
				DEFAULT_MOTION_STRICTNESS, DEFAULT_MOTION_FAR_PENALTY, DEFAULT_MOTION_STILL_ZONE);
			}
			if (ImGui::CollapsingHeader(g_motion_debug || g_fsr_debug || g_fsr_ui_layer || g_fsr_only_generated ? "Debug views (ON)###dbg" : "Debug views###dbg", 0))
			{
			ImGui::Checkbox("Debug view (replaces the game picture; frame generation pauses so you see exactly this frame)", &g_motion_debug);
			ImGui::RadioButton("Motion kinds", &g_debug_mode, 0);
			ImGui::SameLine();
			ImGui::RadioButton("World layers / depth", &g_debug_mode, 1);
			ImGui::TextUnformatted("Snapshots:");
			ImGui::SameLine();
			ImGui::RadioButton("Ground", &g_debug_mode, 2);
			ImGui::SameLine();
			ImGui::RadioButton("Floor", &g_debug_mode, 3);
			ImGui::SameLine();
			ImGui::RadioButton("Entities", &g_debug_mode, 4);
			ImGui::SameLine();
			ImGui::RadioButton("World with air", &g_debug_mode, 5);
			ImGui::SameLine();
			ImGui::RadioButton("Air split", &g_debug_mode, 6);
			ImGui::TextDisabled("  World layers: dark = ground (far, always camera motion), blue = floor: belts, pipes, shadows, white = objects: buildings, character, vehicles, cyan = alt-mode icons (camera motion unless that clearly does not match), yellow = flying: robots, effects (nearest).");
			ImGui::TextDisabled("  No tint: moves with the camera. Blue: still (the character). Other colours: own motion, colour = direction. Tints that flicker on things that do not move are what shimmers. Close the overlay to see it. Not saved: off after a restart.");
			{
				float cam = 0, still = 0, own = 0;
				bridge::motion_shares(cam, still, own);
				ImGui::Text("Blocks: camera %.0f%% | still %.0f%% | own motion %.0f%%", cam * 100.0f, still * 100.0f, own * 100.0f);
			}
			{
				const bool fsr_on = bridge::active() && std::strcmp(bridge::backend_name(), "FSR") == 0;
				ImGui::BeginDisabled(!fsr_on);
				ImGui::Checkbox("FSR debug view (AMD's own)", &g_fsr_debug);
				ImGui::TextDisabled("  Shows only the generated frames, with FSR's internal pictures (motion, disocclusion, ...) drawn over them. Not saved.");
				ImGui::EndDisabled();
				ImGui::Checkbox("Show the GUI layer", &g_fsr_ui_layer);
				ImGui::TextDisabled("  The world turns black; only what the add-on took for GUI (and the held character) stays. That layer is laid over every shown frame, so it should stay sharp when panning. Not saved.");
				ImGui::BeginDisabled(!fsr_on);
				ImGui::Checkbox("Show generated frames only (FSR)", &g_fsr_only_generated);
				ImGui::TextDisabled("  Only FSR's in-between frames reach the screen (60 fps, choppy): a screenshot then shows a generated frame. Not saved.");
				ImGui::EndDisabled();
			}
			}
			if (changed)
				save_config();
			ImGui::PopItemWidth();
			ImGui::EndTabItem();
		}

		if (!g_dev_tools && ImGui::BeginTabItem("HUD-less capture", nullptr, 0))
		{
			// Compact view: the built-in Factorio rule, its status, and the preview. The rule editor and frame trace
			// are developer tools (Frame generation tab).
			ImGui::TextWrapped("The game picture is copied right before Factorio draws its GUI; DLSS-G uses that copy to keep the GUI sharp.");
			if (g_last_matched_at >= 0)
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Capture rule matched (draw %d)", g_last_matched_at);
			else if (g_last_auto_at >= 0)
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Fallback in use (normal in map view)");
			else
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "No capture this frame");
			if (ImGui::Button("Use Factorio default rule"))
			{
				g_mode = boundary_mode::after_anchor;
				g_anchor_ps = FACTORIO_DEFAULT_ANCHOR_PS;
				g_anchor_occurrence = 1;
				g_auto_fallback = true;
				g_anchor_extra = 1;
				g_extra_needs_marker = true;
				save_config();
			}
			ImGui::SameLine();
			if (ImGui::Button("Dump images (F10)"))
				g_dump_requested = true;
			if (!g_last_status.empty())
				ImGui::TextWrapped("%s", g_last_status.c_str());
			if (g_preview_srv.handle != 0 && ImGui::CollapsingHeader("Preview (should show no GUI)", 0))
			{
				g_preview_requested_frame = g_frame_counter;
				const float avail = std::max(64.0f, ImGui::GetWindowWidth() - 2.0f * ImGui::GetStyle().WindowPadding.x);
				const float aspect = static_cast<float>(g_bb_desc.texture.height) / std::max(1u, g_bb_desc.texture.width);
				ImGui::Image(ImTextureRef(g_preview_srv.handle), ImVec2(avail, avail * aspect), ImVec2(0, 0), ImVec2(1, 1));
			}
			ImGui::EndTabItem();
		}

		if (g_dev_tools && ImGui::BeginTabItem("HUD-less capture", nullptr, 0))
		{
			bool changed = false;
			int mode = static_cast<int>(g_mode);
			changed |= ImGui::RadioButton("Off", &mode, 0); ImGui::SameLine();
			changed |= ImGui::RadioButton("Shader rule", &mode, 1); ImGui::SameLine();
			changed |= ImGui::RadioButton("Draw index (scrub)", &mode, 2); ImGui::SameLine();
			changed |= ImGui::RadioButton("Back-buffer draw", &mode, 3); ImGui::SameLine();
			changed |= ImGui::RadioButton("After anchor", &mode, 4); ImGui::SameLine();
			changed |= ImGui::RadioButton("Anchor -> GUI", &mode, 5);
			g_mode = static_cast<boundary_mode>(mode);

			if (g_mode == boundary_mode::shader_rule)
			{
				ImGui::Text("GUI starts at occurrence %d of pixel shader %08X", g_rule_occurrence, g_rule_ps);
				changed |= ImGui::SliderInt("Occurrence", &g_rule_occurrence, 1, 64, "%d", 0);
			}
			else if (g_mode == boundary_mode::draw_index)
			{
				const int max_index = std::max<int>(1, g_last_draw_count) - 1;
				changed |= ImGui::SliderInt("Copy before draw", &g_rule_index, 0, max_index, "%d", 0);
				changed |= ImGui::InputInt("Fine (+/-1, Ctrl +/-100)", &g_rule_index, 1, 100, 0);
				g_rule_index = std::clamp(g_rule_index, 0, max_index);
				ImGui::TextDisabled("Find the highest draw where the world is complete and no GUI shows.");
				if (!g_trace.empty() && ImGui::Button("Suggest anchor at or before this draw"))
				{
					// Walk back from the scrub point to the nearest draw whose pixel shader runs once per frame.
					for (int i = std::min<int>(g_rule_index, static_cast<int>(g_trace.size())) - 1; i >= 0; --i)
					{
						if (g_trace_ps_uses[i] == 1 && g_trace[i].ps != 0 && g_trace[i].ps != 0xFFFFFFFFu)
						{
							g_selected_row = i;
							g_mode = boundary_mode::after_anchor;
							g_anchor_ps = g_trace[i].ps;
							g_anchor_occurrence = 1;
							changed = true;
							break;
						}
					}
				}
			}
			else if (g_mode == boundary_mode::anchor_to_gui)
			{
				ImGui::Text("After PS %08X (occurrence %d), copy before the first draw with PS %08X", g_anchor_ps, g_anchor_occurrence, g_gui_ps);
				if (g_gui_strict)
					ImGui::Text("  ...that also has VS %08X and blend %08X", g_gui_vs, g_gui_blend);
				changed |= ImGui::Checkbox("Strict GUI match (VS + PS + blend)", &g_gui_strict);
				ImGui::TextDisabled("Capture a trace, then click rows after the ANCHOR row. Each click makes that row's\nshader the GUI start and the preview updates live. Pick the first row where the GUI\nis gone but the dust is still there.");
			}
			else if (g_mode == boundary_mode::after_anchor)
			{
				ImGui::Text("Copy right after occurrence %d of pixel shader %08X", g_anchor_occurrence, g_anchor_ps);
				changed |= ImGui::SliderInt("Anchor occurrence", &g_anchor_occurrence, 1, 16, "%d", 0);
				changed |= ImGui::SliderInt("Include draws after anchor", &g_anchor_extra, 0, 8, "%d", 0);
				changed |= ImGui::Checkbox("Only in space (space background pass ran this frame)", &g_extra_needs_marker);
				ImGui::SameLine();
				if (g_last_marker_seen)
					ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[space: yes]");
				else
					ImGui::TextDisabled("[space: no]");
				if (g_anchor_extra > 0)
				{
					std::string list;
					for (const auto &e : g_extra_last)
						list += "#" + std::to_string(e.first) + " (" + std::to_string(e.second) + "i)  ";
					ImGui::TextDisabled("Included last frame: %s", list.empty() ? "none" : list.c_str());
				}
				ImGui::TextDisabled("Pick an anchor in the frame trace: the last once-per-frame pass before the GUI (e.g. lighting).");
			}
			else if (g_mode == boundary_mode::back_buffer_draw)
			{
				const int max_bb = std::max<int>(1, g_last_bb_draw_count);
				changed |= ImGui::SliderInt("Copy before back-buffer draw", &g_rule_bb_index, 0, max_bb, "%d", 0);
				changed |= ImGui::InputInt("Fine##bb", &g_rule_bb_index, 1, 10, 0);
				g_rule_bb_index = std::clamp(g_rule_bb_index, 0, max_bb);
				ImGui::TextDisabled("Counts only draws into the back buffer. 0 = before the first one; the max means 'at present'.");
			}
			if (g_mode == boundary_mode::after_anchor || g_mode == boundary_mode::anchor_to_gui)
				changed |= ImGui::Checkbox("Fallback: if the anchor is missing, use the last full-screen pass", &g_auto_fallback);
			if (ImGui::Button("Use Factorio default rule"))
			{
				g_mode = boundary_mode::after_anchor;
				g_anchor_ps = FACTORIO_DEFAULT_ANCHOR_PS;
				g_anchor_occurrence = 1;
				g_auto_fallback = true;
				g_anchor_extra = 1;
				g_extra_needs_marker = true;
				changed = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(after post pass %08X, +1 draw in space, with fallback)", FACTORIO_DEFAULT_ANCHOR_PS);
			if (changed)
				save_config();

			if (g_mode != boundary_mode::off)
			{
				if (g_last_matched_at >= 0)
					ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Matched at draw %d, copied from %s  (streak %u frames)", g_last_matched_at,
						g_last_capture_from_offscreen ? "offscreen target" : "back buffer", g_match_streak);
				else if (g_last_auto_at >= 0)
					ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Anchor not found: fallback after full-screen pass at draw %d  (%u frames)", g_last_auto_at, g_miss_streak);
				else
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "No match this frame  (miss streak %u frames)", g_miss_streak);
			}


			if (ImGui::Button("Capture frame trace"))
				g_trace_requested = true;
			ImGui::SameLine();
			if (ImGui::Button("Dump images (F10)"))
				g_dump_requested = true;
			if (!g_last_status.empty())
				ImGui::TextWrapped("%s", g_last_status.c_str());


			if (g_preview_srv.handle != 0 && ImGui::CollapsingHeader("HUD-less preview", ImGuiTreeNodeFlags_DefaultOpen))
			{
				g_preview_requested_frame = g_frame_counter;
				if (g_preview_alpha_max >= 0)
					ImGui::TextDisabled("%s, alpha ignored (back buffer alpha max: %d)", g_preview_crop ? "Centre crop 1:1" : "Half resolution", g_preview_alpha_max);
				ImGui::Checkbox("1:1 centre crop (shows thin details like dust)", &g_preview_crop);
				ImGui::TextUnformatted("Preview:");
				ImGui::SameLine();
				ImGui::RadioButton("HUD-less##src", &g_preview_source, 0);
				ImGui::SameLine();
				ImGui::RadioButton("Ground snapshot##src", &g_preview_source, 1);
				ImGui::SameLine();
				ImGui::RadioButton("Floor snapshot##src", &g_preview_source, 3);
				ImGui::SameLine();
				ImGui::RadioButton("Entity snapshot##src", &g_preview_source, 2);
				ImGui::SameLine();
				ImGui::RadioButton("World (with air)##src", &g_preview_source, 4);
				if (g_preview_source != 0)
					ImGui::TextDisabled("Ground snapshot after PS %08X occurrence %d. Pick another point in the frame trace below: the last\n"
						"draw where the ground snapshot shows only tiles (no belts, pipes, inserters, shadows).", g_layer_ground_ps, g_layer_ground_occ);
				const float avail = std::max(64.0f, ImGui::GetWindowWidth() - 2.0f * ImGui::GetStyle().WindowPadding.x); // GetContentRegionAvail returns a struct (ABI hazard)
				const float aspect = static_cast<float>(g_bb_desc.texture.height) / std::max(1u, g_bb_desc.texture.width);
				ImGui::Image(ImTextureRef(g_preview_srv.handle), ImVec2(avail, avail * aspect), ImVec2(0, 0), ImVec2(1, 1));
			}


			if (!g_trace.empty() && ImGui::CollapsingHeader("Frame trace", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (g_selected_row >= 0 && g_selected_row < static_cast<int>(g_trace.size()))
				{
					const draw_record &sel = g_trace[g_selected_row];
					int occurrence = 0;
					for (int i = 0; i <= g_selected_row; ++i)
						occurrence += (g_trace[i].ps == sel.ps);
					ImGui::Text("Selected draw %u: PS %08X, occurrence %d", sel.index, sel.ps, occurrence);
					if (sel.rt_is_back_buffer)
					{
						int bb_ordinal = 0;
						for (int i = 0; i < g_selected_row; ++i)
							bb_ordinal += g_trace[i].rt_is_back_buffer ? 1 : 0;
						ImGui::SameLine();
						if (ImGui::Button("Use as back-buffer draw"))
						{
							g_mode = boundary_mode::back_buffer_draw;
							g_rule_bb_index = bb_ordinal;
							save_config();
						}
					}
					ImGui::SameLine();
					if (ImGui::Button("Ground snapshot after this draw"))
					{
						g_layer_ground_ps = sel.ps;
						g_layer_ground_occ = occurrence;
						g_preview_source = 1;
						save_config();
					}
					ImGui::SameLine();
					if (ImGui::Button("Copy after this draw (anchor)"))
					{
						if (g_mode != boundary_mode::anchor_to_gui)
							g_mode = boundary_mode::after_anchor;
						g_anchor_ps = sel.ps;
						g_anchor_occurrence = occurrence;
						save_config();
					}
					ImGui::SameLine();
					if (ImGui::Button("Use as GUI start"))
					{
						g_mode = boundary_mode::shader_rule;
						g_rule_ps = sel.ps;
						g_rule_occurrence = occurrence;
						g_rule_index = static_cast<int>(sel.index);
						save_config();
					}
				}

				const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
				if (ImGui::BeginTable("trace", 8, flags, ImVec2(0, 320), 0.0f))
				{
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("#", 0, 0.0f, 0);
					ImGui::TableSetupColumn("Target", 0, 0.0f, 0);
					ImGui::TableSetupColumn("VS", 0, 0.0f, 0);
					ImGui::TableSetupColumn("PS", 0, 0.0f, 0);
					ImGui::TableSetupColumn("Blend", 0, 0.0f, 0);
					ImGui::TableSetupColumn("Count", 0, 0.0f, 0);
					ImGui::TableSetupColumn("Inst", 0, 0.0f, 0);
					ImGui::TableSetupColumn("PS uses", 0, 0.0f, 0);
					ImGui::TableHeadersRow();

					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(g_trace.size()), -1.0f);
					while (clipper.Step())
					{
						for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
						{
							const draw_record &r = g_trace[i];
							const bool switches_target = (i > 0 && g_trace[i - 1].rt != r.rt);
							ImGui::TableNextRow(0, 0.0f);
							ImGui::TableSetColumnIndex(0);
							char label[32];
							std::snprintf(label, sizeof(label), "%u", r.index);
							if (ImGui::Selectable(label, g_selected_row == i, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0)))
							{
								g_selected_row = i;
								g_rule_index = static_cast<int>(r.index);
								if (g_mode == boundary_mode::anchor_to_gui)
								{
									g_gui_ps = r.ps; g_gui_vs = r.vs; g_gui_blend = r.blend;
									save_config();
								}
							}
							ImGui::TableSetColumnIndex(1);
							if (r.rt_is_back_buffer)
								ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%sBACKBUFFER", switches_target ? "> " : "");
							else
								ImGui::Text("%s%ux%u f%u", switches_target ? "> " : "", r.rt_width, r.rt_height, static_cast<uint32_t>(r.rt_format));
							ImGui::TableSetColumnIndex(2); ImGui::Text("%08X", r.vs);
							ImGui::TableSetColumnIndex(3);
							if (r.ps == g_anchor_ps && g_anchor_ps != 0)
								ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%08X ANCHOR", r.ps);
							else if (g_mode == boundary_mode::anchor_to_gui && r.ps == g_gui_ps && g_gui_ps != 0)
								ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.8f, 1.0f), "%08X GUI", r.ps);
							else
								ImGui::Text("%08X", r.ps);
							ImGui::TableSetColumnIndex(4); ImGui::Text("%08X", r.blend);
							ImGui::TableSetColumnIndex(5); ImGui::Text("%u%s", r.count, r.indexed ? "i" : "");
							ImGui::TableSetColumnIndex(6); ImGui::Text("%u", r.instances);
							ImGui::TableSetColumnIndex(7);
							if (g_trace_ps_uses[i] == 1)
								ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "1 (anchor?)");
							else
								ImGui::Text("%u", g_trace_ps_uses[i]);
						}
					}
					clipper.End();
					ImGui::EndTable();
				}
			}
			ImGui::EndTabItem();
		}

		if (g_dev_tools && ImGui::BeginTabItem("Diagnostics", nullptr, 0))
		{
			ImGui::Text("Back buffer: %ux%u, format %u", g_bb_desc.texture.width, g_bb_desc.texture.height, static_cast<uint32_t>(g_bb_desc.texture.format));
			ImGui::Text("Draws last frame: %u   into back buffer: %u (first at draw %d)", g_last_draw_count, g_last_bb_draw_count, g_last_first_bb_draw);
			if (g_last_first_bb_draw < 0)
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Nothing is drawn into the back buffer directly: the frame is built offscreen.\nUse Draw index; captures come from the bound screen-sized target.");


			if (ImGui::CollapsingHeader("Camera constant-buffer probe (step 3a)", 0))
			{
				ImGui::Checkbox("Track vertex-shader constant buffers", &g_cam_probe);
				if (g_cam_probe)
				{
					ImGui::SameLine();
					if (g_cam_recording)
						ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "recording... %d s left",
							static_cast<int>((g_cam_record_end - std::min(g_cam_record_end, g_frame_counter)) / 60));
					else if (ImGui::Button("Record 10 s"))
						start_camera_recording();
					ImGui::TextDisabled("Each row: first draw that bound it, VS hash, target size, VS slot, first 16 floats.");
					int shown = 0;
					for (const cam_entry &e : g_cam_last)
					{
						if (shown++ >= 12)
							break;
						ImGui::Text("#%u VS %08X %ux%u b%u", e.draw, e.vs, e.rt_w, e.rt_h, e.slot);
						char line[512];
						int n = 0;
						for (int i = 0; i < 16 && n < static_cast<int>(sizeof(line)) - 16; ++i)
							n += std::snprintf(line + n, sizeof(line) - n, "%9.4g ", e.data.f[i]);
						ImGui::TextUnformatted(line);
					}
				}
			}

			ImGui::EndTabItem();
		}

		ImGui::PopTextWrapPos();
		ImGui::EndTabBar();
	}
}

#ifdef FG_RELEASE
#define ADDON_DISPLAY_NAME "Factorio Frame Generation"
#define ADDON_OTHER_FILE L"hudless_probe.addon64"
#else
#define ADDON_DISPLAY_NAME "Factorio HUD-less Probe"
#define ADDON_OTHER_FILE L"factorio_fg.addon64"
#endif

extern "C" __declspec(dllexport) const char *NAME = ADDON_DISPLAY_NAME;
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Frame generation for Factorio: HUD-less capture, D3D12 bridge, NVIDIA DLSS-G (Streamline) or AMD FSR, image-based motion vectors. Version " ADDON_VERSION_STRING " (" ADDON_VERSION_STAGE ").";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// The developer and release builds must not both run: they would both hook Factorio and both create a bridge.
		if (GetModuleHandleW(ADDON_OTHER_FILE) != nullptr)
		{
			OutputDebugStringW(L"Factorio frame generation: the other build of this add-on is already loaded; this one stays off.\n");
			return FALSE;
		}
		if (!reshade::register_addon(module))
			return FALSE;
		g_module = module;
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
		reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
		reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
		reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
		reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
		reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_overlay(ADDON_DISPLAY_NAME, draw_overlay);
		log_info("HUD-less probe " ADDON_VERSION_STRING " (" ADDON_VERSION_STAGE ", built %s) loaded", BUILD_STAMP);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
