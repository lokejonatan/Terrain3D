// Copyright © 2025 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>

#include "logger.h"
#include "terrain_3d_data.h"

#if defined(GDEXTENSION) && !defined(GODOT_MODULE)
using namespace godot;
#endif

namespace {

CLASS_NAME_STATIC("Terrain3DGpuWorkflow");

static bool _image_has_pixels(const Ref<Image> &p_image, const String &p_label) {
	if (p_image.is_null()) {
		LOG(WARN, "Image for ", p_label, " is null");
		return false;
	}
	Vector2i size = p_image->get_size();
	if (size.x <= 0 || size.y <= 0) {
		LOG(WARN, "Image for ", p_label, " has invalid size ", size);
		return false;
	}
	if (p_image->is_empty()) {
		LOG(WARN, "Image for ", p_label, " has no pixel data");
		return false;
	}
	return true;
}

struct ColorBrushPushConstant {
	int32_t target_origin_x = 0;
	int32_t target_origin_y = 0;
	int32_t target_size_x = 0;
	int32_t target_size_y = 0;
	int32_t texture_width = 0;
	int32_t texture_height = 0;
	int32_t mask_width = 0;
	int32_t mask_height = 0;
	float brush_center_x = 0.f;
	float brush_center_y = 0.f;
	float radius = 0.f;
	float inv_radius = 0.f;
	float strength = 0.f;
	float gamma = 1.f;
	float rotation_cos = 1.f;
	float rotation_sin = 0.f;
	float color_r = 1.f;
	float color_g = 1.f;
	float color_b = 1.f;
	float color_a = 1.f;
	int32_t color_mode = 0;
	float padding[3] = { 0.f, 0.f, 0.f };
};

struct HeightBrushPushConstant {
	int32_t target_origin_x = 0;
	int32_t target_origin_y = 0;
	int32_t target_size_x = 0;
	int32_t target_size_y = 0;
	int32_t texture_width = 0;
	int32_t texture_height = 0;
	int32_t mask_width = 0;
	int32_t mask_height = 0;
	float brush_center_x = 0.f;
	float brush_center_y = 0.f;
	float radius = 0.f;
	float inv_radius = 0.f;
	float strength = 0.f;
	float gamma = 1.f;
	float rotation_cos = 1.f;
	float rotation_sin = 0.f;
	float target_height = 0.f;
	float cursor_height = 0.f;
	int32_t height_mode = 0;
	int32_t use_alt = 0;
	float padding[4] = { 0.f, 0.f, 0.f, 0.f };
};

} // namespace

void Terrain3DGpuWorkflow::_bind_methods() {
}

Terrain3DGpuWorkflow::~Terrain3DGpuWorkflow() {
	shutdown();
}

void Terrain3DGpuWorkflow::initialize(Terrain3DData *p_data) {
	_data = p_data;
	_rd = RS->get_rendering_device();
	if (!_rd) {
		LOG(ERROR, "RenderingDevice unavailable. GPU workflow disabled");
		_ready = false;
		return;
	}
	if (!_ensure_color_pipeline()) {
		_ready = false;
		return;
	}
	_ensure_fallback_mask();
	_ready = _color_pipeline.is_valid();
	// Test whether async readback callbacks actually work on this backend.
	if (_ready) {
		_test_async_readback_support();
	}
}

void Terrain3DGpuWorkflow::shutdown() {
	for (auto &entry : _region_gpu_states) {
		_free_region_state(entry.second);
	}
	_region_gpu_states.clear();
	_pending_brushes.clear();
	_inflight_brushes.clear();
	if (_rd) {
		if (_color_pipeline.is_valid()) {
			_rd->free_rid(_color_pipeline);
		}
		if (_color_shader.is_valid()) {
			_rd->free_rid(_color_shader);
		}
		if (_height_pipeline.is_valid()) {
			_rd->free_rid(_height_pipeline);
		}
		if (_height_shader.is_valid()) {
			_rd->free_rid(_height_shader);
		}
		if (_fallback_mask_texture.is_valid()) {
			_rd->free_rid(_fallback_mask_texture);
		}
	}
	_color_pipeline = RID();
	_color_shader = RID();
	_height_pipeline = RID();
	_height_shader = RID();
	_fallback_mask_texture = RID();
	_rd = nullptr;
	_ready = false;
	_next_brush_id = 1;
}

bool Terrain3DGpuWorkflow::apply_color_brush(const Terrain3DGpuBrushRequest &p_request) {
	if (!_ready || !_rd) {
		return false;
	}
	if (p_request.map_type != TYPE_COLOR) {
		return false;
	}
	if (!_ensure_color_pipeline()) {
		return false;
	}
	Vector2i mask_size = _fallback_mask_size;
	RID mask_texture = _fallback_mask_texture;
	if (p_request.mask.is_valid()) {
		mask_texture = _create_mask_texture(p_request.mask, mask_size);
		if (!mask_texture.is_valid()) {
			mask_texture = _fallback_mask_texture;
			mask_size = _fallback_mask_size;
		}
	}

	int dispatch_count = 0;
	std::vector<Terrain3DGpuBrushRegion> processed_regions;
	processed_regions.reserve(p_request.regions.size());
	for (const Terrain3DGpuBrushRegion &region_info : p_request.regions) {
		Terrain3DRegion *region_ptr = region_info.region.ptr();
		if (!region_ptr) {
			continue;
		}
		RegionGpuState &state = _get_or_create_region_state(region_info.location, region_ptr, p_request.map_type);
		if (!state.color_texture.is_valid()) {
			continue;
		}
		bool dispatched = _dispatch_color_brush(p_request, region_info, state, mask_texture, mask_size);
		if (dispatched) {
			processed_regions.push_back(region_info);
			dispatch_count++;

			// If we're in GPU preview mode, immediately blit the GPU texture
			// to the material so the user sees realtime feedback without
			// performing CPU readbacks.
			if (_preview_mode) {
				bool synced = _upload_region_to_material(TYPE_COLOR, region_info, state);
				if (synced && _data) {
					_data->_notify_gpu_maps_synced(TYPE_COLOR);
				}
			}
		}
	}

	if (mask_texture.is_valid() && mask_texture != _fallback_mask_texture) {
		_rd->free_rid(mask_texture);
	}
	if (dispatch_count == 0) {
		LOG(INFO, "GPU workflow skipped request: no eligible regions or target bounds");
		return false;
	}
	Terrain3DGpuBrushRequest queued_request = p_request;
	queued_request.regions = std::move(processed_regions);
	if (_preview_mode) {
		// Store brush for later final readback but avoid scheduling readbacks
		// immediately — CPU work is deferred until stroke end.
		_preview_brushes.push_back(std::move(queued_request));
	} else {
		_enqueue_readback_brush(queued_request, true);
	}
	LOG(INFO, "GPU workflow dispatched ", dispatch_count, " region(s)");
	return true;
}

bool Terrain3DGpuWorkflow::apply_height_brush(const Terrain3DGpuBrushRequest &p_request) {
	if (!_ready || !_rd) {
		return false;
	}
	if (p_request.map_type != TYPE_HEIGHT) {
		return false;
	}
	if (!_ensure_height_pipeline()) {
		return false;
	}
	Vector2i mask_size = _fallback_mask_size;
	RID mask_texture = _fallback_mask_texture;
	if (p_request.mask.is_valid()) {
		mask_texture = _create_mask_texture(p_request.mask, mask_size);
		if (!mask_texture.is_valid()) {
			mask_texture = _fallback_mask_texture;
			mask_size = _fallback_mask_size;
		}
	}
	int dispatch_count = 0;
	std::vector<Terrain3DGpuBrushRegion> processed_regions;
	processed_regions.reserve(p_request.regions.size());
	for (const Terrain3DGpuBrushRegion &region_info : p_request.regions) {
		Terrain3DRegion *region_ptr = region_info.region.ptr();
		if (!region_ptr) {
			continue;
		}
		RegionGpuState &state = _get_or_create_region_state(region_info.location, region_ptr, p_request.map_type);
		if (!state.height_texture.is_valid()) {
			continue;
		}
		bool dispatched = _dispatch_height_brush(p_request, region_info, state, mask_texture, mask_size);
		if (dispatched) {
			processed_regions.push_back(region_info);
			dispatch_count++;

			// Provide realtime GPU-only preview by blitting the updated texture
			// to the material. This shows the user the brush result without
			// performing expensive CPU readbacks during the stroke.
			if (_preview_mode) {
				bool synced = _upload_region_to_material(TYPE_HEIGHT, region_info, state);
				if (synced && _data) {
					_data->_notify_gpu_maps_synced(TYPE_HEIGHT);
				}
			}
		}
	}
	if (mask_texture.is_valid() && mask_texture != _fallback_mask_texture) {
		_rd->free_rid(mask_texture);
	}
	if (dispatch_count == 0) {
		LOG(INFO, "GPU workflow skipped height request: no eligible regions or target bounds");
		return false;
	}
	Terrain3DGpuBrushRequest queued_request = p_request;
	queued_request.regions = std::move(processed_regions);
	if (_preview_mode) {
		_preview_brushes.push_back(std::move(queued_request));
	} else {
		_enqueue_readback_brush(queued_request, false);
	}
	LOG(INFO, "GPU workflow dispatched height brush to ", dispatch_count, " region(s)");
	return true;
}

void Terrain3DGpuWorkflow::remove_region(const Vector2i &p_region_loc) {
	auto found = _region_gpu_states.find(p_region_loc);
	if (found != _region_gpu_states.end()) {
		_free_region_state(found->second);
		_region_gpu_states.erase(found);
	}
}

bool Terrain3DGpuWorkflow::_ensure_color_pipeline() {
	if (_color_pipeline.is_valid()) {
		return true;
	}
	if (!_rd) {
		return false;
	}
	String shader_code = String(
#include "shaders/terrain_gpu_brush.glsl"
	);
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, shader_code);
	Ref<RDShaderSPIRV> spirv = _rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		LOG(ERROR, "Failed to compile GPU color brush shader");
		return false;
	}
	_color_shader = _rd->shader_create_from_spirv(spirv);
	if (!_color_shader.is_valid()) {
		LOG(ERROR, "Failed to create GPU color brush shader RID");
		return false;
	}
	_color_pipeline = _rd->compute_pipeline_create(_color_shader);
	if (!_color_pipeline.is_valid()) {
		LOG(ERROR, "Failed to create GPU color brush pipeline");
		_rd->free_rid(_color_shader);
		_color_shader = RID();
		return false;
	}
	return true;
}

bool Terrain3DGpuWorkflow::_ensure_height_pipeline() {
	if (_height_pipeline.is_valid()) {
		return true;
	}
	if (!_rd) {
		return false;
	}
	String shader_code = String(
#include "shaders/terrain_gpu_height_brush.glsl"
	);
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, shader_code);
	Ref<RDShaderSPIRV> spirv = _rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		LOG(ERROR, "Failed to compile GPU height brush shader");
		return false;
	}
	_height_shader = _rd->shader_create_from_spirv(spirv);
	if (!_height_shader.is_valid()) {
		LOG(ERROR, "Failed to create GPU height brush shader RID");
		return false;
	}
	_height_pipeline = _rd->compute_pipeline_create(_height_shader);
	if (!_height_pipeline.is_valid()) {
		LOG(ERROR, "Failed to create GPU height brush pipeline");
		_rd->free_rid(_height_shader);
		_height_shader = RID();
		return false;
	}
	return true;
}

void Terrain3DGpuWorkflow::_ensure_fallback_mask() {
	if (_fallback_mask_texture.is_valid() || !_rd) {
		return;
	}
	Ref<Image> img = Image::create(1, 1, false, Image::FORMAT_RF);
	if (img.is_null()) {
		LOG(ERROR, "Failed to create fallback mask image");
		return;
	}
	img->set_pixel(0, 0, Color(1.f, 1.f, 1.f, 1.f));
	_fallback_mask_size = img->get_size();
	_fallback_mask_texture = _create_texture_from_image(img, RenderingDevice::DATA_FORMAT_R32_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	if (!_fallback_mask_texture.is_valid()) {
		LOG(ERROR, "Failed to create fallback mask texture");
		_fallback_mask_size = V2I(1);
	}
}

Terrain3DGpuWorkflow::RegionGpuState &Terrain3DGpuWorkflow::_get_or_create_region_state(const Vector2i &p_region_loc, Terrain3DRegion *p_region, MapType p_map_type) {
	auto found = _region_gpu_states.find(p_region_loc);
	if (found == _region_gpu_states.end()) {
		found = _region_gpu_states.insert({ p_region_loc, RegionGpuState{} }).first;
	}
	RegionGpuState &state = found->second;
	if (!p_region) {
		return state;
	}
	if (p_region->get_region_size() <= 0) {
		LOG(WARN, "Region ", p_region_loc, " has invalid region size; skipping GPU upload");
		return state;
	}
	auto ensure_size = [&](const Vector2i &p_candidate) {
		if (state.size == V2I_ZERO) {
			state.size = p_candidate;
			return true;
		}
		if (state.size != p_candidate) {
			LOG(WARN, "Region ", p_region_loc, " map sizes mismatch between uploads (existing=", state.size, ", new=", p_candidate, ")");
			return false;
		}
		return true;
	};
	auto upload_color = [&]() {
		if (state.color_texture.is_valid()) {
			return;
		}
		Ref<Image> color_map = p_region->get_color_map();
		if (color_map.is_null()) {
			p_region->sanitize_maps();
			color_map = p_region->get_color_map();
		}
		if (color_map.is_null()) {
			return;
		}
		Ref<Image> upload_image = color_map;
		bool needs_copy = color_map->has_mipmaps() || color_map->get_format() != Image::FORMAT_RGBA8;
		if (needs_copy) {
			upload_image = color_map->duplicate();
			if (upload_image.is_null()) {
				LOG(ERROR, "Failed to duplicate color map for region ", p_region_loc);
				return;
			}
			if (upload_image->get_format() != Image::FORMAT_RGBA8) {
				upload_image->convert(Image::FORMAT_RGBA8);
			}
		}
		if (upload_image->has_mipmaps()) {
			upload_image->clear_mipmaps();
		}
		if (!_image_has_pixels(upload_image, "region color map")) {
			return;
		}
		Vector2i map_size = upload_image->get_size();
		if (!ensure_size(map_size)) {
			return;
		}
		state.color_texture = _create_texture_from_image(upload_image, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	};
	auto upload_height = [&]() {
		if (state.height_texture.is_valid()) {
			return;
		}
		Ref<Image> height_map = p_region->get_height_map();
		if (height_map.is_null()) {
			p_region->sanitize_maps();
			height_map = p_region->get_height_map();
		}
		if (height_map.is_null()) {
			return;
		}
		Ref<Image> upload_image = height_map;
		bool needs_copy = height_map->has_mipmaps() || height_map->get_format() != Image::FORMAT_RF;
		if (needs_copy) {
			upload_image = height_map->duplicate();
			if (upload_image.is_null()) {
				LOG(ERROR, "Failed to duplicate height map for region ", p_region_loc);
				return;
			}
			if (upload_image->get_format() != Image::FORMAT_RF) {
				upload_image->convert(Image::FORMAT_RF);
			}
		}
		if (upload_image->has_mipmaps()) {
			upload_image->clear_mipmaps();
		}
		if (!_image_has_pixels(upload_image, "region height map")) {
			return;
		}
		Vector2i map_size = upload_image->get_size();
		if (!ensure_size(map_size)) {
			return;
		}
		state.height_texture = _create_texture_from_image(upload_image, RenderingDevice::DATA_FORMAT_R32_SFLOAT,
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	};
	switch (p_map_type) {
		case TYPE_COLOR:
			upload_color();
			break;
		case TYPE_HEIGHT:
			upload_height();
			break;
		default:
			break;
	}
	return state;
}

RID Terrain3DGpuWorkflow::_create_texture_from_image(const Ref<Image> &p_image, RenderingDevice::DataFormat p_format,
		BitField<RenderingDevice::TextureUsageBits> p_usage) {
	if (!_rd || !_image_has_pixels(p_image, "texture upload")) {
		return RID();
	}
	Ref<RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_width(p_image->get_width());
	fmt->set_height(p_image->get_height());
	fmt->set_depth(1);
	fmt->set_array_layers(1);
	fmt->set_mipmaps(1);
	fmt->set_format(p_format);
	fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	fmt->set_usage_bits(p_usage);

	Ref<RDTextureView> view;
	view.instantiate();

	TypedArray<PackedByteArray> data;
	data.push_back(p_image->get_data());
	RID texture = _rd->texture_create(fmt, view, data);
	if (!texture.is_valid()) {
		LOG(ERROR, "RenderingDevice failed to create texture for GPU workflow upload");
	}
	return texture;
}

RID Terrain3DGpuWorkflow::_create_mask_texture(const Ref<Image> &p_mask, Vector2i &r_size) {
	if (p_mask.is_null()) {
		r_size = _fallback_mask_size;
		return _fallback_mask_texture;
	}
	Ref<Image> mask = p_mask;
	if (mask->get_format() != Image::FORMAT_RF) {
		mask = mask->duplicate();
		if (mask.is_null()) {
			LOG(ERROR, "Failed to duplicate brush mask image");
			r_size = _fallback_mask_size;
			return _fallback_mask_texture;
		}
		mask->convert(Image::FORMAT_RF);
	}
	if (!_image_has_pixels(mask, "brush mask")) {
		r_size = _fallback_mask_size;
		return _fallback_mask_texture;
	}
	r_size = mask->get_size();
	return _create_texture_from_image(mask, RenderingDevice::DATA_FORMAT_R32_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
}

void Terrain3DGpuWorkflow::_free_region_state(RegionGpuState &p_state) {
	if (_rd && p_state.color_texture.is_valid()) {
		_rd->free_rid(p_state.color_texture);
	}
	if (_rd && p_state.height_texture.is_valid()) {
		_rd->free_rid(p_state.height_texture);
	}
	p_state.color_texture = RID();
	p_state.height_texture = RID();
	p_state.size = V2I_ZERO;
}

bool Terrain3DGpuWorkflow::_readback_color_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state, int64_t p_brush_id) {
	if (!_rd || !p_state.color_texture.is_valid() || p_region_info.region.is_null()) {
		return false;
	}
	LOG(INFO, "Color readback request (async=", _async_readbacks_supported, ") brush=", p_brush_id,
		" tex=", p_state.color_texture.get_id());
	if (_async_readbacks_supported) {
		Callable callback = callable_mp(this, &Terrain3DGpuWorkflow::_on_async_texture_readback);
		callback = callback.bind(p_region_info.region, p_state.size, int(TYPE_COLOR), p_brush_id);
		Error err = _rd->texture_get_data_async(p_state.color_texture, 0, callback);
		if (err == OK) {
			LOG(INFO, "Color async readback scheduled for brush=", p_brush_id);
			return true;
		}
		_async_readbacks_supported = false;
		LOG(WARN, "RenderingDevice::texture_get_data_async unavailable (err=", err,
			"), using synchronous GPU readbacks instead");
	}
	PackedByteArray data = _rd->texture_get_data(p_state.color_texture, 0);
	if (data.is_empty()) {
		return false;
	}
	LOG(INFO, "Color readback using synchronous path brush=", p_brush_id, " bytes=", data.size());
	_apply_readback_data(TYPE_COLOR, data, p_region_info.region, p_state.size);
	return false;
}

void Terrain3DGpuWorkflow::_apply_readback_data(MapType p_map_type, const PackedByteArray &p_data, const Ref<Terrain3DRegion> &p_region, const Vector2i &p_size) {
	if (p_region.is_null() || p_data.is_empty() || p_size.x <= 0 || p_size.y <= 0) {
		return;
	}
	switch (p_map_type) {
		case TYPE_COLOR: {
			Ref<Image> img = p_region->get_color_map();
			if (img.is_null()) {
				img.instantiate();
				img->create(p_size.x, p_size.y, false, Image::FORMAT_RGBA8);
				p_region->set_color_map(img);
			}
			img->set_data(p_size.x, p_size.y, false, Image::FORMAT_RGBA8, p_data);
			break;
		}
		case TYPE_HEIGHT: {
			Ref<Image> img = p_region->get_height_map();
			if (img.is_null()) {
				img.instantiate();
				img->create(p_size.x, p_size.y, false, Image::FORMAT_RF);
				p_region->set_height_map(img);
			}
			img->set_data(p_size.x, p_size.y, false, Image::FORMAT_RF, p_data);
			p_region->calc_height_range();
			if (_data) {
				_data->update_master_heights(p_region->get_height_range());
			}
			break;
		}
		default:
			return;
	}
	p_region->set_modified(true);
	p_region->set_edited(true);
}

void Terrain3DGpuWorkflow::_handle_async_readback_complete(int64_t p_brush_id) {
	auto brush_it = _inflight_brushes.find(p_brush_id);
	if (brush_it == _inflight_brushes.end()) {
		return;
	}
	PendingBrush &brush = brush_it->second;
	if (brush.pending_readbacks > 0) {
		brush.pending_readbacks--;
	}
	if (brush.pending_readbacks == 0) {
		_finalize_brush_readback(brush);
		_inflight_brushes.erase(brush_it);
	}
}

void Terrain3DGpuWorkflow::_on_async_texture_readback(const RID &p_texture, uint32_t p_layer, const PackedByteArray &p_data,
		Ref<Terrain3DRegion> p_region, Vector2i p_size, int p_map_type, int64_t p_brush_id) {
	(void)p_texture;
	(void)p_layer;
	_apply_readback_data(static_cast<MapType>(p_map_type), p_data, p_region, p_size);
	_handle_async_readback_complete(p_brush_id);
	LOG(INFO, "Async callback fired, map=", p_map_type, " brush=", p_brush_id);
}

bool Terrain3DGpuWorkflow::_readback_height_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state, int64_t p_brush_id) {
	if (!_rd || !p_state.height_texture.is_valid() || p_region_info.region.is_null()) {
		return false;
	}
	LOG(INFO, "Height readback request (async=", _async_readbacks_supported, ") brush=", p_brush_id,
		" tex=", p_state.height_texture.get_id());
	if (_async_readbacks_supported) {
		Callable callback = callable_mp(this, &Terrain3DGpuWorkflow::_on_async_texture_readback);
		callback = callback.bind(p_region_info.region, p_state.size, int(TYPE_HEIGHT), p_brush_id);
		Error err = _rd->texture_get_data_async(p_state.height_texture, 0, callback);
		if (err == OK) {
			LOG(INFO, "Height async readback scheduled for brush=", p_brush_id);
			return true;
		}
		_async_readbacks_supported = false;
		LOG(WARN, "RenderingDevice::texture_get_data_async unavailable (err=", err,
			"), using synchronous GPU readbacks instead");
	}
	PackedByteArray data = _rd->texture_get_data(p_state.height_texture, 0);
	if (data.is_empty()) {
		return false;
	}
	LOG(INFO, "Height readback using synchronous path brush=", p_brush_id, " bytes=", data.size());
	_apply_readback_data(TYPE_HEIGHT, data, p_region_info.region, p_state.size);
	return false;
}

void Terrain3DGpuWorkflow::_test_async_readback_support() {
	if (!_rd || _async_test_completed) {
		return;
	}
	_async_test_completed = true;
	_async_test_callback_fired = false;
	
	// Create a small 1x1 test texture with some data.
	Ref<Image> test_img;
	test_img.instantiate();
	test_img->create(1, 1, false, Image::FORMAT_RGBA8);
	test_img->set_pixel(0, 0, Color(1.f, 0.f, 0.f, 1.f));
	
	RID test_texture = _create_texture_from_image(test_img, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
		RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	
	if (!test_texture.is_valid()) {
		LOG(WARN, "Failed to create test texture for async detection");
		_async_readbacks_supported = false;
		return;
	}
	
	// Try async readback with a callback.
	Callable callback = callable_mp(this, &Terrain3DGpuWorkflow::_on_async_test_readback);
	Error err = _rd->texture_get_data_async(test_texture, 0, callback);
	
	_rd->free_rid(test_texture);
	
	if (err != OK) {
		LOG(WARN, "Async readback not supported (err=", err, "); using synchronous readbacks only");
		_async_readbacks_supported = false;
		return;
	}
	
	// Give the callback a frame to fire.
	// In most cases it will fire immediately or very soon.
	// If not, we'll detect it next frame and fall back to sync.
	LOG(INFO, "Testing async readback support...");
}

void Terrain3DGpuWorkflow::_on_async_test_readback(const RID &p_texture, uint32_t p_layer, const PackedByteArray &p_data) {
	(void)p_texture;
	(void)p_layer;
	(void)p_data;
	_async_test_callback_fired = true;
	LOG(INFO, "Async readback test callback fired - async is fully supported!");
}

bool Terrain3DGpuWorkflow::_dispatch_color_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size) {
	if (!_rd || !_color_pipeline.is_valid()) {
		return false;
	}
	Vector2 center_descaled = Vector2(p_request.center_world.x, p_request.center_world.z) / p_request.vertex_spacing;
	Vector2 region_origin = Vector2(p_region_info.location) * real_t(p_request.region_size);
	Vector2 brush_center = center_descaled - region_origin;
	float radius_pixels = p_request.radius_world / p_request.vertex_spacing;
	Rect2i region_bounds(Vector2i(), p_state.size);
	Rect2i brush_bounds(
		Vector2i(int(Math::floor(brush_center.x - radius_pixels - 1.f)), int(Math::floor(brush_center.y - radius_pixels - 1.f))),
		Vector2i(int(Math::ceil(radius_pixels * 2.f) + 2), int(Math::ceil(radius_pixels * 2.f) + 2)));
	Rect2i target_rect = brush_bounds.intersection(region_bounds);
	if (target_rect.size.x <= 0 || target_rect.size.y <= 0) {
		return false;
	}

	Ref<RDUniform> map_uniform;
	map_uniform.instantiate();
	map_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	map_uniform->set_binding(0);
	map_uniform->add_id(p_state.color_texture);

	Ref<RDUniform> mask_uniform;
	mask_uniform.instantiate();
	mask_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	mask_uniform->set_binding(1);
	mask_uniform->add_id(p_mask_texture);

	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(map_uniform);
	uniforms.push_back(mask_uniform);
	RID uniform_set = _rd->uniform_set_create(uniforms, _color_shader, 0);
	if (!uniform_set.is_valid()) {
		LOG(ERROR, "Failed to create uniform set for GPU brush");
		return false;
	}

	ColorBrushPushConstant push;
	push.target_origin_x = target_rect.position.x;
	push.target_origin_y = target_rect.position.y;
	push.target_size_x = target_rect.size.x;
	push.target_size_y = target_rect.size.y;
	push.texture_width = p_state.size.x;
	push.texture_height = p_state.size.y;
	push.mask_width = p_mask_size.x;
	push.mask_height = p_mask_size.y;
	push.brush_center_x = brush_center.x;
	push.brush_center_y = brush_center.y;
	push.radius = Math::max(radius_pixels, 0.001f);
	push.inv_radius = 1.f / push.radius;
	push.strength = p_request.strength;
	push.gamma = Math::max(p_request.gamma, 0.0001f);
	push.rotation_cos = Math::cos(p_request.rotation);
	push.rotation_sin = Math::sin(p_request.rotation);
	push.color_r = p_request.color.r;
	push.color_g = p_request.color.g;
	push.color_b = p_request.color.b;
	push.color_a = p_request.color.a;
	push.color_mode = static_cast<int32_t>(p_request.color_mode);

	PackedByteArray push_bytes;
	push_bytes.resize(sizeof(ColorBrushPushConstant));
	std::memcpy(push_bytes.ptrw(), &push, sizeof(ColorBrushPushConstant));

	int64_t compute_list = _rd->compute_list_begin();
	_rd->compute_list_bind_compute_pipeline(compute_list, _color_pipeline);
	_rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	_rd->compute_list_set_push_constant(compute_list, push_bytes, push_bytes.size());
	uint32_t group_x = (target_rect.size.x + 7) / 8;
	uint32_t group_y = (target_rect.size.y + 7) / 8;
	_rd->compute_list_dispatch(compute_list, group_x, group_y, 1);
	_rd->compute_list_end();
	_rd->free_rid(uniform_set);
	return true;
}

bool Terrain3DGpuWorkflow::_dispatch_height_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size) {
	if (!_rd || !_height_pipeline.is_valid()) {
		return false;
	}
	Vector2 center_descaled = Vector2(p_request.center_world.x, p_request.center_world.z) / p_request.vertex_spacing;
	Vector2 region_origin = Vector2(p_region_info.location) * real_t(p_request.region_size);
	Vector2 brush_center = center_descaled - region_origin;
	float radius_pixels = p_request.radius_world / p_request.vertex_spacing;
	Rect2i region_bounds(Vector2i(), p_state.size);
	Rect2i brush_bounds(
		Vector2i(int(Math::floor(brush_center.x - radius_pixels - 1.f)), int(Math::floor(brush_center.y - radius_pixels - 1.f))),
		Vector2i(int(Math::ceil(radius_pixels * 2.f) + 2), int(Math::ceil(radius_pixels * 2.f) + 2)));
	Rect2i target_rect = brush_bounds.intersection(region_bounds);
	if (target_rect.size.x <= 0 || target_rect.size.y <= 0) {
		return false;
	}

	Ref<RDUniform> map_uniform;
	map_uniform.instantiate();
	map_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	map_uniform->set_binding(0);
	map_uniform->add_id(p_state.height_texture);

	Ref<RDUniform> mask_uniform;
	mask_uniform.instantiate();
	mask_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	mask_uniform->set_binding(1);
	mask_uniform->add_id(p_mask_texture);

	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(map_uniform);
	uniforms.push_back(mask_uniform);
	RID uniform_set = _rd->uniform_set_create(uniforms, _height_shader, 0);
	if (!uniform_set.is_valid()) {
		LOG(ERROR, "Failed to create uniform set for GPU height brush");
		return false;
	}

	HeightBrushPushConstant push;
	push.target_origin_x = target_rect.position.x;
	push.target_origin_y = target_rect.position.y;
	push.target_size_x = target_rect.size.x;
	push.target_size_y = target_rect.size.y;
	push.texture_width = p_state.size.x;
	push.texture_height = p_state.size.y;
	push.mask_width = p_mask_size.x;
	push.mask_height = p_mask_size.y;
	push.brush_center_x = brush_center.x;
	push.brush_center_y = brush_center.y;
	push.radius = Math::max(radius_pixels, 0.001f);
	push.inv_radius = 1.f / push.radius;
	push.strength = p_request.strength;
	push.gamma = Math::max(p_request.gamma, 0.0001f);
	push.rotation_cos = Math::cos(p_request.rotation);
	push.rotation_sin = Math::sin(p_request.rotation);
	push.target_height = p_request.target_height;
	push.cursor_height = p_request.cursor_height;
	push.height_mode = static_cast<int32_t>(p_request.height_mode);
	push.use_alt = p_request.height_use_alt ? 1 : 0;

	PackedByteArray push_bytes;
	push_bytes.resize(sizeof(HeightBrushPushConstant));
	std::memcpy(push_bytes.ptrw(), &push, sizeof(HeightBrushPushConstant));

	int64_t compute_list = _rd->compute_list_begin();
	_rd->compute_list_bind_compute_pipeline(compute_list, _height_pipeline);
	_rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	_rd->compute_list_set_push_constant(compute_list, push_bytes, push_bytes.size());
	uint32_t group_x = (target_rect.size.x + 7) / 8;
	uint32_t group_y = (target_rect.size.y + 7) / 8;
	_rd->compute_list_dispatch(compute_list, group_x, group_y, 1);
	_rd->compute_list_end();
	_rd->free_rid(uniform_set);
	return true;
}

void Terrain3DGpuWorkflow::process_pending_readbacks(int p_max_brushes) {
	if (p_max_brushes == 0) {
		return;
	}
	// Determine how many brushes we are allowed to move/process this frame.
	// A negative value means "process all", but that can freeze the editor
	// when finalizing a very large stroke. Cap to a small number in that case.
	int allowed = p_max_brushes;
	if (allowed < 0) {
		allowed = 2; // safe default cap when caller requested "all"
	}

	// Move up to `allowed` brushes from deferred finalizations into the
	// pending queue. This stages the work so we don't enqueue everything at once.
	int moved = 0;
	while (moved < allowed && !_deferred_finalizations.empty()) {
		PendingBrush b = std::move(_deferred_finalizations.front());
		_deferred_finalizations.pop_front();
		_pending_brushes.push_back(std::move(b));
		moved++;
	}

	int processed = 0;
	while (processed < allowed && !_pending_brushes.empty()) {
		PendingBrush brush = std::move(_pending_brushes.front());
		_pending_brushes.pop_front();
		brush.id = _next_brush_id++;
		brush.pending_readbacks = 0;
		for (const Terrain3DGpuBrushRegion &region_info : brush.regions) {
			auto state_it = _region_gpu_states.find(region_info.location);
			if (state_it == _region_gpu_states.end()) {
				LOG(WARN, "No GPU state for region ", region_info.location,
					" map=", brush.map_type, " (region ptr=", region_info.region.ptr(), ")");
				continue;
			}
			bool scheduled = false;
			switch (brush.map_type) {
				case TYPE_COLOR:
					scheduled = _readback_color_region(region_info, state_it->second, brush.id);
					break;
				case TYPE_HEIGHT:
					scheduled = _readback_height_region(region_info, state_it->second, brush.id);
					break;
				default:
					break;
			}
			if (scheduled) {
				brush.pending_readbacks++;
			}
		}
		if (brush.pending_readbacks == 0) {
			_finalize_brush_readback(brush);
		} else {
			_inflight_brushes.emplace(brush.id, std::move(brush));
		}
		processed++;
	}
}

void Terrain3DGpuWorkflow::flush_gpu_commands() {
	if (!_rd) {
		return;
	}
	// Only LocalRenderingDevice supports explicit submit/sync. Calling these on the
	// main device prints errors like "Only local devices can submit and sync.".
	String cls = _rd->get_class();
	if (cls == String("LocalRenderingDevice")) {
		_rd->submit();
		_rd->sync();
		LOG(DEBUG, "GPU commands flushed (submit + sync) via LocalRenderingDevice");
	} else {
		// For the main rendering device, the engine drives Submit/Sync each frame.
		// We skip explicit submit/sync to avoid error logs; async callbacks will
		// be invoked by the engine when the frame completes.
		LOG(DEBUG, "Skipping submit/sync on non-local RenderingDevice (class=", cls, ")");
	}
}

void Terrain3DGpuWorkflow::finalize_preview() {
	if (_preview_brushes.empty()) {
		return;
	}
	// Convert queued preview requests into deferred finalization brushes and
	// move them into `_deferred_finalizations`. They will be processed in
	// small batches by `process_pending_readbacks` to avoid blocking the UI.
	while (!_preview_brushes.empty()) {
		Terrain3DGpuBrushRequest req = std::move(_preview_brushes.front());
		_preview_brushes.pop_front();
		PendingBrush brush;
		brush.map_type = req.map_type;
		brush.regions = std::move(req.regions);
		brush.edited_area = req.edited_area;
		brush.update_instancer = req.update_instancer;
		brush.update_collision = req.update_collision;
		brush.generate_color_mipmaps = (req.map_type == TYPE_COLOR);
		// id and pending_readbacks set later when processing
		_deferred_finalizations.push_back(std::move(brush));
	}

	// Kick the readback processing loop (it will process a small batch).
	if (_data) {
		_data->request_gpu_readback_flush();
	}
}

void Terrain3DGpuWorkflow::set_preview_mode(bool p_enabled) {
	_preview_mode = p_enabled;
}

void Terrain3DGpuWorkflow::_enqueue_readback_brush(const Terrain3DGpuBrushRequest &p_request, bool p_generate_color_mipmaps) {
	if (p_request.regions.empty()) {
		return;
	}
	PendingBrush brush;
	brush.map_type = p_request.map_type;
	brush.regions = p_request.regions;
	brush.edited_area = p_request.edited_area;
	brush.update_instancer = p_request.update_instancer;
	brush.update_collision = p_request.update_collision;
	brush.generate_color_mipmaps = p_generate_color_mipmaps;
	_pending_brushes.push_back(std::move(brush));
	if (_data) {
		_data->request_gpu_readback_flush();
	}
}

void Terrain3DGpuWorkflow::_finalize_brush_readback(const PendingBrush &p_brush) {
	if (!_data) {
		return;
	}
	LOG(INFO, "Finalizing brush id=", p_brush.id, " map=", p_brush.map_type,
		" regions=", int(p_brush.regions.size()), " edited_area=", p_brush.edited_area);
	bool visuals_synced = false;
	bool gpu_blit_failed = false;
	if (_rd) {
		for (const Terrain3DGpuBrushRegion &region_info : p_brush.regions) {
			auto state_it = _region_gpu_states.find(region_info.location);
			if (state_it == _region_gpu_states.end()) {
				continue;
			}
			bool synced = _upload_region_to_material(p_brush.map_type, region_info, state_it->second);
			LOG(INFO, "Finalize: region=", region_info.location, " blit_synced=", synced,
				" tex_color=", state_it->second.color_texture.get_id(),
				" tex_height=", state_it->second.height_texture.get_id());
			visuals_synced = visuals_synced || synced;
			gpu_blit_failed = gpu_blit_failed || !synced;
		}
	}
	if (visuals_synced) {
		_data->_notify_gpu_maps_synced(p_brush.map_type);
	}
	if (gpu_blit_failed || !visuals_synced) {
		switch (p_brush.map_type) {
			case TYPE_COLOR:
				_data->update_maps(TYPE_COLOR, true, p_brush.generate_color_mipmaps);
				break;
			case TYPE_HEIGHT:
				_data->update_maps(TYPE_HEIGHT, true, false);
				break;
			default:
				break;
		}
	}
	switch (p_brush.map_type) {
		case TYPE_COLOR:
			break;
		case TYPE_HEIGHT:
			_data->notify_gpu_height_brush_complete(p_brush.edited_area, p_brush.update_instancer, p_brush.update_collision);
			break;
		default:
			break;
	}
}

bool Terrain3DGpuWorkflow::_upload_region_to_material(MapType p_map_type, const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state) {
	if (!_data || !_rd) {
		return false;
	}
	RID src_texture;
	switch (p_map_type) {
		case TYPE_COLOR:
			src_texture = p_state.color_texture;
			break;
		case TYPE_HEIGHT:
			src_texture = p_state.height_texture;
			break;
		default:
			return false;
	}
	if (!src_texture.is_valid()) {
		return false;
	}
	return _data->_blit_gpu_region_texture(p_map_type, p_region_info.location, p_state.size, _rd, src_texture);
}
