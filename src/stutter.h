// Stutter monitor (1.2): finds frames that took much longer than usual and says how much of that was the add-on.
//
// Per Factorio frame (at its present): the time since the last present, and the add-on's CPU time on Factorio's render
// thread that went into it - the work at the last present (mask at present, captures, the bridge with its scene-cut
// wait) and, while this frame was drawn, recording draws and copying their vertices. A frame over twice the usual
// time (and 10 ms over it) is a stutter. Each is logged with the breakdown (ReShade.log and hudless_probe\stutters.txt,
// which survives a restart as stutters_previous.txt) and counted for the on-screen readout.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace stutter
{
	using log_fn = void (*)(const char *message);

	struct frame_info
	{
		// the add-on's work at the previous present (it delays this frame)
		float present_ms = 0;      // our whole present handler, bridge included
		float bridge_ms = 0;       // ... of it the bridge (D3D12 copy, motion, frame generation)
		float cut_wait_ms = 0;     // ... of that the wait for the scene-cut check
		float alloc_wait_ms = 0;   // ... the wait for a free command allocator (the GPU 3 frames behind)
		float present_call_ms = 0; // ... inside the swap chain's Present (frame generation and its pacing)
		float mask_end_ms = 0;     // ... the mask at present
		float mask_dump_ms = 0;    // ... writing a mask dump
		// while this frame was drawn
		float mask_draw_ms = 0;    // recording draws, copying vertices
		uint32_t mask_draws = 0, mask_own_maps = 0;
		float mask_vertex_kb = 0;
		bool mask_copy_all = false, mask_dumped = false, mask_valid = true;
		bool scene_cut = false, multiplier_changed = false;
		int multiplier = 1;
		float base_fps = 0;
	};

	void set_output(const std::wstring &dir, log_fn log); // directory for stutters.txt
	// Once per Factorio present, with the time since the last one.
	void frame(double interval_ms, const frame_info &f);
	// On-screen readout line; 'addon_ms': the add-on's time on the render thread last frame.
	void readout(char *buf, size_t size);
	void flush(); // write pending lines (also done about once a second)
}
