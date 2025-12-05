// Copyright © 2025 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_GPU_WORKFLOW_H
#define TERRAIN3D_GPU_WORKFLOW_H

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <deque>
#include <unordered_map>
#include <vector>

#include "constants.h"
#include "terrain_3d_region.h"

class Terrain3DData;

enum class Terrain3DGpuColorMode {
	LERP_TO_COLOR = 0,
	LERP_TO_WHITE = 1,
};

enum class Terrain3DGpuHeightMode {
	RAISE = 0,
	LOWER = 1,
	SET_TO_HEIGHT = 2,
};

struct Terrain3DGpuBrushRegion {
	Vector2i location;
	Ref<Terrain3DRegion> region;
};

struct Terrain3DGpuBrushRequest {
	MapType map_type = TYPE_MAX;
	Vector3 center_world = V3_ZERO;
	real_t radius_world = 0.f;
	real_t strength = 0.f;
	real_t gamma = 1.f;
	real_t rotation = 0.f;
	Color color = Color(1.f, 1.f, 1.f, 1.f);
	Terrain3DGpuColorMode color_mode = Terrain3DGpuColorMode::LERP_TO_COLOR;
	Terrain3DGpuHeightMode height_mode = Terrain3DGpuHeightMode::RAISE;
	bool height_use_alt = false;
	real_t target_height = 0.f;
	real_t cursor_height = 0.f;
	Ref<Image> mask;
	real_t vertex_spacing = 1.f;
	int region_size = 0;
	AABB edited_area;
	bool update_instancer = false;
	bool update_collision = false;
	std::vector<Terrain3DGpuBrushRegion> regions;
};

class Terrain3DGpuWorkflow {
public:
	Terrain3DGpuWorkflow() = default;
	~Terrain3DGpuWorkflow();

	void initialize(Terrain3DData *p_data);
	void shutdown();

	bool is_ready() const { return _ready; }

	bool apply_color_brush(const Terrain3DGpuBrushRequest &p_request);
	bool apply_height_brush(const Terrain3DGpuBrushRequest &p_request);

	void remove_region(const Vector2i &p_region_loc);
	void process_pending_readbacks(int p_max_brushes = 1);
	bool has_pending_readbacks() const { return !_pending_brushes.empty(); }

private:
	struct RegionGpuState {
		RID color_texture;
		RID height_texture;
		Vector2i size = V2I_ZERO;
	};

	struct PendingBrush {
		MapType map_type = TYPE_MAX;
		std::vector<Terrain3DGpuBrushRegion> regions;
		AABB edited_area;
		bool update_instancer = false;
		bool update_collision = false;
		bool generate_color_mipmaps = false;
	};

	Terrain3DData *_data = nullptr;
	RenderingDevice *_rd = nullptr;
	bool _ready = false;
	RID _color_shader;
	RID _color_pipeline;
	RID _height_shader;
	RID _height_pipeline;
	RID _fallback_mask_texture;
	Vector2i _fallback_mask_size = V2I(1);
	std::unordered_map<Vector2i, RegionGpuState, Vector2iHash> _region_gpu_states;
	std::deque<PendingBrush> _pending_brushes;

	bool _ensure_color_pipeline();
	bool _ensure_height_pipeline();
	void _ensure_fallback_mask();
	RegionGpuState &_get_or_create_region_state(const Vector2i &p_region_loc, Terrain3DRegion *p_region, MapType p_map_type);
	RID _create_texture_from_image(const Ref<Image> &p_image, RenderingDevice::DataFormat p_format,
		BitField<RenderingDevice::TextureUsageBits> p_usage);
	RID _create_mask_texture(const Ref<Image> &p_mask, Vector2i &r_size);
	void _free_region_state(RegionGpuState &p_state);
	void _readback_color_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state);
	void _readback_height_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state);
	bool _dispatch_color_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size);
	bool _dispatch_height_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size);
	void _enqueue_readback_brush(const Terrain3DGpuBrushRequest &p_request, bool p_generate_color_mipmaps);
	void _finalize_brush_readback(const PendingBrush &p_brush);
};

#endif // TERRAIN3D_GPU_WORKFLOW_H
