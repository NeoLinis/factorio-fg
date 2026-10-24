// Image-based motion vectors for DLSS-G (step 3b). See motion.h.

#include "motion.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// ------------------------------------------------------------ shaders (compiled at runtime with d3dcompiler_47)

	const char SHADER_SOURCE[] =
		// split into pieces: MSVC limits a single string literal to about 16 KB
		R"hlsl(
cbuffer Params : register(b0)
{
	uint2 full_size;
	uint2 half_size;
	uint2 quarter_size;
	uint2 eighth_size;
	uint2 blocks;
	uint flags;            // bit 0 flip sign, bit 1 output enabled, bit 2 last frame's camera valid, bit 3 last frame's blocks valid,
	                       // bit 7 mover search, bit 8 world layers valid (ground / entity snapshots),
	                       // bits 9-11 debug view: 1 world layers (= depth), 2 ground, 3 floor, 4 entity, 5 world, 6 air split
	                       // snapshot; bit 12 alt-mode icons are drawn before the air split (else after); bit 13 wide mover
	                       // search (fast trains)
	float penalty_zero;    // extra cost for "no motion": the camera has to be clearly worse
	float penalty_other;   // extra cost for temporal / neighbour / refined candidates
	float mismatch;        // block SAD above this = no real match (disocclusion, new content): use the camera
	float hysteresis;      // extra cost for a block to change kind (camera / still / own motion) from last frame
	uint frame;
	float2 cursor;         // mouse position in full-res px (a zoom centre candidate); negative if unknown
	float strictness;      // new own motion (not last frame's) must cut the camera's SAD by this fraction
	uint debug_view;       // 1: write the motion debug picture
	float far_penalty;     // own motion: extra cost per half-res px of difference from the camera (repeating patterns)
	float still_zone;      // "still" only within this radius of the screen centre, in screen heights
	float2 sprite_cam;     // 1.3 (flag 524288): the camera's pan from Factorio's sprites, full-res px, previous minus
	                       // current position (as t); used instead of the picture fit when there is no zoom
	float2 icon_cam;       // 1.3 beta 5 (flag 2097152): alt-mode icons' own shift from their sprites (previous minus current)
};

Texture2D<float4> hudless : register(t0);
RWTexture2D<float> luma_half : register(u0);
RWTexture2D<float> luma_half_prev : register(u1);
RWTexture2D<float> luma_quarter : register(u2);
RWTexture2D<float> luma_quarter_prev : register(u3);
RWStructuredBuffer<float2> block_mv : register(u4);
RWStructuredBuffer<float2> block_mv_prev : register(u5);
RWStructuredBuffer<uint> sads : register(u6);
// Camera model: a pixel p of this frame was at  Z + a * (p - Z) + t  in the previous frame (a: zoom, Z: zoom centre,
// t: pan). Entries: [0] t (half-res px), confidence, SAD; [1] last frame's [0]; [2] coarse t (1/8-res px);
// [3] fine t (1/4-res px); [4] a, Z (full-res px), valid; [5] last frame's [4]; [6] coarse a, Z; [7] fine a, Z
RWStructuredBuffer<float4> global_motion : register(u7);
RWTexture2D<float2> mvec : register(u8);
RWStructuredBuffer<float2> block_smooth : register(u9);
RWTexture2D<float> luma_eighth : register(u10);
RWTexture2D<float> luma_eighth_prev : register(u11);
RWTexture2D<float> luma_full : register(u12);
RWTexture2D<float> luma_full_prev : register(u13);
RWStructuredBuffer<uint> stats : register(u14);       // blocks per kind: camera, still, own; searched, wide, movers
RWTexture2D<unorm float4> debug_out : register(u15); // motion debug picture
// Half-res luma minus its local 5 x 5 mean: the block search compares detail only, so smooth brightness changes
// (drifting cloud shadows, day/night, light flicker) do not look like motion.
RWTexture2D<float> detail_half : register(u16);
RWTexture2D<float> detail_half_prev : register(u17);
// Depth for DLSS-G from the world layers: near = something placed on the ground (from the entity snapshot), far =
// bare ground. Without layers (map view, space) it is flat (far).
RWTexture2D<float> depth_out : register(u18);
// Character pixels held still while the camera moves: 1 on the ground (object, shadow), 0.5 flying (the character in
// mech armour), else 0. FSR shows them from the newest real frame instead of blending two animation poses.
RWTexture2D<unorm float> hold_out : register(u22);
// Half res: running average of "stays put on screen" evidence (-1..1), updated while the camera moves.
RWTexture2D<float> still_score : register(u23);
// World layers (step 8): Factorio's world picture after the tiles (ground), after the floor stage (belts, pipes,
// inserters, floor entities, all shadows) and right before the cloud pass (all objects in, no clouds, no alt-mode
// icons). Plus the air stage: the world right after the cloud pass and right before the composite pass; what differs
// between those two was drawn in the air (flying robots, alt-mode icons, flying effects).
// entity_mask (half res): 0 bare ground (moves exactly with the camera), 1/3 floor, 2/3 object, 5/6 alt-mode icon,
// 1 flying (robots, effects).
Texture2D<float4> layer_ground : register(t1);
Texture2D<float4> layer_entities : register(t2);
Texture2D<float4> layer_floor : register(t3);
Texture2D<float4> layer_postcloud : register(t4);
Texture2D<float4> layer_world : register(t5);
// The air stage split in two at the first draw of one shader (the air split snapshot): alt-mode icons on one side
// (they sit on buildings: camera motion, unless that clearly does not match), flying robots and effects on the other.
Texture2D<float4> layer_airsplit : register(t6);
// The exact character mask (1.2, from Factorio's own sprites): R the character, G its shadow, B the character flying.
// With it (flag 65536) nothing is guessed: those pixels stay put on screen (the camera follows the character), and
// nothing else is held.
Texture2D<float4> char_mask : register(t7);
bool exact_mask() { return (flags & 65536) != 0; }
#define LAYER_FLOOR (1.0 / 3.0)
#define LAYER_OBJECT (2.0 / 3.0)
#define LAYER_ICON (5.0 / 6.0)
#define LAYER_AIR 1.0
bool is_icon(float m) { return abs(m - LAYER_ICON) < 0.01; }
RWTexture2D<float> entity_mask : register(u20);
bool layers_valid() { return (flags & 256) != 0; }
#define DEPTH_FAR 0.75
#define DEPTH_NEAR 0.25

// Camera search, three levels:
//  coarse: 1/8 res. Pan only: +-CR px (+-128 full-res px) around zero AND around last frame's pan.
//          Zoom: NS zoom factors (0.62x .. 1.62x) about the screen centre and about the mouse, +-ZT px around last
//          frame's pan.
//  fine:   1/4 res, +-FR px around the coarse result, zoom in 1 % steps (+-2 %)
//  refine: full res, +-RR px around the fine result, zoom in 0.25 % steps (+-0.5 %), then a sub-pixel fit
#define CR 16
#define CN (2 * CR + 1)
#define CGRID (CN * CN)
#define NS 25
#define SCALE_STEP 0.04
#define ZT 3
#define ZTN (2 * ZT + 1)
#define ZGRID (2 * NS * ZTN * ZTN)
#define ZOOM_BASE (2 * CGRID)
#define FR 3
#define FN (2 * FR + 1)
#define FINE_BASE (ZOOM_BASE + ZGRID)
#define RR 3
#define RN (2 * RR + 1)
#define NV 5 // zoom variants per level (centre: NV / 2)
#define REFINE_BASE (FINE_BASE + NV * FN * FN)
#define FINE_SCALE_STEP 0.01
#define REFINE_SCALE_STEP 0.0025

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

[numthreads(8, 8, 1)]
void cs_luma(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= half_size)) return;
	int2 p = int2(id.xy) * 2;
	float a = luma(hudless.Load(int3(p, 0)).rgb), b = luma(hudless.Load(int3(p + int2(1, 0), 0)).rgb);
	float c = luma(hudless.Load(int3(p + int2(0, 1), 0)).rgb), d = luma(hudless.Load(int3(p + int2(1, 1), 0)).rgb);
	luma_full[p] = a; luma_full[p + int2(1, 0)] = b; luma_full[p + int2(0, 1)] = c; luma_full[p + int2(1, 1)] = d;
	luma_half[id.xy] = 0.25 * (a + b + c + d);
	float placed = 1.0;
	if (layers_valid())
	{
		float over_ground = 0, over_floor = 0, air_early = 0, air_late = 0;
		[unroll] for (int k = 0; k < 4; ++k)
		{
			int3 q = int3(p + int2(k & 1, k >> 1), 0);
			float3 ent = layer_entities.Load(q).rgb;
			float3 eg = abs(ent - layer_ground.Load(q).rgb);
			float3 ef = abs(ent - layer_floor.Load(q).rgb);
			float3 split = layer_airsplit.Load(q).rgb;
			float3 ee = abs(split - layer_postcloud.Load(q).rgb); // drawn in the air stage before the split
			float3 el = abs(layer_world.Load(q).rgb - split);     // ... after it (on top)
			over_ground = max(over_ground, max(eg.r, max(eg.g, eg.b)));
			over_floor = max(over_floor, max(ef.r, max(ef.g, ef.b)));
			air_early = max(air_early, max(ee.r, max(ee.g, ee.b)));
			air_late = max(air_late, max(el.r, max(el.g, el.b)));
		}
		const float t = 1.5 / 255.0;
		const bool icons_first = (flags & 4096) != 0;
		placed = air_late > t ? (icons_first ? LAYER_AIR : LAYER_ICON) :
		         (air_early > t ? (icons_first ? LAYER_ICON : LAYER_AIR) :
		         (over_ground <= t ? 0.0 : (over_floor <= t ? LAYER_FLOOR : LAYER_OBJECT)));
	}
	entity_mask[id.xy] = placed;
}

[numthreads(8, 8, 1)]
void cs_detail(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= half_size)) return;
	int2 p = int2(id.xy);
	int2 hmax = int2(half_size) - 1;
	float sum = 0;
	[unroll] for (int y = -2; y <= 2; ++y)
	[unroll] for (int x = -2; x <= 2; ++x)
		sum += luma_half[clamp(p + int2(x, y), int2(0, 0), hmax)];
	detail_half[p] = luma_half[p] - sum / 25.0;
}

[numthreads(8, 8, 1)]
void cs_down(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= quarter_size)) return;
	int2 p = int2(id.xy) * 2;
	luma_quarter[id.xy] = 0.25 * (luma_half[p] + luma_half[p + int2(1, 0)] + luma_half[p + int2(0, 1)] + luma_half[p + int2(1, 1)]);
}

[numthreads(8, 8, 1)]
void cs_down8(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= eighth_size)) return;
	int2 p = int2(id.xy) * 2;
	luma_eighth[id.xy] = 0.25 * (luma_quarter[p] + luma_quarter[p + int2(1, 0)] + luma_quarter[p + int2(0, 1)] + luma_quarter[p + int2(1, 1)]);
}

// Sample region shared by every shift in [c - r, c + r]: all of them compare the same pixels.
void search_region(int2 size, int2 c, int r, out int2 lo, out int2 span)
{
	lo = max(int2(0, 0), int2(r, r) - c) + 1;
	int2 hi = min(size - 1, size - 1 - c - int2(r, r)) - 1;
	span = max(hi - lo, int2(1, 1));
}

int2 grid_point(uint s, int2 lo, int2 span)
{
	return lo + int2(((s % 32) * uint(span.x)) / 32, ((s / 32) * uint(span.y)) / 24);
}

int2 clamp_center(int2 c)
{
	int2 lim = max(int2(eighth_size) / 2 - CR - 2, int2(0, 0));
	return clamp(c, -lim, lim);
}

// Last frame's camera motion in 1/8-res px: panning speed changes little from one frame to the next.
int2 predicted_center()
{
	if ((flags & 4) == 0) return int2(0, 0);
	float2 g = global_motion[0].xy; // still last frame's value: written again only in cs_global_pick
	if (any(isnan(g)) || any(abs(g) > 4096.0)) return int2(0, 0);
	return clamp_center(int2(round(g * 0.25)));
}

groupshared float gs_sum[64];
float group_sum(uint gi, float v)
{
	GroupMemoryBarrierWithGroupSync(); // a previous call's result may still be being read
	gs_sum[gi] = v;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (gi < stride) gs_sum[gi] += gs_sum[gi + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	return gs_sum[0];
}

// Bilinear reads of the previous frame (zoomed positions are fractional). Callers keep p inside [0, size - 2].
float bl_eighth_prev(float2 p)
{
	int2 i = int2(floor(p)); float2 f = p - float2(i);
	return lerp(lerp(luma_eighth_prev[i], luma_eighth_prev[i + int2(1, 0)], f.x), lerp(luma_eighth_prev[i + int2(0, 1)], luma_eighth_prev[i + int2(1, 1)], f.x), f.y);
}
float bl_quarter_prev(float2 p)
{
	int2 i = int2(floor(p)); float2 f = p - float2(i);
	return lerp(lerp(luma_quarter_prev[i], luma_quarter_prev[i + int2(1, 0)], f.x), lerp(luma_quarter_prev[i + int2(0, 1)], luma_quarter_prev[i + int2(1, 1)], f.x), f.y);
}
float bl_full_prev(float2 p)
{
	int2 i = int2(floor(p)); float2 f = p - float2(i);
	return lerp(lerp(luma_full_prev[i], luma_full_prev[i + int2(1, 0)], f.x), lerp(luma_full_prev[i + int2(0, 1)], luma_full_prev[i + int2(1, 1)], f.x), f.y);
}
float bl_detail_prev(float2 p)
{
	int2 i = int2(floor(p)); float2 f = p - float2(i);
	return lerp(lerp(detail_half_prev[i], detail_half_prev[i + int2(1, 0)], f.x), lerp(detail_half_prev[i + int2(0, 1)], detail_half_prev[i + int2(1, 1)], f.x), f.y);
}

// Zoom centre candidates in full-res px: 0 = screen centre, 1 = mouse (screen centre when unknown).
float2 zoom_centre(uint i)
{
	float2 c = float2(full_size) * 0.5;
	if (i == 1 && all(cursor >= 0) && all(cursor < float2(full_size))) c = cursor;
	return c;
}

// Spread sample grid over the whole frame (32 x 24 points, 2 px margin), for the zoom-aware levels.
float2 spread_point(uint s, float2 size)
{
	return float2(2.0, 2.0) + floor(float2(float(s % 32) * (size.x - 4.0) / 32.0, float(s / 32) * (size.y - 4.0) / 24.0));
}

// Sum over 768 samples x 1024, as an integer; samples whose previous position is outside the frame are left out and
// the rest rescaled. Too few valid samples: worst possible.
uint encode_sad(float sum, float count)
{
)hlsl"
		R"hlsl(	return count >= 384.0 ? uint(sum * 768.0 / count * 1024.0) : 0xffffffff;
}

// Groups: x = candidate (33 x 33), y = grid (0: around zero, 1: around the prediction). 64 threads x 12 samples.
[numthreads(64, 1, 1)]
void cs_global_coarse(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	int2 c = gid.y == 0 ? int2(0, 0) : predicted_center();
	int2 d = c + int2(gid.x % CN, gid.x / CN) - CR;
	int2 lo, span;
	search_region(int2(eighth_size), c, CR, lo, span);
	float sum = 0;
	for (uint k = 0; k < 12; ++k)
	{
		int2 p = grid_point(gi * 12 + k, lo, span);
		sum += abs(luma_eighth[p] - luma_eighth_prev[p + d]);
	}
	sum = group_sum(gi, sum);
	if (gi == 0) sads[gid.y * CGRID + gid.x] = uint(sum * 1024.0);
}

// Groups: x = pan candidate (7 x 7 around last frame's pan), y = zoom factor (NS), z = zoom centre (screen, mouse).
[numthreads(64, 1, 1)]
void cs_global_zoom(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	float a = exp((float(gid.y) - float(NS / 2)) * SCALE_STEP);
	float2 z = zoom_centre(gid.z) / 8.0;
	float2 t = float2(predicted_center() + int2(gid.x % ZTN, gid.x / ZTN) - ZT);
	float2 size = float2(eighth_size);
	float sum = 0, count = 0;
	for (uint k = 0; k < 12; ++k)
	{
		float2 p = spread_point(gi * 12 + k, size);
		float2 q = z + a * (p - z) + t;
		if (all(q >= 0) && all(q <= size - 2.0))
		{
			sum += abs(luma_eighth[int2(p)] - bl_eighth_prev(q));
			count += 1;
		}
	}
	sum = group_sum(gi, sum);
	count = group_sum(gi, count);
	if (gi == 0) sads[ZOOM_BASE + (gid.z * NS + gid.y) * ZTN * ZTN + gid.x] = encode_sad(sum, count);
}

// Shadow darkness (1.2): how much the character's shadow darkens what is under it, per unit of its mask coverage (G).
// Factorio draws shadows into their own target and blends that in, so a shadow is a darkening of whatever is under
// it. With the darkness known, frame generation gets the picture without the shadow (moving with the camera) and the
// shadow as a darkening in the UI layer that stays put with the character.
// Measured in cs_expand where the camera moves the world across the shadow's edge: the same point inside the shadow in
// one frame and outside it in the other. Four classes, as they may differ: walking / flying (the mask's B on the
// shadow) x on ground and floor / on objects (an object drawn after the shadow is not darkened by it - unless the
// flying shadow is blended in later; the measurement tells). stats[]: SH_K + class the running value (float bits),
// SH_FRAMES + class frames it was measured in, SH_SAMPLES + class samples last frame, SH_COUNT + class samples this
// frame, SH_HIST + class x SH_BINS .. this frame's histogram (darkness per coverage in 1/SH_BINS steps, weighted by
// the coverage x 16).
#define SH_K 8
#define SH_FRAMES 12
#define SH_SAMPLES 16
#define SH_COUNT 20
#define SH_HIST 32
#define SH_BINS 32
#define SH_CLASSES 4
#define SH_MIN_SAMPLES 60
// 1.3 motion fit (cs_fit): per class (0 ground, 1 floor, 2 objects, 3 icons, 4 flying, 5 no layers) the summed luma
// difference x 256 with our vectors, with the camera alone, and the sample count.
#define FIT_BASE 160
#define FIT_CLASSES 6
void shadow_finish()
{
	[loop] for (uint cls = 0; cls < SH_CLASSES; ++cls)
	{
		const uint h = SH_HIST + cls * SH_BINS;
		if (frame == 0)
		{
			stats[SH_K + cls] = 0; stats[SH_FRAMES + cls] = 0; stats[SH_SAMPLES + cls] = 0; stats[SH_COUNT + cls] = 0;
			[loop] for (uint z = 0; z < SH_BINS; ++z) stats[h + z] = 0;
			continue;
		}
		uint total = 0;
		[loop] for (uint i = 0; i < SH_BINS; ++i) total += stats[h + i];
		const uint count = stats[SH_COUNT + cls];
		stats[SH_SAMPLES + cls] = count;
		if (count >= SH_MIN_SAMPLES && total > 0)
		{
			// weighted median: robust against what did not match (animated water and machines, a passing bot)
			const float half_w = 0.5 * float(total);
			float acc = 0.0, k = 0.0;
			[loop] for (uint j = 0; j < SH_BINS; ++j)
			{
				const float w = float(stats[h + j]);
				if (w > 0.0 && acc + w >= half_w)
				{
					k = (float(j) + saturate((half_w - acc) / w)) / float(SH_BINS);
					break;
				}
				acc += w;
			}
			const uint n = stats[SH_FRAMES + cls];
			const float old_k = asfloat(stats[SH_K + cls]);
			// a plain average over the first 10 frames, then a slow running average
			const float rate = max(0.1, 1.0 / float(n + 1));
			const float nk = n == 0 || !(old_k >= 0.0 && old_k < 1.0) ? k : lerp(old_k, k, rate);
			stats[SH_K + cls] = asuint(clamp(nk, 0.0, 0.95));
			stats[SH_FRAMES + cls] = min(n + 1, 1000000u);
		}
		stats[SH_COUNT + cls] = 0;
		[loop] for (uint b = 0; b < SH_BINS; ++b) stats[h + b] = 0;
	}
}
// Darkness per unit of coverage of one class once measured in a few frames, else -1.
float shadow_k(uint cls)
{
	const float k = asfloat(stats[SH_K + cls]);
	return stats[SH_FRAMES + cls] >= 3 && k >= 0.0 && k < 1.0 ? k : -1.0;
}

)hlsl"
		R"hlsl([numthreads(1, 1, 1)]
void cs_global_coarse_pick(uint3 id : SV_DispatchThreadID)
{
	int2 pc = predicted_center();
	// Ties (flat areas, black space) go to "no motion", then to the prediction, never to a grid corner.
	uint zero = sads[CR * CN + CR];
	uint best = zero;
	int2 bd = int2(0, 0);
	uint pred = sads[CGRID + CR * CN + CR];
	if (pred < best) { best = pred; bd = pc; }
	for (uint g = 0; g < 2; ++g)
	{
		int2 c = g == 0 ? int2(0, 0) : pc;
		for (uint i = 0; i < CGRID; ++i)
		{
			uint v = sads[g * CGRID + i];
			if (v < best) { best = v; bd = c + int2(i % CN, i / CN) - CR; }
		}
	}
	// Zoom must beat the best pan clearly (3 %): a still or panning picture never turns into a tiny zoom.
	float a = 1.0;
	float2 zc = zoom_centre(0);
	uint zbest = 0xffffffff;
	uint zi = 0;
	for (uint j = 0; j < ZGRID; ++j)
	{
		uint s = (j / (ZTN * ZTN)) % NS;
		if (s == NS / 2) continue; // factor 1 = a pan, covered above
		uint v = sads[ZOOM_BASE + j];
		if (v < zbest) { zbest = v; zi = j; }
	}
	if (zbest != 0xffffffff && float(zbest) < float(best) * 0.97)
	{
		uint tj = zi % (ZTN * ZTN);
		uint sj = (zi / (ZTN * ZTN)) % NS;
		uint cj = zi / (ZTN * ZTN * NS);
		a = exp((float(sj) - float(NS / 2)) * SCALE_STEP);
		zc = zoom_centre(cj);
		bd = pc + int2(tj % ZTN, tj / ZTN) - ZT;
		best = zbest;
	}
	float confidence = zero > 0 ? saturate(1.0 - float(best) / float(zero)) : 0.0;
	global_motion[2] = float4(float2(bd), confidence, float(best));
	global_motion[6] = float4(a, zc, 0);
	stats[0] = 0; stats[1] = 0; stats[2] = 0; stats[3] = 0; stats[4] = 0; stats[5] = 0;
	[unroll] for (uint fi = 0; fi < FIT_CLASSES * 3; ++fi)
		stats[FIT_BASE + fi] = 0;
	shadow_finish();
}

// Zoom-aware SAD at one level: pan t and zoom a about z (both in that level's px).
#define LEVEL_SAD(CUR, PREV_BL, SIZE) \
	float2 size = float2(SIZE); \
	float sum = 0, count = 0; \
	for (uint k = 0; k < 12; ++k) \
	{ \
		float2 p = spread_point(gi * 12 + k, size); \
		float2 q = z + a * (p - z) + t; \
		if (all(q >= 0) && all(q <= size - 2.0)) \
		{ \
			sum += abs(CUR[int2(p)] - PREV_BL(q)); \
			count += 1; \
		} \
	} \
	sum = group_sum(gi, sum); \
	count = group_sum(gi, count);

// Groups: x = pan candidate (7 x 7), y = zoom variant (NV steps; all 1 when there is no zoom).
[numthreads(64, 1, 1)]
void cs_global_fine(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	float4 coarse = global_motion[6];
	float a = coarse.x != 1.0 ? coarse.x * exp((float(gid.y) - float(NV / 2)) * FINE_SCALE_STEP) : 1.0;
	float2 z = coarse.yz / 4.0;
	float2 t = float2(int2(global_motion[2].xy) * 2 + int2(gid.x % FN, gid.x / FN) - FR);
	LEVEL_SAD(luma_quarter, bl_quarter_prev, quarter_size)
	if (gi == 0) sads[FINE_BASE + gid.y * FN * FN + gid.x] = encode_sad(sum, count);
}

// Best of an NV x (N x N) grid (zoom variant x pan), with sub-pixel / sub-step fits. Returns the pan offset from the
// grid centre and the zoom variant offset from the middle variant (both fractional).
void pick_grid(uint base, int n, out float2 d, out float sv, out uint best)
{
	int r = n / 2;
	int bi = r * n + r;
	uint bs = NV / 2;
	best = sads[base + bs * n * n + bi]; // centre of the middle variant wins ties
	for (uint v = 0; v < NV; ++v)
	for (int i = 0; i < n * n; ++i)
	{
		uint x = sads[base + v * n * n + i];
		if (x < best) { best = x; bi = i; bs = v; }
	}
	uint o = base + bs * n * n;
	int2 b = int2(bi % n, bi / n);
	d = float2(b - r);
	if (b.x > 0 && b.x < n - 1)
	{
		float l = sads[o + bi - 1], m = best, rr = sads[o + bi + 1], den = l - 2 * m + rr;
		if (den > 0) d.x += clamp(0.5 * (l - rr) / den, -0.5, 0.5);
	}
	if (b.y > 0 && b.y < n - 1)
	{
		float u = sads[o + bi - n], m = best, dn = sads[o + bi + n], den = u - 2 * m + dn;
		if (den > 0) d.y += clamp(0.5 * (u - dn) / den, -0.5, 0.5);
	}
	sv = float(bs) - float(NV / 2);
	if (bs > 0 && bs < NV - 1)
	{
		uint lu = sads[o - n * n + bi], ru = sads[o + n * n + bi];
		float l = lu, m = best, rr = ru, den = l - 2 * m + rr;
		if (den > 0 && lu != 0xffffffff && ru != 0xffffffff) sv += clamp(0.5 * (l - rr) / den, -0.5, 0.5);
	}
}

[numthreads(1, 1, 1)]
void cs_global_pick(uint3 id : SV_DispatchThreadID)
{
	float4 coarse = global_motion[6];
	float2 d; float sv; uint best;
	pick_grid(FINE_BASE, FN, d, sv, best);
	float a = coarse.x != 1.0 ? coarse.x * exp(sv * FINE_SCALE_STEP) : 1.0;
	global_motion[3] = float4(float2(int2(global_motion[2].xy) * 2) + d, global_motion[2].z, float(best));
	global_motion[7] = float4(a, coarse.yz, 0);
}

// Groups: x = pan candidate (7 x 7) at full resolution, y = zoom variant (NV steps; all 1 without zoom).
// Most of the picture moves with the camera, so this precision decides how clean frame generation looks.
[numthreads(64, 1, 1)]
void cs_global_refine(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	float4 fine = global_motion[7];
	float a = fine.x != 1.0 ? fine.x * exp((float(gid.y) - float(NV / 2)) * REFINE_SCALE_STEP) : 1.0;
	float2 z = fine.yz;
	float2 t = float2(int2(round(global_motion[3].xy * 4.0)) + int2(gid.x % RN, gid.x / RN) - RR);
	LEVEL_SAD(luma_full, bl_full_prev, full_size)
	if (gi == 0) sads[REFINE_BASE + gid.y * RN * RN + gid.x] = encode_sad(sum, count);
}

[numthreads(1, 1, 1)]
void cs_global_final(uint3 id : SV_DispatchThreadID)
{
	float4 fine = global_motion[7];
	float2 d; float sv; uint best;
	pick_grid(REFINE_BASE, RN, d, sv, best);
	float a = fine.x != 1.0 ? fine.x * exp(sv * REFINE_SCALE_STEP) : 1.0;
	float2 t = float2(int2(round(global_motion[3].xy * 4.0))) + d;
	bool valid = (flags & 4) != 0;
	global_motion[1] = valid ? global_motion[0] : float4(0, 0, 0, 0);
	global_motion[5] = valid ? global_motion[4] : float4(1, float2(full_size) * 0.5, 0);
	// 1.3: the picture's own fit is kept in [6].w / [7].w (half-res px) for the comparison with the sprites' shift
	float4 c6 = global_motion[6], c7 = global_motion[7];
	c6.w = t.x * 0.5;
	c7.w = t.y * 0.5;
	global_motion[6] = c6;
	global_motion[7] = c7;
	// The sprites' shift is exact (Factorio's own positions); the fit lands within a fraction of a pixel of it, which
	// is what repeating sharp things (module icons, rails) show as jitter in generated frames. Pan only: while zooming
	// the sprites' sizes change and the shift is not known (the CPU does not set the flag then).
	if ((flags & 524288) != 0 && abs(a - 1.0) < 0.0005)
	{
		t = sprite_cam;
		a = 1.0;
	}
	global_motion[0] = float4(t * 0.5, global_motion[2].z, float(best)); // full-res -> half-res px
	global_motion[4] = float4(a, fine.yz, 1);
}

// Camera motion at a half-res position, from the model (this frame or the last).
float2 camera_at(float2 p)
{
	float4 mdl = global_motion[4];
	return (mdl.x - 1.0) * (p - mdl.yz * 0.5) + global_motion[0].xy;
}

// Block SAD against the camera model per pixel (zoom: the motion changes across the block).
float block_sad_camera(int2 base)
{
	float s = 0;
	int2 hmax = int2(half_size) - 1;
	float2 qmax = float2(half_size) - 2.0;
	[unroll] for (int y = 0; y < 8; ++y)
	[unroll] for (int x = 0; x < 8; ++x)
	{
		int2 p = min(base + int2(x, y), hmax);
		float2 q = clamp(float2(p) + camera_at(float2(p)), float2(0, 0), qmax);
		s += abs(detail_half[p] - bl_detail_prev(q));
	}
	return s;
}

float block_sad(int2 base, int2 d)
{
	float s = 0;
	int2 hmax = int2(half_size) - 1;
	[unroll] for (int y = 0; y < 8; ++y)
	[unroll] for (int x = 0; x < 8; ++x)
	{
		int2 p = min(base + int2(x, y), hmax);
		int2 q = clamp(p + d, int2(0, 0), hmax);
		s += abs(detail_half[p] - detail_half_prev[q]);
	}
	return s;
}

// A genuine match, not merely "less bad than the camera": the difference must be small compared with how much detail
// the two patches have. Unrelated textures (ground revealed behind the walking character compared with other ground)
// differ by about as much as their detail; a real match (even the animated character) by much less.
bool real_match(int2 base, int2 d, float sad)
{
	float e = 0;
	int2 hmax = int2(half_size) - 1;
	[unroll] for (int y = 0; y < 8; ++y)
	[unroll] for (int x = 0; x < 8; ++x)
	{
		int2 p = min(base + int2(x, y), hmax);
		int2 q = clamp(p + d, int2(0, 0), hmax);
		e += abs(detail_half[p]) + abs(detail_half_prev[q]);
	}
	return e > 0.15 && sad < 0.45 * e;
}

// The whole block, displaced, must lie inside the previous frame; otherwise the match is meaningless.
bool inside(int2 base, int2 d)
{
	int2 lo = base + d;
	int2 hi = base + int2(7, 7) + d;
	return all(lo >= 0) && all(hi <= int2(half_size) - 1);
}

float parabola(float m, float c, float p)
{
	float den = m - 2 * c + p;
	return den > 1e-5 ? clamp(0.5 * (m - p) / den, -0.5, 0.5) : 0.0;
}

// Blocks that move with the camera store this marker instead of a vector: with zoom, the camera motion differs
// from pixel to pixel, so it is evaluated per pixel at the end (cs_expand). As a temporal candidate it is far
// outside the frame and gets skipped.
#define CAMERA_MARK 100000.0
bool is_camera(float2 v) { return v.x > CAMERA_MARK * 0.5; }

// Kind of motion: 0 moves with the camera, 1 still on screen (the character), 2 its own motion (belts, trains).
uint kind_of(float2 v)
{
	if (is_camera(v)) return 0;
	if (all(abs(v) < 0.25)) return 1;
	return 2;
}

uint kind_of_int(int2 v, int2 camera)
{
	if (all(v == camera)) return 0;
	if (all(v == int2(0, 0))) return 1;
	return 2;
}

// Only what the camera follows stays still on screen (the character, the vehicle being driven, their shadow), and
// that is near the centre. Elsewhere "still" is wagon-wheel aliasing: identical, evenly spaced things (module icons,
// belt items, machine grids) that moved by one spacing per frame match "no motion" perfectly.
bool in_still_zone(float2 p_half)
{
	float2 d = (p_half * 2.0 - float2(full_size) * 0.5) / float(full_size.y);
	return dot(d, d) < still_zone * still_zone;
}

// Mover search, 1/4 res: every shift within +-MR px of the camera in 2 px steps, then +-1 around the best, over an
// 8 x 8 window (32 x 32 full-res px) centred on the block. Accepted only if the best match is unique - nothing
// comparably good 3+ px away (repeating patterns have several equally good matches) - and clearly better than the
// camera. Finds trains, cars, bots and the character wherever they are and however fast (up to +-48 full-res px per
// frame relative to the camera); the old candidates (last frame's motion +-1) could only follow slow, steady motion.
#define MR 12
#define MN (MR + 1)          // grid points per axis (step 2)
float window_sad_q(int2 qbase, int2 d)
{
	float s = 0;
	int2 qmax = int2(quarter_size) - 1;
	[unroll] for (int y = 0; y < 8; ++y)
	[unroll] for (int x = 0; x < 8; ++x)
	{
		int2 p = clamp(qbase + int2(x, y), int2(0, 0), qmax);
		int2 q = clamp(p + d, int2(0, 0), qmax);
		s += abs(luma_quarter[p] - luma_quarter_prev[q]);
	}
	return s;
}

// Wide mover search for fast movers (trains with speed upgrades), 1/8 res: +-WR px (+-192 full-res px) around the
// camera in 2 px steps over an 8 x 8 window (64 x 64 full-res px), then +-1, then +-2 at 1/4 res. Only for blocks with
// objects or flying things where the camera does not match and the normal search found nothing.
// Same uniqueness test (identical wagons one wagon apart are rejected, as before).
#define WR 24
)hlsl"
		R"hlsl(#define WN (WR + 1)          // grid points per axis (step 2)
float window_sad_e(int2 ebase, int2 d)
{
	float s = 0;
	int2 emax = int2(eighth_size) - 1;
	[unroll] for (int y = 0; y < 8; ++y)
	[unroll] for (int x = 0; x < 8; ++x)
	{
		int2 p = clamp(ebase + int2(x, y), int2(0, 0), emax);
		int2 q = clamp(p + d, int2(0, 0), emax);
		s += abs(luma_eighth[p] - luma_eighth_prev[q]);
	}
	return s;
}

// The block search runs in three passes (since 0.13): cs_block_pre decides per block what is needed (most blocks are
// done there: bare ground, new ground at the edges, alt-mode icons), cs_mover runs the mover searches with 64 threads
// per block (was one thread per block: a few searching blocks held up whole warps), cs_block finishes the rest.
// block_pre (float4 per block): x camera SAD, y state, zw search centre (half-res px). State: 0 done (block_mv written),
// 1 continue without search, 2 continue with the mover search; +4 wide search allowed, +8 following a mover.
RWStructuredBuffer<float4> block_pre : register(u19);
// mover_out (float4 per block): xy mover (half-res px, refined), z its block SAD, w 1 = accepted
RWStructuredBuffer<float4> mover_out : register(u21);
#define PRE_DONE 0
#define PRE_CONTINUE 1
#define PRE_SEARCH 2
#define PRE_WIDE 4
#define PRE_FOLLOWING 8

// One thread per 8 x 8 half-res block (16 x 16 full-res pixels).
[numthreads(8, 8, 1)]
void cs_block_pre(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= blocks)) return;
	int2 base = int2(id.xy) * 8;
	uint bi = id.y * blocks.x + id.x;
	float2 g = camera_at(float2(base) + 3.5); // camera motion at the block centre
	int2 gi = int2(round(g));
	bool history = (flags & 8) != 0;
	float2 last_v = history ? block_mv_prev[bi] : float2(CAMERA_MARK, CAMERA_MARK);
	uint last_kind = kind_of(last_v);
	const float2 camera_mark = float2(CAMERA_MARK, CAMERA_MARK);
	block_pre[bi] = float4(0, PRE_DONE, 0, 0);

	// Where the camera says this block was outside the previous frame, it is new ground scrolling in at the edge:
	// nothing to match against, and it belongs to the world.
	if (!inside(base, gi))
	{
		block_mv[bi] = camera_mark;
		return;
	}

	// Bare ground (nothing placed on it in this block): exactly the camera, no search.
	uint icon_px = 0, mover_px = 0; // alt-mode icon pixels; object or flying pixels (what can move fast)
	if (layers_valid())
	{
		float placed = 0;
		[unroll] for (int my = 0; my < 8; ++my)
		[unroll] for (int mx = 0; mx < 8; ++mx)
		{
			float mk = entity_mask[min(base + int2(mx, my), int2(half_size) - 1)];
			placed += mk;
			icon_px += is_icon(mk) ? 1u : 0u;
			mover_px += (mk >= LAYER_OBJECT - 0.01 && !is_icon(mk)) ? 1u : 0u;
		}
		if (placed == 0)
		{
			block_mv[bi] = camera_mark;
			return;
		}
	}

	// The camera is the default answer: always allowed, no penalty except for changing kind.
	float camera_sad;
	[branch] if (global_motion[4].x != 1.0)
		camera_sad = block_sad_camera(base);
	else
		camera_sad = block_sad(base, gi);
	const bool camera_genuine = real_match(base, gi, camera_sad);

	// Alt-mode icons sit on buildings: where they are and the camera is a genuine match, it is the camera - no search,
	// no candidates (rows of identical module icons match a shift by one icon just as well).
	if (icon_px >= 8 && camera_genuine)
	{
		block_mv[bi] = camera_mark;
		return;
	}

	// Mover search only where the camera does not explain the block: a mover always leaves a poor camera match (it is
	// somewhere else now), while ground moving with the camera is a genuine camera match - most of the screen.
	// Not while zooming: the camera then differs across the search window and shifted matches look falsely good.
	const bool following = last_kind == 2;
	const bool zooming = abs(global_motion[4].x - 1.0) > 0.002;
	const bool camera_poor = camera_sad > 0.1 && !camera_genuine;
	const bool searched = (flags & 128) != 0 && !zooming && (following || camera_poor);
	// too fast for the normal search (it reaches +-48 full-res px): the wide search, where something can move
	const bool wide = searched && !following && (flags & 8192) != 0 && (!layers_valid() || mover_px >= 6);
	uint state = searched ? PRE_SEARCH : PRE_CONTINUE;
	state += wide ? PRE_WIDE : 0;
	state += following ? PRE_FOLLOWING : 0;
	block_pre[bi] = float4(camera_sad, float(state), following ? last_v : g);
	uint dummy;
	if (searched)
		InterlockedAdd(stats[3], 1, dummy);
}

// Mover search, one group of 64 threads per block: the search grid is shared out over the threads, the picks are
// made by thread 0. Every thread passes every barrier (blocks without a search just do no work).
groupshared float gs_v[WN * WN];
groupshared float gs_r[32];
groupshared float gs_b, gs_second, gs_cam;
groupshared int2 gs_bo, gs_mover;
groupshared uint gs_flags; // bit 0 accepted, bit 1 wide search, bit 2 wide candidate passed

int2 off3(uint k) { return int2(int(k % 3) - 1, int(k / 3) - 1); }

[numthreads(64, 1, 1)]
void cs_mover(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	const uint2 blk = min(gid.xy, blocks - 1);
	const uint bi = blk.y * blocks.x + blk.x;
	const int2 base = int2(blk) * 8;
	const float4 pre = block_pre[bi];
	const uint state = uint(pre.y + 0.5);
	const bool req = (state & 3) == PRE_SEARCH && all(gid.xy < blocks);
	const bool wide_ok = (state & PRE_WIDE) != 0;
	const bool following = (state & PRE_FOLLOWING) != 0;
	const float2 g = camera_at(float2(base) + 3.5);
	const int2 gcam = int2(round(g));
	const int2 qbase = base / 2 - 2;
	const int2 camq = int2(round(g * 0.5));
	const int2 gq = int2(round(pre.zw * 0.5)); // search centre: the camera, or the motion being followed

	// 1. normal search, 1/4 res: +-MR px in 2 px steps
	[branch] if (req)
	{
		for (uint i = gi; i < MN * MN; i += 64)
			gs_v[i] = window_sad_q(qbase, gq + int2(int(i % MN) * 2 - MR, int(i / MN) * 2 - MR));
	}
	[branch] if (gi == 0)
	{
		gs_cam = 0.0;
		[branch] if (req)
			gs_cam = window_sad_q(qbase, camq);
		gs_flags = 0;
		gs_mover = int2(0, 0);
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0 && req)
	{
		float b = 1e30;
		uint bj = 0;
		for (uint j = 0; j < MN * MN; ++j)
			if (gs_v[j] < b) { b = gs_v[j]; bj = j; }
		int2 bo = int2(int(bj % MN) * 2 - MR, int(bj / MN) * 2 - MR);
		float second = 1e30; // second best at least 3 px away from the best
		for (uint j2 = 0; j2 < MN * MN; ++j2)
		{
			int2 o = int2(int(j2 % MN) * 2 - MR, int(j2 / MN) * 2 - MR);
			if (max(abs(o.x - bo.x), abs(o.y - bo.y)) >= 3) second = min(second, gs_v[j2]);
		}
		gs_b = b; gs_bo = bo; gs_second = second;
	}
	GroupMemoryBarrierWithGroupSync();
	// refine +-1 around the best
	// (if, not ?: - HLSL evaluates both sides of ?:, and idle groups must not do the work)
	[branch] if (gi < 9)
	{
		gs_r[gi] = 1e30;
		[branch] if (req && gi != 4)
			gs_r[gi] = window_sad_q(qbase, gq + gs_bo + off3(gi));
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0 && req)
	{
		float b = gs_b;
		int2 best = gs_bo;
		for (uint k = 0; k < 9; ++k)
			if (gs_r[k] < b) { b = gs_r[k]; best = gs_bo + off3(k); }
		int2 rel = gq + best - camq;
		bool moved = max(abs(rel.x), abs(rel.y)) >= 1;    // at least 4 full-res px from the camera
		// A mover already being followed only has to stay plausible; a new one must be unique and clearly better.
		bool unique = b < (following ? 0.8 : 0.6) * gs_second;
		bool better = b < gs_cam * (following ? 0.9 : (1.0 - strictness));
		bool acc = req && moved && unique && better;
		gs_mover = (gq + best) * 2; // quarter -> half-res px
		bool wide = req && !acc && wide_ok;
		gs_flags = (acc ? 1u : 0u) | (wide ? 2u : 0u);
		uint dummy;
		[branch] if (wide)
			InterlockedAdd(stats[4], 1, dummy);
	}
	GroupMemoryBarrierWithGroupSync();

	// 2. wide search, 1/8 res: +-WR px in 2 px steps around the camera
	const bool wide = (gs_flags & 2) != 0;
	const int2 ebase = base / 4 - 3;
	const int2 came = int2(round(g * 0.25));
	[branch] if (wide)
	{
		for (uint w = gi; w < WN * WN; w += 64)
			gs_v[w] = window_sad_e(ebase, came + int2(int(w % WN) * 2 - WR, int(w / WN) * 2 - WR));
		[branch] if (gi == 0)
			gs_cam = window_sad_e(ebase, came);
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0 && wide)
	{
		float b = 1e30;
		uint bj = 0;
		for (uint j = 0; j < WN * WN; ++j)
			if (gs_v[j] < b) { b = gs_v[j]; bj = j; }
		int2 bo = int2(int(bj % WN) * 2 - WR, int(bj / WN) * 2 - WR);
		float second = 1e30;
		for (uint j2 = 0; j2 < WN * WN; ++j2)
		{
			int2 o = int2(int(j2 % WN) * 2 - WR, int(j2 / WN) * 2 - WR);
			if (max(abs(o.x - bo.x), abs(o.y - bo.y)) >= 3) second = min(second, gs_v[j2]);
		}
		gs_b = b; gs_bo = bo; gs_second = second;
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi < 9)
	{
		gs_r[gi] = 1e30;
		[branch] if (wide && gi != 4)
			gs_r[gi] = window_sad_e(ebase, came + gs_bo + off3(gi));
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0 && wide)
	{
		float b = gs_b;
		int2 best = gs_bo;
		for (uint k = 0; k < 9; ++k)
			if (gs_r[k] < b) { b = gs_r[k]; best = gs_bo + off3(k); }
		// beyond the normal search's reach (it covers +-6 px at this resolution), clearly better than the camera, unique
		bool good = wide && max(abs(best.x), abs(best.y)) >= 5 && b < gs_cam * (1.0 - strictness) && b < 0.6 * gs_second;
		gs_bo = (came + best) * 2; // centre of the 1/4-res refinement
		if (good)
			gs_flags |= 4u;
	}
	GroupMemoryBarrierWithGroupSync();
	// 1/4 res: +-2 around it (the block's own window, finer detail)
	const bool wide_pass = (gs_flags & 4) != 0;
	[branch] if (gi < 25)
	{
		gs_r[gi] = 1e30;
		[branch] if (wide_pass)
			gs_r[gi] = window_sad_q(qbase, gs_bo + int2(int(gi % 5) - 2, int(gi / 5) - 2));
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0 && wide_pass)
	{
		float b = 1e30;
		uint bk = 12;
		for (uint k = 0; k < 25; ++k)
			if (gs_r[k] < b) { b = gs_r[k]; bk = k; }
		gs_mover = (gs_bo + int2(int(bk % 5) - 2, int(bk / 5) - 2)) * 2;
		gs_flags |= 1u;
	}
	GroupMemoryBarrierWithGroupSync();

	// 3. the search works at 1/4 res (up to 1 px there = 2 px here off): refine at half res, two steps of +-1
	const bool acc = (gs_flags & 1) != 0 && inside(base, gs_mover) && kind_of_int(gs_mover, gcam) == 2;
	[branch] if (gi < 9)
	{
		int2 d = gs_mover + off3(gi);
		gs_r[gi] = 1e30;
		[branch] if (acc && inside(base, d))
			gs_r[gi] = block_sad(base, d);
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0)
	{
		float b = 1e30;
		int2 m = gs_mover;
		for (uint k = 0; k < 9; ++k)
			if (gs_r[k] < b) { b = gs_r[k]; m = gs_mover + off3(k); }
		gs_b = b;
		gs_bo = m;
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi < 9)
	{
		int2 d = gs_bo + off3(gi);
		gs_r[gi] = 1e30;
		[branch] if (acc && gi != 4 && inside(base, d))
			gs_r[gi] = block_sad(base, d);
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi == 0 && req)
	{
		float b = gs_b;
		int2 m = gs_bo;
		for (uint k = 0; k < 9; ++k)
			if (gs_r[k] < b) { b = gs_r[k]; m = gs_bo + off3(k); }
		mover_out[bi] = float4(float2(m), b, (acc && b < 1e29) ? 1.0 : 0.0);
		uint dummy;
		[branch] if (acc)
			InterlockedAdd(stats[5], 1, dummy);
	}
}

// One thread per 8 x 8 half-res block: candidates, refinement and the final decision for blocks cs_block_pre left open.
[numthreads(8, 8, 1)]
void cs_block(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= blocks)) return;
	uint bi = id.y * blocks.x + id.x;
	const float4 pre = block_pre[bi];
	const uint state = uint(pre.y + 0.5);
	if ((state & 3) == PRE_DONE) return;
	int2 base = int2(id.xy) * 8;
	float2 g = camera_at(float2(base) + 3.5); // camera motion at the block centre
	int2 gi = int2(round(g));
	bool history = (flags & 8) != 0;
	float2 last_v = history ? block_mv_prev[bi] : float2(CAMERA_MARK, CAMERA_MARK);
	uint last_kind = kind_of(last_v);
	const float2 camera_mark = float2(CAMERA_MARK, CAMERA_MARK);
	const bool still_allowed = in_still_zone(float2(base) + 3.5);

	// The camera is the default answer: always allowed, no penalty except for changing kind.
	int2 best = gi;
	const float camera_sad = pre.x;
)hlsl"
		R"hlsl(	float best_sad = camera_sad;
	float best_cost = best_sad + (last_kind != 0 ? hysteresis : 0.0);
	// Own motion that this block did not have last frame must be clearly better than the camera: animated sprites
	// (water, trees, smoke, machines) change shape without moving, and a shifted patch sometimes matches an animation
	// frame a little better. Real movers (trains, cars, bots) match far better, or keep last frame's motion.
	const float unconfirmed_limit = camera_sad * (1.0 - strictness);

	// A unique, clearly better match found by the mover search: no distance penalty (that guard was for repeating
	// patterns, which the uniqueness test already rejects) and no confirmation needed.
	const bool following = (state & PRE_FOLLOWING) != 0;
	if ((state & 3) == PRE_SEARCH)
	{
		const float4 mo = mover_out[bi];
		if (mo.w > 0.5)
		{
			const int2 mover = int2(round(mo.xy));
			const float s = mo.z;
			float cost = s + penalty_other + (following ? 0.0 : hysteresis);
			if (s < camera_sad * (following ? 0.9 : (1.0 - strictness)) && cost < best_cost) { best_cost = cost; best_sad = s; best = mover; }
		}
	}

	int2 cands[6];
	cands[0] = int2(0, 0);
	if (history)
	{
		cands[1] = int2(round(block_mv_prev[bi]));
		cands[2] = int2(round(block_mv_prev[id.y * blocks.x + (id.x > 0 ? id.x - 1 : 0)]));
		cands[3] = int2(round(block_mv_prev[id.y * blocks.x + min(id.x + 1, blocks.x - 1)]));
		cands[4] = int2(round(block_mv_prev[(id.y > 0 ? id.y - 1 : 0) * blocks.x + id.x]));
		cands[5] = int2(round(block_mv_prev[min(id.y + 1, blocks.y - 1) * blocks.x + id.x]));
	}
	else
	{
		cands[1] = gi; cands[2] = gi; cands[3] = gi; cands[4] = gi; cands[5] = gi;
	}
	[loop] for (int i = 0; i < 6; ++i)
	{
		int2 d = cands[i];
		if (!inside(base, d)) continue;
		uint k = kind_of_int(d, gi);
		if (k == 0) continue; // same as the camera, already scored
		if (k == 1 && !still_allowed) continue;
		float s = block_sad(base, d);
		if (k == 2 && !(last_kind == 2 && all(abs(float2(d) - last_v) <= 1.5)) && s > unconfirmed_limit) continue;
		float cost = s + (k == 1 ? penalty_zero : penalty_other) + (k != last_kind ? hysteresis : 0.0)
		           + (k == 2 ? far_penalty * dot(abs(float2(d) - g), float2(1, 1)) : 0.0);
		if (cost < best_cost) { best_cost = cost; best_sad = s; best = d; }
	}

	// Refine one step in the 3 x 3 neighbourhood (stays inside the previous frame).
	int2 center = best;
	[loop] for (int dy = -1; dy <= 1; ++dy)
	[loop] for (int dx = -1; dx <= 1; ++dx)
	{
		int2 c = center + int2(dx, dy);
		if ((dx == 0 && dy == 0) || !inside(base, c)) continue;
		uint k = kind_of_int(c, gi);
		if (k == 0) continue;
		if (k == 1 && !still_allowed) continue;
		float s = block_sad(base, c);
		if (k == 2 && !(last_kind == 2 && all(abs(float2(c) - last_v) <= 1.5)) && s > unconfirmed_limit) continue;
		float cost = s + (k == 1 ? penalty_zero : penalty_other * 1.25) + (k != last_kind ? hysteresis : 0.0)
		           + (k == 2 ? far_penalty * dot(abs(float2(c) - g), float2(1, 1)) : 0.0);
		if (cost < best_cost) { best_cost = cost; best_sad = s; best = c; }
	}

	// Nothing really matches (content revealed behind the character, or entering at the screen edge): it belongs to
	// the world, which moves with the camera. But only if the best match is not clearly better than the camera: an
	// animated thing (the walking character's legs, a train's wheels) never matches perfectly, yet far better than
	// the camera - it must keep its motion.
	uint kind = kind_of_int(best, gi);
	// A block that had the same kind last frame keeps it unless the camera is nearly as good (no flicker between
	// "still" and "camera" on the walking character's animation).
	const float keep_ratio = (kind == last_kind) ? 0.85 : 0.6;
	// "Still" last frame and still better than the camera at all: stay still (the walking character's animation must
	// not make it alternate).
	const bool keep_still = kind == 1 && last_kind == 1 && best_sad < camera_sad;
	// Revealed ground (behind the walking character, behind a train): the camera points at what used to cover it, so
	// any other candidate looks better; only a genuine match may leave the camera.
	if (kind == 0 || !real_match(base, best, best_sad) || (!keep_still && best_sad > mismatch && best_sad > camera_sad * keep_ratio))
	{
		block_mv[bi] = camera_mark; // the camera, evaluated per pixel with its sub-pixel estimate
		return;
	}
	if (kind == 1)
	{
		block_mv[bi] = float2(0, 0); // still on screen: exactly zero, no sub-pixel jitter
		return;
	}
	// Own motion on belts and other floor (1.2): an empty belt's surface is a repeating pattern (the chevrons) that
	// scrolls, so shifts by one repeat match as well as its real motion - blocks of one belt took opposite directions and
	// kept them (stuck, blurred lanes). Along the motion's axis, any other shift (2+ half-res px away) that matches nearly
	// as well makes it ambiguous: the camera then (the pattern cross-fades, but nothing moves the wrong way). A real
	// mover over the floor (a bot, items on a sparse belt) matches only at its own motion and keeps it.
	[branch] if (layers_valid())
	{
		uint floor_px = 0, upper_px = 0;
		[unroll] for (int my = 0; my < 8; ++my)
		[unroll] for (int mx = 0; mx < 8; ++mx)
		{
			const float mk = entity_mask[min(base + int2(mx, my), int2(half_size) - 1)];
			floor_px += (mk > 0.0 && mk < LAYER_OBJECT - 0.01) ? 1u : 0u;
			upper_px += mk >= LAYER_OBJECT - 0.01 ? 1u : 0u;
		}
		[branch] if (floor_px >= 24 && upper_px <= 8)
		{
			const int2 rel = best - gi;
			const int2 axis = abs(rel.x) >= abs(rel.y) ? int2(sign(rel.x), 0) : int2(0, sign(rel.y));
			bool ambiguous = false;
			[loop] for (int r = -6; r <= 6 && !ambiguous; ++r)
			{
				const int2 d = gi + axis * r;
				if (all(abs(d - best) <= 1) || !inside(base, d)) continue;
				ambiguous = block_sad(base, d) <= 1.3 * best_sad + 0.05;
			}
			if (ambiguous)
			{
				block_mv[bi] = camera_mark;
				return;
			}
		}
	}
	float sp[4]; // left, right, up, down
	[loop] for (int k = 0; k < 4; ++k)
	{
		int2 o = k < 2 ? int2(k * 2 - 1, 0) : int2(0, k * 2 - 5);
		sp[k] = block_sad(base, best + o);
	}
	float2 sub = float2(parabola(sp[0], best_sad, sp[1]), parabola(sp[2], best_sad, sp[3]));
	float2 est = float2(best) + sub;
	// Within a fraction of a pixel of the camera or of "still": that is what it is. Without this, blocks next to
	// each other land on slightly different vectors when the camera sits between pixels (diagonal moves): shimmer.
	if (all(abs(est - g) < 0.75)) est = camera_mark;
	else if (all(abs(est) < 0.75)) est = still_allowed ? float2(0, 0) : camera_mark;
	else if (last_kind == 2 && all(abs(est - last_v) < 1.0))
		est = 0.5 * (est + last_v); // same motion as last frame: average out sub-pixel jitter (belts, trains)
	block_mv[bi] = est;
}

// 3 x 3 vector median over blocks: removes isolated wrong blocks.
[numthreads(8, 8, 1)]
void cs_smooth(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= blocks)) return;
	float2 v[9];
	int n = 0;
	[unroll] for (int dy = -1; dy <= 1; ++dy)
	[unroll] for (int dx = -1; dx <= 1; ++dx)
	{
		int2 b = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(blocks) - 1);
		v[n++] = block_mv[b.y * blocks.x + b.x];
	}
	float best = 1e30;
	float2 m = v[4];
	[unroll] for (int i = 0; i < 9; ++i)
	{
		float d = 0;
		[unroll] for (int j = 0; j < 9; ++j)
			d += abs(v[i].x - v[j].x) + abs(v[i].y - v[j].y);
		if (d < best) { best = d; m = v[i]; }
	}
	// Small movers (a bot, a biter) cover one or two blocks: keep the block's own motion if a neighbour agrees, so the
	// median only removes truly isolated vectors.
	float2 own_v = v[4];
	if (kind_of(own_v) == 2 && kind_of(m) != 2)
	{
		uint agree = 0;
		[unroll] for (int k = 0; k < 9; ++k)
			agree += (k != 4 && kind_of(v[k]) == 2 && all(abs(v[k] - own_v) <= 1.5)) ? 1u : 0u;
		if (agree >= 1) m = own_v;
	}
	block_smooth[id.y * blocks.x + id.x] = m;
	uint dummy;
	InterlockedAdd(stats[kind_of(m)], 1, dummy);
}

// Mean brightness difference over 5 x 5 half-res pixels, current at p against previous at p + d.
float luma_sad5(int2 p, int2 d)
{
	float s = 0;
	int2 hmax = int2(half_size) - 1;
	[unroll] for (int y = -2; y <= 2; ++y)
	[unroll] for (int x = -2; x <= 2; ++x)
	{
		int2 a = clamp(p + int2(x, y), int2(0, 0), hmax);
		int2 q = clamp(a + d, int2(0, 0), hmax);
		s += abs(luma_half[a] - luma_half_prev[q]);
	}
	return s / 25.0;
}

float pixel_sad(int2 p, int2 d)
{
	float s = 0;
	int2 hmax = int2(half_size) - 1;
	[unroll] for (int y = -1; y <= 1; ++y)
	[unroll] for (int x = -1; x <= 1; ++x)
	{
		int2 a = clamp(p + int2(x, y), int2(0, 0), hmax);
		int2 q = clamp(a + d, int2(0, 0), hmax);
		s += abs(detail_half[a] - detail_half_prev[q]);
	}
	return s;
}

// Repeating-pattern guard per pixel, capped: a repeat is a few icons / belt lanes away, while a fast train found by
// the wide search can be 100 px from the camera and must not lose its edges to the distance cost.
#define FAR_CAP 24.0
)hlsl"
		R"hlsl(
// Shadow darkness samples (see shadow_finish). The mask's coverage around q (3 x 3 at 2 px steps): G range, R max.
void mask_around(int2 q, out float gmin, out float gmax, out float rmax)
{
	gmin = 1.0; gmax = 0.0; rmax = 0.0;
	const int2 hi = int2(full_size) - 1;
	[unroll] for (int y = -1; y <= 1; ++y)
	[unroll] for (int x = -1; x <= 1; ++x)
	{
		const float4 m = char_mask.Load(int3(clamp(q + int2(x, y) * 2, int2(0, 0), hi), 0));
		gmin = min(gmin, m.g);
		gmax = max(gmax, m.g);
		rmax = max(rmax, m.r);
	}
}
// q in this frame, q + v_cam the same world point last frame. The shadow stays put on screen with the character, so
// this frame's mask stands for last frame's too. One end well inside the shadow (flat coverage, not a held shadow),
// the other clear of it, neither near the character, both on ground / floor or both on objects.
void shadow_sample(int2 q, float2 v_cam, float g_now)
{
	const float2 pp = float2(q) + v_cam;
	if (any(pp < 1.0) || any(pp > float2(full_size) - 2.0)) return;
	const int2 qp = int2(round(pp));
	const float g_then = char_mask.Load(int3(qp, 0)).g;
	const bool trailing = g_now < 0.004 && g_then >= 0.25; // left the shadow: inside it last frame
	const bool leading = g_then < 0.004 && g_now >= 0.25;  // entered it
	[branch] if (!trailing && !leading) return;
	const int2 qi = trailing ? qp : q, qo = trailing ? q : qp;
	const float4 ci = char_mask.Load(int3(qi, 0));
	if (ci.a < 0.6 * ci.g) return; // a held shadow: its place is only estimated
	float imin, imax, ir, omin, omax, orr;
	mask_around(qi, imin, imax, ir);
	mask_around(qo, omin, omax, orr);
	if (imax - imin > 0.08 || omax > 0.004 || ir > 0.004 || orr > 0.004) return;
	uint cls = ci.b > 0.5 * ci.g ? 1u : 0u;
	if (layers_valid())
	{
		const float li = entity_mask[qi / 2], lo = entity_mask[qo / 2];
		const bool obj_i = li >= LAYER_OBJECT - 0.01, obj_o = lo >= LAYER_OBJECT - 0.01;
		if (obj_i != obj_o || li >= LAYER_ICON - 0.01 || lo >= LAYER_ICON - 0.01) return;
		if (obj_i) cls += 2u;
	}
	const float l_now = luma_full[q], l_then = bl_full_prev(pp);
	const float l_in = trailing ? l_then : l_now, l_out = trailing ? l_now : l_then;
	if (l_out < 0.05) return;
	const float r = l_in / l_out;
	if (r > 1.03 || r < 0.05) return;
	const float c = trailing ? g_then : g_now;
	const float k = saturate((1.0 - r) / c);
	uint dummy;
	InterlockedAdd(stats[SH_HIST + cls * SH_BINS + min(uint(k * float(SH_BINS)), uint(SH_BINS - 1))], uint(c * 16.0 + 0.5), dummy);
	InterlockedAdd(stats[SH_COUNT + cls], 1, dummy);
}
// One thread per half-res pixel, writes 2 x 2 full-res pixels. Near block borders, each pixel picks the motion of
// its own block or a neighbouring block, whichever matches locally: object outlines get 2 px precision.
[numthreads(8, 8, 1)]
void cs_expand(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= half_size)) return;
	int2 p = int2(id.xy);
	int2 b = min(p / 8, int2(blocks) - 1);
	// Camera-marked blocks take the camera motion at this pixel.
	float2 cam = camera_at(float2(p));
	float2 own = block_smooth[b.y * blocks.x + b.x];
	bool use_camera = is_camera(own);
	if (use_camera) own = cam;
	float2 mv = own;

	float2 cands[4];
	bool cand_camera[4];
	cands[0] = block_smooth[b.y * blocks.x + max(b.x - 1, 0)];
	cands[1] = block_smooth[b.y * blocks.x + min(b.x + 1, int(blocks.x) - 1)];
	cands[2] = block_smooth[max(b.y - 1, 0) * blocks.x + b.x];
	cands[3] = block_smooth[min(b.y + 1, int(blocks.y) - 1) * blocks.x + b.x];
	[unroll] for (int j = 0; j < 4; ++j)
	{
		cand_camera[j] = is_camera(cands[j]);
		if (cand_camera[j]) cands[j] = cam;
	}
	bool differs = false;
	[unroll] for (int i = 0; i < 4; ++i)
		differs = differs || any(abs(cands[i] - own) > 0.75);
	// Alt-mode icon pixel: the camera when its block is camera, or when the camera matches it at least as well as the
	// block's own motion (a static icon in a block taken over by a passing train or robot).
	// Beta 36: always - rows of identical module icons match a shift by one icon (or a neighbouring block's vector)
	// about as well as the camera, and running sideways along a row picked that (blurred modules). Icons sit on
	// buildings; nothing they mark moves.
	const bool icon_here = layers_valid() && is_icon(entity_mask[p]);
	if (icon_here && !use_camera)
	{
		use_camera = true;
		own = cam;
		mv = cam;
	}
	if (differs && !(icon_here && use_camera))
	{
		int2 hmax = int2(half_size) - 1;
		// preference for the block's own motion (9 pixels here vs 64 per block: scale the hysteresis)
		float best = pixel_sad(p, int2(round(own))) - (0.03 + hysteresis * 0.15);
		if (!use_camera && any(abs(own) >= 0.25))
			best += far_penalty * (9.0 / 64.0) * min(dot(abs(own - cam), float2(1, 1)), FAR_CAP);
		[unroll] for (int i = 0; i < 4; ++i)
		{
			int2 d = int2(round(cands[i]));
			if (any(p + d < 0) || any(p + d > hmax)) continue;
			if (!cand_camera[i] && all(abs(cands[i]) < 0.25) && !in_still_zone(float2(p))) continue;
			float s = pixel_sad(p, d);
			// own motion far from the camera: on repeating patterns (module rows, belts, assembler grids) a shift by
			// one repeat matches just as well locally; make it pay for the distance (9 px here vs 64 per block)
			if (!cand_camera[i] && any(abs(cands[i]) >= 0.25))
				s += far_penalty * (9.0 / 64.0) * min(dot(abs(cands[i] - cam), float2(1, 1)), FAR_CAP);
			if (s < best) { best = s; mv = cands[i]; use_camera = cand_camera[i]; }
		}
	}

	// The character (or the vehicle being driven) near the centre: the camera follows it, so it stays put on screen.
	// Per pixel, as the block search misses it (a running animation never matches well, and its blocks are mostly
	// ground moving with the camera). Object, floor (shadow) and flying pixels in the still zone while the camera
	// moves: if the camera does not explain the pixel (brightness over 5 x 5 half-res pixels: on the character the
	// camera's shift lands on ground) it follows the camera, i.e. it is still. Brightness, not detail: the armour's
	// flat areas have no detail but clearly differ from the ground. A real mover crossing the centre (a big own motion
	// from the block search) keeps it unless "no motion" matches clearly better. Buildings and belts passing the
	// centre match the camera: unaffected. Held (FSR shows them from the real frame): the still object pixels.
	const float mask_here = layers_valid() ? entity_mask[p] : 0.0;
	bool hold = false;
	// Flying counts too: the character flying in mech armour is drawn with the flying things (bots, the air layer).
	// Bare ground too: the shadow of the flying character is not in the floor snapshot, so it sits on "ground" pixels
	// (bare ground itself always votes for the camera).
	// Alt-mode icon pixels too: the air split puts whatever is drawn early in the air stage there, and the flying
	// character is (it was never held in flight, and got the camera motion icons get). Real icons vote for the camera.
	[branch] if (!exact_mask() && in_still_zone(float2(p)) &&
		any(abs(cam) >= 1.0))
	{
		float l_zero = luma_sad5(p, int2(0, 0));
		const float l_cam = luma_sad5(p, int2(round(cam)));
		// "Stays put" allows a small vertical move: the character hovering in mech armour bobs up and down, and
)hlsl"
		R"hlsl(		// rises / sinks while taking off and landing (it was missed then, only its shadow held). Not on bare
		// ground, where a vertical shift has no meaning for the shadow and would only cost time.
		[branch] if (mask_here > 0.0)
		{
			[unroll] for (int k = 1; k <= 4; ++k)
			{
				const int dy = (k & 1) != 0 ? (k + 1) : -k; // +2, -2, +4, -4 half-res px
				l_zero = min(l_zero, luma_sad5(p, int2(0, dy)));
			}
		}
		const bool big_own = !use_camera && length(mv) >= 2.0 + 0.25 * length(cam);
		bool still = false;
		[branch] if (big_own)
		{
			const float l_given = luma_sad5(p, int2(round(mv)));
			still = l_zero < 0.7 * l_given && l_zero < 0.7 * l_cam;
		}
		else
		{
			// Evidence that this pixel stays put on screen while the world moves under it, averaged over frames: the
			// running / flying animation makes single frames unreliable (parts dropped out: FSR moved them with the
			// ground, copies), but on average the character matches "no motion" better than the camera, and
			// everything else (decoratives, animated machines, trees) the camera better.
			// A vote per frame ("no motion" better than the camera at all: +1, else -1), not the size of the
			// difference: on the running character both matches are poor and the difference small, while its shadow
			// differs strongly; the vote treats them alike. Held while most recent frames voted "stays put".
			const float e = l_cam > l_zero + 0.005 ? 1.0 : -1.0;
			const float score = lerp(still_score[p], e, 0.35);
			still_score[p] = score;
			still = score > 0.2;
		}
		if (still)
		{
			use_camera = false;
			mv = float2(0, 0);
			hold = true; // the character and its shadow (the ground inside the shadow comes along: dark, little to see)
		}
	}

	// Bare ground pixel: the camera, whatever the block decided (exact edges around the player, revealed ground).
	// Floor too (flag 262144, 1.2): belts, rails, pipes, grass and decoratives do not move on their own; an empty belt's
	// scrolling surface repeats and drew wrong motion (stuck, blurred lanes). Items on belts then cross-fade (they move
	// a pixel or two per frame). Their depth becomes the ground's: grass that flickers between ground and floor in the
	// layer split no longer flickers in depth either.
	const bool floor_cam = (flags & 262144) != 0;
	const float layer_here = layers_valid() ? entity_mask[p] : -1.0;
	if (layers_valid() && layer_here == 0 && !hold)
		use_camera = true;
	// Beta 35: a floor pixel keeps its own block's motion where that is certain - the block passed the repeat test in
	// cs_block (or is not mostly floor), the motion is belt-like (at most 6 half-res px from the camera), and this pixel
	// matches it clearly better than the camera. Items and the surface of a belt move together, so a belt whose motion
	// is unambiguous slides (no stepping at 4x); repeating surfaces, a neighbouring belt's vector, and the other belt of
	// a block over two belts take the camera.
	if (floor_cam && layers_valid() && layer_here > 0.0 && layer_here < LAYER_OBJECT - 0.01 && !hold)
	{
		const float2 ob = block_smooth[b.y * blocks.x + b.x];
		bool keep = false;
		[branch] if (!is_camera(ob) && kind_of(ob) == 2)
		{
			const float2 rel = ob - cam;
			[branch] if (max(abs(rel.x), abs(rel.y)) <= 6.0)
				keep = pixel_sad(p, int2(round(ob))) < 0.7 * pixel_sad(p, int2(round(cam))) - 0.01;
		}
		use_camera = !keep;
		if (keep)
			mv = ob;
	}
	// Character painted over (flag 32768): frame generation gets what is under it there - the world, moving with the
	// camera - and the character comes from the UI layer. So the camera's motion and the ground's depth: the world
	// behind the character is interpolated like the rest of it (no noise where it is uncovered).
	// Only objects and flying things: a held shadow (floor, or bare ground in flight) stays in the frame as it is, still -
	// the floor snapshot it would be painted from contains that very shadow, which would then move with the ground.
	const bool as_ground = hold && (flags & 32768) != 0 && layers_valid() && mask_here >= LAYER_OBJECT - 0.01;
	if (as_ground)
		use_camera = true;

	float4 mdl = global_motion[4];
	float2 t_full = global_motion[0].xy * 2.0;
	// ground far, floor (belts, shadows), objects, alt-mode icons, flying (bots) nearest
	const float layer = layers_valid() ? entity_mask[p] : 0.0;
	// 1.3 beta 3 (flag 1048576): alt-mode icons get the depth of the building they sit on. Their own nearer depth puts a
	// depth edge around every icon (zoomed out: around every few pixels, at half-res precision), which frame generation
	// may treat as occlusion boundaries and blend across - the suspect for module rows blurring together with DLSS-G.
	const bool icon_flat = (flags & 1048576) != 0;
	float depth = layer >= LAYER_AIR ? 0.1 : (is_icon(layer) ? (icon_flat ? DEPTH_NEAR : 0.15) : (layer >= LAYER_OBJECT ? DEPTH_NEAR :
		(layer > 0.0 && !floor_cam ? 0.5 * (DEPTH_NEAR + DEPTH_FAR) : DEPTH_FAR)));
	// FSR: the same order, spread far apart once FSR turns depth into distances (near 0.1, far 100: ground ~50,
	// floor ~1, objects ~0.2, flying ~0.13), so it never pulls the character into the ground around it.
	if (flags & 16384)
		depth = layer >= LAYER_AIR ? 0.2 : (is_icon(layer) ? (icon_flat ? 0.5 : 0.4) : (layer >= LAYER_OBJECT ? 0.5 : (layer > 0.0 && !floor_cam ? 0.9 : 0.999)));
	if (as_ground)
		depth = (flags & 16384) ? 0.999 : DEPTH_FAR;
	uint2 f = id.xy * 2;
	// The character's shadow as a darkening laid over (flag 131072: the UI layer is used) once its darkness is known.
	// Darkness samples while the camera moves steadily (no zoom, a good camera match, last frame's image valid).
	const bool shade_on = exact_mask() && (flags & 131072) != 0;
	const bool shade_measure = exact_mask() && (flags & 2) != 0 && abs(mdl.x - 1.0) < 0.0005 && global_motion[0].z > 0.5 &&
		length(global_motion[0].xy) >= 0.75;
	[unroll] for (uint y = 0; y < 2; ++y)
	[unroll] for (uint x = 0; x < 2; ++x)
	{
		uint2 q = f + uint2(x, y);
		// half-res -> full-res pixels; the camera exactly at this full-res pixel
		const float2 v_cam = (mdl.x - 1.0) * (float2(q) - mdl.yz) + t_full;
		float2 v = use_camera ? v_cam : mv * 2.0;
		// 1.3 beta 5: alt-mode icons with their own sprites' shift. Factorio places the world on whole pixels; when the
		// icons are rounded on their own they can land a pixel off the world's shift in some frames.
		if ((flags & 2097152) != 0 && icon_here)
			v = icon_cam;
		float d = depth;
		float hv = hold ? (mask_here >= LAYER_ICON - 0.01 ? 0.5 : (mask_here >= LAYER_OBJECT - 0.01 ? 1.0 : 0.8)) : 0.0;
		[branch] if (exact_mask() && all(q < full_size))
		{
			// Hold values: 1 the character on the ground, 0.5 flying (both laid over or painted over when asked), 0.8 its
			// shadow held as it is (darkness not known yet, or the shadow's place only estimated), below 0.25 the shadow's
			// darkening / 4 (laid over as a darkening; what is under it moves with the camera). The character's outline
			// from 2 % coverage: its anti-aliased edge comes from the real frame too.
			const float4 cm = char_mask.Load(int3(q, 0));
			const bool is_char = cm.r > 0.02, in_shadow = !is_char && cm.g > 0.004;
			[branch] if (shade_measure && !is_char)
				shadow_sample(int2(q), v_cam, cm.g);
			hv = is_char ? (cm.b > 0.2 ? 0.5 : 1.0) : 0.0;
			const bool still_px = !use_camera && all(abs(mv) < 0.25);
			// the shadow's darkness here: -1 unknown (or a held shadow), 0 not darkened (icons, flying things: drawn after)
			float sk = -1.0;
			[branch] if (in_shadow && shade_on && cm.a >= 0.6 * cm.g)
			{
				const uint fly = cm.b > 0.5 * cm.g ? 1u : 0u;
				if (layers_valid() && mask_here >= LAYER_ICON - 0.01)
					sk = 0.0;
				else if (layers_valid() && mask_here >= LAYER_OBJECT - 0.01)
				{
					sk = shadow_k(2u + fly);
					if (sk >= 0.0 && sk < 0.1) sk = 0.0; // objects are not darkened (noise of the measurement)
				}
				else
				{
					sk = shadow_k(fly);
					if (sk < 0.0) sk = shadow_k(1u - fly); // walking and flying shadows: the same blend so far
				}
			}
			if (is_char && (flags & 32768) != 0 && layers_valid())
			{
				v = v_cam;
				d = (flags & 16384) ? 0.999 : DEPTH_FAR;
			}
			else if (is_char)
			{
				v = float2(0, 0);
				d = cm.b > 0.2 ? ((flags & 16384) ? 0.2 : 0.1) : ((flags & 16384) ? 0.5 : DEPTH_NEAR);
			}
			else if (in_shadow && sk > 0.02)
			{
				// still there = the camera (the block search may have taken the shadow for something staying put),
				// own motion kept (belt items)
				hv = 0.25 * min(sk * cm.g, 0.9);
				if (still_px)
					v = v_cam;
			}
			else if (in_shadow && sk < 0.0 && cm.g > 0.2)
			{
				v = float2(0, 0);
				hv = 0.8;
			}
			else if (still_px)
				v = v_cam; // "stays put" outside the character: only the character does (a block mostly covering it)
			// 1.3 beta 1: also while the camera moves less than 1 half-res px per frame. Zoomed out, running moves the
			// world by only 1-2 full-res px per frame, and inside the still zone thin, sharp things (elevated rails, power
			// wires, building edges) matched "no motion" about as well as that sub-pixel shift: they kept zero motion and
			// stood still in generated frames while everything outside the zone slid - a circle around the character,
			// like a force field.
		}
		if (flags & 1) v = -v;
		if ((flags & 2) == 0) v = float2(0, 0);
		if (all(q < full_size)) { mvec[q] = v; depth_out[q] = d; hold_out[q] = hv; }
	}
}

// 1.3 motion fit: how well the motion vectors explain this frame - the previous frame moved by them vs this one, per
// layer, against the camera alone. Every 2nd full-res pixel in x and y; the character and its shadow left out (they
// stay put on screen while their animation changes). Sums per group in shared memory, then one atomic per counter.
groupshared uint gs_fit[FIT_CLASSES * 3];
[numthreads(8, 8, 1)]
void cs_fit(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	if (gi < FIT_CLASSES * 3) gs_fit[gi] = 0;
	GroupMemoryBarrierWithGroupSync();
	const uint2 q = id.xy * 2;
	[branch] if (all(q < full_size) && (flags & 6) == 6)
	{
		float2 v = mvec[q];
		if (flags & 1) v = -v;
		const float4 mdl = global_motion[4];
		const float2 v_cam = (mdl.x - 1.0) * (float2(q) - mdl.yz) + global_motion[0].xy * 2.0;
		const float2 pp = float2(q) + v, pc = float2(q) + v_cam;
		const float2 hi = float2(full_size) - 2.0;
		bool use = all(pp >= 1.0) && all(pp <= hi) && all(pc >= 1.0) && all(pc <= hi);
		if (use && exact_mask())
		{
			const float4 cm = char_mask.Load(int3(q, 0));
			use = cm.r <= 0.02 && cm.g <= 0.004;
		}
		else if (use)
			use = !(all(abs(v) < 0.25) && any(abs(v_cam) >= 1.0)); // held still (the guessed character)
		[branch] if (use)
		{
			const float l = luma_full[q];
			const uint e_own = uint(abs(l - bl_full_prev(pp)) * 256.0 + 0.5);
			const uint e_cam = uint(abs(l - bl_full_prev(pc)) * 256.0 + 0.5);
			uint cls = 5;
			if (layers_valid())
			{
				const float mk = entity_mask[q / 2];
				cls = mk >= LAYER_AIR ? 4u : (is_icon(mk) ? 3u : (mk >= LAYER_OBJECT - 0.01 ? 2u : (mk > 0.0 ? 1u : 0u)));
			}
			uint dummy;
			InterlockedAdd(gs_fit[cls * 3 + 0], e_own, dummy);
			InterlockedAdd(gs_fit[cls * 3 + 1], e_cam, dummy);
			InterlockedAdd(gs_fit[cls * 3 + 2], 1u, dummy);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gi < FIT_CLASSES * 3 && gs_fit[gi] != 0)
	{
		uint dummy;
		InterlockedAdd(stats[FIT_BASE + gi], gs_fit[gi], dummy);
	}
}

// Motion debug picture: the HUD-less frame darkened, tinted by what each pixel was given. No tint: moves with the
// camera. Blue: still (the character). Other colours: own motion, hue = direction. Flickering tints = shimmer sources.
[numthreads(8, 8, 1)]
void cs_debug(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= full_size)) return;
	float4 mdl = global_motion[4];
	float2 cam = (mdl.x - 1.0) * (float2(id.xy) - mdl.yz) + global_motion[0].xy * 2.0;
	float2 v = mvec[id.xy];
	if (flags & 1) v = -v;
	const uint debug_mode = (flags >> 9) & 7;
	if (debug_mode == 1)
	{
		// world layers = depth sent to DLSS-G: dark = ground (far), blue = floor: belts, shadows, white = objects,
		// cyan = alt-mode icons, yellow = flying: robots, effects (nearest); magenta tint everywhere = no snapshots
		float3 g = hudless.Load(int3(id.xy, 0)).rgb * 0.3;
		if (!layers_valid()) { debug_out[id.xy] = float4(g + float3(0.3, 0.0, 0.3), 1); return; }
		float mk = entity_mask[id.xy / 2];
		float3 tint = mk >= LAYER_AIR ? float3(0.75, 0.6, 0.0) : is_icon(mk) ? float3(0.0, 0.55, 0.6) : (mk >= LAYER_OBJECT ? float3(0.7, 0.7, 0.7) : (mk > 0.0 ? float3(0.05, 0.25, 0.6) : float3(0, 0, 0)));
		debug_out[id.xy] = float4(g + tint, 1);
		return;
	}
	if (debug_mode >= 2 && debug_mode <= 6)
	{
		// the snapshots themselves (pre-lighting world picture); magenta = not available this frame
		float4 l = float4(0.6, 0.0, 0.6, 1);
		if (layers_valid())
			l = debug_mode == 2 ? layer_ground.Load(int3(id.xy, 0)) : (debug_mode == 3 ? layer_floor.Load(int3(id.xy, 0)) :
				(debug_mode == 4 ? layer_entities.Load(int3(id.xy, 0)) : (debug_mode == 5 ? layer_world.Load(int3(id.xy, 0)) :
				layer_airsplit.Load(int3(id.xy, 0)))));
		debug_out[id.xy] = float4(l.rgb, 1);
		return;
	}
	float3 c = hudless.Load(int3(id.xy, 0)).rgb * 0.55;
	{
		// still-zone outline
		float2 d = (float2(id.xy) - float2(full_size) * 0.5) / float(full_size.y);
		if (abs(length(d) - still_zone) < 1.5 / float(full_size.y)) { debug_out[id.xy] = float4(0.3, 0.5, 1.0, 1); return; }
	}
	if ((flags & 2) == 0) { debug_out[id.xy] = float4(c, 1); return; }
	if (all(abs(v - cam) < 0.3)) { debug_out[id.xy] = float4(c, 1); return; }
	if (all(abs(v) < 0.3)) { debug_out[id.xy] = float4(c * 0.6 + float3(0.0, 0.15, 0.45), 1); return; }
	float2 r = v - cam; // motion relative to the camera
	float2 n = r / max(length(r), 1e-3);
	float3 tint = float3(0.5 + 0.5 * n.x, 0.5 + 0.5 * n.y, 0.25);
	debug_out[id.xy] = float4(c * 0.5 + tint * 0.5, 1);
}
)hlsl";

	enum pass { PASS_LUMA, PASS_DETAIL, PASS_DOWN, PASS_DOWN8, PASS_GLOBAL_COARSE, PASS_GLOBAL_ZOOM, PASS_GLOBAL_COARSE_PICK, PASS_GLOBAL_FINE, PASS_GLOBAL_PICK,
		PASS_GLOBAL_REFINE, PASS_GLOBAL_FINAL, PASS_BLOCK_PRE, PASS_MOVER, PASS_BLOCK, PASS_SMOOTH, PASS_EXPAND, PASS_DEBUG, PASS_FIT, PASS_COUNT };
	const char *const ENTRY_POINTS[PASS_COUNT] = { "cs_luma", "cs_detail", "cs_down", "cs_down8", "cs_global_coarse", "cs_global_zoom", "cs_global_coarse_pick",
		"cs_global_fine", "cs_global_pick", "cs_global_refine", "cs_global_final", "cs_block_pre", "cs_mover", "cs_block", "cs_smooth", "cs_expand",
		"cs_debug", "cs_fit" };

	struct constants
	{
		uint32_t full_w, full_h, half_w, half_h, quarter_w, quarter_h, eighth_w, eighth_h, blocks_x, blocks_y;
		uint32_t flags;
		float penalty_zero, penalty_other, mismatch, hysteresis;
		uint32_t frame;
		float cursor_x, cursor_y;
		float strictness;
		uint32_t debug_view;
		float far_penalty;
		float still_zone;
		float sprite_cam_x, sprite_cam_y; // 1.3: previous minus current, full-res px
		float icon_cam_x, icon_cam_y;     // 1.3 beta 5: alt-mode icons, previous minus current
	};
	constexpr UINT CONSTANT_COUNT = sizeof(constants) / 4;
	constexpr UINT TABLE_SIZE = 32; // t0, u0..u23, t1..t6 (world layers), t7 (exact character mask)
	constexpr UINT UAV_COUNT = 24;
	constexpr UINT LAYER_SLOT = 1 + UAV_COUNT; // t1 ground, t2 entities, t3 floor, t4 post-cloud, t5 world, from this slot
	constexpr UINT LAYER_COUNT = 6;
	constexpr UINT CHAR_SLOT = LAYER_SLOT + LAYER_COUNT; // t7 exact character mask
	constexpr UINT STATS_BYTES = 768; // uint[3] blocks per kind, searched, wide-searched, movers found; [8..159] shadow darkness (SH_*);
	                                  // [160..177] motion fit (FIT_*)
	constexpr UINT FIT_BASE = 160, FIT_CLASSES = 6;
	constexpr UINT COARSE_N = 33, ZOOM_SCALES = 25, ZOOM_T = 7, ZOOM_VARIANTS = 5, FINE_N = 7, REFINE_N = 7;
	constexpr UINT SAD_COUNT = 2 * COARSE_N * COARSE_N + 2 * ZOOM_SCALES * ZOOM_T * ZOOM_T + ZOOM_VARIANTS * FINE_N * FINE_N + ZOOM_VARIANTS * REFINE_N * REFINE_N;
	constexpr UINT GLOBAL_ENTRIES = 8; // float4 each

	template <class T>
	void release(T *&p)
	{
		if (p != nullptr) { p->Release(); p = nullptr; }
	}

	struct state
	{
		motion::log_fn log = nullptr;
		ID3D12Device *dev = nullptr;
		ID3D12RootSignature *root = nullptr;
		ID3D12PipelineState *pso[PASS_COUNT] = {};
		ID3D12DescriptorHeap *heap = nullptr;
		UINT increment = 0;
		DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;

		ID3D12Resource *luma_half[2] = {}, *luma_quarter[2] = {}, *luma_eighth[2] = {}, *luma_full[2] = {}, *detail_half[2] = {}, *block_mv[2] = {};
		ID3D12Resource *sads = nullptr, *global = nullptr, *mvec = nullptr, *block_smooth = nullptr;
		ID3D12Resource *hold = nullptr; // R8_UNORM, full res: character pixels held still (UAV state)
		ID3D12Resource *still_score = nullptr; // R32_FLOAT, half res: averaged "stays put on screen" evidence
		ID3D12Resource *stats = nullptr, *debug = nullptr, *depth = nullptr, *block_alt = nullptr; // block_alt: block_pre (u19)
		ID3D12Resource *mover_out = nullptr;
		uint32_t searched = 0, wide_searched = 0, movers = 0; // blocks, from the stats (two frames late)
		motion::shadow_info shadow = {};                              // shadow darkness per class (stats, two frames late)
		bool shadow_overlay = false;                          // the shadow is laid over as a darkening (when measured)
		bool debug_written = false;
		float share_camera = 0, share_still = 0, share_own = 0; // block shares, a few frames old
		ID3D12Resource *readback[3] = {};
		void *readback_ptr[3] = {};
		ID3D12QueryHeap *timestamps = nullptr; // TS_PER_SLOT per frame slot: start, camera done, blocks done, end
		ID3D12Resource *timestamp_readback = nullptr;
		void *timestamp_ptr = nullptr;
		uint64_t timestamp_frequency = 0;
		float gpu_ms = 0;

		uint32_t w = 0, h = 0, half_w = 0, half_h = 0, quarter_w = 0, quarter_h = 0, eighth_w = 0, eighth_h = 0, blocks_x = 0, blocks_y = 0;
		ID3D12Resource *bound_hudless = nullptr; // not owned
		ID3D12Resource *bound_char = nullptr;    // not owned
		bool exact_used = false;                 // the last frame used the exact character mask
		ID3D12Resource *bound_layers[LAYER_COUNT] = {}; // not owned
		ID3D12Resource *entity_mask = nullptr;
		bool layers_used = false;
		uint64_t frame = 0;
		uint32_t history = 0; // frames since (re)initialisation
		float gdx = 0, gdy = 0, gconf = 0, gzoom = 1;
		char status[200] = "off";
		float gpu_camera_ms = 0, gpu_block_ms = 0, gpu_rest_ms = 0; // per stage, smoothed
		bool pipelines_ready = false, pipelines_failed = false;
		// 1.3: the sprites' camera shift per readback slot (previous minus current, full-res px), for the comparison
		bool scam_valid[3] = {}, scam_used[3] = {};
		float scam_x[3] = {}, scam_y[3] = {};
		bool scam_wanted = false, scam_used_last = false;
		int scam_support[3] = {}, scam_tight[3] = {};
		float stage_diag[3][12] = {};
		bool icon_used[3] = {};
		uint32_t trace_left = 0;  // camera trace: frames still to record
		bool trace_done = false;
		std::string trace;
		uint32_t cam_frames = 0;           // compared frames (camera moving)
		float cam_diff = 0, cam_off = 0;   // smoothed |picture fit - sprites| (px) and share of frames over 0.25 px
		// 1.3: motion fit, smoothed (per class: mean luma difference with our vectors / camera alone, share of samples)
		float fit_ours[6] = {}, fit_cam[6] = {}, fit_share[6] = {};
		bool fit_valid = false;
	};
	state *m = nullptr;
	char g_off[] = "off";

	void log_line(const char *fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (m != nullptr && m->log != nullptr)
			m->log(buf);
	}

	ID3D12Resource *create_texture(uint32_t w, uint32_t h, DXGI_FORMAT format)
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		ID3D12Resource *res = nullptr;
		if (FAILED(m->dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res))))
			return nullptr;
		return res;
	}

	ID3D12Resource *create_buffer(uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags)
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = type;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = flags;
		ID3D12Resource *res = nullptr;
		if (FAILED(m->dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res))))
			return nullptr;
		return res;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(UINT table, UINT slot)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = m->heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(table * TABLE_SIZE + slot) * m->increment;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpu_table(UINT table)
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = m->heap->GetGPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<UINT64>(table * TABLE_SIZE) * m->increment;
		return h;
	}

	void write_texture_uav(UINT table, UINT slot, ID3D12Resource *res, DXGI_FORMAT format)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
		d.Format = format;
		d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		m->dev->CreateUnorderedAccessView(res, nullptr, &d, cpu_slot(table, slot));
	}

	void write_buffer_uav(UINT table, UINT slot, ID3D12Resource *res, UINT elements, UINT stride)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		d.Buffer.NumElements = elements;
		d.Buffer.StructureByteStride = stride;
		m->dev->CreateUnorderedAccessView(res, nullptr, &d, cpu_slot(table, slot));
	}

	void write_hudless_srv(ID3D12Resource *hudless)
	{
		for (UINT table = 0; table < 2; ++table)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
			d.Format = m->color_format;
			d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			d.Texture2D.MipLevels = 1;
			m->dev->CreateShaderResourceView(hudless, &d, cpu_slot(table, 0));
		}
		m->bound_hudless = hudless;
	}

	// The exact character mask (R8G8B8A8_UNORM); null writes a null descriptor (reads 0).
	void write_char_srv(ID3D12Resource *res)
	{
		for (UINT table = 0; table < 2; ++table)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			d.Texture2D.MipLevels = 1;
			m->dev->CreateShaderResourceView(res, &d, cpu_slot(table, CHAR_SLOT));
		}
		m->bound_char = res;
	}

	void write_layer_srvs(ID3D12Resource *const *res)
	{
		for (UINT table = 0; table < 2; ++table)
			for (UINT i = 0; i < LAYER_COUNT; ++i)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
				d.Format = m->color_format;
				d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				d.Texture2D.MipLevels = 1;
				m->dev->CreateShaderResourceView(res[i], &d, cpu_slot(table, LAYER_SLOT + i));
			}
		for (UINT i = 0; i < LAYER_COUNT; ++i)
			m->bound_layers[i] = res[i];
	}

	void release_size_dependent()
	{
		for (int i = 0; i < 2; ++i)
		{
			release(m->luma_half[i]);
			release(m->luma_quarter[i]);
			release(m->luma_eighth[i]);
			release(m->luma_full[i]);
			release(m->detail_half[i]);
			release(m->block_mv[i]);
		}
		release(m->mvec);
		release(m->block_smooth);
		release(m->debug);
		release(m->depth);
		release(m->hold);
		release(m->still_score);
		release(m->block_alt);
		release(m->mover_out);
		m->bound_hudless = nullptr;
		m->bound_char = nullptr;
		for (auto &l : m->bound_layers)
			l = nullptr;
		release(m->entity_mask);
	}

	bool create_size_dependent(uint32_t w, uint32_t h)
	{
		release_size_dependent();
		m->w = w;
		m->h = h;
		m->half_w = w / 2 > 0 ? w / 2 : 1;
		m->half_h = h / 2 > 0 ? h / 2 : 1;
		m->quarter_w = m->half_w / 2 > 0 ? m->half_w / 2 : 1;
		m->quarter_h = m->half_h / 2 > 0 ? m->half_h / 2 : 1;
		m->eighth_w = m->quarter_w / 2 > 0 ? m->quarter_w / 2 : 1;
		m->eighth_h = m->quarter_h / 2 > 0 ? m->quarter_h / 2 : 1;
		m->blocks_x = (m->half_w + 7) / 8;
		m->blocks_y = (m->half_h + 7) / 8;
		const UINT block_count = m->blocks_x * m->blocks_y;

		for (int i = 0; i < 2; ++i)
		{
			m->luma_half[i] = create_texture(m->half_w, m->half_h, DXGI_FORMAT_R32_FLOAT);
			m->luma_quarter[i] = create_texture(m->quarter_w, m->quarter_h, DXGI_FORMAT_R32_FLOAT);
			m->luma_eighth[i] = create_texture(m->eighth_w, m->eighth_h, DXGI_FORMAT_R32_FLOAT);
			m->luma_full[i] = create_texture(w, h, DXGI_FORMAT_R32_FLOAT);
			m->detail_half[i] = create_texture(m->half_w, m->half_h, DXGI_FORMAT_R32_FLOAT);
			m->block_mv[i] = create_buffer(static_cast<uint64_t>(block_count) * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			if (m->luma_half[i] == nullptr || m->luma_quarter[i] == nullptr || m->luma_eighth[i] == nullptr || m->luma_full[i] == nullptr || m->detail_half[i] == nullptr || m->block_mv[i] == nullptr)
				return false;
		}
		m->mvec = create_texture(w, h, DXGI_FORMAT_R16G16_FLOAT);
		m->debug = create_texture(w, h, m->color_format);
		m->depth = create_texture(w, h, DXGI_FORMAT_R32_FLOAT);
		m->hold = create_texture(w, h, DXGI_FORMAT_R8_UNORM);
		m->still_score = create_texture(m->half_w, m->half_h, DXGI_FORMAT_R32_FLOAT);
		m->entity_mask = create_texture(m->half_w, m->half_h, DXGI_FORMAT_R32_FLOAT);
		m->block_alt = create_buffer(static_cast<uint64_t>(block_count) * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		m->mover_out = create_buffer(static_cast<uint64_t>(block_count) * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		m->block_smooth = create_buffer(static_cast<uint64_t>(block_count) * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		if (m->mvec == nullptr || m->block_smooth == nullptr || m->depth == nullptr || m->hold == nullptr || m->still_score == nullptr || m->block_alt == nullptr || m->entity_mask == nullptr ||
			m->mover_out == nullptr)
			return false;

		// Table p is used on frames with parity p: "current" resources are index p, "previous" are 1 - p.
		for (UINT p = 0; p < 2; ++p)
		{
			const UINT q = 1 - p;
			write_texture_uav(p, 1, m->luma_half[p], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 2, m->luma_half[q], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 3, m->luma_quarter[p], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 4, m->luma_quarter[q], DXGI_FORMAT_R32_FLOAT);
			write_buffer_uav(p, 5, m->block_mv[p], block_count, 8);
			write_buffer_uav(p, 6, m->block_mv[q], block_count, 8);
			write_buffer_uav(p, 7, m->sads, SAD_COUNT, 4);
			write_buffer_uav(p, 8, m->global, GLOBAL_ENTRIES, 16);
			write_texture_uav(p, 9, m->mvec, DXGI_FORMAT_R16G16_FLOAT);
			write_buffer_uav(p, 10, m->block_smooth, block_count, 8);
			write_texture_uav(p, 11, m->luma_eighth[p], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 12, m->luma_eighth[q], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 13, m->luma_full[p], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 14, m->luma_full[q], DXGI_FORMAT_R32_FLOAT);
			write_buffer_uav(p, 15, m->stats, STATS_BYTES / 4, 4);
			if (m->debug != nullptr)
				write_texture_uav(p, 16, m->debug, m->color_format);
			write_texture_uav(p, 17, m->detail_half[p], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 18, m->detail_half[q], DXGI_FORMAT_R32_FLOAT);
			write_texture_uav(p, 19, m->depth, DXGI_FORMAT_R32_FLOAT);
			write_buffer_uav(p, 20, m->block_alt, block_count, 16);
			write_texture_uav(p, 21, m->entity_mask, DXGI_FORMAT_R32_FLOAT);
			write_buffer_uav(p, 22, m->mover_out, block_count, 16);
			write_texture_uav(p, 23, m->hold, DXGI_FORMAT_R8_UNORM);
			write_texture_uav(p, 24, m->still_score, DXGI_FORMAT_R32_FLOAT);
		}
		write_char_srv(nullptr);
		m->history = 0;
		return true;
	}

	// Shader compilation: once per process, on a worker thread (fxc needs a long time for the block search; on the
	// render thread that was a black screen at start), and cached on disk next to the add-on keyed by the shader
	// source, so later starts load it in milliseconds. Until it is done, motion is inactive (zero motion vectors).
	constexpr UINT COMPILE_FLAGS = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	constexpr UINT TS_PER_SLOT = 4;

	struct compile_job
	{
		volatile LONG state = 0; // 0 not started, 1 running, 2 done, 3 failed
		volatile LONG compiled = 0; // entry points done (progress)
		std::vector<unsigned char> code[PASS_COUNT];
		char message[1024] = {}; // for the log, written by the worker, read once it is done
		bool reported = false;
	};
	compile_job g_job;

	uint64_t shader_key()
	{
		uint64_t h = 1469598103934665603ull;
		auto mix = [&h](const void *data, size_t n) {
			const unsigned char *b = static_cast<const unsigned char *>(data);
			for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
		};
		mix(SHADER_SOURCE, sizeof(SHADER_SOURCE) - 1);
		for (const char *e : ENTRY_POINTS)
			mix(e, std::strlen(e) + 1);
		const UINT flags = COMPILE_FLAGS;
		mix("cs_5_1", 6);
		mix(&flags, sizeof(flags));
		return h;
	}

	std::wstring cache_path()
	{
		HMODULE self = nullptr;
		wchar_t buf[MAX_PATH * 2] = {};
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&shader_key), &self))
			return {};
		const DWORD n = GetModuleFileNameW(self, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
		std::wstring path(buf, n);
		const size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
			return {};
		return path.substr(0, slash + 1) + L"factorio_fg_shaders.cache";
	}

	constexpr uint32_t CACHE_MAGIC = 0x43534746; // "FGSC"

	bool load_cache(compile_job &job, uint64_t key)
	{
		const std::wstring path = cache_path();
		FILE *f = path.empty() ? nullptr : _wfopen(path.c_str(), L"rb");
		if (f == nullptr)
			return false;
		uint32_t magic = 0, count = 0;
		uint64_t stored = 0;
		bool ok = std::fread(&magic, 4, 1, f) == 1 && std::fread(&stored, 8, 1, f) == 1 && std::fread(&count, 4, 1, f) == 1 &&
			magic == CACHE_MAGIC && stored == key && count == PASS_COUNT;
		for (int i = 0; ok && i < PASS_COUNT; ++i)
		{
			uint32_t size = 0;
			ok = std::fread(&size, 4, 1, f) == 1 && size > 0 && size < (16u << 20);
			if (ok)
			{
				job.code[i].resize(size);
				ok = std::fread(job.code[i].data(), 1, size, f) == size;
			}
		}
		std::fclose(f);
		return ok;
	}

	void save_cache(const compile_job &job, uint64_t key)
	{
		const std::wstring path = cache_path();
		FILE *f = path.empty() ? nullptr : _wfopen(path.c_str(), L"wb");
		if (f == nullptr)
			return;
		const uint32_t magic = CACHE_MAGIC, count = PASS_COUNT;
		std::fwrite(&magic, 4, 1, f);
		std::fwrite(&key, 8, 1, f);
		std::fwrite(&count, 4, 1, f);
		for (int i = 0; i < PASS_COUNT; ++i)
		{
			const uint32_t size = static_cast<uint32_t>(job.code[i].size());
			std::fwrite(&size, 4, 1, f);
			std::fwrite(job.code[i].data(), 1, size, f);
		}
		std::fclose(f);
	}

	// Worker: compiles every entry point, keeps the bytecode, writes the cache. Holds a reference on this module so
	// it cannot be unloaded underneath (released by FreeLibraryAndExitThread).
	DWORD WINAPI compile_thread(void *module)
	{
		compile_job &job = g_job;
		HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler != nullptr ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
		bool ok = compile != nullptr;
		if (!ok)
			std::snprintf(job.message, sizeof(job.message), "motion: d3dcompiler_47.dll not available");
		LARGE_INTEGER freq = {}, t0 = {}, t1 = {};
		QueryPerformanceFrequency(&freq);
		std::string times;
		double total = 0;
		for (int i = 0; ok && i < PASS_COUNT; ++i)
		{
			ID3DBlob *code = nullptr, *err = nullptr;
			QueryPerformanceCounter(&t0);
			const HRESULT hr = compile(SHADER_SOURCE, sizeof(SHADER_SOURCE) - 1, "motion.hlsl", nullptr, nullptr, ENTRY_POINTS[i], "cs_5_1",
				COMPILE_FLAGS, 0, &code, &err);
			QueryPerformanceCounter(&t1);
			const double sec = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
			total += sec;
			if (FAILED(hr) || code == nullptr)
			{
				std::snprintf(job.message, sizeof(job.message), "motion: compiling %s failed: %.700s", ENTRY_POINTS[i],
					err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
				ok = false;
			}
			else
			{
				const unsigned char *b = static_cast<const unsigned char *>(code->GetBufferPointer());
				job.code[i].assign(b, b + code->GetBufferSize());
				InterlockedIncrement(&job.compiled);
				if (sec >= 0.5)
				{
					char t[64];
					std::snprintf(t, sizeof(t), " %s %.1f s", ENTRY_POINTS[i], sec);
					times += t;
				}
			}
			release(code);
			release(err);
		}
		if (ok)
		{
			save_cache(job, shader_key());
			std::snprintf(job.message, sizeof(job.message), "motion: shaders compiled in %.1f s (slowest:%s), cached for the next start",
				total, times.empty() ? " none over 0.5 s" : times.c_str());
		}
		InterlockedExchange(&job.state, ok ? 2 : 3);
		FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
		return 0;
	}

	void start_compile()
	{
		if (InterlockedCompareExchange(&g_job.state, 1, 0) != 0)
			return; // running or done
		if (load_cache(g_job, shader_key()))
		{
			std::snprintf(g_job.message, sizeof(g_job.message), "motion: shaders loaded from the cache");
			InterlockedExchange(&g_job.state, 2);
			return;
		}
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&start_compile), &self); // +1 reference
		HANDLE th = CreateThread(nullptr, 0, compile_thread, self, 0, nullptr);
		if (th == nullptr)
		{
			if (self != nullptr)
				FreeLibrary(self);
			std::snprintf(g_job.message, sizeof(g_job.message), "motion: could not start the shader compile thread");
			InterlockedExchange(&g_job.state, 3);
			return;
		}
		CloseHandle(th);
	}

	// Render thread: once the worker is done, create the pipelines (fast). True when they exist.
	bool finish_pipelines()
	{
		if (m->pipelines_ready)
			return true;
		const LONG st = InterlockedCompareExchange(&g_job.state, 0, 0);
		if (st < 2)
			return false;
		if (!g_job.reported)
		{
			log_line("%s", g_job.message);
			g_job.reported = true;
		}
		if (st == 3 || m->pipelines_failed)
		{
			if (!m->pipelines_failed)
				std::snprintf(m->status, sizeof(m->status), "shader compilation failed (see ReShade.log): zero motion vectors");
			m->pipelines_failed = true;
			return false;
		}
		for (int i = 0; i < PASS_COUNT; ++i)
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature = m->root;
			pd.CS.pShaderBytecode = g_job.code[i].data();
			pd.CS.BytecodeLength = g_job.code[i].size();
			const HRESULT hr = m->dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m->pso[i]));
			if (FAILED(hr))
			{
				log_line("motion: pipeline %s failed (0x%08X)", ENTRY_POINTS[i], static_cast<unsigned>(hr));
				std::snprintf(m->status, sizeof(m->status), "pipeline creation failed: zero motion vectors");
				m->pipelines_failed = true;
				return false;
			}
		}
		m->pipelines_ready = true;
		m->history = 0;
		std::snprintf(m->status, sizeof(m->status), "ready");
		log_line("motion: ready, %ux%u blocks of 16x16 px", m->blocks_x, m->blocks_y);
		return true;
	}

	bool build_pipelines()
	{
		start_compile();

		// Root signature: constants (b0) + one table {t0, u0..u13}.
		D3D12_DESCRIPTOR_RANGE ranges[4] = {};
		ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[3].NumDescriptors = 1;
		ranges[3].BaseShaderRegister = 7;
		ranges[3].OffsetInDescriptorsFromTableStart = CHAR_SLOT;
		ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[2].NumDescriptors = LAYER_COUNT;
		ranges[2].BaseShaderRegister = 1;
		ranges[2].OffsetInDescriptorsFromTableStart = LAYER_SLOT;
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 1;
		ranges[0].BaseShaderRegister = 0;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = UAV_COUNT;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].OffsetInDescriptorsFromTableStart = 1;
		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = CONSTANT_COUNT;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 4;
		params[1].DescriptorTable.pDescriptorRanges = ranges;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rs = {};
		rs.NumParameters = 2;
		rs.pParameters = params;
		ID3DBlob *blob = nullptr, *errors = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
		if (SUCCEEDED(hr))
			hr = m->dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m->root));
		release(blob);
		release(errors);
		if (FAILED(hr))
		{
			log_line("motion: root signature failed (0x%08X)", static_cast<unsigned>(hr));
			return false;
		}

		return true; // pipelines follow in finish_pipelines()
	}

	void uav_barrier(ID3D12GraphicsCommandList *list)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		b.UAV.pResource = nullptr; // all UAV accesses
		list->ResourceBarrier(1, &b);
	}

	void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
		list->ResourceBarrier(1, &b);
	}

	UINT groups(uint32_t n, uint32_t size)
	{
		return (n + size - 1) / size;
	}
}

namespace motion
{
	bool init(ID3D12Device *device, uint32_t width, uint32_t height, DXGI_FORMAT color_format, log_fn log)
	{
		shutdown();
		m = new state();
		m->log = log;
		m->dev = device;
		m->dev->AddRef();
		m->color_format = color_format;

		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 2 * TABLE_SIZE;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		bool ok = SUCCEEDED(m->dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m->heap)));
		if (ok)
		{
			m->increment = m->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			m->sads = create_buffer(SAD_COUNT * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			m->global = create_buffer(GLOBAL_ENTRIES * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			m->stats = create_buffer(STATS_BYTES, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			for (int i = 0; i < 3 && ok; ++i)
			{
				m->readback[i] = create_buffer(GLOBAL_ENTRIES * 16 + STATS_BYTES, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
				const D3D12_RANGE none = { 0, 0 };
				ok = m->readback[i] != nullptr && SUCCEEDED(m->readback[i]->Map(0, &none, &m->readback_ptr[i]));
			}
			ok = ok && m->sads != nullptr && m->global != nullptr && m->stats != nullptr && build_pipelines() && create_size_dependent(width, height);
			if (ok)
			{
				// GPU timing is optional: failure only hides the number.
				D3D12_QUERY_HEAP_DESC qd = {};
				qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
				qd.Count = 3 * TS_PER_SLOT;
				if (SUCCEEDED(m->dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&m->timestamps))))
				{
					m->timestamp_readback = create_buffer(3 * TS_PER_SLOT * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
					const D3D12_RANGE none = { 0, 0 };
					if (m->timestamp_readback == nullptr || FAILED(m->timestamp_readback->Map(0, &none, &m->timestamp_ptr)))
					{
						release(m->timestamp_readback);
						release(m->timestamps);
						m->timestamp_ptr = nullptr;
					}
				}
			}
		}
		if (!ok)
		{
			log_line("motion: initialisation failed, DLSS-G keeps zero motion vectors");
			shutdown();
			return false;
		}
		std::snprintf(m->status, sizeof(m->status), "compiling shaders (first start after an update, about a minute): zero motion vectors meanwhile");
		finish_pipelines(); // at once when they came from the cache
		return true;
	}

	bool resize(uint32_t width, uint32_t height)
	{
		if (m == nullptr)
			return false;
		if (width == m->w && height == m->h)
			return true;
		if (!create_size_dependent(width, height))
		{
			log_line("motion: resize to %ux%u failed", width, height);
			shutdown();
			return false;
		}
		return true;
	}

	void shutdown()
	{
		if (m == nullptr)
			return;
		release_size_dependent();
		for (int i = 0; i < 3; ++i)
			release(m->readback[i]);
		release(m->sads);
		release(m->global);
		release(m->stats);
		release(m->timestamp_readback);
		release(m->timestamps);
		for (auto &p : m->pso)
			release(p);
		release(m->root);
		release(m->heap);
		release(m->dev);
		delete m;
		m = nullptr;
	}

	bool active()
	{
		return m != nullptr && finish_pipelines();
	}

	ID3D12Resource *record(ID3D12GraphicsCommandList *list, ID3D12Resource *hudless, const layer_set &layer_in, const settings &options,
		split_fn split, void *split_context)
	{
		if (m == nullptr || hudless == nullptr || list == nullptr)
			return nullptr;
		// SRV order: t1 ground, t2 entities, t3 floor, t4 post-cloud, t5 world, t6 air split
		ID3D12Resource *const res[LAYER_COUNT] = { layer_in.ground, layer_in.entities, layer_in.floor, layer_in.postcloud, layer_in.world,
			layer_in.airsplit };
		bool layers = options.world_layers;
		for (ID3D12Resource *r : res)
			layers = layers && r != nullptr;
		bool rebind = false;
		for (UINT i = 0; i < LAYER_COUNT; ++i)
			rebind = rebind || res[i] != m->bound_layers[i];
		if (layers && rebind)
			write_layer_srvs(res);
		m->layers_used = layers;

		if (hudless != m->bound_hudless)
		{
			write_hudless_srv(hudless);
			m->history = 0;
		}
		ID3D12Resource *const charm = layer_in.character;
		if (charm != nullptr && charm != m->bound_char)
			write_char_srv(charm);
		m->exact_used = charm != nullptr;

		const UINT parity = static_cast<UINT>(m->frame & 1);
		constants c = {};
		c.full_w = m->w; c.full_h = m->h;
		c.half_w = m->half_w; c.half_h = m->half_h;
		c.quarter_w = m->quarter_w; c.quarter_h = m->quarter_h;
		c.eighth_w = m->eighth_w; c.eighth_h = m->eighth_h;
		c.blocks_x = m->blocks_x; c.blocks_y = m->blocks_y;
		c.flags = (options.flip_sign ? 1u : 0u) | ((options.enabled && m->history >= 1) ? 2u : 0u)
			| (m->history >= 2 ? 4u : 0u)  // last frame's camera motion came from valid images
			| (m->history >= 3 ? 8u : 0u)  // last frame's block motion too
			| (options.mover_search ? 128u : 0u)
			| (layers ? 256u : 0u)
			| ((options.debug_layer_view & 7u) << 9)
			| (options.icons_first ? 4096u : 0u)
			| (options.wide_search ? 8192u : 0u)
			| (options.spread_depth ? 16384u : 0u)
			| (options.hold_as_ground ? 32768u : 0u)
			| (charm != nullptr ? 65536u : 0u)
			| (charm != nullptr && options.shadow_overlay ? 131072u : 0u)
			| (options.floor_camera ? 262144u : 0u)
			| (options.sprite_camera && options.sprite_camera_valid ? 524288u : 0u)
			| (options.icon_depth_flat ? 1048576u : 0u)
			| (options.icon_motion && options.icon_shift_valid && options.sprite_camera_valid ? 2097152u : 0u);
		// SAD units: sum over 64 half-res pixels of |luma difference| (each 0..1)
		c.penalty_zero = options.penalty_zero;
		c.penalty_other = options.penalty_zero * 1.4f;
		c.mismatch = options.mismatch;
		c.hysteresis = options.hysteresis;
		c.frame = static_cast<uint32_t>(m->frame);
		c.cursor_x = options.cursor_x;
		c.cursor_y = options.cursor_y;
		c.strictness = options.strictness;
		c.debug_view = options.debug_view ? 1u : 0u;
		c.far_penalty = options.far_penalty;
		c.still_zone = options.still_zone;
		c.sprite_cam_x = -options.sprite_camera_dx; // the sprites moved by d: a pixel was at p - d
		c.sprite_cam_y = -options.sprite_camera_dy;
		c.icon_cam_x = -options.icon_dx;
		c.icon_cam_y = -options.icon_dy;

		// State for a (new) command list: bindings, buffers back to UAV (they decay to COMMON after every
		// ExecuteCommandLists), the inputs readable.
		auto begin_list = [&](ID3D12GraphicsCommandList *l) {
			ID3D12DescriptorHeap *heaps[] = { m->heap };
			l->SetDescriptorHeaps(1, heaps);
			l->SetComputeRootSignature(m->root);
			l->SetComputeRoot32BitConstants(0, CONSTANT_COUNT, &c, 0);
			l->SetComputeRootDescriptorTable(1, gpu_table(parity));
			ID3D12Resource *const buffers[] = { m->sads, m->global, m->block_mv[0], m->block_mv[1], m->block_smooth, m->stats, m->block_alt,
				m->mover_out };
			D3D12_RESOURCE_BARRIER to_uav[9] = {};
			for (int i = 0; i < 8; ++i)
			{
				to_uav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				to_uav[i].Transition.pResource = buffers[i];
				to_uav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				to_uav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
				to_uav[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			to_uav[8].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			to_uav[8].Transition.pResource = hudless;
			to_uav[8].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			to_uav[8].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			to_uav[8].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			l->ResourceBarrier(9, to_uav);
			if (layers)
			{
				for (ID3D12Resource *r : res)
					transition(l, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}
			if (charm != nullptr)
				transition(l, charm, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		};
		// Inputs back to COMMON (shared with D3D11, and the state the caller expects).
		auto end_inputs = [&](ID3D12GraphicsCommandList *l) {
			transition(l, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (layers)
			{
				for (ID3D12Resource *r : res)
					transition(l, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			}
			if (charm != nullptr)
				transition(l, charm, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		};
		begin_list(list);

		const UINT slot = static_cast<UINT>(m->frame % 3);
		m->scam_valid[slot] = options.sprite_camera_valid && m->history >= 2;
		m->scam_used[slot] = (c.flags & 524288u) != 0;
		m->scam_x[slot] = c.sprite_cam_x;
		m->scam_y[slot] = c.sprite_cam_y;
		m->scam_wanted = options.sprite_camera;
		m->scam_support[slot] = options.sprite_camera_support;
		m->scam_tight[slot] = options.sprite_camera_tight;
		std::memcpy(m->stage_diag[slot], options.stage_diag, sizeof(options.stage_diag));
		m->icon_used[slot] = (c.flags & 2097152u) != 0;
		if (m->timestamps != nullptr)
			list->EndQuery(m->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT);
		list->SetPipelineState(m->pso[PASS_LUMA]);
		list->Dispatch(groups(m->half_w, 8), groups(m->half_h, 8), 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_DETAIL]);
		list->Dispatch(groups(m->half_w, 8), groups(m->half_h, 8), 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_DOWN]);
		list->Dispatch(groups(m->quarter_w, 8), groups(m->quarter_h, 8), 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_DOWN8]);
		list->Dispatch(groups(m->eighth_w, 8), groups(m->eighth_h, 8), 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_GLOBAL_COARSE]);
		list->Dispatch(COARSE_N * COARSE_N, 2, 1);
		list->SetPipelineState(m->pso[PASS_GLOBAL_ZOOM]);
		list->Dispatch(ZOOM_T * ZOOM_T, ZOOM_SCALES, 2); // independent of the pan search: no barrier between
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_GLOBAL_COARSE_PICK]);
		list->Dispatch(1, 1, 1);
		uav_barrier(list);
		if (split != nullptr)
		{
			// The scene-cut metric (coarse SAD at the best camera shift, [2].w) is known now: copy it for the CPU and
			// let the caller submit this much, so its wait does not include the rest of the motion passes.
			transition(list, m->global, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->CopyBufferRegion(m->readback[slot], 0, m->global, 0, 3 * 16);
			transition(list, m->global, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			end_inputs(list);
			split(split_context); // closes, submits and resets 'list'
			begin_list(list);
		}
		list->SetPipelineState(m->pso[PASS_GLOBAL_FINE]);
		list->Dispatch(FINE_N * FINE_N, ZOOM_VARIANTS, 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_GLOBAL_PICK]);
		list->Dispatch(1, 1, 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_GLOBAL_REFINE]);
		list->Dispatch(REFINE_N * REFINE_N, ZOOM_VARIANTS, 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_GLOBAL_FINAL]);
		list->Dispatch(1, 1, 1);
		uav_barrier(list);
		if (m->timestamps != nullptr)
			list->EndQuery(m->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT + 1);
		list->SetPipelineState(m->pso[PASS_BLOCK_PRE]);
		list->Dispatch(groups(m->blocks_x, 8), groups(m->blocks_y, 8), 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_MOVER]);
		list->Dispatch(m->blocks_x, m->blocks_y, 1); // one group of 64 threads per block (most return at once)
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_BLOCK]);
		list->Dispatch(groups(m->blocks_x, 8), groups(m->blocks_y, 8), 1);
		uav_barrier(list);
		if (m->timestamps != nullptr)
			list->EndQuery(m->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT + 2);
		list->SetPipelineState(m->pso[PASS_SMOOTH]);
		list->Dispatch(groups(m->blocks_x, 8), groups(m->blocks_y, 8), 1);
		uav_barrier(list);
		list->SetPipelineState(m->pso[PASS_EXPAND]);
		list->Dispatch(groups(m->half_w, 8), groups(m->half_h, 8), 1);
		uav_barrier(list);
		if (options.fit_measure)
		{
			list->SetPipelineState(m->pso[PASS_FIT]);
			list->Dispatch(groups((m->w + 1) / 2, 8), groups((m->h + 1) / 2, 8), 1);
			uav_barrier(list);
		}
		m->debug_written = false;
		if (options.debug_view && m->debug != nullptr)
		{
			list->SetPipelineState(m->pso[PASS_DEBUG]);
			list->Dispatch(groups(m->w, 8), groups(m->h, 8), 1);
			uav_barrier(list);
			m->debug_written = true;
		}

		end_inputs(list);

		if (m->timestamps != nullptr)
		{
			list->EndQuery(m->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT + 3);
			list->ResolveQueryData(m->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT, TS_PER_SLOT, m->timestamp_readback,
				slot * TS_PER_SLOT * 8);
			// read the slot of two frames ago: finished by now
			const uint64_t *t = static_cast<const uint64_t *>(m->timestamp_ptr) + ((slot + 1) % 3) * TS_PER_SLOT;
			if (m->timestamp_frequency != 0 && t[3] > t[0] && t[1] >= t[0] && t[2] >= t[1] && t[3] >= t[2] && m->frame >= 3)
			{
				const double f = 1000.0 / static_cast<double>(m->timestamp_frequency);
				// with a split, [0]..[1] also holds the gap until the second part was submitted: report the passes
				m->gpu_camera_ms = m->gpu_camera_ms * 0.9f + static_cast<float>(static_cast<double>(t[1] - t[0]) * f) * 0.1f;
				m->gpu_block_ms = m->gpu_block_ms * 0.9f + static_cast<float>(static_cast<double>(t[2] - t[1]) * f) * 0.1f;
				m->gpu_rest_ms = m->gpu_rest_ms * 0.9f + static_cast<float>(static_cast<double>(t[3] - t[2]) * f) * 0.1f;
				m->gpu_ms = m->gpu_camera_ms + m->gpu_block_ms + m->gpu_rest_ms;
			}
		}

		// Global motion to the CPU: for the overlay (read two frames later) and, after a GPU wait, for scene cuts.
		transition(list, m->global, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->CopyBufferRegion(m->readback[slot], 0, m->global, 0, GLOBAL_ENTRIES * 16);
		transition(list, m->stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->CopyBufferRegion(m->readback[slot], GLOBAL_ENTRIES * 16, m->stats, 0, STATS_BYTES);
		{
			uint32_t counts[24];
			std::memcpy(counts, static_cast<const char *>(m->readback_ptr[(slot + 1) % 3]) + GLOBAL_ENTRIES * 16, sizeof(counts));
			if (m->frame >= 3)
			{
				for (int k = 0; k < 4; ++k)
				{
					std::memcpy(&m->shadow.k[k], &counts[8 + k], sizeof(float));
					m->shadow.frames[k] = counts[12 + k];
					m->shadow.samples[k] = counts[16 + k];
				}
			}
			m->shadow_overlay = charm != nullptr && options.shadow_overlay;
			if (m->frame >= 3)
			{
				m->searched = counts[3];
				m->wide_searched = counts[4];
				m->movers = counts[5];
			}
			const float total = static_cast<float>(m->blocks_x * m->blocks_y);
			if (m->frame >= 3 && total > 0 && counts[0] + counts[1] + counts[2] <= m->blocks_x * m->blocks_y)
			{
				m->share_camera = counts[0] / total;
				m->share_still = counts[1] / total;
				m->share_own = counts[2] / total;
			}
		}
		// (the buffers decay back to COMMON when this list completes)
		float g[4];
		std::memcpy(g, m->readback_ptr[(slot + 1) % 3], sizeof(g));
		m->gdx = g[0] * 2.0f; // half-res -> full-res pixels
		m->gdy = g[1] * 2.0f;
		m->gconf = g[2];
		{
			float z[4];
			std::memcpy(z, static_cast<const char *>(m->readback_ptr[(slot + 1) % 3]) + 4 * 16, sizeof(z));
			m->gzoom = (z[0] > 0.1f && z[0] < 10.0f) ? 1.0f / z[0] : 1.0f; // > 1: things got bigger (zooming in)
		}
		{
			// 1.3, from the frame two back (its readback is done): the picture's own camera fit ([6].w, [7].w) against the
			// sprites' exact shift, while the camera moves without zoom; and the motion fit per layer.
			const UINT r = (slot + 1) % 3;
			const char *rb = static_cast<const char *>(m->readback_ptr[r]);
			float gg[GLOBAL_ENTRIES * 4];
			std::memcpy(gg, rb, sizeof(gg));
			m->scam_used_last = m->scam_used[r];
			if (m->trace_left > 0 && m->frame >= 3)
			{
				char line[512];
				const float *sd = m->stage_diag[r];
				std::snprintf(line, sizeof(line), "%llu,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.6f,%.3f,%.4f,%.4f,%.0f,%.2f,%.2f,%.2f,%.0f,%.2f,%.2f,%.2f,%.0f,%.2f,%.2f,%.2f,%d\n",
					static_cast<unsigned long long>(m->frame - 2), m->scam_valid[r] ? 1 : 0, m->scam_used[r] ? 1 : 0, m->scam_support[r],
					m->scam_tight[r], m->scam_x[r], m->scam_y[r], gg[27] * 2.0f, gg[31] * 2.0f, gg[16], gg[2], gg[0] * 2.0f, gg[1] * 2.0f,
					sd[0], sd[1], sd[2], sd[3], sd[4], sd[5], sd[6], sd[7], sd[8], sd[9], sd[10], sd[11], m->icon_used[r] ? 1 : 0);
				m->trace += line;
				if (--m->trace_left == 0)
					m->trace_done = true;
			}
			if (m->frame >= 3 && m->scam_valid[r] && std::fabs(gg[16] - 1.0f) < 0.0005f)
			{
				const float px = gg[27] * 2.0f, py = gg[31] * 2.0f, sx = m->scam_x[r], sy = m->scam_y[r];
				if (std::fabs(sx) + std::fabs(sy) >= 0.5f && px == px && py == py)
				{
					const float d = std::sqrt((px - sx) * (px - sx) + (py - sy) * (py - sy));
					const float k = m->cam_frames < 50 ? 1.0f / static_cast<float>(m->cam_frames + 1) : 0.02f;
					m->cam_diff += (d - m->cam_diff) * k;
					m->cam_off += ((d > 0.25f ? 1.0f : 0.0f) - m->cam_off) * k;
					++m->cam_frames;
				}
			}
			uint32_t fit[FIT_CLASSES * 3];
			std::memcpy(fit, rb + GLOBAL_ENTRIES * 16 + FIT_BASE * 4, sizeof(fit));
			uint64_t total = 0;
			for (UINT i = 0; i < FIT_CLASSES; ++i)
				total += fit[i * 3 + 2];
			if (m->frame >= 3 && total >= 1000)
			{
				for (UINT i = 0; i < FIT_CLASSES; ++i)
				{
					const uint32_t n = fit[i * 3 + 2];
					const float share = static_cast<float>(n) / static_cast<float>(total);
					m->fit_share[i] += (share - m->fit_share[i]) * 0.1f;
					if (n == 0)
						continue;
					const float ours = static_cast<float>(fit[i * 3]) / (256.0f * static_cast<float>(n));
					const float cam = static_cast<float>(fit[i * 3 + 1]) / (256.0f * static_cast<float>(n));
					m->fit_ours[i] = m->fit_valid ? m->fit_ours[i] + (ours - m->fit_ours[i]) * 0.1f : ours;
					m->fit_cam[i] = m->fit_valid ? m->fit_cam[i] + (cam - m->fit_cam[i]) * 0.1f : cam;
				}
				m->fit_valid = true;
			}
		}

		++m->frame;
		++m->history;
		std::snprintf(m->status, sizeof(m->status), "camera shift %+.1f, %+.1f px, zoom %.3f (match %.0f%%)%s", m->gdx, m->gdy, m->gzoom, m->gconf * 100.0f,
			options.enabled ? "" : ", vectors off");
		return m->mvec;
	}

	bool current_difference(float &mean_difference)
	{
		if (m == nullptr || m->frame == 0 || m->history < 2)
			return false;
		float g[GLOBAL_ENTRIES * 4];
		std::memcpy(g, m->readback_ptr[(m->frame - 1) % 3], sizeof(g));
		// [2].w: coarse SAD at the best camera shift, sum over 768 samples x 1024
		mean_difference = g[2 * 4 + 3] / (1024.0f * 768.0f);
		return mean_difference == mean_difference; // not NaN
	}

	void invalidate_history()
	{
		if (m != nullptr)
			m->history = 1; // this frame's images stay usable as "previous"; its camera and block motion do not
	}

	void set_timestamp_frequency(uint64_t ticks_per_second)
	{
		if (m != nullptr)
			m->timestamp_frequency = ticks_per_second;
	}

	void prewarm()
	{
		start_compile();
	}

	shader_state shaders(int &compiled, int &total)
	{
		total = PASS_COUNT;
		compiled = static_cast<int>(InterlockedCompareExchange(&g_job.compiled, 0, 0));
		const LONG st = InterlockedCompareExchange(&g_job.state, 0, 0);
		return st == 2 ? shader_state::ready : (st == 3 ? shader_state::failed : shader_state::compiling);
	}

	void search_counts(uint32_t &searched, uint32_t &wide, uint32_t &movers)
	{
		searched = m != nullptr ? m->searched : 0;
		wide = m != nullptr ? m->wide_searched : 0;
		movers = m != nullptr ? m->movers : 0;
	}

	void gpu_breakdown(float &camera, float &blocks, float &rest)
	{
		camera = m != nullptr ? m->gpu_camera_ms : 0.0f;
		blocks = m != nullptr ? m->gpu_block_ms : 0.0f;
		rest = m != nullptr ? m->gpu_rest_ms : 0.0f;
	}

	float gpu_ms()
	{
		return m != nullptr ? m->gpu_ms : 0.0f;
	}

	bool layers_used()
	{
		return m != nullptr && m->layers_used;
	}

	bool shadow_darkness(shadow_info &out)
	{
		out = m != nullptr ? m->shadow : shadow_info{};
		const bool known = out.frames[0] >= 3 || out.frames[1] >= 3;
		return m != nullptr && m->shadow_overlay && m->exact_used && known;
	}

	bool exact_mask_used()
	{
		return m != nullptr && m->exact_used;
	}

	ID3D12Resource *hold_texture()
	{
		return m != nullptr ? m->hold : nullptr;
	}

	ID3D12Resource *depth_texture()
	{
		return m != nullptr ? m->depth : nullptr;
	}

	ID3D12Resource *debug_texture()
	{
		return m != nullptr && m->debug_written ? m->debug : nullptr;
	}

	void block_shares(float &camera, float &still, float &own)
	{
		camera = m != nullptr ? m->share_camera : 0.0f;
		still = m != nullptr ? m->share_still : 0.0f;
		own = m != nullptr ? m->share_own : 0.0f;
	}

	float last_zoom()
	{
		return m != nullptr ? m->gzoom : 1.0f;
	}

	void last_global(float &dx, float &dy, float &confidence)
	{
		dx = m != nullptr ? m->gdx : 0.0f;
		dy = m != nullptr ? m->gdy : 0.0f;
		confidence = m != nullptr ? m->gconf : 0.0f;
	}

	camera_check camera_compare()
	{
		camera_check c;
		if (m == nullptr)
			return c;
		c.wanted = m->scam_wanted;
		c.used_last = m->scam_used_last;
		c.frames = m->cam_frames;
		c.mean_diff = m->cam_diff;
		c.share_off = m->cam_off;
		return c;
	}

	int trace_left()
	{
		return m != nullptr ? static_cast<int>(m->trace_left) : 0;
	}

	bool trace_start(uint32_t frames)
	{
		if (m == nullptr || frames == 0)
			return false;
		m->trace = "frame,sprites_exact,sprites_used,support,tight,sprites_x,sprites_y,picture_x,picture_y,zoom,match,used_x,used_y,"
			"floor_n,floor_rx,floor_ry,floor_share,objects_n,objects_rx,objects_ry,objects_share,air_n,air_rx,air_ry,air_share,icons_own\n";
		m->trace_left = frames;
		m->trace_done = false;
		return true;
	}

	bool trace_take(std::string &out)
	{
		if (m == nullptr || !m->trace_done)
			return false;
		out.swap(m->trace);
		m->trace.clear();
		m->trace_done = false;
		return true;
	}

	bool motion_fit(fit_info &out)
	{
		out = fit_info{};
		if (m == nullptr || !m->fit_valid)
			return false;
		for (int i = 0; i < 6; ++i)
		{
			out.ours[i] = m->fit_ours[i];
			out.camera[i] = m->fit_cam[i];
			out.share[i] = m->fit_share[i];
		}
		return true;
	}

	const char *status()
	{
		return m != nullptr ? m->status : g_off;
	}
}
