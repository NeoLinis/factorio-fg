// Finding the world layer snapshot points from the structure of a frame, without shader hashes.
//
// The built-in points are pixel shader hashes (tile sprites 7C158CCA, floor pass BCD79626, cloud pass 703619E0,
// composite 575CE4FC). A Factorio update that recompiles those shaders changes the hashes. The structure of the frame
// stays: the world is drawn into one full-size target with most of the frame's draws; the floor and cloud passes are
// full-screen triangles (3 vertices, not indexed) on that target, early and late; the composite is the first
// full-screen triangle on another full-size target after the world is done; the tiles are drawn first, with the
// sprite shader that draws most of the world. This file reads those roles off one frame's draw list.
//
// Header-only and free of Windows / ReShade types, so it can be tested on its own against a frame trace.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace layer_learn
{
	struct draw
	{
		uint32_t ps = 0;
		uint64_t rt = 0;          // render target (any unique id)
		uint32_t count = 0;       // vertices or indices
		uint32_t instances = 0;
		bool indexed = false;
		bool full = false;        // the target has the screen's size
		bool to_bb = false;       // the target is the back buffer
	};

	struct points
	{
		bool valid = false;
		uint32_t ground_ps = 0;   // ground snapshot after occurrence 'ground_occ' of this shader (counted over all draws)
		int ground_occ = 0;
		bool has_floor = false;   // floor snapshot after the full-screen pass 'floor_ps' (none: floor = ground)
		uint32_t floor_ps = 0;
		bool has_cloud = false;   // objects snapshot before the full-screen pass 'cloud_ps' (none: taken at the composite)
		uint32_t cloud_ps = 0;
		uint32_t composite_ps = 0; // world snapshot right before this draw (on another target)

		bool same(const points &o) const
		{
			return valid == o.valid && ground_ps == o.ground_ps && ground_occ == o.ground_occ && has_floor == o.has_floor &&
				floor_ps == o.floor_ps && has_cloud == o.has_cloud && cloud_ps == o.cloud_ps && composite_ps == o.composite_ps;
		}
	};

	inline bool full_screen_pass(const draw &d)
	{
		return !d.indexed && d.count == 3 && d.instances == 1 && d.full && !d.to_bb;
	}

	inline points analyse(const std::vector<draw> &f)
	{
		points p;
		if (f.size() < 16)
			return p;

		// The world target: the full-size target (not the back buffer) with the most draws.
		std::unordered_map<uint64_t, int> per_rt;
		for (const draw &d : f)
			if (d.full && !d.to_bb && d.rt != 0)
				++per_rt[d.rt];
		uint64_t world = 0;
		int most = 0;
		for (const auto &kv : per_rt)
			if (kv.second > most)
			{
				most = kv.second;
				world = kv.first;
			}
		if (most < 16)
			return p;
		int first_w = -1, last_w = -1;
		for (int i = 0; i < static_cast<int>(f.size()); ++i)
			if (f[i].rt == world)
			{
				if (first_w < 0)
					first_w = i;
				last_w = i;
			}

		// The composite: the first full-screen pass on another full-size target after the world is done.
		int composite = -1;
		for (int i = last_w + 1; i < static_cast<int>(f.size()); ++i)
			if (f[i].rt != world && full_screen_pass(f[i]))
			{
				composite = i;
				break;
			}
		if (composite < 0)
			return p;

		// The tiles: the sprite shader that draws most of the world (indexed draws on the world target); the ground
		// snapshot follows its first draw there.
		std::unordered_map<uint32_t, int> per_ps;
		for (int i = first_w; i <= last_w; ++i)
			if (f[i].rt == world && f[i].indexed)
				++per_ps[f[i].ps];
		uint32_t sprite_ps = 0;
		int uses = 0;
		for (const auto &kv : per_ps)
			if (kv.second > uses)
			{
				uses = kv.second;
				sprite_ps = kv.first;
			}
		int first_tile = -1;
		for (int i = first_w; i <= last_w; ++i)
			if (f[i].rt == world && f[i].ps == sprite_ps && f[i].indexed)
			{
				first_tile = i;
				break;
			}
		if (first_tile < 0)
			return p;
		int occurrence = 0;
		for (int i = 0; i <= first_tile; ++i)
			occurrence += f[i].ps == sprite_ps ? 1 : 0;

		// Full-screen passes on the world target after the first tiles: in the first half of the world's draws the
		// floor pass, in the second half the cloud pass.
		int floor_at = -1, cloud_at = -1;
		const int span = last_w - first_w > 0 ? last_w - first_w : 1;
		for (int i = first_tile + 1; i <= last_w; ++i)
		{
			if (f[i].rt != world || !full_screen_pass(f[i]))
				continue;
			if (2 * (i - first_w) < span)
			{
				if (floor_at < 0)
					floor_at = i;
			}
			else
				cloud_at = i; // the last one
		}

		p.ground_ps = sprite_ps;
		p.ground_occ = occurrence;
		p.has_floor = floor_at >= 0;
		p.floor_ps = floor_at >= 0 ? f[floor_at].ps : 0;
		p.has_cloud = cloud_at >= 0;
		p.cloud_ps = cloud_at >= 0 ? f[cloud_at].ps : 0;
		p.composite_ps = f[composite].ps;
		p.valid = true;
		return p;
	}
}
