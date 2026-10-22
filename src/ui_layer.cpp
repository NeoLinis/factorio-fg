// The UI layer for frame generation (GUI and held character) and the clean frame. See ui_layer.h.

#include "ui_layer.h"

#include <d3dcompiler.h>

#include <cstdarg>
#include <cstdio>

namespace
{
	ui_layer::log_fn g_log = nullptr;
	ID3D12Device *g_device = nullptr; // not owned
	DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	uint32_t g_width = 0, g_height = 0;

	ID3D12RootSignature *g_root = nullptr;
	ID3D12PipelineState *g_pso = nullptr;
	ID3D12DescriptorHeap *g_heap = nullptr;
	bool g_failed = false;

	ID3D12Resource *g_ui = nullptr;    // R8G8B8A8_UNORM, alpha 1 where the UI layer applies
	ID3D12Resource *g_clean = nullptr; // R8G8B8A8_UNORM: the HUD-less frame with the character painted over
	D3D12_RESOURCE_STATES g_ui_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	D3D12_RESOURCE_STATES g_clean_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	bool g_inpainted = false;
	bool g_clean_written = false; // the clean frame was written (painted over and / or the shadow taken out)

	// the resources the descriptors were written for
	ID3D12Resource *g_d_final = nullptr, *g_d_hudless = nullptr, *g_d_hold = nullptr, *g_d_floor = nullptr, *g_d_objects = nullptr;
	bool g_descriptors = false;

	// How much of the screen the UI layer covers (diagnostic): counted on the GPU, read back two frames later.
	ID3D12Resource *g_count = nullptr;
	ID3D12Resource *g_zero = nullptr;
	ID3D12Resource *g_readback[3] = {};
	uint32_t g_readback_index = 0;
	float g_coverage = -1.0f;

	const D3D12_RESOURCE_STATES READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	const D3D12_RESOURCE_STATES CLEAN_READ = READ | D3D12_RESOURCE_STATE_COPY_SOURCE;

	void log_line(const char *fmt, ...)
	{
		if (g_log == nullptr)
			return;
		char buf[512];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		g_log(buf);
	}

	template <typename T>
	void release(T *&p)
	{
		if (p != nullptr)
			p->Release();
		p = nullptr;
	}

	const char UI_SHADER[] = R"hlsl(
Texture2D<float4> final_frame : register(t0);
Texture2D<float4> hudless : register(t1);
// Character pixels held still (motion pass), shown from the real frame: 1 on the ground, 0.5 flying, 0.8 its shadow as
// it is. Below 0.25: the shadow laid over as a darkening (value x 4), the ground under it moving with the camera.
Texture2D<float> hold : register(t2);
Texture2D<float4> floor_snap : register(t3); // world snapshot after the floor stage: the ground under the character
Texture2D<float4> objects_snap : register(t4); // world snapshot with all objects, before the flying stage: under it in flight
RWTexture2D<unorm float4> ui : register(u0);
RWStructuredBuffer<uint> ui_count : register(u1);
RWTexture2D<unorm float4> clean : register(u2); // the frame generation blends, with the character painted over
// hold_char: lay the character over (else only its shadow's darkening); grow: px the character's hold is grown by;
// clean_on: write the clean frame (painted over and / or the shadow taken out)
cbuffer constants : register(b0) { uint2 size; float threshold; uint hold_on; uint inpaint_on; uint hold_char; uint grow; uint clean_on; };
groupshared uint group_count;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	if (gi == 0) group_count = 0;
	GroupMemoryBarrierWithGroupSync();
	if (all(id.xy < size))
	{
		float3 f = final_frame[id.xy].rgb;
		float3 h = hudless[id.xy].rgb;
		float3 d = abs(f - h);
		bool is_ui = max(d.r, max(d.g, d.b)) > threshold;
		bool held = false;
		float hold_max = 0.0, shade = 0.0;
		int2 hi = int2(size) - 1;
		if (hold_on != 0)
		{
			// held character, grown by 'grow' pixels (its anti-aliased outline, single missed pixels inside: 2 for
			// the guessed hold, 1 for the exact mask)
			if (hold_char != 0)
			{
				[unroll] for (int y = -2; y <= 2; ++y)
				[unroll] for (int x = -2; x <= 2; ++x)
				{
					const float v = hold.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), hi), 0));
					if (max(abs(x), abs(y)) <= int(grow) && v > 0.25)
						hold_max = max(hold_max, v);
				}
				held = hold_max > 0.25;
			}
			const float own = hold.Load(int3(id.xy, 0));
			if (own > 0.001 && own < 0.249)
				shade = min(own * 4.0, 0.9);
		}
		is_ui = is_ui || held;
		// painted over: a wider area (4 px around the hold), so no rim of the character stays in what frame generation
		// blends; the ring between 2 and 4 px is ground, which the painted ground matches
		// Hold values: 1 object (painted with the floor snapshot), 0.5 flying (with the objects snapshot), 0.8 shadow
		// (not painted: it stays in the frame as it is).
		bool paint_ground = false, paint_flying = false;
		if (inpaint_on != 0)
		{
			[unroll] for (int py = -2; py <= 2; ++py)
			[unroll] for (int px = -2; px <= 2; ++px)
			{
				const float v = hold.Load(int3(clamp(int2(id.xy) + int2(px, py) * 2, int2(0, 0), hi), 0));
				paint_ground = paint_ground || v > 0.9;
				paint_flying = paint_flying || (v > 0.4 && v < 0.6);
			}
		}
		const bool paint = paint_ground || paint_flying;
		// The shadow: black at its darkness over the generated frame (stays put with the character); the frame
		// generation input has the ground there without it (below).
		ui[id.xy] = is_ui ? float4(f, 1.0) : float4(0.0, 0.0, 0.0, shade);
		if (is_ui) InterlockedAdd(group_count, 1);
		if (clean_on != 0)
		{
			// The character removed from what frame generation blends: what is under it from a world snapshot taken before it
			// is drawn (on the ground: the floor snapshot, ground and shadows; flying: the objects snapshot, trees and
			// buildings below), brought to the frame's lighting by the ratio finished frame / snapshot around it.
			float3 c = h;
			if (paint)
			{
				const bool flying = !paint_ground;
				float3 sum_h = float3(0, 0, 0), sum_f = float3(0, 0, 0);
				[unroll] for (int k = 0; k < 8; ++k)
				{
					const float a = 0.7853982 * float(k);
					int2 q = clamp(int2(id.xy) + int2(round(float2(cos(a), sin(a)) * 48.0)), int2(0, 0), hi);
					if (hold.Load(int3(q, 0)) < 0.25)
					{
						sum_h += hudless[q].rgb;
						sum_f += flying ? objects_snap[q].rgb : floor_snap[q].rgb;
					}
				}
				float3 ratio = float3(1, 1, 1);
				if (sum_f.r + sum_f.g + sum_f.b > 0.05)
					ratio = clamp(sum_h / max(sum_f, float3(0.01, 0.01, 0.01)), 0.0, 4.0);
				c = saturate((flying ? objects_snap[id.xy].rgb : floor_snap[id.xy].rgb) * ratio);
			}
			// The shadow taken out: the ground as it is without it (the darkening comes back with the UI layer, so
			// FSR's real frames, composed the same way, stay as Factorio drew them).
			if (shade > 0.0)
				c = saturate(c / (1.0 - shade));
			clean[id.xy] = float4(c, 1.0);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0 && group_count > 0) InterlockedAdd(ui_count[0], group_count);
}
)hlsl";

	void barrier(D3D12_RESOURCE_BARRIER &b, ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
	}

	ID3D12Resource *create_texture()
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = g_width;
		desc.Height = g_height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		ID3D12Resource *res = nullptr;
		if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
				IID_PPV_ARGS(&res))))
			return nullptr;
		return res;
	}

	void release_pipeline()
	{
		release(g_pso);
		release(g_root);
		release(g_heap);
		release(g_count);
		release(g_zero);
		for (ID3D12Resource *&rb : g_readback)
			release(rb);
	}

	bool build_pipeline()
	{
		HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler != nullptr ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
		if (compile == nullptr)
		{
			log_line("ui: d3dcompiler_47.dll not available, no UI layer");
			return false;
		}
		ID3DBlob *code = nullptr, *errors = nullptr;
		HRESULT hr = compile(UI_SHADER, sizeof(UI_SHADER) - 1, "ui_layer.hlsl", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
			&code, &errors);
		if (FAILED(hr))
		{
			log_line("ui: shader failed to compile: %s", errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
			release(errors);
			return false;
		}
		release(errors);

		D3D12_DESCRIPTOR_RANGE ranges[3] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // t0 final, t1 HUD-less, t2 hold, t3 floor, t4 objects snapshot
		ranges[0].NumDescriptors = 5;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; // u0 UI layer
		ranges[1].NumDescriptors = 1;
		ranges[1].OffsetInDescriptorsFromTableStart = 5;
		ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; // u2 clean frame (u1: root UAV, the counter)
		ranges[2].NumDescriptors = 1;
		ranges[2].BaseShaderRegister = 2;
		ranges[2].OffsetInDescriptorsFromTableStart = 6;
		D3D12_ROOT_PARAMETER params[3] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = 8;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 3;
		params[1].DescriptorTable.pDescriptorRanges = ranges;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[2].Descriptor.ShaderRegister = 1;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rs = {};
		rs.NumParameters = 3;
		rs.pParameters = params;
		ID3DBlob *blob = nullptr;
		hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
		release(errors);
		if (SUCCEEDED(hr))
			hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root));
		release(blob);
		if (SUCCEEDED(hr))
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature = g_root;
			pd.CS.pShaderBytecode = code->GetBufferPointer();
			pd.CS.BytecodeLength = code->GetBufferSize();
			hr = g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
		}
		release(code);
		auto make_buffer = [](D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource **out) {
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = type;
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = 256;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = flags;
			return g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(out));
		};
		if (SUCCEEDED(hr))
			hr = make_buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &g_count);
		if (SUCCEEDED(hr))
			hr = make_buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &g_zero);
		for (ID3D12Resource *&rb : g_readback)
			if (SUCCEEDED(hr))
				hr = make_buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &rb);
		if (SUCCEEDED(hr))
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			hd.NumDescriptors = 7;
			hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			hr = g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
		}
		if (FAILED(hr))
		{
			log_line("ui: pipeline creation failed (0x%08X), no UI layer", static_cast<unsigned>(hr));
			release_pipeline();
			return false;
		}
		log_line("ui: UI layer ready");
		return true;
	}
}

namespace ui_layer
{
	bool init(ID3D12Device *device, DXGI_FORMAT format, log_fn log)
	{
		shutdown();
		g_log = log;
		g_device = device;
		g_format = format;
		g_failed = device == nullptr || !build_pipeline();
		return !g_failed;
	}

	bool resize(uint32_t width, uint32_t height)
	{
		release(g_ui);
		release(g_clean);
		g_descriptors = false;
		g_width = width;
		g_height = height;
		if (g_failed || g_device == nullptr)
			return false;
		g_ui = create_texture();
		g_clean = create_texture();
		g_ui_state = g_clean_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		return g_ui != nullptr && g_clean != nullptr;
	}

	void shutdown()
	{
		release(g_ui);
		release(g_clean);
		release_pipeline();
		g_descriptors = false;
		g_device = nullptr;
		g_failed = false;
		g_inpainted = false;
		g_clean_written = false;
		g_coverage = -1.0f;
	}

	bool record(ID3D12GraphicsCommandList *list, const inputs &in)
	{
		if (g_failed || g_pso == nullptr || g_ui == nullptr || g_clean == nullptr || list == nullptr || in.final_frame == nullptr ||
			in.hudless == nullptr)
			return false;
		ID3D12Resource *const hold = in.hold;
		// painting the character over needs it held and both snapshots
		const bool inpaint = in.inpaint && in.hold_character && hold != nullptr && in.floor != nullptr && in.objects != nullptr;
		// the clean frame: painted over, and / or the shadow taken out
		const bool write_clean = inpaint || (in.shadow && hold != nullptr);
		ID3D12Resource *const floor = inpaint ? in.floor : nullptr;
		ID3D12Resource *const objects = inpaint ? in.objects : nullptr;
		g_inpainted = inpaint;
		g_clean_written = write_clean;

		const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (!g_descriptors || in.final_frame != g_d_final || in.hudless != g_d_hudless || hold != g_d_hold || floor != g_d_floor || objects != g_d_objects)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = g_format;
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sd.Texture2D.MipLevels = 1;
			g_device->CreateShaderResourceView(in.final_frame, &sd, h);
			h.ptr += inc;
			g_device->CreateShaderResourceView(in.hudless, &sd, h);
			h.ptr += inc;
			D3D12_SHADER_RESOURCE_VIEW_DESC hd = sd;
			hd.Format = DXGI_FORMAT_R8_UNORM;
			g_device->CreateShaderResourceView(hold, &hd, h); // null: reads 0
			h.ptr += inc;
			g_device->CreateShaderResourceView(floor, &sd, h);
			h.ptr += inc;
			g_device->CreateShaderResourceView(objects, &sd, h);
			h.ptr += inc;
			D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			g_device->CreateUnorderedAccessView(g_ui, nullptr, &ud, h);
			h.ptr += inc;
			g_device->CreateUnorderedAccessView(g_clean, nullptr, &ud, h);
			g_d_final = in.final_frame;
			g_d_hudless = in.hudless;
			g_d_hold = hold;
			g_d_floor = floor;
			g_d_objects = objects;
			g_descriptors = true;
		}

		// coverage from two frames ago (diagnostic; a torn value would only show in the readout)
		{
			ID3D12Resource *const rb = g_readback[(g_readback_index + 1) % 3];
			D3D12_RANGE range = { 0, sizeof(uint32_t) };
			void *data = nullptr;
			if (SUCCEEDED(rb->Map(0, &range, &data)))
			{
				const uint32_t count = *static_cast<const uint32_t *>(data);
				D3D12_RANGE none = { 0, 0 };
				rb->Unmap(0, &none);
				g_coverage = static_cast<float>(count) / static_cast<float>(g_width * g_height);
			}
		}
		list->CopyBufferRegion(g_count, 0, g_zero, 0, sizeof(uint32_t)); // both promoted from COMMON

		D3D12_RESOURCE_BARRIER b[9];
		UINT n = 0;
		if (floor != nullptr)
		{
			barrier(b[n++], floor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			barrier(b[n++], objects, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		if (write_clean && g_clean_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			barrier(b[n++], g_clean, g_clean_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (write_clean)
			g_clean_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if (hold != nullptr)
			barrier(b[n++], hold, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(b[n++], g_count, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barrier(b[n++], in.final_frame, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(b[n++], in.hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		if (g_ui_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			barrier(b[n++], g_ui, g_ui_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		list->ResourceBarrier(n, b);

		ID3D12DescriptorHeap *heaps[] = { g_heap };
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(g_root);
		list->SetPipelineState(g_pso);
		struct { uint32_t w, h; float threshold; uint32_t hold_on, inpaint_on, hold_char, grow, clean_on; } c = { g_width, g_height, 1.5f / 255.0f,
			hold != nullptr ? 1u : 0u, inpaint ? 1u : 0u, in.hold_character ? 1u : 0u, in.exact ? 1u : 2u, write_clean ? 1u : 0u };
		list->SetComputeRoot32BitConstants(0, 8, &c, 0);
		list->SetComputeRootDescriptorTable(1, g_heap->GetGPUDescriptorHandleForHeapStart());
		list->SetComputeRootUnorderedAccessView(2, g_count->GetGPUVirtualAddress());
		list->Dispatch((g_width + 7) / 8, (g_height + 7) / 8, 1);

		n = 0;
		barrier(b[n++], g_count, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		if (hold != nullptr)
			barrier(b[n++], hold, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (floor != nullptr)
		{
			barrier(b[n++], floor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			barrier(b[n++], objects, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		}
		if (write_clean)
		{
			barrier(b[n++], g_clean, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, CLEAN_READ);
			g_clean_state = CLEAN_READ;
		}
		barrier(b[n++], in.final_frame, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		barrier(b[n++], in.hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		barrier(b[n++], g_ui, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, READ);
		g_ui_state = READ;
		list->ResourceBarrier(n, b);
		list->CopyBufferRegion(g_readback[g_readback_index], 0, g_count, 0, sizeof(uint32_t)); // decays to COMMON after the list
		g_readback_index = (g_readback_index + 1) % 3;
		return true;
	}

	ID3D12Resource *ui_texture()
	{
		return g_ui;
	}

	D3D12_RESOURCE_STATES ui_state()
	{
		return g_ui_state;
	}

	ID3D12Resource *clean_texture()
	{
		return g_clean_written ? g_clean : nullptr;
	}

	D3D12_RESOURCE_STATES clean_state()
	{
		return g_clean_state;
	}

	float coverage()
	{
		return g_coverage;
	}

	bool inpainted()
	{
		return g_inpainted;
	}
}
