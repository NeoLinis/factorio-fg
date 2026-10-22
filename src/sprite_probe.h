// Sprite probe (1.2, step 0): how Factorio sends its sprites to the GPU.
//
// Factorio positions every sprite on the CPU and sends finished screen rectangles (step 3a). Version 1.2 wants to find
// the character's sprites among them (the ones that stay put on screen while the camera moves) to get an exact mask.
// Before building that, this probe records for a few seconds:
//   - the vertex layout of the world's draws (input layouts), vertex / index buffers and how they are written
//     (Map with discard / no-overwrite, UpdateSubresource, copies), draws and indices per frame and per world stage;
//   - on a few pairs of consecutive frames, every sprite of the world decoded to a screen rectangle, matched against
//     the previous frame: the camera shift, and which sprites stayed put ("held": the character), moved with the camera,
//     or moved on their own;
//   - how fast the vertex data can be read back on the CPU.
// Summary to ReShade.log, details to a text file. Nothing changes in the picture. Off unless started.
//
// All functions are called with the add-on's lock held.

#pragma once

#include <reshade_api.hpp>
#include <cstdint>
#include <filesystem>

namespace sprite_probe
{
	using log_fn = void (*)(const char *message);

	struct draw_info
	{
		uint32_t vs = 0, ps = 0;
		bool indexed = false;
		uint32_t count = 0, instances = 1, first = 0, first_instance = 0;
		int32_t vertex_offset = 0;
		bool world = false;     // drawn on the world target
		bool screen = false;    // drawn on another screen-sized target (not the back buffer): shadows?
		uint64_t rt = 0;        // the render target
		uint32_t rt_fmt = 0;    // its DXGI format (other targets only)
		int stage = 0;          // 0 tiles, 1 floor (belts, shadows), 2 objects, 3 air, 4 after the world
		uint32_t rt_w = 0, rt_h = 0;
	};

	void set_log(log_fn log);
	void start(const std::filesystem::path &file, uint32_t frames);
	bool active();
	const char *status();

	void on_init_resource(const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial, reshade::api::resource res);
	void on_destroy_resource(reshade::api::resource res);
	void on_init_pipeline(uint32_t count, const reshade::api::pipeline_subobject *subobjects, reshade::api::pipeline pipe);
	void on_destroy_pipeline(reshade::api::pipeline pipe);
	void on_bind_input_layout(reshade::api::pipeline pipe);
	void on_bind_vertex_buffers(uint32_t first, uint32_t count, const reshade::api::resource *buffers, const uint64_t *offsets, const uint32_t *strides);
	void on_bind_index_buffer(reshade::api::resource buffer, uint64_t offset, uint32_t index_size);
	void on_bind_ps_texture(uint32_t slot, uint64_t texture, uint32_t width, uint32_t height);
	void on_map(reshade::api::resource res, reshade::api::map_access access, void *data);
	void on_unmap(reshade::api::resource res);
	void on_update(reshade::api::resource res, uint64_t offset, const void *data, uint64_t size);
	void on_copy(reshade::api::resource src, uint64_t src_offset, reshade::api::resource dst, uint64_t dst_offset, uint64_t size);
	void on_draw(const draw_info &d);
	void frame_end();
}
