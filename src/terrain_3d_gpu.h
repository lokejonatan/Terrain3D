// Copyright © 2025 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_GPU_WORKFLOW_H
#define TERRAIN3D_GPU_WORKFLOW_H

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/callable.hpp>
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
	SMOOTH = 3,
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

class Terrain3DGpuWorkflow : public Object {
	GDCLASS(Terrain3DGpuWorkflow, Object);
	CLASS_NAME();

public:
	Terrain3DGpuWorkflow() = default;
	~Terrain3DGpuWorkflow();

	void initialize(Terrain3DData *p_data);
	void shutdown();

	bool is_ready() const { return _ready; }

	bool apply_color_brush(const Terrain3DGpuBrushRequest &p_request);
	bool apply_height_brush(const Terrain3DGpuBrushRequest &p_request);

	// Preview API: enable/disable GPU preview mode and finalize preview (enqueue readbacks)
	void set_preview_mode(bool p_enabled);
	void finalize_preview();
	// Blocking finalize: commit any GPU previews synchronously (safe to call
	// before saving or unloading a scene).
	void finalize_preview_blocking();

	void remove_region(const Vector2i &p_region_loc);
	void process_pending_readbacks(int p_max_brushes = 1);
	bool has_pending_readbacks() const {
		return !_preview_brushes.empty() || !_deferred_finalizations.empty() || !_pending_brushes.empty() || !_inflight_brushes.empty();
	}
	bool has_pending_work() const { return has_pending_readbacks(); }
	void call_when_idle(const Callable &p_callable);
	void flush_gpu_commands(); // Submit and sync GPU commands to trigger async readback callbacks

protected:
	static void _bind_methods();

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
		int64_t id = 0;
		int pending_readbacks = 0;
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
	std::unordered_map<int64_t, PendingBrush> _inflight_brushes;
	int64_t _next_brush_id = 1;
	bool _async_readbacks_supported = true;
	bool _async_test_completed = false;
	bool _async_test_callback_fired = false;
	RID _async_test_texture;
	// Preview mode: when true, GPU dispatches will update visuals but readbacks
	// are deferred until `finalize_preview()` is called. This avoids per-frame
	// CPU work while painting large areas.
	bool _preview_mode = false;
	std::deque<Terrain3DGpuBrushRequest> _preview_brushes;
	// Deferred finalization queue: stores pending brushes created when a preview
	// is finalized. These are moved into `_pending_brushes` incrementally to
	// avoid performing a large number of CPU readbacks in a single frame.
	std::deque<PendingBrush> _deferred_finalizations;
	std::deque<Callable> _idle_callbacks;

	bool _ensure_color_pipeline();
	bool _ensure_height_pipeline();
	void _ensure_fallback_mask();
	RegionGpuState &_get_or_create_region_state(const Vector2i &p_region_loc, Terrain3DRegion *p_region, MapType p_map_type);
	RID _create_texture_from_image(const Ref<Image> &p_image, RenderingDevice::DataFormat p_format,
		BitField<RenderingDevice::TextureUsageBits> p_usage);
	RID _create_mask_texture(const Ref<Image> &p_mask, Vector2i &r_size);
	void _free_region_state(RegionGpuState &p_state);
	bool _readback_color_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state, int64_t p_brush_id);
	bool _readback_height_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state, int64_t p_brush_id);
	bool _dispatch_color_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size);
	bool _dispatch_height_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size);
	void _enqueue_readback_brush(const Terrain3DGpuBrushRequest &p_request, bool p_generate_color_mipmaps);
	void _finalize_brush_readback(const PendingBrush &p_brush);
	void _coalesce_brush_queue(std::deque<PendingBrush> &p_queue);
	bool _upload_region_to_material(MapType p_map_type, const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state);
	void _apply_readback_data(MapType p_map_type, const PackedByteArray &p_data, const Ref<Terrain3DRegion> &p_region, const Vector2i &p_size);
	void _on_async_texture_readback(const RID &p_texture, uint32_t p_layer, const PackedByteArray &p_data,
		Ref<Terrain3DRegion> p_region, Vector2i p_size, int p_map_type, int64_t p_brush_id);
	void _handle_async_readback_complete(int64_t p_brush_id);
	void _free_async_test_texture();

	// Detect if async readback callbacks are actually supported by the rendering backend.
	void _test_async_readback_support();
	void _on_async_test_readback(const RID &p_texture, uint32_t p_layer, const PackedByteArray &p_data);

	// Preview helpers
	// Implementations in cpp: set_preview_mode(), finalize_preview()
	void _notify_idle_if_needed();
};

#endif // TERRAIN3D_GPU_WORKFLOW_H
