// Exact character mask from Factorio's own sprites (1.2). See sprite_mask.h.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "sprite_mask.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <smmintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

namespace sprite_mask
{
	namespace
	{
		log_fn g_log = nullptr;
		void logf(const char *fmt, ...)
		{
			char buf[480];
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			if (g_log != nullptr)
				g_log(buf);
		}

		template <typename T>
		void release(T *&p)
		{
			if (p != nullptr)
			{
				p->Release();
				p = nullptr;
			}
		}

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

		uint64_t mix(uint64_t h, int64_t v)
		{
			h ^= static_cast<uint64_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			return h * 0x100000001B3ull;
		}

		// ------------------------------------------------------------ tracking

		bool g_enabled = true;

		struct buffer_info
		{
			uint64_t size = 0;
			bool dynamic_vb = false;
			std::vector<uint8_t> indices; // static index buffer contents (initial data)
			bool quads = false;           // ... the quad pattern: quad q = vertices 4q + (0 1 2 2 1 3)
			uint32_t quad_count = 0;      // ... for this many quads from the start
			uint32_t quad_index_size = 0; // ... with 2 or 4 byte indices (Factorio: 4)
			// dynamic vertex buffer: Factorio writes it as a ring (no-overwrite maps) and discards it when full - zoomed far
			// out several times a frame, so a draw's vertices must be copied before the next discard
			std::vector<uint32_t> pending; // draws (g_draws indices) whose vertices are not copied yet
			uint64_t gen_fill = 0;         // bytes used since the last discard (as far as the draws show)
			uint64_t fill_at_map = 0;      // ... at the last map
			double max_batch = 0.0;        // the most bytes written between two maps (decaying)
		};
		std::unordered_map<uint64_t, buffer_info> g_buffers;
		uint64_t g_kept_bytes = 0;

		std::unordered_set<uint64_t> g_sprite_layouts; // input layouts: float2 +0, float2 +8, RGBA8 +16, uint +20 in slot 0

		uint64_t g_layout = 0;
		uint64_t g_vb = 0, g_vb_off = 0;
		uint32_t g_vb_stride = 0;
		uint64_t g_ib = 0, g_ib_off = 0;
		uint32_t g_ib_size = 2;
		uint64_t g_view = 0;

		struct draw_rec
		{
			uint64_t vb, vb_off, ib, ib_off, view;
			uint32_t stride, ib_size, first, count, ps;
			int32_t base;
			uint8_t stage, target;
			bool red;
			bool fast;          // quad index buffer: quad q = vertices 4q..4q+3
			uint8_t state;      // 0 vertices not copied yet, 1 copied, 2 lost (the buffer was discarded first)
			uint32_t vmin;      // first vertex used (relative to base)
			uint64_t from, bytes; // the vertex bytes the draw uses, in the buffer
			size_t data;        // where they were copied to (g_vbuf)
		};
		std::unordered_map<uint64_t, bool> g_view_red; // texture view -> its format has no alpha channel
		std::vector<draw_rec> g_draws;
		std::vector<uint64_t> g_pending_vbs; // vertex buffers with pending draws
		ID3D11DeviceContext *g_ctx = nullptr; // the immediate context (from frame_end), for copying before a discard
		// this frame's copied vertex bytes
		std::vector<uint8_t> g_vbuf;
		size_t g_vlen = 0;
		uint32_t g_lost = 0, g_early = 0; // this frame: draws whose vertices were lost / copied at the draw
		uint32_t g_fast = 0;              // this frame: draws on the quad index pattern
		int g_copy_all = 0;               // frames left copying every draw's vertices at the draw (after a loss)
		// CPU cost on Factorio's render thread, this frame (QPC ticks): recording draws and copying vertices while Factorio
		// draws, and the work at present; the last finished frame's values for the stutter readout
		int64_t g_draw_ticks = 0;
		uint32_t g_own_maps = 0; // our own no-overwrite maps (copies right at a draw)
		frame_costs g_costs = {};
		double qpc_ms(int64_t ticks)
		{
			static int64_t freq = 0;
			if (freq == 0)
			{
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				freq = f.QuadPart;
			}
			return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
		}
		int64_t qpc_now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}
		std::unordered_map<uint64_t, int> g_targets; // other screen-sized targets: stable ids (1..)

		// ------------------------------------------------------------ sprites

		struct sprite
		{
			float px[4], py[4], u[4], v[4];
			float x0, y0, x1, y1, cx, cy;
			uint32_t color, ps;
			uint64_t view;
			uint8_t target, stage;
			bool glow, picked, red; // red: the texture has no alpha channel (shadow atlas): coverage is in red
			uint32_t order;         // place in this frame's draw order (all decoded sprites, also those dropped later)
			bool colour_layer;      // the player-colour mask: picked, but not drawn (its texture is not a coverage)
		};
		// what each sprite was picked for, this frame (status)
		int g_n_follows = 0, g_n_continues = 0, g_n_tracked = 0, g_n_place = 0, g_n_held = 0;
		// per frame, for the on-screen readout: 0 character and shadow found, 1 shadow held, 2 shadow missing, 3 no character
		constexpr int HISTORY = 600; // about 10 s at 60 fps
		uint8_t g_history[HISTORY] = {};
		int g_history_pos = 0, g_history_len = 0;
		uint8_t g_last_state = 3;
		bool g_last_flying = false;
		// where the character's shadow sits relative to its body, in body sizes (0 running, 1 flying); the running
		// defaults come from beta 22's dumps (left edge 0.1-0.5 right of the body's, bottom 0-0.25 below, 1.1-1.4 x 0.45-0.6)
		struct shadow_shape { float dx0, dy1, w, h; int n; };
		// the last shadow found, held for a few frames when it is missed (shifted with the body): no flicker on a miss
		struct held_shadow { sprite s; float bx0, by1; int age; bool valid; };
		held_shadow g_held = {};
		void release_held()
		{
			if (g_held.valid && g_held.s.view != 0)
				reinterpret_cast<IUnknown *>(g_held.s.view)->Release();
			g_held.valid = false;
		}
		struct frame_place { float dx, dy, w, h; };
		std::unordered_map<uint64_t, frame_place> g_frame_place; // body animation frame -> its shadow's place
		shadow_shape g_shape[2] = { { 0.3f, 0.1f, 1.3f, 0.55f, 0 }, { 0.3f, 0.6f, 1.3f, 0.55f, 0 } };

		uint8_t g_shadow_target = 0;     // the other target the shadow was found on (0: not yet)
		int g_shadow_missing = 0;        // frames with a character but no shadow on that target
		std::vector<sprite> g_cur, g_prev;
		// for the camera shift: world sprites anywhere on the screen (texture region + size key, top-left corner)
		struct cam_sprite { uint64_t key; float x, y; };
		std::vector<cam_sprite> g_cam_cur, g_cam_prev;
		bool g_prev_valid = false;
		float g_cam_x = 0, g_cam_y = 0;
		bool g_cam_known = false;

		// ------------------------------------------------------------ D3D11 objects

		ID3D11Device *g_dev = nullptr;
		ID3D11VertexShader *g_vs = nullptr, *g_vs_full = nullptr;
		ID3D11PixelShader *g_ps_mask = nullptr, *g_ps_overlay = nullptr;
		ID3D11InputLayout *g_il = nullptr;
		ID3D11Buffer *g_quad_vb = nullptr;
		uint32_t g_quad_capacity = 0;
		ID3D11SamplerState *g_sampler = nullptr;
		ID3D11BlendState *g_blend_max = nullptr, *g_blend_alpha = nullptr;
		ID3D11RasterizerState *g_raster = nullptr;
		ID3D11DepthStencilState *g_depth_off = nullptr;
		ID3D11Texture2D *g_mask = nullptr;
		ID3D11RenderTargetView *g_mask_rtv = nullptr;
		ID3D11ShaderResourceView *g_mask_srv = nullptr;
		uint32_t g_w = 0, g_h = 0;
		ID3D11Texture2D *g_bb = nullptr; // not owned: only compared
		ID3D11RenderTargetView *g_bb_rtv = nullptr;
		bool g_failed = false, g_valid = false;
		char g_status[384] = "not run yet";

		const char SHADER[] = R"hlsl(
Texture2D atlas : register(t0);
SamplerState samp : register(s0);
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 sel : TEXCOORD1; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 sel : TEXCOORD1; };
PSIn vs_main(VSIn i)
{
	PSIn o;
	o.pos = float4(i.pos, 0, 1);
	o.uv = i.uv;
	o.sel = i.sel;
	return o;
}
// sel: x = character weight, y = shadow weight (2: a shadow held from an earlier frame, its place estimated), z = the
// texture has no alpha channel (coverage in red), w = flying (the character, and its shadow while it flies).
// Out: R character, G shadow, B flying, A coverage (a held shadow: a quarter of it, so A < G marks it).
float4 ps_mask(PSIn i) : SV_Target
{
	const float4 t = atlas.Sample(samp, i.uv);
	const float a = saturate(i.sel.z > 0.5 ? t.r : t.a);
	return float4(i.sel.x * a, saturate(i.sel.y) * a, i.sel.w * a, i.sel.y > 1.5 ? 0.25 * a : a);
}
float4 vs_full(uint id : SV_VertexID) : SV_Position
{
	const float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_overlay(float4 pos : SV_Position) : SV_Target
{
	const float4 m = atlas.Load(int3(pos.xy, 0));
	if (m.r < 0.02 && m.g < 0.02)
		discard;
	const float3 c = m.r >= m.g ? float3(1.0, 0.15, 0.1) : float3(0.15, 0.45, 1.0);
	return float4(c, 0.65 * max(m.r, m.g));
}
)hlsl";

		bool compile_one(pD3DCompile compile, const char *entry, const char *target, ID3DBlob **out)
		{
			ID3DBlob *errors = nullptr;
			const HRESULT hr = compile(SHADER, sizeof(SHADER) - 1, "sprite_mask.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
			if (FAILED(hr))
			{
				logf("sprite mask: shader %s failed to compile: %s", entry, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
				release(errors);
				return false;
			}
			release(errors);
			return true;
		}

		bool create_objects(ID3D11Device *dev)
		{
			HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
			const auto compile = compiler != nullptr ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
			if (compile == nullptr)
			{
				logf("sprite mask: d3dcompiler_47.dll not available");
				return false;
			}
			ID3DBlob *vs = nullptr, *vs_full = nullptr, *ps_mask = nullptr, *ps_overlay = nullptr;
			bool ok = compile_one(compile, "vs_main", "vs_4_0", &vs) && compile_one(compile, "vs_full", "vs_4_0", &vs_full) &&
				compile_one(compile, "ps_mask", "ps_4_0", &ps_mask) && compile_one(compile, "ps_overlay", "ps_4_0", &ps_overlay);
			if (ok)
			{
				ok = SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
					SUCCEEDED(dev->CreateVertexShader(vs_full->GetBufferPointer(), vs_full->GetBufferSize(), nullptr, &g_vs_full)) &&
					SUCCEEDED(dev->CreatePixelShader(ps_mask->GetBufferPointer(), ps_mask->GetBufferSize(), nullptr, &g_ps_mask)) &&
					SUCCEEDED(dev->CreatePixelShader(ps_overlay->GetBufferPointer(), ps_overlay->GetBufferSize(), nullptr, &g_ps_overlay));
				const D3D11_INPUT_ELEMENT_DESC il[3] = {
					{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				};
				ok = ok && SUCCEEDED(dev->CreateInputLayout(il, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g_il));
			}
			release(vs);
			release(vs_full);
			release(ps_mask);
			release(ps_overlay);
			if (!ok)
			{
				logf("sprite mask: could not create the shaders");
				return false;
			}
			D3D11_SAMPLER_DESC sd = {};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			D3D11_BLEND_DESC bmax = {};
			bmax.RenderTarget[0].BlendEnable = TRUE;
			bmax.RenderTarget[0].SrcBlend = bmax.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bmax.RenderTarget[0].DestBlend = bmax.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bmax.RenderTarget[0].BlendOp = bmax.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
			bmax.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			D3D11_BLEND_DESC balpha = {};
			balpha.RenderTarget[0].BlendEnable = TRUE;
			balpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			balpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			balpha.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			balpha.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			balpha.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			balpha.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			balpha.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			D3D11_RASTERIZER_DESC rd = {};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			D3D11_DEPTH_STENCIL_DESC dd = {};
			dd.DepthEnable = FALSE;
			dd.StencilEnable = FALSE;
			ok = SUCCEEDED(dev->CreateSamplerState(&sd, &g_sampler)) && SUCCEEDED(dev->CreateBlendState(&bmax, &g_blend_max)) &&
				SUCCEEDED(dev->CreateBlendState(&balpha, &g_blend_alpha)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster)) &&
				SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_depth_off));
			if (!ok)
				logf("sprite mask: could not create the render states");
			return ok;
		}

		bool ensure_mask(ID3D11Device *dev, uint32_t w, uint32_t h)
		{
			if (g_mask != nullptr && g_w == w && g_h == h)
				return true;
			release(g_mask_srv);
			release(g_mask_rtv);
			release(g_mask);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = w;
			td.Height = h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_mask)) || FAILED(dev->CreateRenderTargetView(g_mask, nullptr, &g_mask_rtv)) ||
				FAILED(dev->CreateShaderResourceView(g_mask, nullptr, &g_mask_srv)))
			{
				logf("sprite mask: could not create the %ux%u mask", w, h);
				release(g_mask_srv);
				release(g_mask_rtv);
				release(g_mask);
				return false;
			}
			g_w = w;
			g_h = h;
			return true;
		}

		bool ensure_quad_vb(ID3D11Device *dev, uint32_t quads)
		{
			if (g_quad_vb != nullptr && quads <= g_quad_capacity)
				return true;
			release(g_quad_vb);
			g_quad_capacity = std::max<uint32_t>(1024, quads + quads / 2);
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = g_quad_capacity * 6 * 32;
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_quad_vb)))
			{
				g_quad_capacity = 0;
				return false;
			}
			return true;
		}

		// The pipeline state this module touches, saved and restored around its draws.
		struct saved_state
		{
			ID3D11InputLayout *il = nullptr;
			ID3D11Buffer *vb = nullptr;
			UINT vb_stride = 0, vb_offset = 0;
			ID3D11Buffer *ib = nullptr;
			DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
			UINT ib_offset = 0;
			D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			ID3D11VertexShader *vs = nullptr;
			ID3D11GeometryShader *gs = nullptr;
			ID3D11HullShader *hs = nullptr;
			ID3D11DomainShader *ds = nullptr;
			ID3D11PixelShader *ps = nullptr;
			ID3D11ShaderResourceView *srv = nullptr;
			ID3D11SamplerState *sampler = nullptr;
			ID3D11RasterizerState *rs = nullptr;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
			UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView *dsv = nullptr;
			ID3D11BlendState *blend = nullptr;
			FLOAT blend_factor[4] = {};
			UINT sample_mask = 0xFFFFFFFF;
			ID3D11DepthStencilState *depth = nullptr;
			UINT stencil_ref = 0;

			void save(ID3D11DeviceContext *c)
			{
				c->IAGetInputLayout(&il);
				c->IAGetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
				c->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
				c->IAGetPrimitiveTopology(&topology);
				c->VSGetShader(&vs, nullptr, nullptr);
				c->GSGetShader(&gs, nullptr, nullptr);
				c->HSGetShader(&hs, nullptr, nullptr);
				c->DSGetShader(&ds, nullptr, nullptr);
				c->PSGetShader(&ps, nullptr, nullptr);
				c->PSGetShaderResources(0, 1, &srv);
				c->PSGetSamplers(0, 1, &sampler);
				c->RSGetState(&rs);
				c->RSGetViewports(&viewport_count, viewports);
				c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				c->OMGetBlendState(&blend, blend_factor, &sample_mask);
				c->OMGetDepthStencilState(&depth, &stencil_ref);
			}

			void restore(ID3D11DeviceContext *c)
			{
				c->IASetInputLayout(il);
				c->IASetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
				c->IASetIndexBuffer(ib, ib_format, ib_offset);
				c->IASetPrimitiveTopology(topology);
				c->VSSetShader(vs, nullptr, 0);
				c->GSSetShader(gs, nullptr, 0);
				c->HSSetShader(hs, nullptr, 0);
				c->DSSetShader(ds, nullptr, 0);
				c->PSSetShader(ps, nullptr, 0);
				c->PSSetShaderResources(0, 1, &srv);
				c->PSSetSamplers(0, 1, &sampler);
				c->RSSetState(rs);
				c->RSSetViewports(viewport_count, viewports);
				c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				c->OMSetBlendState(blend, blend_factor, sample_mask);
				c->OMSetDepthStencilState(depth, stencil_ref);
				release(il);
				release(vb);
				release(ib);
				release(vs);
				release(gs);
				release(hs);
				release(ds);
				release(ps);
				release(srv);
				release(sampler);
				release(rs);
				for (ID3D11RenderTargetView *&r : rtvs)
					release(r);
				release(dsv);
				release(blend);
				release(depth);
			}
		};

		// ------------------------------------------------------------ index buffers

		uint32_t index_at(const draw_rec &d, const buffer_info *ib, uint32_t i, bool &ok)
		{
			const uint32_t k = d.first + i;
			if (ib == nullptr || ib->indices.empty())
				return (k / 6) * 4 + (k % 6 == 0 ? 0 : k % 6 == 1 ? 1 : k % 6 == 2 ? 2 : k % 6 == 3 ? 2 : k % 6 == 4 ? 1 : 3); // Factorio's quad pattern
			const uint64_t at = d.ib_off + static_cast<uint64_t>(k) * d.ib_size;
			if (at + d.ib_size > ib->indices.size())
			{
				ok = false;
				return 0;
			}
			if (d.ib_size == 2)
			{
				uint16_t v;
				std::memcpy(&v, ib->indices.data() + at, 2);
				return v;
			}
			uint32_t v;
			std::memcpy(&v, ib->indices.data() + at, 4);
			return v;
		}

		// ------------------------------------------------------------ copying the vertices before Factorio reuses them

		// The vertex range a draw uses (from the index buffer); false if it cannot be read.
		bool vertex_range(draw_rec &d)
		{
			const auto iit = g_buffers.find(d.ib);
			const buffer_info *ib = iit != g_buffers.end() ? &iit->second : nullptr;
			bool ok = true;
			uint32_t vmin = UINT32_MAX, vmax = 0;
			// the quad index buffer: the vertices of quads first/6 .. (first+count)/6 in order
			d.fast = ib != nullptr && ib->quads && d.ib_size == ib->quad_index_size && d.first % 6 == 0 && d.count % 6 == 0 && d.ib_off == 0 &&
				(static_cast<uint64_t>(d.first) + d.count) / 6 <= ib->quad_count;
			if (d.fast && d.count != 0)
			{
				vmin = d.first / 6 * 4;
				vmax = (d.first + d.count) / 6 * 4 - 1;
			}
			else
				for (uint32_t i = 0; i < d.count && ok; ++i)
				{
					const uint32_t v = index_at(d, ib, i, ok);
					vmin = std::min(vmin, v);
					vmax = std::max(vmax, v);
				}
			if (!ok || vmin > vmax)
				return false;
			const int64_t first_vertex = static_cast<int64_t>(vmin) + d.base;
			if (first_vertex < 0)
				return false;
			d.vmin = vmin;
			d.from = d.vb_off + static_cast<uint64_t>(first_vertex) * d.stride;
			d.bytes = static_cast<uint64_t>(vmax - vmin + 1) * d.stride;
			return d.bytes <= (8ull << 20);
		}

		void capture(draw_rec &d, const uint8_t *base, uint64_t vb_size)
		{
			if (d.state != 0)
				return;
			if (base == nullptr || d.from + d.bytes > vb_size || g_vlen + d.bytes > (64ull << 20))
			{
				d.state = 2;
				++g_lost;
				return;
			}
			if (g_vlen + d.bytes > g_vbuf.size())
				g_vbuf.resize(std::max<size_t>(g_vbuf.size() * 2, std::max<size_t>(4u << 20, g_vlen + static_cast<size_t>(d.bytes))));
			d.data = g_vlen;
			stream_copy(g_vbuf.data() + g_vlen, base + d.from, static_cast<size_t>(d.bytes));
			g_vlen += static_cast<size_t>(d.bytes);
			d.state = 1;
		}

		void lose_pending(buffer_info &b)
		{
			for (const uint32_t i : b.pending)
				if (i < g_draws.size() && g_draws[i].state == 0)
				{
					g_draws[i].state = 2;
					++g_lost;
				}
			b.pending.clear();
		}

		void flush(buffer_info &b, const uint8_t *base)
		{
			for (const uint32_t i : b.pending)
				if (i < g_draws.size())
					capture(g_draws[i], base, b.size);
			b.pending.clear();
		}

		// Copies a buffer's pending draws now, mapping it ourselves (no-overwrite: no stall, nothing changes).
		void flush_own(ID3D11DeviceContext *ctx, uint64_t vb, buffer_info &b)
		{
			if (b.pending.empty())
				return;
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			++g_own_maps;
			if (ctx == nullptr || FAILED(ctx->Map(reinterpret_cast<ID3D11Buffer *>(vb), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)) || mapped.pData == nullptr)
			{
				lose_pending(b);
				return;
			}
			flush(b, static_cast<const uint8_t *>(mapped.pData));
			ctx->Unmap(reinterpret_cast<ID3D11Buffer *>(vb), 0);
		}

		void flush_all(ID3D11DeviceContext *ctx)
		{
			for (const uint64_t vb : g_pending_vbs)
			{
				const auto it = g_buffers.find(vb);
				if (it != g_buffers.end())
					flush_own(ctx, vb, it->second);
			}
			g_pending_vbs.clear();
		}

		// ------------------------------------------------------------ reading this frame's sprites

		uint32_t g_order = 0;
		float g_char_size = 0.0f; // the character's size on screen (smoothed), 0 while unknown
		uint64_t g_cam_sub = 1;   // keep 1 in g_cam_sub camera sprites (by key)
		int g_cam_matches = 0, g_cam_best = 0; // camera shift: distinct sprites matched, how many agreed (diagnostics)
		std::wstring g_dump_dir;
		int g_dumps = 0;
		bool g_dump_now = false, g_found_last = false, g_shadow_last = false;
		int g_shadow_dumps = 0; // dumps for a lost shadow (at most 6 of the 18)

		void read_sprites()
		{
			g_cur.clear();
			g_cam_cur.clear();
			g_order = 0;
			if (g_draws.empty())
				return;
			const float W = static_cast<float>(g_w), H = static_cast<float>(g_h);
			const float cx = W * 0.5f, cy = H * 0.5f;
			// near the centre: a few times the character's size once it is known (zoomed far out a forest holds tens of
			// thousands of sprites), else 20% of the screen height
			const float full_reach = 0.2f * std::min(W, H);
			const float reach = g_char_size > 0.0f ? std::min(full_reach, std::max(64.0f, 5.0f * g_char_size)) : full_reach;
			const float max_side = 0.45f * std::min(W, H), cam_box = 0.3f * std::min(W, H);
			// camera sprites: key = texture view + region + size, place = top-left corner
			auto add_cam = [&](uint64_t view, float x, float y, float x3, float y3, float u0, float v0, float u3, float v3) {
				uint64_t k = mix(mix(1469598103934665603ull, static_cast<int64_t>(view)), std::llround(u0 * 16384.0f));
				k = mix(mix(mix(k, std::llround(v0 * 16384.0f)), std::llround(u3 * 16384.0f)), std::llround(v3 * 16384.0f));
				k = mix(mix(k, std::llround((x3 - x) * 4.0f)), std::llround((y3 - y) * 4.0f));
				// thinned by key (the same sprites in both frames) when there are too many
				if (((k >> 17) % g_cam_sub) == 0)
					g_cam_cur.push_back({ k, std::min(x, x3), std::min(y, y3) });
			};
			for (const draw_rec &d : g_draws)
			{
				if (d.state != 1)
				{
					g_order += d.count / 6;
					continue;
				}
				const auto iit = g_buffers.find(d.ib);
				const buffer_info *ib = iit != g_buffers.end() ? &iit->second : nullptr;
				if (!d.fast && ib == nullptr && d.ib != 0)
					continue;
				const uint8_t *const data = g_vbuf.data() + d.data;
				const uint32_t vmin = d.vmin, verts = static_cast<uint32_t>(d.bytes / d.stride);
				bool ok = true;
				for (uint32_t i = 0; i + 6 <= d.count; i += 6)
				{
					const uint32_t this_order = g_order++;
					uint32_t corner[4];
					if (d.fast)
					{
						const uint32_t q = (d.first + i) / 6 * 4;
						corner[0] = q; corner[1] = q + 1; corner[2] = q + 2; corner[3] = q + 3;
						// cheap early out: the first corner far from the centre
						float x, y;
						const uint8_t *p0 = data + static_cast<size_t>(q - vmin) * d.stride;
						std::memcpy(&x, p0, 4);
						std::memcpy(&y, p0 + 4, 4);
						if (d.target == 0 && g_cam_cur.size() < 16000 && std::fabs(x - cx) < cam_box && std::fabs(y - cy) < cam_box)
						{
							const uint8_t *p3 = p0 + 3 * d.stride;
							float x3, y3, u0, v0, u3, v3;
							std::memcpy(&x3, p3, 4);
							std::memcpy(&y3, p3 + 4, 4);
							std::memcpy(&u0, p0 + 8, 4);
							std::memcpy(&v0, p0 + 12, 4);
							std::memcpy(&u3, p3 + 8, 4);
							std::memcpy(&v3, p3 + 12, 4);
							add_cam(d.view, x, y, x3, y3, u0, v0, u3, v3);
						}
						if (d.stage == 1 && d.target == 0) // floor stage: camera only
							continue;
						if (std::fabs(x - cx) > reach + max_side || std::fabs(y - cy) > reach + max_side)
							continue;
					}
					else
					{
						uint32_t idx[6];
						for (uint32_t k = 0; k < 6; ++k)
							idx[k] = index_at(d, ib, i + k, ok);
						if (!ok)
							break;
						// the two triangles (a b c)(c b d) of a quad: corners a, b, c, d
						corner[0] = idx[0]; corner[1] = idx[1]; corner[2] = idx[2]; corner[3] = idx[5];
					}
					sprite s = {};
					s.x0 = s.y0 = 1e30f;
					s.x1 = s.y1 = -1e30f;
					bool inside = true;
					for (int k = 0; k < 4; ++k)
					{
						if (corner[k] < vmin || corner[k] - vmin >= verts)
						{
							inside = false;
							break;
						}
						const uint8_t *p = data + static_cast<size_t>(corner[k] - vmin) * d.stride;
						std::memcpy(&s.px[k], p + 0, 4);
						std::memcpy(&s.py[k], p + 4, 4);
						std::memcpy(&s.u[k], p + 8, 4);
						std::memcpy(&s.v[k], p + 12, 4);
						s.x0 = std::min(s.x0, s.px[k]); s.x1 = std::max(s.x1, s.px[k]);
						s.y0 = std::min(s.y0, s.py[k]); s.y1 = std::max(s.y1, s.py[k]);
						if (k == 0)
							std::memcpy(&s.color, p + 16, 4);
					}
					if (!inside)
						continue;
					if (!d.fast && d.target == 0 && g_cam_cur.size() < 16000 && std::fabs(s.px[0] - cx) < cam_box && std::fabs(s.py[0] - cy) < cam_box)
						add_cam(d.view, s.px[0], s.py[0], s.px[3], s.py[3], s.u[0], s.v[0], s.u[3], s.v[3]);
					const float w = s.x1 - s.x0, h = s.y1 - s.y0;
					s.cx = (s.x0 + s.x1) * 0.5f;
					s.cy = (s.y0 + s.y1) * 0.5f;
					if (!(w >= 1.0f && h >= 1.0f && std::max(w, h) <= max_side && std::fabs(s.cx - cx) <= reach && std::fabs(s.cy - cy) <= reach))
						continue;
					if (d.stage == 1 && d.target == 0)
						continue;
					s.ps = d.ps;
					s.view = d.view;
					s.target = d.target;
					s.stage = d.stage;
					s.glow = (s.color >> 24) == 0; // alpha 0: drawn additively
					s.red = d.red;
					s.order = this_order;
					g_cur.push_back(s);
					if (g_cur.size() >= 20000)
						return;
				}
			}
		}

		// ------------------------------------------------------------ which sprites follow the camera


		// The camera shift: world sprites whose texture region and size appear once in both frames, anywhere on screen.
		void find_camera()
		{
			g_cam_known = false;
			if (!g_prev_valid || g_cam_prev.empty())
				return;
			std::unordered_map<uint64_t, int> count_prev, count_cur;
			std::unordered_map<uint64_t, size_t> where_prev;
			for (size_t j = 0; j < g_cam_prev.size(); ++j)
				if (++count_prev[g_cam_prev[j].key] == 1)
					where_prev[g_cam_prev[j].key] = j;
			for (const cam_sprite &c : g_cam_cur)
				++count_cur[c.key];
			// 1 px bins; the shift is the mean of the deltas in the best 3x3 bins (swaying trees spread around it)
			std::vector<std::pair<float, float>> deltas;
			std::map<std::pair<long long, long long>, int> hist;
			for (const cam_sprite &c : g_cam_cur)
			{
				if (count_cur[c.key] != 1)
					continue;
				const auto it = count_prev.find(c.key);
				if (it == count_prev.end() || it->second != 1)
					continue;
				const cam_sprite &p = g_cam_prev[where_prev[c.key]];
				deltas.push_back({ c.x - p.x, c.y - p.y });
				++hist[{ std::llround(c.x - p.x), std::llround(c.y - p.y) }];
			}
			const int total = static_cast<int>(deltas.size());
			int best = 0;
			long long bx = 0, by = 0;
			for (const auto &kv : hist)
			{
				int sum = 0;
				for (int oy = -1; oy <= 1; ++oy)
					for (int ox = -1; ox <= 1; ++ox)
					{
						const auto it = hist.find({ kv.first.first + ox, kv.first.second + oy });
						sum += it != hist.end() ? it->second : 0;
					}
				if (sum > best)
				{
					best = sum;
					bx = kv.first.first;
					by = kv.first.second;
				}
			}
			double mx = 0, my = 0;
			int m = 0;
			for (const auto &d : deltas)
				if (std::fabs(d.first - static_cast<float>(bx)) <= 1.5f && std::fabs(d.second - static_cast<float>(by)) <= 1.5f)
				{
					mx += d.first;
					my += d.second;
					++m;
				}
			if (m > 0)
			{
				g_cam_x = static_cast<float>(mx / m);
				g_cam_y = static_cast<float>(my / m);
			}
			// enough of the distinct sprites agree, or it is not trusted
			g_cam_known = best >= 3 && best * 10 >= total * 3;
			g_cam_matches = total;
			g_cam_best = best;
		}

		float overlap(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1)
		{
			const float iw = std::min(ax1, bx1) - std::max(ax0, bx0), ih = std::min(ay1, by1) - std::max(ay0, by0);
			if (iw <= 0.0f || ih <= 0.0f)
				return 0.0f;
			const float a = (ax1 - ax0) * (ay1 - ay0), b = (bx1 - bx0) * (by1 - by0);
			return iw * ih / std::max(1.0f, std::min(a, b)); // share of the smaller rectangle
		}

		// The character, without guessing: its player-colour mask is the one sprite near the centre tinted with a
		// colour at about half alpha (the player's colour; vehicles the player drives have one too). The body is drawn
		// right before it, over the same place; flying, an additive glow right after. Its shadow (on the shadow target)
		// is found by the camera rule near the body, carried on while standing, and remembered by texture region.
		std::unordered_set<uint64_t> g_shadow_known; // texture regions seen as the character's shadow
		std::unordered_set<uint64_t> g_body_known;   // texture regions seen as the character's body (found by its colour layer)
		bool g_by_region = false;                    // this frame's body was found by a remembered region
		uint32_t g_mask_color = 0;
		uint32_t g_lock_color = 0;
		int g_lock_count = 0, g_lock_missing = 0;
		float g_lock_ox = 0.0f, g_lock_oy = 0.0f; // where the colour being locked sat, relative to the screen centre
		bool g_color_locked = false;
		bool g_have_body = false;

		uint64_t region_key(const sprite &s)
		{
			uint64_t h = mix(2166136261ull, static_cast<int64_t>(s.view));
			for (int k = 0; k < 4; ++k)
			{
				h = mix(h, std::llround(s.u[k] * 16384.0f));
				h = mix(h, std::llround(s.v[k] * 16384.0f));
			}
			return h;
		}

		float dist_to_rect(float px, float py, const sprite &s)
		{
			const float dx = std::max(0.0f, std::max(s.x0 - px, px - s.x1)), dy = std::max(0.0f, std::max(s.y0 - py, py - s.y1));
			return std::sqrt(dx * dx + dy * dy);
		}

		int pick()
		{
			g_n_follows = g_n_continues = g_n_tracked = g_n_place = g_n_held = 0;
			const float cx = static_cast<float>(g_w) * 0.5f, cy = static_cast<float>(g_h) * 0.5f;
			const size_t n = g_cur.size();
			for (sprite &s : g_cur)
				s.picked = false;
			g_have_body = false;
			g_by_region = false;

			// the body of a player-colour mask: right before it in draw order, over the same place (the largest sprite there
			// that lies mostly within the mask's place and is not much smaller than it - nor much larger: a tree the
			// character hugs is drawn right before it too, and covers it; the body is 1.3-2.5x the mask's area and up to 2.2x
			// its height facing up or down - a tree 14-17x and 4.2-4.7x)
			auto body_of = [&](size_t mi) {
				const sprite &m = g_cur[mi];
				const float marea = (m.x1 - m.x0) * (m.y1 - m.y0), mside = std::max(m.x1 - m.x0, m.y1 - m.y0);
				int body = -1;
				float body_area = 0.0f;
				for (int j = static_cast<int>(mi) - 1; j >= 0 && g_cur[j].order + 3 >= m.order; --j)
				{
					const sprite &b = g_cur[j];
					const float area = (b.x1 - b.x0) * (b.y1 - b.y0);
					if (b.target == 0 && !b.glow && area >= 0.3f * marea && area <= 4.0f * marea && std::max(b.x1 - b.x0, b.y1 - b.y0) <= 3.0f * mside &&
						area > body_area && overlap(b.x0, b.y0, b.x1, b.y1, m.x0, m.y0, m.x1, m.y1) >= 0.5f)
					{
						body = j;
						body_area = area;
					}
				}
				return body;
			};

			// 1. the player-colour mask nearest the centre: the player's colour at half alpha, drawn right after a body
			// (tree leaves and smoke are tinted too, but alone). Once the same colour won 20 frames in a row it is the only
			// one accepted.
			int mask = -1, mask_body = -1;
			float best = 1e30f;
			for (size_t i = 0; i < n; ++i)
			{
				const sprite &s = g_cur[i];
				const uint32_t a = s.color >> 24, rgb = s.color & 0x00FFFFFFu;
				if (s.target != 0 || a < 0x70 || a > 0x90 || rgb == 0 || rgb == 0x00FFFFFFu)
					continue;
				if (g_color_locked && s.color != g_lock_color)
					continue;
				const float size = std::max(s.x1 - s.x0, s.y1 - s.y0);
				// The camera is centred on our character (its feet): the screen centre lies under its colour layer
				// sideways, and at most a little below it - or well below it in flight (the sprite is raised). Other
				// players (multiplayer) are only accepted standing right there.
				const float side = std::max(0.0f, std::max(s.x0 - cx, cx - s.x1));
				if (side > std::max(6.0f, 0.35f * size) || s.y0 - cy > std::max(6.0f, 0.35f * size) || cy - s.y1 > 1.2f * size)
					continue;
				const float d = std::fabs(s.cx - cx) + std::fabs(s.cy - cy);
				if (d >= best)
					continue;
				const int body = body_of(i);
				if (body < 0)
					continue;
				best = d;
				mask = static_cast<int>(i);
				mask_body = body;
			}
			int picked = 0;
			int body_sprite = -1; // this frame's body sprite (its animation frame keys the shadow's exact place)
			if (mask >= 0)
			{
				sprite &m = g_cur[mask];
				m.colour_layer = true;
				g_lock_missing = 0;
				if (!g_color_locked)
				{
					// locking needs the same colour 20 frames in a row at the same place on screen (relative to the
					// centre): our character stays there, another player walking past does not
					const float ox = m.cx - cx, oy = m.cy - cy, msize = std::max(m.x1 - m.x0, m.y1 - m.y0);
					const bool same_place = std::fabs(ox - g_lock_ox) <= 0.6f * msize && std::fabs(oy - g_lock_oy) <= 0.6f * msize;
					g_lock_count = m.color == g_lock_color && same_place ? g_lock_count + 1 : 1;
					if (g_lock_count == 1)
					{
						g_lock_ox = ox;
						g_lock_oy = oy;
					}
					g_lock_color = m.color;
					g_color_locked = g_lock_count >= 20;
				}
				m.picked = true;
				g_mask_color = m.color;
				picked = 1;
				// 2. its body (found above); the glow right after it
				const int body = mask_body;
				body_sprite = body;
				if (body >= 0)
				{
					g_cur[body].picked = true;
					g_have_body = true;
					++picked;
					if (g_body_known.size() < 16384)
						g_body_known.insert(region_key(g_cur[body])); // its animation frames, for when the colour layer is missing
				}
				for (size_t j = mask + 1; j < n && g_cur[j].order <= m.order + 3; ++j)
				{
					sprite &gl = g_cur[j];
					if (gl.target == 0 && gl.glow && overlap(gl.x0, gl.y0, gl.x1, gl.y1, m.x0, m.y0, m.x1, m.y1) >= 0.3f)
					{
						gl.picked = true;
						++picked;
					}
				}
			}
			else
			{
				// No colour layer this frame: a body animation frame seen before, near the centre (nearest wins).
				int body = -1;
				float bbest = 1e30f;
				for (size_t i = 0; i < n; ++i)
				{
					const sprite &b = g_cur[i];
					if (b.target != 0 || b.glow)
						continue;
					const float size = std::max(b.x1 - b.x0, b.y1 - b.y0);
					if (dist_to_rect(cx, cy, b) > std::max(8.0f, size) || g_body_known.count(region_key(b)) == 0)
						continue;
					if (g_char_size > 0.0f && (size > 2.0f * g_char_size || size < 0.5f * g_char_size))
						continue;
					const float d = std::fabs(b.cx - cx) + std::fabs(b.cy - cy);
					if (d < bbest)
					{
						bbest = d;
						body = static_cast<int>(i);
					}
				}
				if (body >= 0)
				{
					g_cur[body].picked = true;
					body_sprite = body;
					g_have_body = true;
					g_by_region = true;
					picked = 1;
				}
				if (++g_lock_missing > 30 && body < 0)
					g_char_size = 0.0f; // search the whole centre area again
				if (g_color_locked && g_lock_missing > 120) // e.g. the player changed colour, or a save was loaded
				{
					g_color_locked = false;
					g_lock_count = 0;
				}
				if (body < 0)
				{
					release_held(); // the character is gone: its last shadow must not come back with it later
					return 0;
				}
			}
			float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
			for (const sprite &s : g_cur)
				if (s.picked && !s.glow)
				{
					bx0 = std::min(bx0, s.x0); by0 = std::min(by0, s.y0);
					bx1 = std::max(bx1, s.x1); by1 = std::max(by1, s.y1);
				}
			const float bsize = std::max(bx1 - bx0, by1 - by0), grow = 1.5f * bsize;
			g_char_size = g_char_size > 0.0f ? 0.8f * g_char_size + 0.2f * bsize : bsize;

			// 3. its shadow: black sprites on a shadow target next to the body. Evidence, strongest first: a remembered
			// texture region (3), follows the camera (2), sits where the character's shadow sits relative to the body
			// (its place: learned, 2), continues last frame's shadow (1). Turning, a new shadow frame can jump by about the
			// camera shift - so "moves with the camera" only rules out sprites with no other evidence.
			bool air = false;
			for (const sprite &q : g_cur)
				if (q.picked && !q.glow && q.target == 0 && q.stage == 3)
					air = true;
			shadow_shape &shape = g_shape[air ? 1 : 0];
			// taking off and landing the shadow moves between its running and flying places: either fits
			const bool air_usable = g_shape[1].n >= 10; // flying: the height is not known before it was seen
			// the exact place for this body animation frame, once seen (in body sprite heights from its left / bottom edge)
			const sprite *body_s = body_sprite >= 0 ? &g_cur[body_sprite] : nullptr;
			const float body_h = body_s != nullptr ? std::max(1.0f, body_s->y1 - body_s->y0) : 1.0f;
			const auto fit = g_frame_place.find(body_s != nullptr ? region_key(*body_s) : 0);
			const frame_place *exact = body_s != nullptr && fit != g_frame_place.end() ? &fit->second : nullptr;
			const float cam_len = g_cam_known ? std::sqrt(g_cam_x * g_cam_x + g_cam_y * g_cam_y) : 0.0f;
			const bool moving = cam_len >= 2.0f;
			int shadow = -1;
			int shadow_rank = 0;
			bool shadow_by_place = false, shadow_evidence = false, shadow_fits = false;
			float shadow_err = 1e30f;
			for (size_t i = 0; i < n; ++i)
			{
				const sprite &s = g_cur[i];
				if (s.target == 0 || (s.color & 0x00FFFFFFu) != 0)
					continue;
				const float w = s.x1 - s.x0, h = s.y1 - s.y0;
				if (!(s.x1 >= bx0 - grow && s.x0 <= bx1 + grow && s.y1 >= by0 - grow && s.y0 <= by1 + grow) || std::max(w, h) > 2.5f * bsize)
					continue;
				// its place relative to the body (in body sizes): left edge, bottom edge, width, height
				// the place lies between the running and the flying one (taking off and landing the shadow moves down
				// with the height): how far along is read from the bottom edge, the rest is interpolated
				const shadow_shape &gs = g_shape[0], &as = g_shape[1];
				const float m_y = (s.y1 - by1) / bsize;
				float t = 0.0f;
				if (air_usable && std::fabs(as.dy1 - gs.dy1) > 0.05f)
					t = std::min(1.0f, std::max(0.0f, (m_y - gs.dy1) / (as.dy1 - gs.dy1)));
				const float p_x = gs.dx0 + t * (as.dx0 - gs.dx0), p_y = gs.dy1 + t * (as.dy1 - gs.dy1);
				const float p_w = gs.w + t * (as.w - gs.w), p_h = gs.h + t * (as.h - gs.h);
				const float e_x = std::fabs((s.x0 - bx0) / bsize - p_x), e_y = std::fabs(m_y - p_y);
				const float e_w = std::fabs(w / bsize - p_w), e_h = std::fabs(h / bsize - p_h);
				// Whatever the evidence, a character's shadow is about 1.1-1.7 body sizes wide and 0.45-0.9 high (all dumps,
				// running and flying), and within where it can be at all (running to high flight: left edge -0.4..1.6, bottom
				// -0.3..1.4 body sizes - flying, the height below the body varies too much for a window around the learned
				// place): tree and building shadows next to the character (or remembered by mistake) are ruled out here, so
				// they can never teach the place either
				const float rw = w / bsize, rh = h / bsize, m_x = (s.x0 - bx0) / bsize;
				if (rw < 0.6f || rw > 2.0f || rh < 0.25f || rh > 1.1f || m_x < -0.4f || m_x > 1.6f || m_y < -0.3f || m_y > 1.4f)
					continue;
				// sideways it depends on the facing more than the rest
				const bool by_place = e_x < 0.4f && e_y < 0.25f && e_w < 0.35f && e_h < 0.25f;
				float err = e_x + e_y + e_w + e_h;
				// this body frame's own shadow place
				bool by_frame = false;
				if (exact != nullptr)
				{
					const float f_x = std::fabs((s.x0 - body_s->x0) / body_h - exact->dx), f_y = std::fabs((s.y1 - body_s->y1) / body_h - exact->dy);
					const float f_w = std::fabs(w / body_h - exact->w), f_h = std::fabs(h / body_h - exact->h);
					by_frame = f_x < 0.08f && f_y < 0.08f && f_w < 0.12f && f_h < 0.12f;
					if (by_frame)
						err = std::min(err, 0.25f * (f_x + f_y + f_w + f_h));
				}
				int rank = g_shadow_known.count(region_key(s)) != 0 || by_frame ? 3 : 0;
				const float tol = std::max(3.0f, 0.12f * std::max(w, h));
				float b0 = 1e30f, bc = 1e30f, bp = 1e30f;
				if (g_prev_valid)
					for (const sprite &p : g_prev)
					{
						if (p.target != s.target || p.ps != s.ps || (p.color & 0x00FFFFFFu) != 0)
							continue;
						const float pw = p.x1 - p.x0, ph = p.y1 - p.y0;
						if (std::fabs(pw - w) > 0.5f * std::max(w, 4.0f) || std::fabs(ph - h) > 0.5f * std::max(h, 4.0f))
							continue;
						const float dx = s.cx - p.cx, dy = s.cy - p.cy, d0 = std::sqrt(dx * dx + dy * dy);
						b0 = std::min(b0, d0);
						if (g_cam_known)
							bc = std::min(bc, std::sqrt((dx - g_cam_x) * (dx - g_cam_x) + (dy - g_cam_y) * (dy - g_cam_y)));
						if (p.picked)
							bp = std::min(bp, d0);
					}
				const bool with_camera = moving && (bc + std::max(1.0f, 0.25f * cam_len) < b0 || (bc <= 0.75f && b0 > 1.0f));
				bool evidence = rank == 3 && !by_frame; // remembered or follows the camera: may teach the shadow's place
				if (rank == 0 && moving && !with_camera && b0 <= tol && b0 < 0.5f * cam_len && bc >= b0 + std::max(1.5f, 0.3f * cam_len))
				{
					rank = 2;
					evidence = true;
				}
				if (rank == 0 && by_place)
					rank = 2;
				if (rank == 0 && !with_camera && bp <= tol)
					rank = 1;
				if (rank == 0 || (with_camera && rank < 3 && !by_place))
					continue;
				// prefer the strongest evidence, then the closest fit to the shadow's place
				if (rank > shadow_rank || (rank == shadow_rank && err < shadow_err))
				{
					shadow = static_cast<int>(i);
					shadow_rank = rank;
					shadow_err = err;
					shadow_by_place = by_place && !evidence;
					shadow_evidence = evidence;
					shadow_fits = by_place || by_frame;
				}
			}
			if (shadow >= 0)
			{
				sprite &sh = g_cur[shadow];
				sh.picked = true;
				++picked;
				if (shadow_evidence)
				{
					// learn its place (separately running and flying), within what a character's shadow can be: beta 26
					// learned itself away from it (-0.84 / -0.62) by learning from tree shadows it then preferred
					const float k = shape.n < 10 ? 0.3f : 0.1f;
					shape.dx0 += k * ((sh.x0 - bx0) / bsize - shape.dx0);
					shape.dy1 += k * ((sh.y1 - by1) / bsize - shape.dy1);
					shape.w += k * ((sh.x1 - sh.x0) / bsize - shape.w);
					shape.h += k * ((sh.y1 - sh.y0) / bsize - shape.h);
					++shape.n;
					auto clampf = [](float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); };
					shape.dx0 = clampf(shape.dx0, -0.1f, air ? 1.4f : 0.7f);
					shape.dy1 = clampf(shape.dy1, air ? 0.15f : -0.15f, air ? 1.2f : 0.35f);
					shape.w = clampf(shape.w, 0.9f, 1.7f);
					shape.h = clampf(shape.h, 0.35f, 0.9f);
				}
				// this body frame's shadow place: only from picks that also fit the place
				if (body_s != nullptr && shadow_rank >= 2 && shadow_fits && g_frame_place.size() < 16384)
				{
					const frame_place now = { (sh.x0 - body_s->x0) / body_h, (sh.y1 - body_s->y1) / body_h, (sh.x1 - sh.x0) / body_h, (sh.y1 - sh.y0) / body_h };
					auto ins = g_frame_place.emplace(region_key(*body_s), now);
					if (!ins.second)
					{
						frame_place &fp = ins.first->second;
						fp.dx += 0.3f * (now.dx - fp.dx);
						fp.dy += 0.3f * (now.dy - fp.dy);
						fp.w += 0.3f * (now.w - fp.w);
						fp.h += 0.3f * (now.h - fp.h);
					}
				}
				if (shadow_rank == 3)
					++g_n_tracked;
				else if (shadow_by_place)
					++g_n_place;
				else if (shadow_rank == 2)
				{
					++g_n_follows;
					if (shadow_fits && g_shadow_known.size() < 8192) // remembered only if it also fits the place
						g_shadow_known.insert(region_key(sh));
				}
				else
					++g_n_continues;
				g_shadow_target = sh.target;
				// hold it
				release_held();
				g_held.s = sh;
				g_held.bx0 = bx0;
				g_held.by1 = by1;
				g_held.age = 0;
				g_held.valid = sh.view != 0;
				if (g_held.valid)
					reinterpret_cast<IUnknown *>(sh.view)->AddRef();
			}
			else if (g_held.valid && ++g_held.age <= 3)
			{
				// missed this frame: last shadow, moved with the body
				sprite hs = g_held.s;
				const float dx = bx0 - g_held.bx0, dy = by1 - g_held.by1;
				for (int k = 0; k < 4; ++k)
				{
					hs.px[k] += dx;
					hs.py[k] += dy;
				}
				hs.x0 += dx; hs.x1 += dx; hs.cx += dx;
				hs.y0 += dy; hs.y1 += dy; hs.cy += dy;
				hs.picked = true;
				hs.order = UINT32_MAX;
				g_cur.push_back(hs);
				++picked;
				++g_n_held;
			}
			return picked;
		}

		// ------------------------------------------------------------ drawing the mask

		void draw_mask(ID3D11DeviceContext *ctx, int picked)
		{
			const FLOAT zero[4] = { 0, 0, 0, 0 };
			ctx->ClearRenderTargetView(g_mask_rtv, zero);
			if (picked == 0 || !ensure_quad_vb(g_dev, static_cast<uint32_t>(picked)))
				return;
			D3D11_MAPPED_SUBRESOURCE m = {};
			if (FAILED(ctx->Map(g_quad_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
				return;
			struct vtx { float x, y, u, v, r, g, red, pad; };
			auto *out = static_cast<vtx *>(m.pData);
			struct batch { uint64_t view; uint32_t first, count; };
			std::vector<batch> batches;
			uint32_t n = 0;
			const float sx = 2.0f / static_cast<float>(g_w), sy = 2.0f / static_cast<float>(g_h);
			// flying: any of the character's sprites in the air stage; its shadow gets the flag too (B), so frame generation
			// can tell the flying shadow from the walking one
			bool flying = false;
			for (const sprite &s : g_cur)
				flying = flying || (s.picked && s.target == 0 && s.stage == 3);
			for (const sprite &s : g_cur)
			{
				// the additive glow is light around the character; the colour layer's texture is a tint mask, not a coverage
				// (it drew bars and blocks): the body sprite carries the character's outline
				if (!s.picked || s.glow || s.colour_layer)
					continue;
				const bool held = s.target != 0 && s.order == UINT32_MAX; // a shadow held from an earlier frame
				const float r = s.target == 0 ? 1.0f : 0.0f, g = s.target == 0 ? 0.0f : (held ? 2.0f : 1.0f), glow = s.red ? 1.0f : 0.0f;
				const float air = (s.target == 0 ? s.stage == 3 : flying) ? 1.0f : 0.0f; // flying (air stage): B
				static const int order[6] = { 0, 1, 2, 2, 1, 3 };
				for (const int k : order)
					*out++ = { s.px[k] * sx - 1.0f, 1.0f - s.py[k] * sy, s.u[k], s.v[k], r, g, glow, air };
				if (batches.empty() || batches.back().view != s.view)
					batches.push_back({ s.view, n * 6, 0 });
				batches.back().count += 6;
				++n;
			}
			ctx->Unmap(g_quad_vb, 0);

			const UINT stride = 32, offset = 0;
			ctx->IASetInputLayout(g_il);
			ctx->IASetVertexBuffers(0, 1, &g_quad_vb, &stride, &offset);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(g_vs, nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->HSSetShader(nullptr, nullptr, 0);
			ctx->DSSetShader(nullptr, nullptr, 0);
			ctx->PSSetShader(g_ps_mask, nullptr, 0);
			ctx->PSSetSamplers(0, 1, &g_sampler);
			ctx->RSSetState(g_raster);
			const D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1 };
			ctx->RSSetViewports(1, &vp);
			ctx->OMSetRenderTargets(1, &g_mask_rtv, nullptr);
			const FLOAT factor[4] = { 1, 1, 1, 1 };
			ctx->OMSetBlendState(g_blend_max, factor, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_depth_off, 0);
			for (const batch &b : batches)
			{
				ID3D11ShaderResourceView *srv = reinterpret_cast<ID3D11ShaderResourceView *>(b.view);
				ctx->PSSetShaderResources(0, 1, &srv);
				ctx->Draw(b.count, b.first);
			}
			ID3D11ShaderResourceView *none = nullptr;
			ctx->PSSetShaderResources(0, 1, &none);
		}

		void draw_overlay(ID3D11DeviceContext *ctx, ID3D11Texture2D *back_buffer)
		{
			if (back_buffer != g_bb || g_bb_rtv == nullptr)
			{
				release(g_bb_rtv);
				g_bb = back_buffer;
				if (FAILED(g_dev->CreateRenderTargetView(back_buffer, nullptr, &g_bb_rtv)))
				{
					g_bb_rtv = nullptr;
					return;
				}
			}
			ctx->IASetInputLayout(nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(g_vs_full, nullptr, 0);
			ctx->PSSetShader(g_ps_overlay, nullptr, 0);
			ctx->RSSetState(g_raster);
			const D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1 };
			ctx->RSSetViewports(1, &vp);
			ctx->OMSetRenderTargets(1, &g_bb_rtv, nullptr);
			const FLOAT factor[4] = { 1, 1, 1, 1 };
			ctx->OMSetBlendState(g_blend_alpha, factor, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_depth_off, 0);
			ctx->PSSetShaderResources(0, 1, &g_mask_srv);
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView *none = nullptr;
			ctx->PSSetShaderResources(0, 1, &none);
		}
	}

	namespace
	{
		// Writes this frame's sprites near the centre (and last frame's picks) to sprite_mask_N.txt: what the character was
		// made of when it was lost, or when asked for.
		void write_dump(const char *why, int picked)
		{
			if (g_dump_dir.empty() || g_dumps >= 18)
				return;
			const std::wstring path = g_dump_dir + L"\\sprite_mask_" + std::to_wstring(++g_dumps) + L".txt";
			FILE *f = _wfopen(path.c_str(), L"w");
			if (f == nullptr)
				return;
			const float cx = static_cast<float>(g_w) * 0.5f, cy = static_cast<float>(g_h) * 0.5f;
			std::fprintf(f, "Factorio FG sprite mask dump (%s)\n%s\n", why, g_status);
			std::fprintf(f, "picked %d; colour %08X%s (count %d, missing %d); character size %.1f px; camera %s %+.2f %+.2f (best %d of %d matches, 1 in %llu sprites, %zu / %zu camera sprites); shadow target %u, %zu regions known; vertices: %zu draws (%u on the quad pattern), %u copied at the draw%s, %u lost, %.0f KB\n",
				picked, g_lock_color, g_color_locked ? " locked" : "", g_lock_count, g_lock_missing, g_char_size, g_cam_known ? "known" : "UNKNOWN", g_cam_x, g_cam_y,
				g_cam_best, g_cam_matches, static_cast<unsigned long long>(g_cam_sub), g_cam_cur.size(), g_cam_prev.size(), g_shadow_target, g_shadow_known.size(),
				g_draws.size(), g_fast, g_early, g_copy_all > 0 ? " (all, after a loss)" : "", g_lost, static_cast<double>(g_vlen) / 1024.0);
			std::fprintf(f, "shadow place (body sizes: left edge, bottom edge, width, height): running %.2f %+.2f %.2f x %.2f (%d learned), flying %.2f %+.2f %.2f x %.2f (%d learned)\n",
				g_shape[0].dx0, g_shape[0].dy1, g_shape[0].w, g_shape[0].h, g_shape[0].n, g_shape[1].dx0, g_shape[1].dy1, g_shape[1].w, g_shape[1].h, g_shape[1].n);
			auto line = [&](const sprite &q, const char *tag) {
				std::fprintf(f, "  %-6s #%-6u %s%u st%u PS %08X col %08X%s rect %+8.1f %+8.1f size %6.1f x %6.1f uv %.4f,%.4f..%.4f,%.4f%s%s\n", tag, q.order,
					q.target == 0 ? "W" : "T", q.target, q.stage, q.ps, q.color, q.glow ? " GLOW" : "", q.x0 - cx, q.y0 - cy, q.x1 - q.x0, q.y1 - q.y0,
					std::min(q.u[0], q.u[3]), std::min(q.v[0], q.v[3]), std::max(q.u[0], q.u[3]), std::max(q.v[0], q.v[3]), q.red ? " red" : "",
					q.colour_layer ? " COLOUR-LAYER" : "");
			};
			std::fprintf(f, "\nThis frame, sprites within 160 px of the centre, in draw order (%zu sprites read near the centre):\n", g_cur.size());
			int k = 0;
			for (const sprite &q : g_cur)
				if (std::fabs(q.cx - cx) <= 160.0f && std::fabs(q.cy - cy) <= 160.0f && k++ < 400)
					line(q, q.picked ? "PICKED" : "");
			std::fprintf(f, "\nLast frame's picks:\n");
			for (const sprite &q : g_prev)
				if (q.picked)
					line(q, "picked");
			std::fclose(f);
			logf("sprite mask: %s - wrote sprite_mask_%d.txt", why, g_dumps);
		}
	}

	void set_dump_dir(const wchar_t *dir) { g_dump_dir = dir != nullptr ? dir : L""; }
	void dump_now() { g_dump_now = true; }

	// ---------------------------------------------------------------- public

	void set_log(log_fn log) { g_log = log; }
	void set_enabled(bool on) { g_enabled = on; }
	bool enabled() { return g_enabled && !g_failed; }
	ID3D11Texture2D *mask() { return g_mask; }
	bool valid() { return g_valid; }
	const char *status() { return g_status; }
	frame_costs last_costs() { return g_costs; }

	void readout(char *buf, size_t size)
	{
		int counts[4] = {};
		for (int i = 0; i < g_history_len; ++i)
			++counts[g_history[i] & 3];
		static const char *const now[4] = { "found", "HELD (estimated place)", "MISSING", "-" };
		if (!g_valid)
			std::snprintf(buf, size, "Mask: no character this frame (map view, menu)");
		else
			std::snprintf(buf, size, "Mask: character %s%s | shadow %s | last 10 s: shadow held %d, missing %d, character lost %d frames",
				g_last_state == 3 ? "NOT FOUND" : "found", g_last_state != 3 && g_last_flying ? " (flying)" : "", now[g_last_state],
				counts[1], counts[2], counts[3]);
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
		b.dynamic_vb = vb && desc.heap == memory_heap::upload;
		if (ib && initial != nullptr && initial->data != nullptr && b.size <= (4ull << 20) && g_kept_bytes + b.size > (32ull << 20))
			logf("sprite mask: index buffer of %llu bytes not kept (32 MB of index data kept already)", static_cast<unsigned long long>(b.size));
		if (ib && initial != nullptr && initial->data != nullptr && b.size <= (4ull << 20) && g_kept_bytes + b.size <= (32ull << 20))
		{
			b.indices.assign(static_cast<const uint8_t *>(initial->data), static_cast<const uint8_t *>(initial->data) + b.size);
			g_kept_bytes += b.size;
			// quad pattern? (Factorio: 0 1 2 2 1 3, +4 per quad; 32-bit indices) - for how many quads from the start
			static const uint32_t pat[6] = { 0, 1, 2, 2, 1, 3 };
			for (const uint32_t size : { 4u, 2u })
			{
				const size_t n = b.indices.size() / size;
				size_t k = 0;
				for (; k < n; ++k)
				{
					uint32_t v = 0;
					std::memcpy(&v, b.indices.data() + size * k, size);
					const uint32_t want = static_cast<uint32_t>((k / 6) * 4 + pat[k % 6]);
					if (v != (size == 2 ? (want & 0xFFFFu) : want))
						break;
				}
				if (k / 6 >= 256)
				{
					b.quad_count = static_cast<uint32_t>(k / 6);
					b.quad_index_size = size;
					b.quads = true;
					break;
				}
			}
			if (b.size >= 4096)
			{
				static int logged = 0;
				if (logged++ < 8)
					logf("sprite mask: index buffer of %llu bytes: %s", static_cast<unsigned long long>(b.size),
						b.quads ? (b.quad_index_size == 4 ? "quad pattern, 32-bit indices" : "quad pattern, 16-bit indices") : "not the quad pattern");
			}
		}
	}

	void on_destroy_resource(resource res)
	{
		const auto it = g_buffers.find(res.handle);
		if (it == g_buffers.end())
			return;
		g_kept_bytes -= std::min<uint64_t>(g_kept_bytes, it->second.indices.size());
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
			bool pos = false, uv = false, col = false, extra = false, other_slot = false;
			uint32_t next = 0;
			for (uint32_t k = 0; k < so.count; ++k)
			{
				uint32_t off = in[k].offset == 0xFFFFFFFFu ? next : in[k].offset;
				if (in[k].buffer_binding != 0 || in[k].instance_step_rate != 0)
					other_slot = true;
				if (in[k].format == format::r32g32_float && off == 0) pos = true;
				if (in[k].format == format::r32g32_float && off == 8) uv = true;
				if (in[k].format == format::r8g8b8a8_unorm && off == 16) col = true;
				if (in[k].format == format::r32_uint && off == 20) extra = true;
				next = off + (in[k].format == format::r8g8b8a8_unorm || in[k].format == format::r32_uint ? 4 : 8);
			}
			if (pos && uv && col && extra && !other_slot)
				g_sprite_layouts.insert(pipe.handle);
			else
				g_sprite_layouts.erase(pipe.handle);
		}
	}

	void on_destroy_pipeline(pipeline pipe) { g_sprite_layouts.erase(pipe.handle); }
	void on_bind_input_layout(pipeline pipe) { g_layout = pipe.handle; }

	void on_bind_vertex_buffers(uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
	{
		if (first != 0 || count == 0)
			return;
		g_vb = buffers != nullptr ? buffers[0].handle : 0;
		g_vb_off = offsets != nullptr ? offsets[0] : 0;
		g_vb_stride = strides != nullptr ? strides[0] : 0;
	}

	void on_bind_index_buffer(resource buffer, uint64_t offset, uint32_t index_size)
	{
		g_ib = buffer.handle;
		g_ib_off = offset;
		g_ib_size = index_size == 4 ? 4 : 2;
	}

	void on_bind_ps_view(uint32_t slot, uint64_t view)
	{
		if (slot == 0)
			g_view = view;
	}

	void on_draw(const sprite_probe::draw_info &d)
	{
		if (!enabled() || g_vb_stride == 0)
			return;
		const auto it = g_buffers.find(g_vb);
		if (it == g_buffers.end() || !it->second.dynamic_vb)
			return;
		buffer_info &b = it->second;
		// how far the ring is used since the last discard (every draw from it, also those not recorded)
		{
			uint64_t end = 0;
			if (d.indexed)
			{
				const auto iit = g_buffers.find(g_ib);
				if (iit != g_buffers.end() && iit->second.quads && g_ib_size == iit->second.quad_index_size && g_ib_off == 0 && d.vertex_offset >= 0 &&
					(static_cast<uint64_t>(d.first) + d.count) / 6 <= iit->second.quad_count)
					end = g_vb_off + ((static_cast<uint64_t>(d.first) + d.count + 5) / 6 * 4 + static_cast<uint64_t>(d.vertex_offset)) * g_vb_stride;
			}
			else
				end = g_vb_off + (static_cast<uint64_t>(d.first) + d.count) * g_vb_stride;
			b.gen_fill = std::max(b.gen_fill, std::min(end, b.size));
		}
		if (!d.indexed || d.instances > 1 || g_view == 0 || g_vb_stride != 24 || g_draws.size() >= 16384)
			return;
		// objects and air; the floor stage (belts, grass, decoratives) only feeds the camera shift: an open field has few objects
		const bool world = d.world && d.stage >= 1 && d.stage <= 3;
		const bool other = d.screen && !d.world && d.stage == 0;
		if (!world && !other)
			return;
		if (g_sprite_layouts.count(g_layout) == 0)
			return;
		const int64_t t_start = qpc_now();
		struct tick_guard { int64_t t; ~tick_guard() { g_draw_ticks += qpc_now() - t; } } guard{ t_start };
		draw_rec r = {};
		r.stage = static_cast<uint8_t>(d.stage);
		if (world)
			r.target = 0;
		else
		{
			auto t = g_targets.find(d.rt);
			if (t == g_targets.end())
				t = g_targets.emplace(d.rt, static_cast<int>(std::min<size_t>(g_targets.size() + 1, 255))).first;
			r.target = static_cast<uint8_t>(t->second);
			// all of them: the shadow is not always on the same one (T3, T4 seen)
		}
		r.vb = g_vb;
		r.vb_off = g_vb_off;
		r.stride = g_vb_stride;
		r.ib = g_ib;
		r.ib_off = g_ib_off;
		r.ib_size = g_ib_size;
		r.first = d.first;
		r.count = d.count;
		r.base = d.vertex_offset;
		r.ps = d.ps;
		if (!vertex_range(r))
			return;
		r.view = g_view;
		reinterpret_cast<IUnknown *>(g_view)->AddRef(); // kept alive until the mask is drawn at present
		{
			auto vit = g_view_red.find(g_view);
			if (vit == g_view_red.end())
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
				reinterpret_cast<ID3D11ShaderResourceView *>(g_view)->GetDesc(&vd);
				bool red = false;
				switch (vd.Format)
				{
				case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT:
				case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_SNORM:
				case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
					red = true;
					break;
				default:
					break;
				}
				if (g_view_red.size() > 4096)
					g_view_red.clear();
				vit = g_view_red.emplace(g_view, red).first;
				static int logged = 0;
				if (logged++ < 24)
					logf("sprite mask: texture view format %u (%s) for %s", static_cast<unsigned>(vd.Format), red ? "coverage in red" : "coverage in alpha",
						d.world ? "a world sprite" : "a shadow-target sprite");
			}
			r.red = vit->second;
		}
		b.gen_fill = std::max(b.gen_fill, std::min(r.from + r.bytes, b.size));
		if (r.fast)
			++g_fast;
		g_draws.push_back(r);
		if (b.pending.empty() && std::find(g_pending_vbs.begin(), g_pending_vbs.end(), g_vb) == g_pending_vbs.end())
			g_pending_vbs.push_back(g_vb);
		b.pending.push_back(static_cast<uint32_t>(g_draws.size() - 1));
		// Pending draws are copied at Factorio's next no-overwrite map of the buffer (the data is still there). When the
		// ring is nearly full the next map may be a discard (the old contents gone): copy now.
		const double next = 1.25 * (b.max_batch > 0.0 ? b.max_batch : 0.5 * static_cast<double>(b.size)) + 16384.0;
		if (g_ctx != nullptr && (g_copy_all > 0 || static_cast<double>(b.size - std::min(b.size, b.gen_fill)) < next))
		{
			++g_early;
			flush_own(g_ctx, g_vb, b);
		}
	}

	void on_map(resource res, map_access access, void *data)
	{
		if (!enabled())
			return;
		const auto it = g_buffers.find(res.handle);
		if (it == g_buffers.end() || !it->second.dynamic_vb)
			return;
		buffer_info &b = it->second;
		if (!b.pending.empty())
		{
			const int64_t t_start = qpc_now();
			// no-overwrite: Factorio will not touch what was drawn, the pointer shows the same data; discard: it is gone
			if (access != map_access::write_discard && data != nullptr)
				flush(b, static_cast<const uint8_t *>(data));
			else
				lose_pending(b);
			g_draw_ticks += qpc_now() - t_start;
		}
		const uint64_t batch = b.gen_fill - std::min(b.gen_fill, b.fill_at_map);
		b.max_batch = std::max(static_cast<double>(batch), 0.995 * b.max_batch);
		if (access == map_access::write_discard)
			b.gen_fill = 0;
		b.fill_at_map = b.gen_fill;
	}

	namespace
	{
		void release_draws()
		{
			for (const draw_rec &d : g_draws)
				reinterpret_cast<IUnknown *>(d.view)->Release();
			for (const uint64_t vb : g_pending_vbs)
			{
				const auto it = g_buffers.find(vb);
				if (it != g_buffers.end())
					it->second.pending.clear();
			}
			g_pending_vbs.clear();
			g_lost = g_early = g_fast = 0;
			g_draws.clear();
			g_vlen = 0;
			g_draw_ticks = 0;
			g_own_maps = 0;
		}
	}

	void frame_end(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, uint32_t width, uint32_t height, bool overlay)
	{
		// this frame's cost so far (while Factorio drew); the rest is added below
		g_costs = {};
		g_costs.draw_ms = static_cast<float>(qpc_ms(g_draw_ticks));
		g_costs.own_maps = g_own_maps;
		g_costs.copy_all = g_copy_all > 0;
		if (!enabled() || device == nullptr || context == nullptr || width == 0 || height == 0)
		{
			release_draws();
			g_valid = false;
			return;
		}
		if (g_dev != device)
		{
			shutdown();
			g_dev = device;
			if (!create_objects(device))
			{
				g_failed = true;
				std::snprintf(g_status, sizeof(g_status), "failed to start (see ReShade.log)");
				release_draws();
				return;
			}
			logf("sprite mask: ready");
		}
		g_ctx = context;
		if (!ensure_mask(device, width, height))
		{
			release_draws();
			g_valid = false;
			return;
		}

		LARGE_INTEGER t0, t1, f;
		QueryPerformanceCounter(&t0);
		flush_all(context); // the rest of the frame's vertices: still in the buffer
		read_sprites();
		find_camera();
		const int picked = pick();
		int shadow = 0;
		uint8_t shadow_on = 0;
		for (const sprite &s : g_cur)
			if (s.picked && s.target != 0)
			{
				++shadow;
				shadow_on = s.target;
			}
		if (shadow_on != 0)
		{
			g_shadow_target = shadow_on;
			g_shadow_missing = 0;
		}
		else if (picked > 0 && g_shadow_target != 0 && ++g_shadow_missing > 600)
		{
			g_shadow_target = 0; // look at all targets again
			g_shadow_missing = 0;
		}

		saved_state st;
		st.save(context);
		draw_mask(context, picked);
		if (overlay && back_buffer != nullptr)
			draw_overlay(context, back_buffer);
		st.restore(context);
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&f);
		const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);

		{
			const uint8_t state = picked == 0 ? 3 : (shadow == 0 ? 2 : (g_n_held ? 1 : 0));
			g_history[g_history_pos] = state;
			g_history_pos = (g_history_pos + 1) % HISTORY;
			g_history_len = g_history_len < HISTORY ? g_history_len + 1 : HISTORY;
			g_last_state = state;
			g_last_flying = false;
			for (const sprite &s : g_cur)
				g_last_flying = g_last_flying || (s.picked && s.target == 0 && s.stage == 3);
		}
		std::snprintf(g_status, sizeof(g_status), "%zu sprites near the centre; character %s (colour %08X%s, %d sprites%s), shadow %s; %zu body / %zu shadow regions known; camera %s%+.0f %+.0f px; %zu draws, %u lost%s; %.2f ms",
			g_cur.size(), picked > 0 ? (g_by_region ? "found by a remembered frame" : "found") : "NOT FOUND", g_mask_color, g_color_locked ? " locked" : "", picked - shadow, g_have_body ? "" : ", no body", shadow == 0 ? "-" :
			(g_n_held ? "held from last frame" : g_n_tracked ? "remembered" : g_n_follows ? "follows the camera" : g_n_place ? "by its place" : "carried on"), g_body_known.size(), g_shadow_known.size(), g_cam_known ? "" : "? ", g_cam_x, g_cam_y, g_draws.size(), g_lost, g_copy_all > 0 ? " (copying at every draw)" : "", ms);
		g_valid = true;
		g_costs.end_ms = static_cast<float>(ms);
		g_costs.draws = static_cast<uint32_t>(g_draws.size());
		g_costs.early = g_early;
		g_costs.lost = g_lost;
		g_costs.vertex_kb = static_cast<float>(static_cast<double>(g_vlen) / 1024.0);
		g_costs.sprites = static_cast<uint32_t>(g_cur.size());
		const int64_t t_dump = qpc_now();
		const int dumps_before = g_dumps;
		if (g_dump_now)
		{
			write_dump("asked for", picked);
			g_dump_now = false;
		}
		else if (g_found_last && picked == 0)
			write_dump("the character was lost", picked);
		else if (g_shadow_last && picked > 0 && shadow == 0 && g_shadow_dumps < 6)
		{
			++g_shadow_dumps;
			write_dump("the shadow was lost", picked);
		}
		g_costs.dumped = g_dumps != dumps_before;
		g_costs.dump_ms = static_cast<float>(qpc_ms(qpc_now() - t_dump));
		g_found_last = picked > 0;
		g_shadow_last = shadow > 0;
		// a lost draw: for a while copy every draw's vertices right at the draw (a map of our own each)
		if (g_lost > 0)
			g_copy_all = 600;
		else if (g_copy_all > 0)
			--g_copy_all;
		// camera sprites: aim for 2000-8000, the same thinning in consecutive frames (changed only when far off)
		if (g_cam_cur.size() >= 12000 && g_cam_sub < 64)
		{
			g_cam_sub *= 2;
			g_cam_cur.clear(); // not comparable with the next frame's
		}
		else if (g_cam_cur.size() < 1500 && g_cam_sub > 1)
		{
			g_cam_sub /= 2;
			g_cam_cur.clear();
		}
		g_prev.swap(g_cur);
		g_cam_prev.swap(g_cam_cur);
		g_prev_valid = true;
		release_draws();
	}

	void shutdown()
	{
		release(g_vs);
		release(g_vs_full);
		release(g_ps_mask);
		release(g_ps_overlay);
		release(g_il);
		release(g_quad_vb);
		g_quad_capacity = 0;
		release(g_sampler);
		release(g_blend_max);
		release(g_blend_alpha);
		release(g_raster);
		release(g_depth_off);
		release(g_mask_srv);
		release(g_mask_rtv);
		release(g_mask);
		release(g_bb_rtv);
		g_bb = nullptr;
		g_w = g_h = 0;
		g_dev = nullptr;
		g_valid = false;
		g_prev.clear();
		g_prev_valid = false;
		g_view_red.clear();
		g_shadow_target = 0;
		g_shadow_known.clear();
		g_body_known.clear();
		g_frame_place.clear();
		release_held();
		g_color_locked = false;
		g_lock_count = g_lock_missing = 0;
		g_cam_prev.clear();
		g_char_size = 0.0f;
		g_ctx = nullptr;
		for (auto &kv : g_buffers)
		{
			kv.second.pending.clear();
			kv.second.gen_fill = kv.second.fill_at_map = 0;
		}
		g_pending_vbs.clear();
	}
}
