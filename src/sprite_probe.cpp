// Sprite probe (1.2, step 0). See sprite_probe.h.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "sprite_probe.h"

#include <windows.h>
#include <smmintrin.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

namespace sprite_probe
{
	namespace
	{
		log_fn g_log = nullptr;

		void plog(const char *fmt, ...)
		{
			char buf[480];
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			if (g_log != nullptr)
				g_log(buf);
		}

		std::string g_rep; // the detail file
		void repf(const char *fmt, ...)
		{
			char buf[1024];
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			g_rep += buf;
		}

		double now_ms()
		{
			LARGE_INTEGER t, f;
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&f);
			return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
		}

		// Reading from a mapped (write-combined) buffer: plain memcpy, and SSE4.1 streaming loads, which are meant for it.
#if defined(__GNUC__)
		__attribute__((target("sse4.1")))
#endif
		void stream_copy(uint8_t *dst, const uint8_t *src, size_t n)
		{
			size_t i = (16 - (reinterpret_cast<uintptr_t>(src) & 15)) & 15;
			if (i > n)
				i = n;
			if (i > 0)
				std::memcpy(dst, src, i);
			for (; i + 64 <= n; i += 64)
			{
				__m128i *s = reinterpret_cast<__m128i *>(const_cast<uint8_t *>(src + i));
				const __m128i a = _mm_stream_load_si128(s), b = _mm_stream_load_si128(s + 1);
				const __m128i c = _mm_stream_load_si128(s + 2), d = _mm_stream_load_si128(s + 3);
				__m128i *o = reinterpret_cast<__m128i *>(dst + i);
				_mm_storeu_si128(o, a);
				_mm_storeu_si128(o + 1, b);
				_mm_storeu_si128(o + 2, c);
				_mm_storeu_si128(o + 3, d);
			}
			if (i < n)
				std::memcpy(dst + i, src + i, n - i);
		}

		// ------------------------------------------------------------ tracked state (always on, cheap)

		struct elem
		{
			std::string semantic;
			uint32_t index = 0;
			format fmt = format::unknown;
			uint32_t slot = 0, offset = 0, step = 0;
		};
		std::unordered_map<uint64_t, std::vector<elem>> g_layouts;

		struct buffer_info
		{
			uint64_t size = 0;
			memory_heap heap = memory_heap::unknown;
			bool vb = false, ib = false, init = false;
			std::vector<uint8_t> shadow;
			bool valid = false;          // shadow holds the current contents (as far as we know)
			void *mapped = nullptr;
			map_access access = map_access::write_only;
			uint64_t drawn_end = 0;      // analysis frames: end of what draws used since the frame start / last discard
			bool window = false;         // analysis frames: a map's data is being drawn from
			uint64_t window_start = 0;   // where that map's new data starts (the draw end before it)
			uint32_t window_draws = 0;
			uint64_t maps_discard = 0, maps_nooverwrite = 0, maps_other = 0, updates = 0, update_bytes = 0, copies = 0, copy_bytes = 0;
			uint32_t maps_frame = 0, max_maps_frame = 0;
			uint64_t world_draws = 0, world_indices = 0;
			int id = -1;
		};
		std::unordered_map<uint64_t, buffer_info> g_buffers;
		uint64_t g_init_bytes = 0;
		constexpr uint64_t INIT_KEEP_MAX = 64ull << 20;
		int g_next_buffer_id = 0;

		// current bindings (only while recording)
		uint64_t g_layout = 0;
		struct vb_bind { uint64_t buf = 0, off = 0; uint32_t stride = 0; };
		vb_bind g_vb[4];
		uint64_t g_ib = 0, g_ib_off = 0;
		uint32_t g_ib_size = 2;
		uint64_t g_tex[2] = {};
		uint32_t g_tex_w[2] = {}, g_tex_h[2] = {};

		// ------------------------------------------------------------ recording

		bool g_active = false;
		uint32_t g_frames_total = 0, g_frame = 0;
		std::filesystem::path g_file;
		std::string g_status = "not run yet";
		int g_run = 0;

		bool analysis_frame(uint32_t f) { return f >= 30 && (f % 30 == 0 || f % 30 == 1) && f + 1 < g_frames_total; }
		bool analysing() { return g_active && analysis_frame(g_frame); }

		struct combo
		{
			uint32_t vs = 0, ps = 0;
			uint64_t layout = 0;
			uint64_t draws = 0, indices = 0, instanced = 0, nonindexed = 0;
			uint32_t stages = 0;
			uint64_t vb = 0, ib = 0;
		};
		std::map<std::tuple<uint32_t, uint32_t, uint64_t>, combo> g_combos;
		uint64_t g_world_draws = 0, g_world_indices = 0, g_other_draws = 0, g_world_instanced = 0, g_world_nonindexed = 0;
		uint64_t g_stage_draws[5] = {}, g_stage_indices[5] = {};
		double g_copy_ms[2] = {};
		uint64_t g_copy_bytes[2] = {}, g_copy_count[2] = {};
		uint64_t g_frame_copy_bytes = 0, g_analysis_copy_bytes = 0, g_budget_skips = 0;
		double g_frame_copy_ms = 0, g_analysis_copy_ms = 0;
		uint32_t g_analysis_frames = 0;
		constexpr uint64_t COPY_BUDGET = 384ull << 20; // per analysis frame
		constexpr uint64_t COPY_CAP = 32ull << 20;     // per unmap
		uint64_t g_nodata_draws = 0, g_decoded_draws = 0, g_instanced_draws_seen = 0;
		// how much each Map's new data spans (what the draws after it use), to size the real reads
		uint64_t g_ext_count = 0, g_ext_sum = 0, g_ext_max = 0, g_ext_draws = 0, g_ext_empty = 0, g_ext_hist[8] = {};
		std::string g_nodata_why;

		struct quad
		{
			float x0, y0, x1, y1, u0, v0, u1, v1;
			uint64_t tex;
			uint32_t ps, draw;
			uint32_t color, extra;   // TEXCOORD2 (RGBA8) and TEXCOORD3 (uint) of the first corner, raw
			uint8_t stage, verts;
			uint8_t target;          // 0 the world target, 1.. other screen-sized targets (in order of first use)
		};
		std::unordered_map<uint64_t, int> g_targets;
		struct target_stats { uint32_t fmt = 0; uint64_t draws = 0, decoded = 0, sprites = 0; uint32_t stages = 0; };
		std::map<int, target_stats> g_target_stats;
		int target_id(uint64_t rt, bool world)
		{
			if (world)
				return 0;
			const auto it = g_targets.find(rt);
			if (it != g_targets.end())
				return it->second;
			const int id = static_cast<int>(g_targets.size()) + 1;
			g_targets.emplace(rt, id);
			return id;
		}
		bool g_world_late = false, g_world_finished = false; // the world pass is over this frame: stop reading
		uint64_t g_skipped_after_world = 0;
		std::vector<quad> g_cur, g_prev;
		uint32_t g_prev_frame = UINT32_MAX;
		uint32_t g_world_draw_index = 0;
		uint32_t g_rt_w = 0, g_rt_h = 0;
		int g_pairs = 0;
		std::unordered_set<uint64_t> g_dumped_layouts;
		bool g_dumped_instanced = false;
		int g_seq_lines = 0;
		constexpr int SEQ_MAX = 300;
		std::unordered_map<uint64_t, int> g_tex_ids;
		std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_tex_size;

		int tex_id(uint64_t t)
		{
			const auto it = g_tex_ids.find(t);
			if (it != g_tex_ids.end())
				return it->second;
			const int id = static_cast<int>(g_tex_ids.size());
			g_tex_ids.emplace(t, id);
			return id;
		}

		void close_window(buffer_info &b)
		{
			if (!b.window)
				return;
			b.window = false;
			const uint64_t ext = b.drawn_end > b.window_start ? b.drawn_end - b.window_start : 0;
			if (b.window_draws == 0)
			{
				++g_ext_empty;
				return;
			}
			++g_ext_count;
			g_ext_sum += ext;
			g_ext_max = std::max(g_ext_max, ext);
			g_ext_draws += b.window_draws;
			int k = 0; // <4K, <16K, <64K, <128K, <256K, <512K, <1M, more
			const uint64_t lim[7] = { 4096, 16384, 65536, 131072, 262144, 524288, 1048576 };
			while (k < 7 && ext >= lim[k])
				++k;
			++g_ext_hist[k];
		}

		int buffer_id(buffer_info &b)
		{
			if (b.id < 0)
				b.id = g_next_buffer_id++;
			return b.id;
		}

		const char *heap_name(memory_heap h)
		{
			switch (h)
			{
			case memory_heap::default_: return "gpu_only (DEFAULT/IMMUTABLE)";
			case memory_heap::upload: return "cpu_to_gpu (DYNAMIC)";
			case memory_heap::readback: return "gpu_to_cpu";
			case memory_heap::scratch: return "cpu_only (STAGING)";
			default: return "unknown";
			}
		}

		const char *access_name(map_access a)
		{
			switch (a)
			{
			case map_access::write_discard: return "discard";
			case map_access::write_only: return "no-overwrite";
			case map_access::read_write: return "read-write";
			case map_access::read_only: return "read";
			default: return "?";
			}
		}

		bool seq_on() { return analysing() && g_analysis_frames == 0 && g_seq_lines < SEQ_MAX; }

		// ------------------------------------------------------------ vertex decoding

		float half_to_float(uint16_t h)
		{
			const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
			float v;
			if (e == 0)
				v = std::ldexp(static_cast<float>(m), -24);
			else if (e == 31)
				v = 65504.0f;
			else
				v = std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
			return s != 0 ? -v : v;
		}

		int decode(format f, const uint8_t *p, float o[4])
		{
			auto f32 = [&](int n) { for (int i = 0; i < n; ++i) std::memcpy(&o[i], p + 4 * i, 4); return n; };
			auto f16 = [&](int n) { for (int i = 0; i < n; ++i) { uint16_t v; std::memcpy(&v, p + 2 * i, 2); o[i] = half_to_float(v); } return n; };
			auto u16 = [&](int n, float scale) { for (int i = 0; i < n; ++i) { uint16_t v; std::memcpy(&v, p + 2 * i, 2); o[i] = v * scale; } return n; };
			auto s16 = [&](int n, float scale) { for (int i = 0; i < n; ++i) { int16_t v; std::memcpy(&v, p + 2 * i, 2); o[i] = std::max(v * scale, scale == 1.0f ? -32768.0f : -1.0f); } return n; };
			auto u32 = [&](int n) { for (int i = 0; i < n; ++i) { uint32_t v; std::memcpy(&v, p + 4 * i, 4); o[i] = static_cast<float>(v); } return n; };
			auto s32 = [&](int n) { for (int i = 0; i < n; ++i) { int32_t v; std::memcpy(&v, p + 4 * i, 4); o[i] = static_cast<float>(v); } return n; };
			auto u8 = [&](int n, float scale) { for (int i = 0; i < n; ++i) o[i] = p[i] * scale; return n; };
			switch (f)
			{
			case format::r32g32b32a32_float: return f32(4);
			case format::r32g32b32_float: return f32(3);
			case format::r32g32_float: return f32(2);
			case format::r32_float: return f32(1);
			case format::r16g16b16a16_float: return f16(4);
			case format::r16g16_float: return f16(2);
			case format::r16g16b16a16_unorm: return u16(4, 1.0f / 65535.0f);
			case format::r16g16_unorm: return u16(2, 1.0f / 65535.0f);
			case format::r16g16b16a16_uint: return u16(4, 1.0f);
			case format::r16g16_uint: return u16(2, 1.0f);
			case format::r16g16b16a16_snorm: return s16(4, 1.0f / 32767.0f);
			case format::r16g16_snorm: return s16(2, 1.0f / 32767.0f);
			case format::r16g16b16a16_sint: return s16(4, 1.0f);
			case format::r16g16_sint: return s16(2, 1.0f);
			case format::r32g32b32a32_uint: return u32(4);
			case format::r32g32_uint: return u32(2);
			case format::r32_uint: return u32(1);
			case format::r32g32_sint: return s32(2);
			case format::r32_sint: return s32(1);
			case format::r8g8b8a8_unorm: return u8(4, 1.0f / 255.0f);
			case format::b8g8r8a8_unorm: return u8(4, 1.0f / 255.0f);
			case format::r8g8b8a8_uint: return u8(4, 1.0f);
			case format::r8g8_unorm: return u8(2, 1.0f / 255.0f);
			default: return 0;
			}
		}

		uint32_t format_bytes(format f)
		{
			switch (f)
			{
			case format::r32g32b32a32_float: case format::r32g32b32a32_uint: return 16;
			case format::r32g32b32_float: return 12;
			case format::r32g32_float: case format::r32g32_uint: case format::r32g32_sint:
			case format::r16g16b16a16_float: case format::r16g16b16a16_unorm: case format::r16g16b16a16_uint:
			case format::r16g16b16a16_snorm: case format::r16g16b16a16_sint: return 8;
			default: return 4;
			}
		}

		bool sem_is(const elem &e, const char *name)
		{
			if (e.semantic.size() != std::strlen(name))
				return false;
			for (size_t i = 0; i < e.semantic.size(); ++i)
				if (std::toupper(static_cast<unsigned char>(e.semantic[i])) != name[i])
					return false;
			return true;
		}

		void dump_vertex(const std::vector<elem> &layout, uint32_t v, int32_t vertex_offset)
		{
			repf("    v%u:", v);
			for (const elem &e : layout)
			{
				const vb_bind &b = g_vb[e.slot & 3];
				const auto it = g_buffers.find(b.buf);
				if (it == g_buffers.end() || !it->second.valid)
				{
					repf(" %s%u=(no data)", e.semantic.c_str(), e.index);
					continue;
				}
				const uint64_t at = b.off + static_cast<uint64_t>(static_cast<int64_t>(v) + vertex_offset) * b.stride + e.offset;
				float o[4] = {};
				int n = 0;
				if (at + format_bytes(e.fmt) <= it->second.shadow.size())
					n = decode(e.fmt, it->second.shadow.data() + at, o);
				repf(" %s%u=(", e.semantic.c_str(), e.index);
				for (int i = 0; i < n; ++i)
					repf(i ? ",%.4g" : "%.4g", o[i]);
				repf(n == 0 ? "?)" : ")");
			}
			repf("\n");
		}

		void decode_draw(const draw_info &d)
		{
			const auto lit = g_layouts.find(g_layout);
			if (lit == g_layouts.end())
			{
				++g_nodata_draws;
				g_nodata_why = "input layout unknown";
				return;
			}
			const std::vector<elem> &layout = lit->second;
			const elem *pos = nullptr, *uv = nullptr;
			bool inst = d.instances > 1;
			for (const elem &e : layout)
			{
				if ((sem_is(e, "POSITION") || sem_is(e, "SV_POSITION")) && e.index == 0 && pos == nullptr)
					pos = &e;
				if (sem_is(e, "TEXCOORD") && e.index == 0 && uv == nullptr)
					uv = &e;
				if (e.step != 0)
					inst = true;
			}
			// Factorio names every vertex field TEXCOORDn: then the first two float2 fields are position and texture coordinate.
			if (pos == nullptr)
			{
				uv = nullptr;
				for (const elem &e : layout)
					if (e.fmt == format::r32g32_float || e.fmt == format::r32g32b32_float || e.fmt == format::r32g32b32a32_float)
					{
						if (pos == nullptr)
							pos = &e;
						else if (uv == nullptr)
							uv = &e;
					}
			}
			if (inst)
			{
				++g_instanced_draws_seen;
				if (!g_dumped_instanced)
				{
					g_dumped_instanced = true;
					repf("\nFirst instanced world draw: %u indices/vertices x %u instances, layout %016llX\n", d.count, d.instances,
						static_cast<unsigned long long>(g_layout));
				}
				return;
			}
			if (pos == nullptr)
			{
				++g_nodata_draws;
				g_nodata_why = "no POSITION element";
				return;
			}
			if (pos->fmt != format::r32g32_float || !d.indexed)
				return; // full-screen passes (float3 positions, 3 vertices): not sprites
			const elem *col = nullptr, *extra = nullptr;
			for (const elem &e : layout)
			{
				if (e.fmt == format::r8g8b8a8_unorm && col == nullptr)
					col = &e;
				if (e.fmt == format::r32_uint && extra == nullptr)
					extra = &e;
			}
			const vb_bind &vb = g_vb[pos->slot & 3];
			const auto vit = g_buffers.find(vb.buf);
			if (vit == g_buffers.end() || !vit->second.valid || vb.stride == 0)
			{
				++g_nodata_draws;
				g_nodata_why = vit == g_buffers.end() ? "vertex buffer not tracked" : (vb.stride == 0 ? "stride 0" : "vertex data not read this frame");
				return;
			}
			buffer_info &vbuf = vit->second;
			++vbuf.window_draws;
			const buffer_info *ibuf = nullptr;
			if (d.indexed)
			{
				const auto iit = g_buffers.find(g_ib);
				if (iit == g_buffers.end() || !iit->second.valid)
				{
					++g_nodata_draws;
					g_nodata_why = iit == g_buffers.end() ? "index buffer not tracked" : "index data not available";
					return;
				}
				ibuf = &iit->second;
			}
			++g_decoded_draws;

			auto index_at = [&](uint32_t i, bool &ok) -> uint32_t {
				if (ibuf == nullptr)
					return d.first + i;
				const uint64_t at = g_ib_off + static_cast<uint64_t>(d.first + i) * g_ib_size;
				if (at + g_ib_size > ibuf->shadow.size())
				{
					ok = false;
					return 0;
				}
				if (g_ib_size == 2)
				{
					uint16_t v;
					std::memcpy(&v, ibuf->shadow.data() + at, 2);
					return v;
				}
				uint32_t v;
				std::memcpy(&v, ibuf->shadow.data() + at, 4);
				return v;
			};
			auto read = [&](const elem *e, uint32_t v, float o[4]) -> bool {
				const vb_bind &b = g_vb[e->slot & 3];
				const auto it = e->slot == pos->slot ? vit : g_buffers.find(b.buf);
				if (it == g_buffers.end() || !it->second.valid)
					return false;
				const int64_t vv = static_cast<int64_t>(v) + (ibuf != nullptr ? d.vertex_offset : 0);
				if (vv < 0)
					return false;
				const uint64_t at = b.off + static_cast<uint64_t>(vv) * b.stride + e->offset;
				if (at + format_bytes(e->fmt) > it->second.shadow.size())
					return false;
				return decode(e->fmt, it->second.shadow.data() + at, o) >= 2;
			};

			if (g_dumped_layouts.insert(g_layout).second)
			{
				repf("\nFirst vertices of layout %016llX (draw %u, stage %d, PS %08X, %u %s, vertex offset %d):\n",
					static_cast<unsigned long long>(g_layout), g_world_draw_index, d.stage, d.ps, d.count, d.indexed ? "indices" : "vertices", d.vertex_offset);
				bool ok = true;
				for (uint32_t i = 0; i < std::min<uint32_t>(d.count, 6) && ok; ++i)
				{
					const uint32_t v = index_at(i, ok);
					if (ok)
						dump_vertex(layout, v, ibuf != nullptr ? d.vertex_offset : 0);
				}
			}

			const uint32_t group = (d.count % 6 == 0) ? 6 : 3;
			uint32_t max_v = 0;
			for (uint32_t i = 0; i + group <= d.count && g_cur.size() < 600000; i += group)
			{
				uint32_t idx[6];
				bool ok = true;
				for (uint32_t k = 0; k < group; ++k)
					idx[k] = index_at(i + k, ok);
				if (!ok)
					break;
				quad q = {};
				q.x0 = q.y0 = q.u0 = q.v0 = 1e30f;
				q.x1 = q.y1 = q.u1 = q.v1 = -1e30f;
				uint32_t uniq[6];
				uint32_t nu = 0;
				bool good = true;
				for (uint32_t k = 0; k < group; ++k)
				{
					bool seen = false;
					for (uint32_t j = 0; j < nu; ++j)
						seen |= uniq[j] == idx[k];
					if (seen)
						continue;
					uniq[nu++] = idx[k];
					max_v = std::max(max_v, idx[k]);
					float p[4] = {}, t[4] = {};
					if (!read(pos, idx[k], p))
					{
						good = false;
						break;
					}
					q.x0 = std::min(q.x0, p[0]); q.x1 = std::max(q.x1, p[0]);
					q.y0 = std::min(q.y0, p[1]); q.y1 = std::max(q.y1, p[1]);
					if (uv != nullptr && read(uv, idx[k], t))
					{
						q.u0 = std::min(q.u0, t[0]); q.u1 = std::max(q.u1, t[0]);
						q.v0 = std::min(q.v0, t[1]); q.v1 = std::max(q.v1, t[1]);
					}
				}
				if (!good)
					continue;
				if (q.u0 > q.u1)
					q.u0 = q.u1 = q.v0 = q.v1 = 0;
				q.tex = g_tex[0];
				q.ps = d.ps;
				q.color = q.extra = 0;
				{
					const int64_t v0 = static_cast<int64_t>(uniq[0]) + d.vertex_offset;
					const uint64_t base = vb.off + static_cast<uint64_t>(std::max<int64_t>(v0, 0)) * vb.stride;
					if (col != nullptr && base + col->offset + 4 <= vbuf.shadow.size())
						std::memcpy(&q.color, vbuf.shadow.data() + base + col->offset, 4);
					if (extra != nullptr && base + extra->offset + 4 <= vbuf.shadow.size())
						std::memcpy(&q.extra, vbuf.shadow.data() + base + extra->offset, 4);
				}
				q.draw = g_world_draw_index;
				q.stage = static_cast<uint8_t>(d.stage);
				q.target = static_cast<uint8_t>(std::min(target_id(d.rt, d.world), 255));
				q.verts = static_cast<uint8_t>(nu);
				g_cur.push_back(q);
			}
			const int64_t last = static_cast<int64_t>(max_v) + (ibuf != nullptr ? d.vertex_offset : 0) + 1;
			const uint64_t end = vb.off + static_cast<uint64_t>(std::max<int64_t>(last, 0)) * vb.stride;
			vbuf.drawn_end = std::max(vbuf.drawn_end, end);
			if (seq_on())
			{
				++g_seq_lines;
				repf("  draw %4u stage %d PS %08X VS %08X: %u %s x%u, first %u, vertex offset %d, VB%d +%llu stride %u -> bytes up to %llu%s\n",
					g_world_draw_index, d.stage, d.ps, d.vs, d.count, d.indexed ? "indices" : "vertices", d.instances, d.first, d.vertex_offset,
					buffer_id(vbuf), static_cast<unsigned long long>(vb.off), vb.stride, static_cast<unsigned long long>(end), d.indexed ? "" : " (not indexed)");
			}
		}

		// ------------------------------------------------------------ frame pair analysis

		uint64_t mix(uint64_t h, int64_t v)
		{
			h ^= static_cast<uint64_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			return h * 0x100000001B3ull;
		}

		struct pxrect { float x0, y0, x1, y1; };

		uint64_t key_uv(const quad &q, const pxrect &r)
		{
			uint64_t h = mix(1469598103934665603ull, static_cast<int64_t>(q.tex));
			h = mix(h, std::llround(q.u0 * 16384.0f));
			h = mix(h, std::llround(q.v0 * 16384.0f));
			h = mix(h, std::llround(q.u1 * 16384.0f));
			h = mix(h, std::llround(q.v1 * 16384.0f));
			h = mix(h, std::llround((r.x1 - r.x0) * 4.0f));
			h = mix(h, std::llround((r.y1 - r.y0) * 4.0f));
			return h;
		}
		uint64_t key_at(uint64_t base, float x, float y) { return mix(mix(base, std::llround(x * 2.0f)), std::llround(y * 2.0f)); }
		uint64_t key_shape(const quad &q, const pxrect &r)
		{
			uint64_t h = mix(7777ull, static_cast<int64_t>(q.tex));
			h = mix(h, std::llround((r.x1 - r.x0) * 2.0f));
			h = mix(h, std::llround((r.y1 - r.y0) * 2.0f));
			return h;
		}

		bool near_lookup(const std::unordered_set<uint64_t> &set, uint64_t base, float x, float y)
		{
			const float hx = std::round(x * 2.0f) / 2.0f, hy = std::round(y * 2.0f) / 2.0f;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (set.count(key_at(base, hx + dx * 0.5f, hy + dy * 0.5f)) != 0)
						return true;
			return false;
		}

		const char *CLASS_NAME[] = { "world", "HELD", "own motion", "HELD (animated)", "world (animated)", "unmatched", "camera still" };
		const char *STAGE_NAME[] = { "tiles", "floor", "objects", "air", "after world" };

		void analyse_pair(const std::vector<quad> &A, const std::vector<quad> &B)
		{
			++g_pairs;
			const float W = static_cast<float>(g_rt_w), H = static_cast<float>(g_rt_h);
			float maxabs = 0;
			for (const quad &q : B)
				maxabs = std::max({ maxabs, std::fabs(q.x0), std::fabs(q.x1), std::fabs(q.y0), std::fabs(q.y1) });
			const bool ndc = maxabs <= 2.5f;
			auto px = [&](const quad &q) -> pxrect {
				if (!ndc)
					return { q.x0, q.y0, q.x1, q.y1 };
				const float ax = (q.x0 * 0.5f + 0.5f) * W, bx = (q.x1 * 0.5f + 0.5f) * W;
				const float ay = (0.5f - q.y1 * 0.5f) * H, by = (0.5f - q.y0 * 0.5f) * H;
				return { ax, ay, bx, by };
			};
			std::vector<pxrect> ra(A.size()), rb(B.size());
			for (size_t i = 0; i < A.size(); ++i) ra[i] = px(A[i]);
			for (size_t i = 0; i < B.size(); ++i) rb[i] = px(B[i]);

			// camera: sprites whose texture region appears exactly once in both frames
			std::unordered_map<uint64_t, int> ca, cb;
			std::unordered_map<uint64_t, size_t> fa;
			for (size_t i = 0; i < A.size(); ++i) { const uint64_t k = key_uv(A[i], ra[i]); if (++ca[k] == 1) fa[k] = i; }
			for (size_t i = 0; i < B.size(); ++i) ++cb[key_uv(B[i], rb[i])];
			std::map<std::pair<long long, long long>, int> hist;
			int unique_matches = 0;
			for (size_t i = 0; i < B.size(); ++i)
			{
				const uint64_t k = key_uv(B[i], rb[i]);
				if (cb[k] != 1)
					continue;
				const auto it = ca.find(k);
				if (it == ca.end() || it->second != 1)
					continue;
				const pxrect &a = ra[fa[k]];
				++hist[{ std::llround((rb[i].x0 - a.x0) * 4.0f), std::llround((rb[i].y0 - a.y0) * 4.0f) }];
				++unique_matches;
			}
			std::pair<long long, long long> mode = { 0, 0 };
			int mode_n = 0;
			for (const auto &kv : hist)
				if (kv.second > mode_n)
				{
					mode_n = kv.second;
					mode = kv.first;
				}
			const float camx = mode.first / 4.0f, camy = mode.second / 4.0f;
			const bool moving = std::fabs(camx) >= 1.0f || std::fabs(camy) >= 1.0f;

			std::unordered_set<uint64_t> a_uvpos, a_shapepos, a_uv;
			for (size_t i = 0; i < A.size(); ++i)
			{
				const uint64_t ku = key_uv(A[i], ra[i]), ks = key_shape(A[i], ra[i]);
				a_uv.insert(ku);
				a_uvpos.insert(key_at(ku, ra[i].x0, ra[i].y0));
				a_shapepos.insert(key_at(ks, ra[i].x0, ra[i].y0));
			}
			int counts[5][7] = {};
			std::vector<int> cls(B.size(), 5);
			for (size_t i = 0; i < B.size(); ++i)
			{
				const pxrect &r = rb[i];
				const uint64_t ku = key_uv(B[i], r), ks = key_shape(B[i], r);
				int c;
				if (near_lookup(a_uvpos, ku, r.x0 - camx, r.y0 - camy))
					c = moving ? 0 : 6;
				else if (moving && near_lookup(a_uvpos, ku, r.x0, r.y0))
					c = 1;
				else if (a_uv.count(ku) != 0)
					c = 2;
				else if (moving && near_lookup(a_shapepos, ks, r.x0, r.y0))
					c = 3;
				else if (near_lookup(a_shapepos, ks, r.x0 - camx, r.y0 - camy))
					c = 4;
				else
					c = 5;
				cls[i] = c;
				++counts[std::min<int>(B[i].stage, 4)][c];
			}

			// report
			int total[7] = {};
			for (int s = 0; s < 5; ++s)
				for (int c = 0; c < 7; ++c)
					total[c] += counts[s][c];
			float hx0 = 1e30f, hy0 = 1e30f, hx1 = -1e30f, hy1 = -1e30f;
			int held = 0;
			for (size_t i = 0; i < B.size(); ++i)
				if (cls[i] == 1 || cls[i] == 3)
				{
					++held;
					hx0 = std::min(hx0, rb[i].x0); hy0 = std::min(hy0, rb[i].y0);
					hx1 = std::max(hx1, rb[i].x1); hy1 = std::max(hy1, rb[i].y1);
				}
			repf("\n=== Pair %d (frames %u/%u): %zu / %zu sprites, positions %s (max |coord| %.1f), screen %ux%u\n", g_pairs, g_frame - 1, g_frame,
				A.size(), B.size(), ndc ? "look like -1..1 (converted to px)" : "in pixels", maxabs, g_rt_w, g_rt_h);
			repf("Camera shift %+.2f %+.2f px (%d of %d unique sprites agree)%s\n", camx, camy, mode_n, unique_matches, moving ? "" : " - camera still, no hold test");
			repf("%-12s %8s %8s %8s %8s %8s %8s %8s\n", "stage", "world", "HELD", "own", "HELD-an", "world-an", "unmatch", "still");
			for (int s = 0; s < 5; ++s)
				repf("%-12s %8d %8d %8d %8d %8d %8d %8d\n", STAGE_NAME[s], counts[s][0], counts[s][1], counts[s][2], counts[s][3], counts[s][4], counts[s][5], counts[s][6]);
			const float cx = W * 0.5f, cy = H * 0.5f;
			if (held > 0)
				repf("Held sprites: %d, together x %.1f..%.1f y %.1f..%.1f (relative to the screen centre: %+.1f..%+.1f, %+.1f..%+.1f)\n", held, hx0, hx1, hy0, hy1,
					hx0 - cx, hx1 - cx, hy0 - cy, hy1 - cy);
			auto line = [&](size_t i) {
				const quad &q = B[i];
				const pxrect &r = rb[i];
				const int t = tex_id(q.tex);
				const auto ts = g_tex_size.find(q.tex);
				repf("  %-16s draw %4u %-7s PS %08X tex%d(%ux%u) rect %+7.1f %+7.1f size %6.1f x %6.1f (from centre) uv %.4f,%.4f..%.4f,%.4f%s\n",
					CLASS_NAME[cls[i]], q.draw, STAGE_NAME[std::min<int>(q.stage, 4)], q.ps, t, ts != g_tex_size.end() ? ts->second.first : 0u,
					ts != g_tex_size.end() ? ts->second.second : 0u, r.x0 - cx, r.y0 - cy, r.x1 - r.x0, r.y1 - r.y0, q.u0, q.v0, q.u1, q.v1,
					q.verts == 4 ? "" : (q.verts == 3 ? " (triangle)" : " (odd)"));
			};
			int listed = 0;
			for (size_t i = 0; i < B.size() && listed < 80; ++i)
				if (cls[i] == 1 || cls[i] == 3)
				{
					line(i);
					++listed;
				}
			// Candidate rule for 1.2: a sprite follows the camera (the character, what it drives, their shadows) when last frame had
			// a sprite with the same shader and colour and a similar size (animation frames are trimmed to their visible pixels,
			// so the size and corner change a little) close to the same place on screen, and clearly closer than to where the
			// camera shift would have put it.
			std::vector<int> follows;
			std::vector<float> f0(B.size(), -1.0f), fc(B.size(), -1.0f);
			{
				for (size_t i = 0; i < B.size(); ++i)
				{
					const pxrect &r = rb[i];
					if ((B[i].stage == 0 && B[i].target == 0) || !(r.x1 >= cx - 400 && r.x0 <= cx + 400 && r.y1 >= cy - 400 && r.y0 <= cy + 400))
						continue;
					const float w = r.x1 - r.x0, h = r.y1 - r.y0, mx = (r.x0 + r.x1) * 0.5f, my = (r.y0 + r.y1) * 0.5f;
					float b0 = 1e30f, bc = 1e30f;
					for (size_t j = 0; j < A.size(); ++j)
					{
						const quad &p = A[j];
						if (p.ps != B[i].ps || p.color != B[i].color || (p.stage == 0 && p.target == 0))
							continue;
						const float pw = ra[j].x1 - ra[j].x0, ph = ra[j].y1 - ra[j].y0;
						if (std::fabs(pw - w) > 0.5f * std::max(w, 4.0f) || std::fabs(ph - h) > 0.5f * std::max(h, 4.0f) || p.target != B[i].target)
							continue;
						const float dx = mx - (ra[j].x0 + ra[j].x1) * 0.5f, dy = my - (ra[j].y0 + ra[j].y1) * 0.5f;
						b0 = std::min(b0, std::sqrt(dx * dx + dy * dy));
						bc = std::min(bc, std::sqrt((dx - camx) * (dx - camx) + (dy - camy) * (dy - camy)));
					}
					if (b0 > 1e29f)
						continue;
					f0[i] = b0;
					fc[i] = bc;
					if (moving && b0 <= std::max(6.0f, 0.12f * std::max(w, h)) && bc > b0 + 4.0f)
						follows.push_back(static_cast<int>(i));
				}
			}
			auto line2 = [&](size_t i, const char *tag) {
				const quad &q = B[i];
				const pxrect &r = rb[i];
				const bool centre = r.x0 <= cx && r.x1 >= cx && r.y0 <= cy && r.y1 >= cy;
				repf("  %-16s %s%d d%-3u %-7s PS %08X tex%d rect %+7.1f %+7.1f size %5.1f x %5.1f uv %.4f,%.4f..%.4f,%.4f col %08X x %08X%s | ", tag, q.target == 0 ? "W" : "T", q.target, q.draw,
					STAGE_NAME[std::min<int>(q.stage, 4)], q.ps, tex_id(q.tex), r.x0 - cx, r.y0 - cy, r.x1 - r.x0, r.y1 - r.y0, q.u0, q.v0, q.u1, q.v1, q.color,
					q.extra, centre ? " CENTRE" : "");
				if (f0[i] < 0.0f)
					repf("no similar sprite last frame\n");
				else
					repf("last frame: %.1f px from staying put, %.1f px from moving with the camera\n", f0[i], fc[i]);
			};
			{
				float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
				for (const int i : follows)
				{
					x0 = std::min(x0, rb[i].x0); y0 = std::min(y0, rb[i].y0);
					x1 = std::max(x1, rb[i].x1); y1 = std::max(y1, rb[i].y1);
				}
				repf("Rule 'follows the camera' (same shader and colour, similar size, size within 50%%, within max(6 px, 12%%) of staying put and 4 px closer than to the camera shift): %zu sprites",
					follows.size());
				if (!follows.empty())
					repf(", together %+.0f..%+.0f, %+.0f..%+.0f px from the centre", x0 - cx, x1 - cx, y0 - cy, y1 - cy);
				repf("\n");
				int n = 0;
				for (const int i : follows)
					if (n++ < 40)
						line2(static_cast<size_t>(i), "FOLLOWS");
				plog("sprite probe: pair %d: rule 'follows the camera': %zu sprites%s", g_pairs, follows.size(), follows.empty() ? "" : "");
				if (!follows.empty())
					plog("sprite probe: pair %d:   together %+.0f..%+.0f, %+.0f..%+.0f px from the centre", g_pairs, x0 - cx, x1 - cx, y0 - cy, y1 - cy);
			}
			{
				repf("Other sprites (not tiles) within 120 px of the centre that did not move exactly with the camera, or cover the centre:\n");
				int n = 0;
				for (size_t i = 0; i < B.size() && n < 40; ++i)
				{
					const pxrect &r = rb[i];
					if ((B[i].stage == 0 && B[i].target == 0) || !(r.x1 >= cx - 120 && r.x0 <= cx + 120 && r.y1 >= cy - 120 && r.y0 <= cy + 120))
						continue;
					if (std::find(follows.begin(), follows.end(), static_cast<int>(i)) != follows.end())
						continue;
					const bool centre = r.x0 <= cx && r.x1 >= cx && r.y0 <= cy && r.y1 >= cy;
					if ((cls[i] == 0 || cls[i] == 6) && !centre)
						continue;
					line2(i, CLASS_NAME[cls[i]]);
					++n;
				}
			}
			plog("sprite probe: pair %d: camera %+.2f %+.2f px (%d/%d agree), %zu sprites: world %d, HELD %d (+%d animated), own motion %d, world animated %d, unmatched %d%s",
				g_pairs, camx, camy, mode_n, unique_matches, B.size(), total[0], total[1], total[3], total[2], total[4], total[5], moving ? "" : " (camera still)");
			if (held > 0)
				plog("sprite probe: pair %d: held sprites span %+.0f..%+.0f, %+.0f..%+.0f px from the screen centre", g_pairs, hx0 - cx, hx1 - cx, hy0 - cy, hy1 - cy);
		}

		// ------------------------------------------------------------ end of the recording

		void finish()
		{
			g_active = false;
			const double frames = g_frames_total > 0 ? static_cast<double>(g_frames_total) : 1.0;
			std::string head;
			char buf[1024];
			std::snprintf(buf, sizeof(buf), "Factorio FG sprite probe (1.2 step 0), run %d, %u frames\n\n", g_run, g_frames_total);
			head += buf;
			std::snprintf(buf, sizeof(buf), "World target: %.1f draws/frame (%.0f indices/vertices), %.1f other draws/frame; %.1f instanced, %.1f not indexed\n",
				g_world_draws / frames, g_world_indices / frames, g_other_draws / frames, g_world_instanced / frames, g_world_nonindexed / frames);
			head += buf;
			for (int s = 0; s < 5; ++s)
			{
				std::snprintf(buf, sizeof(buf), "  stage %-12s %.1f draws/frame, %.0f indices/frame\n", STAGE_NAME[s], g_stage_draws[s] / frames, g_stage_indices[s] / frames);
				head += buf;
			}
			plog("sprite probe: run %d: world %.1f draws/frame, %.0f indices/frame, %.1f instanced, %.1f not indexed; other targets %.1f draws/frame",
				g_run, g_world_draws / frames, g_world_indices / frames, g_world_instanced / frames, g_world_nonindexed / frames, g_other_draws / frames);

			std::vector<const combo *> cs;
			for (const auto &kv : g_combos)
				cs.push_back(&kv.second);
			std::sort(cs.begin(), cs.end(), [](const combo *a, const combo *b) { return a->indices > b->indices; });
			head += "\nShader / layout combinations on the world target (by indices):\n";
			std::unordered_set<uint64_t> layouts_used;
			int shown = 0;
			for (const combo *c : cs)
			{
				std::snprintf(buf, sizeof(buf), "  VS %08X PS %08X layout %016llX: %.1f draws/frame, %.0f indices/frame, stages %s%s%s%s%s, instanced %llu, not indexed %llu\n",
					c->vs, c->ps, static_cast<unsigned long long>(c->layout), c->draws / frames, c->indices / frames,
					(c->stages & 1) ? "T" : "", (c->stages & 2) ? "F" : "", (c->stages & 4) ? "O" : "", (c->stages & 8) ? "A" : "", (c->stages & 16) ? "W" : "",
					static_cast<unsigned long long>(c->instanced), static_cast<unsigned long long>(c->nonindexed));
				head += buf;
				layouts_used.insert(c->layout);
				if (shown++ < 6)
					plog("sprite probe: VS %08X PS %08X layout %016llX: %.1f draws/frame, %.0f indices/frame, stages %s%s%s%s%s", c->vs, c->ps,
						static_cast<unsigned long long>(c->layout), c->draws / frames, c->indices / frames,
						(c->stages & 1) ? "T" : "", (c->stages & 2) ? "F" : "", (c->stages & 4) ? "O" : "", (c->stages & 8) ? "A" : "", (c->stages & 16) ? "W" : "");
			}
			head += "\nInput layouts used there:\n";
			for (const uint64_t l : layouts_used)
			{
				const auto it = g_layouts.find(l);
				std::string s = "  ";
				std::snprintf(buf, sizeof(buf), "%016llX:", static_cast<unsigned long long>(l));
				s += buf;
				if (it == g_layouts.end())
					s += " (not seen being created)";
				else
					for (const elem &e : it->second)
					{
						std::snprintf(buf, sizeof(buf), " %s%u fmt %u slot %u +%u%s;", e.semantic.c_str(), e.index, static_cast<unsigned>(e.fmt), e.slot, e.offset,
							e.step != 0 ? " PER-INSTANCE" : "");
						s += buf;
					}
				head += s + "\n";
				plog("sprite probe: layout%s", s.c_str() + 1);
			}
			head += "\nVertex / index buffers drawn from on the world target:\n";
			for (auto &kv : g_buffers)
			{
				buffer_info &b = kv.second;
				if (b.world_draws == 0)
					continue;
				std::snprintf(buf, sizeof(buf), "  %s%d: %llu bytes, %s%s; maps/frame: discard %.1f, no-overwrite %.1f, other %.1f (max %u in a frame); updates/frame %.1f (%.0f bytes); copies in %.1f/frame; %.1f draws/frame\n",
					b.ib ? "IB" : "VB", buffer_id(b), static_cast<unsigned long long>(b.size), heap_name(b.heap), b.init ? ", initial data" : "",
					b.maps_discard / frames, b.maps_nooverwrite / frames, b.maps_other / frames, b.max_maps_frame, b.updates / frames, b.update_bytes / frames,
					b.copies / frames, b.world_draws / frames);
				head += buf;
				std::string ln = buf + 2;
				if (!ln.empty() && ln.back() == '\n')
					ln.pop_back();
				plog("sprite probe: %s", ln.c_str());
			}
			const double mb0 = g_copy_bytes[0] / 1048576.0, mb1 = g_copy_bytes[1] / 1048576.0;
			std::snprintf(buf, sizeof(buf), "\nReading mapped vertex data on the CPU: memcpy %.1f MB in %.1f ms (%.0f MB/s, %llu copies); streaming loads %.1f MB in %.1f ms (%.0f MB/s, %llu copies)\n"
				"Analysis frames: %u, read %.1f MB per frame in %.1f ms per frame (until the world is drawn); %llu reads skipped (budget). Draws decoded %llu, without data %llu (last reason: %s), instanced %llu\n",
				mb0, g_copy_ms[0], g_copy_ms[0] > 0 ? mb0 * 1000.0 / g_copy_ms[0] : 0.0, static_cast<unsigned long long>(g_copy_count[0]),
				mb1, g_copy_ms[1], g_copy_ms[1] > 0 ? mb1 * 1000.0 / g_copy_ms[1] : 0.0, static_cast<unsigned long long>(g_copy_count[1]),
				g_analysis_frames, g_analysis_frames ? g_analysis_copy_bytes / 1048576.0 / g_analysis_frames : 0.0,
				g_analysis_frames ? g_analysis_copy_ms / g_analysis_frames : 0.0, static_cast<unsigned long long>(g_budget_skips),
				static_cast<unsigned long long>(g_decoded_draws), static_cast<unsigned long long>(g_nodata_draws),
				g_nodata_why.empty() ? "-" : g_nodata_why.c_str(), static_cast<unsigned long long>(g_instanced_draws_seen));
			head += buf;
			plog("sprite probe: CPU read (streaming loads) %.0f MB/s; %.1f MB and %.1f ms per analysis frame; decoded %llu draws, %llu without data (%s)",
				g_copy_ms[1] > 0 ? mb1 * 1000.0 / g_copy_ms[1] : 0.0,
				g_analysis_frames ? g_analysis_copy_bytes / 1048576.0 / g_analysis_frames : 0.0, g_analysis_frames ? g_analysis_copy_ms / g_analysis_frames : 0.0,
				static_cast<unsigned long long>(g_decoded_draws), static_cast<unsigned long long>(g_nodata_draws), g_nodata_why.empty() ? "-" : g_nodata_why.c_str());
			std::snprintf(buf, sizeof(buf), "New data per Map (bytes the following draws use): %llu maps, average %.1f KB, max %.1f KB, %.1f draws per map, %llu maps not drawn from\n"
				"  <4K %llu, 4-16K %llu, 16-64K %llu, 64-128K %llu, 128-256K %llu, 256-512K %llu, 512K-1M %llu, more %llu\n",
				static_cast<unsigned long long>(g_ext_count), g_ext_count ? g_ext_sum / 1024.0 / g_ext_count : 0.0, g_ext_max / 1024.0,
				g_ext_count ? static_cast<double>(g_ext_draws) / g_ext_count : 0.0, static_cast<unsigned long long>(g_ext_empty),
				static_cast<unsigned long long>(g_ext_hist[0]), static_cast<unsigned long long>(g_ext_hist[1]), static_cast<unsigned long long>(g_ext_hist[2]),
				static_cast<unsigned long long>(g_ext_hist[3]), static_cast<unsigned long long>(g_ext_hist[4]), static_cast<unsigned long long>(g_ext_hist[5]),
				static_cast<unsigned long long>(g_ext_hist[6]), static_cast<unsigned long long>(g_ext_hist[7]));
			head += buf;
			plog("sprite probe: new data per Map: average %.1f KB, max %.1f KB, %.1f draws per map (%llu maps)", g_ext_count ? g_ext_sum / 1024.0 / g_ext_count : 0.0,
				g_ext_max / 1024.0, g_ext_count ? static_cast<double>(g_ext_draws) / g_ext_count : 0.0, static_cast<unsigned long long>(g_ext_count));
			head += "\nOther screen-sized targets drawn to during the world (T1, T2...):\n";
			for (const auto &kv : g_target_stats)
			{
				std::snprintf(buf, sizeof(buf), "  T%d: format %u, %.1f draws/frame, stages %s%s%s%s; on analysis frames %llu draws decoded, %llu sprites\n", kv.first,
					kv.second.fmt, kv.second.draws / frames, (kv.second.stages & 1) ? "T" : "", (kv.second.stages & 2) ? "F" : "", (kv.second.stages & 4) ? "O" : "",
					(kv.second.stages & 8) ? "A" : "", static_cast<unsigned long long>(kv.second.decoded), static_cast<unsigned long long>(kv.second.sprites));
				head += buf;
				plog("sprite probe: target T%d: format %u, %.1f draws/frame, %llu draws decoded, %llu sprites", kv.first, kv.second.fmt, kv.second.draws / frames,
					static_cast<unsigned long long>(kv.second.decoded), static_cast<unsigned long long>(kv.second.sprites));
			}
			if (g_target_stats.empty())
				plog("sprite probe: no other screen-sized target drawn to during the world");
			head += "\nTextures (id: size): ";
			for (const auto &kv : g_tex_ids)
			{
				const auto ts = g_tex_size.find(kv.first);
				std::snprintf(buf, sizeof(buf), "tex%d %ux%u; ", kv.second, ts != g_tex_size.end() ? ts->second.first : 0u, ts != g_tex_size.end() ? ts->second.second : 0u);
				head += buf;
			}
			head += "\n";

			std::error_code ec;
			std::filesystem::create_directories(g_file.parent_path(), ec);
			if (FILE *f = _wfopen(g_file.c_str(), L"w"))
			{
				std::fwrite(head.data(), 1, head.size(), f);
				std::fwrite(g_rep.data(), 1, g_rep.size(), f);
				std::fclose(f);
				g_status = "saved to " + g_file.u8string();
			}
			else
				g_status = "could not write " + g_file.u8string();
			plog("sprite probe: run %d %s", g_run, g_status.c_str());
			g_rep.clear();
			g_rep.shrink_to_fit();
			g_cur.clear();
			g_cur.shrink_to_fit();
			g_prev.clear();
			g_prev.shrink_to_fit();
			for (auto &kv : g_buffers)
				if (!kv.second.init)
				{
					kv.second.shadow.clear();
					kv.second.shadow.shrink_to_fit();
					kv.second.valid = false;
				}
		}
	}

	// ---------------------------------------------------------------- public

	void set_log(log_fn log) { g_log = log; }
	bool active() { return g_active; }
	const char *status() { return g_status.c_str(); }

	void start(const std::filesystem::path &file, uint32_t frames)
	{
		if (g_active)
			return;
		++g_run;
		g_file = file;
		g_frames_total = std::max<uint32_t>(frames, 64);
		g_frame = 0;
		g_combos.clear();
		g_world_draws = g_world_indices = g_other_draws = g_world_instanced = g_world_nonindexed = 0;
		std::memset(g_stage_draws, 0, sizeof(g_stage_draws));
		std::memset(g_stage_indices, 0, sizeof(g_stage_indices));
		g_copy_ms[0] = g_copy_ms[1] = 0;
		g_copy_bytes[0] = g_copy_bytes[1] = g_copy_count[0] = g_copy_count[1] = 0;
		g_frame_copy_bytes = g_analysis_copy_bytes = g_budget_skips = 0;
		g_frame_copy_ms = g_analysis_copy_ms = 0;
		g_analysis_frames = 0;
		g_nodata_draws = g_decoded_draws = g_instanced_draws_seen = 0;
		g_skipped_after_world = 0;
		g_world_late = g_world_finished = false;
		g_ext_count = g_ext_sum = g_ext_max = g_ext_draws = g_ext_empty = 0;
		std::memset(g_ext_hist, 0, sizeof(g_ext_hist));
		g_nodata_why.clear();
		g_cur.clear();
		g_prev.clear();
		g_prev_frame = UINT32_MAX;
		g_world_draw_index = 0;
		g_pairs = 0;
		g_dumped_layouts.clear();
		g_dumped_instanced = false;
		g_seq_lines = 0;
		g_tex_ids.clear();
		g_targets.clear();
		g_target_stats.clear();
		g_rep = "\nFirst analysis frame, in order: vertex / index buffer writes and world draws (up to 300 lines), first vertices of each layout:\n";
		for (auto &kv : g_buffers)
		{
			buffer_info &b = kv.second;
			b.maps_discard = b.maps_nooverwrite = b.maps_other = b.updates = b.update_bytes = b.copies = b.copy_bytes = 0;
			b.maps_frame = b.max_maps_frame = 0;
			b.world_draws = b.world_indices = 0;
			b.drawn_end = 0;
			b.mapped = nullptr;
			b.window = false;
		}
		g_status = "recording...";
		g_active = true;
		plog("sprite probe: run %d started (%u frames)", g_run, g_frames_total);
	}

	void on_init_resource(const resource_desc &desc, const subresource_data *initial, resource res)
	{
		if (desc.type != resource_type::buffer)
			return;
		const bool vb = (desc.usage & resource_usage::vertex_buffer) == resource_usage::vertex_buffer;
		const bool ib = (desc.usage & resource_usage::index_buffer) == resource_usage::index_buffer;
		if (!vb && !ib)
			return;
		buffer_info &b = g_buffers[res.handle];
		b = buffer_info();
		b.size = desc.buffer.size;
		b.heap = desc.heap;
		b.vb = vb;
		b.ib = ib;
		if (initial != nullptr && initial->data != nullptr && b.size <= (4ull << 20) && g_init_bytes + b.size <= INIT_KEEP_MAX)
		{
			b.shadow.assign(static_cast<const uint8_t *>(initial->data), static_cast<const uint8_t *>(initial->data) + b.size);
			b.init = true;
			b.valid = true;
			g_init_bytes += b.size;
		}
	}

	void on_destroy_resource(resource res)
	{
		const auto it = g_buffers.find(res.handle);
		if (it == g_buffers.end())
			return;
		if (it->second.init)
			g_init_bytes -= std::min(g_init_bytes, it->second.size);
		g_buffers.erase(it);
	}

	void on_init_pipeline(uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
	{
		for (uint32_t i = 0; i < count; ++i)
		{
			const pipeline_subobject &so = subobjects[i];
			if (so.type != pipeline_subobject_type::input_layout || so.data == nullptr)
				continue;
			const auto *in = static_cast<const input_element *>(so.data);
			std::vector<elem> layout;
			for (uint32_t k = 0; k < so.count; ++k)
			{
				elem e;
				e.semantic = in[k].semantic != nullptr ? in[k].semantic : "";
				e.index = in[k].semantic_index;
				e.fmt = in[k].format;
				e.slot = in[k].buffer_binding;
				e.offset = in[k].offset;
				e.step = in[k].instance_step_rate;
				layout.push_back(e);
			}
			// D3D11_APPEND_ALIGNED_ELEMENT: offsets follow each other within a slot
			uint32_t next[16] = {};
			for (elem &e : layout)
			{
				const uint32_t s = e.slot & 15;
				if (e.offset == 0xFFFFFFFFu)
					e.offset = next[s];
				next[s] = e.offset + format_bytes(e.fmt);
			}
			g_layouts[pipe.handle] = std::move(layout);
		}
	}

	void on_destroy_pipeline(pipeline pipe) { g_layouts.erase(pipe.handle); }
	void on_bind_input_layout(pipeline pipe) { g_layout = pipe.handle; }

	void on_bind_vertex_buffers(uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
	{
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint32_t s = first + i;
			if (s >= 4)
				break;
			g_vb[s].buf = buffers != nullptr ? buffers[i].handle : 0;
			g_vb[s].off = offsets != nullptr ? offsets[i] : 0;
			g_vb[s].stride = strides != nullptr ? strides[i] : 0;
		}
	}

	void on_bind_index_buffer(resource buffer, uint64_t offset, uint32_t index_size)
	{
		g_ib = buffer.handle;
		g_ib_off = offset;
		g_ib_size = index_size == 4 ? 4 : 2;
	}

	void on_bind_ps_texture(uint32_t slot, uint64_t texture, uint32_t width, uint32_t height)
	{
		if (slot >= 2)
			return;
		g_tex[slot] = texture;
		g_tex_w[slot] = width;
		g_tex_h[slot] = height;
		if (texture != 0 && slot == 0)
			g_tex_size[texture] = { width, height };
	}

	void on_map(resource res, map_access access, void *data)
	{
		if (!g_active)
			return;
		const auto it = g_buffers.find(res.handle);
		if (it == g_buffers.end())
			return;
		buffer_info &b = it->second;
		if (access == map_access::write_discard)
			++b.maps_discard;
		else if (access == map_access::write_only)
			++b.maps_nooverwrite;
		else
			++b.maps_other;
		++b.maps_frame;
		if (!analysing())
		{
			b.valid = false; // written without us reading it
			return;
		}
		close_window(b);
		b.mapped = data;
		b.access = access;
		if (access == map_access::write_discard)
			b.drawn_end = 0;
		b.window = true;
		b.window_start = access == map_access::write_only ? b.drawn_end : 0;
		b.window_draws = 0;
	}

	void on_unmap(resource res)
	{
		if (!g_active)
			return;
		const auto it = g_buffers.find(res.handle);
		if (it == g_buffers.end())
			return;
		buffer_info &b = it->second;
		if (b.mapped == nullptr)
			return;
		const uint8_t *src = static_cast<const uint8_t *>(b.mapped);
		b.mapped = nullptr;
		if (g_world_finished)
		{
			++g_skipped_after_world;
			b.valid = false;
			return;
		}
		const uint64_t start = (b.access == map_access::write_only) ? std::min(b.drawn_end, b.size) : 0;
		const uint64_t len = std::min(b.size - start, COPY_CAP);
		if (g_frame_copy_bytes + len > COPY_BUDGET)
		{
			++g_budget_skips;
			b.valid = false;
			return;
		}
		if (b.shadow.size() < b.size)
			b.shadow.resize(static_cast<size_t>(b.size));
		const int method = 1; // streaming loads (plain memcpy from write-combined memory measured ~16x slower in run 1)
		const double t0 = now_ms();
		stream_copy(b.shadow.data() + start, src + start, static_cast<size_t>(len));
		const double ms = now_ms() - t0;
		g_copy_ms[method] += ms;
		g_copy_bytes[method] += len;
		++g_copy_count[method];
		g_frame_copy_bytes += len;
		g_frame_copy_ms += ms;
		b.valid = true;
		if (seq_on())
		{
			++g_seq_lines;
			repf("  %s%d mapped (%s), read bytes %llu..%llu in %.3f ms\n", b.ib ? "IB" : "VB", buffer_id(b), access_name(b.access),
				static_cast<unsigned long long>(start), static_cast<unsigned long long>(start + len), ms);
		}
	}

	void on_update(resource res, uint64_t offset, const void *data, uint64_t size)
	{
		if (!g_active)
			return;
		const auto it = g_buffers.find(res.handle);
		if (it == g_buffers.end() || data == nullptr)
			return;
		buffer_info &b = it->second;
		++b.updates;
		const uint64_t n = (size == UINT64_MAX || offset + size > b.size) ? (offset < b.size ? b.size - offset : 0) : size;
		b.update_bytes += n;
		if (b.shadow.size() < b.size)
			b.shadow.resize(static_cast<size_t>(b.size));
		if (n > 0)
			std::memcpy(b.shadow.data() + offset, data, static_cast<size_t>(n));
		b.valid = true;
		if (seq_on())
		{
			++g_seq_lines;
			repf("  %s%d updated bytes %llu..%llu\n", b.ib ? "IB" : "VB", buffer_id(b), static_cast<unsigned long long>(offset),
				static_cast<unsigned long long>(offset + n));
		}
	}

	void on_copy(resource src, uint64_t src_offset, resource dst, uint64_t dst_offset, uint64_t size)
	{
		if (!g_active)
			return;
		const auto it = g_buffers.find(dst.handle);
		if (it == g_buffers.end())
			return;
		buffer_info &b = it->second;
		++b.copies;
		b.copy_bytes += size == UINT64_MAX ? b.size : size;
		b.valid = false; // GPU-side copy: we do not follow it
		if (seq_on())
		{
			++g_seq_lines;
			repf("  %s%d written by a GPU copy (source %016llX +%llu, %llu bytes at %llu)\n", b.ib ? "IB" : "VB", buffer_id(b), static_cast<unsigned long long>(src.handle),
				static_cast<unsigned long long>(src_offset), static_cast<unsigned long long>(size), static_cast<unsigned long long>(dst_offset));
		}
	}

	void on_draw(const draw_info &d)
	{
		if (!g_active)
			return;
		if (d.stage >= 4)
			g_world_finished = true; // the world is composited: what comes now is the GUI
		if (!d.world)
		{
			++g_other_draws;
			if (d.screen && d.stage <= 3)
			{
				target_stats &ts = g_target_stats[target_id(d.rt, false)];
				ts.fmt = d.rt_fmt;
				++ts.draws;
				ts.stages |= 1u << std::min(std::max(d.stage, 0), 4);
				if (analysing())
				{
					g_rt_w = d.rt_w;
					g_rt_h = d.rt_h;
					const size_t before = g_cur.size();
					const uint64_t decoded = g_decoded_draws;
					decode_draw(d);
					ts.decoded += g_decoded_draws - decoded;
					ts.sprites += g_cur.size() - before;
					++g_world_draw_index;
				}
			}
			return;
		}
		g_rt_w = d.rt_w;
		g_rt_h = d.rt_h;
		const int s = std::min(std::max(d.stage, 0), 4);
		++g_world_draws;
		g_world_indices += d.count;
		++g_stage_draws[s];
		g_stage_indices[s] += d.count;
		combo &c = g_combos[std::make_tuple(d.vs, d.ps, g_layout)];
		c.vs = d.vs;
		c.ps = d.ps;
		c.layout = g_layout;
		++c.draws;
		c.indices += d.count;
		c.stages |= 1u << s;
		if (d.instances > 1)
		{
			++c.instanced;
			++g_world_instanced;
		}
		if (!d.indexed)
		{
			++c.nonindexed;
			++g_world_nonindexed;
		}
		const auto vit = g_buffers.find(g_vb[0].buf);
		if (vit != g_buffers.end())
		{
			++vit->second.world_draws;
			vit->second.world_indices += d.count;
		}
		if (d.indexed)
		{
			const auto iit = g_buffers.find(g_ib);
			if (iit != g_buffers.end())
				++iit->second.world_draws;
		}
		if (analysing())
			decode_draw(d);
		++g_world_draw_index;
	}

	void frame_end()
	{
		if (!g_active)
			return;
		for (auto &kv : g_buffers)
		{
			buffer_info &b = kv.second;
			close_window(b);
			b.max_maps_frame = std::max(b.max_maps_frame, b.maps_frame);
			b.maps_frame = 0;
			b.drawn_end = 0;
		}
		if (analysing())
		{
			++g_analysis_frames;
			g_analysis_copy_bytes += g_frame_copy_bytes;
			g_analysis_copy_ms += g_frame_copy_ms;
			if (g_prev_frame != UINT32_MAX && g_prev_frame + 1 == g_frame)
				analyse_pair(g_prev, g_cur);
			g_prev.swap(g_cur);
			g_prev_frame = g_frame;
			g_cur.clear();
		}
		g_frame_copy_bytes = 0;
		g_frame_copy_ms = 0;
		g_world_draw_index = 0;
		g_world_late = g_world_finished = false;
		if (++g_frame >= g_frames_total)
			finish();
	}
}
