// Copyright © 2025 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_gpu.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "logger.h"
#include "terrain_3d_data.h"

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
}

void Terrain3DGpuWorkflow::shutdown() {
	for (auto &entry : _region_gpu_states) {
		_free_region_state(entry.second);
	}
	_region_gpu_states.clear();
	_pending_brushes.clear();
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
	_enqueue_readback_brush(queued_request, true);
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
	_enqueue_readback_brush(queued_request, false);
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

void Terrain3DGpuWorkflow::_readback_color_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state) {
	if (!_rd || !p_state.color_texture.is_valid()) {
		return;
	}
	PackedByteArray data = _rd->texture_get_data(p_state.color_texture, 0);
	if (data.is_empty()) {
		return;
	}
	Ref<Image> img = p_region_info.region->get_color_map();
	if (img.is_null()) {
		img.instantiate();
		img->create(p_state.size.x, p_state.size.y, false, Image::FORMAT_RGBA8);
		img->set_data(p_state.size.x, p_state.size.y, false, Image::FORMAT_RGBA8, data);
		p_region_info.region->set_color_map(img);
	} else {
		img->set_data(p_state.size.x, p_state.size.y, false, Image::FORMAT_RGBA8, data);
	}
	p_region_info.region->set_modified(true);
	p_region_info.region->set_edited(true);
}

void Terrain3DGpuWorkflow::_readback_height_region(const Terrain3DGpuBrushRegion &p_region_info, const RegionGpuState &p_state) {
	if (!_rd || !p_state.height_texture.is_valid()) {
		return;
	}
	PackedByteArray data = _rd->texture_get_data(p_state.height_texture, 0);
	if (data.is_empty()) {
		return;
	}
	Ref<Image> img = p_region_info.region->get_height_map();
	if (img.is_null()) {
		img.instantiate();
		img->create(p_state.size.x, p_state.size.y, false, Image::FORMAT_RF);
		img->set_data(p_state.size.x, p_state.size.y, false, Image::FORMAT_RF, data);
		p_region_info.region->set_height_map(img);
	} else {
		img->set_data(p_state.size.x, p_state.size.y, false, Image::FORMAT_RF, data);
	}
	p_region_info.region->calc_height_range();
	p_region_info.region->set_modified(true);
	p_region_info.region->set_edited(true);
	if (_data) {
		_data->update_master_heights(p_region_info.region->get_height_range());
	}
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
	if (p_max_brushes < 0) {
		p_max_brushes = int(_pending_brushes.size());
	}
	int processed = 0;
	while (processed < p_max_brushes && !_pending_brushes.empty()) {
		PendingBrush brush = std::move(_pending_brushes.front());
		_pending_brushes.pop_front();
		for (const Terrain3DGpuBrushRegion &region_info : brush.regions) {
			auto state_it = _region_gpu_states.find(region_info.location);
			if (state_it == _region_gpu_states.end()) {
				continue;
			}
			switch (brush.map_type) {
				case TYPE_COLOR:
					_readback_color_region(region_info, state_it->second);
					break;
				case TYPE_HEIGHT:
					_readback_height_region(region_info, state_it->second);
					break;
				default:
					break;
			}
		}
		_finalize_brush_readback(brush);
		processed++;
	}
	if (_data && !_pending_brushes.empty()) {
		_data->request_gpu_readback_flush();
	}
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
	switch (p_brush.map_type) {
		case TYPE_COLOR:
			_data->update_maps(TYPE_COLOR, true, p_brush.generate_color_mipmaps);
			break;
		case TYPE_HEIGHT:
			_data->update_maps(TYPE_HEIGHT, true, false);
			_data->notify_gpu_height_brush_complete(p_brush.edited_area, p_brush.update_instancer, p_brush.update_collision);
			break;
		default:
			break;
	}
}
