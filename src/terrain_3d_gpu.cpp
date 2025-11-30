// Copyright © 2025 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_gpu.h"

#include <cstdint>
#include <cstring>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
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

struct BrushPushConstant {
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
	if (!_ensure_pipeline()) {
		_ready = false;
		return;
	}
	_ensure_fallback_mask();
	_ready = _brush_pipeline.is_valid();
}

void Terrain3DGpuWorkflow::shutdown() {
	for (auto &entry : _region_color_textures) {
		_free_region_state(entry.second);
	}
	_region_color_textures.clear();
	if (_rd) {
		if (_brush_pipeline.is_valid()) {
			_rd->free_rid(_brush_pipeline);
		}
		if (_brush_shader.is_valid()) {
			_rd->free_rid(_brush_shader);
		}
		if (_fallback_mask_texture.is_valid()) {
			_rd->free_rid(_fallback_mask_texture);
		}
	}
	_brush_pipeline = RID();
	_brush_shader = RID();
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
	Vector2i mask_size = _fallback_mask_size;
	RID mask_texture = _fallback_mask_texture;
	if (p_request.mask.is_valid()) {
		mask_texture = _create_mask_texture(p_request.mask, mask_size);
		if (!mask_texture.is_valid()) {
			mask_texture = _fallback_mask_texture;
			mask_size = _fallback_mask_size;
		}
	}

	bool any_dispatched = false;
	for (const Terrain3DGpuBrushRegion &region_info : p_request.regions) {
		Terrain3DRegion *region_ptr = region_info.region.ptr();
		if (!region_ptr) {
			continue;
		}
		RegionGpuState &state = _get_or_create_region_state(region_info.location, region_ptr);
		if (!state.color_texture.is_valid()) {
			continue;
		}
		bool dispatched = _dispatch_color_brush(p_request, region_info, state, mask_texture, mask_size);
		if (dispatched) {
			_readback_color_region(region_info, state);
			any_dispatched = true;
		}
	}

	if (mask_texture.is_valid() && mask_texture != _fallback_mask_texture) {
		_rd->free_rid(mask_texture);
	}
	return any_dispatched;
}

void Terrain3DGpuWorkflow::remove_region(const Vector2i &p_region_loc) {
	auto found = _region_color_textures.find(p_region_loc);
	if (found != _region_color_textures.end()) {
		_free_region_state(found->second);
		_region_color_textures.erase(found);
	}
}

bool Terrain3DGpuWorkflow::_ensure_pipeline() {
	if (_brush_pipeline.is_valid()) {
		return true;
	}
	String shader_code = String(
#include "shaders/terrain_gpu_brush.glsl"
	);
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, shader_code);
	Ref<RDShaderSPIRV> spirv = _rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		LOG(ERROR, "Failed to compile GPU brush shader");
		return false;
	}
	_brush_shader = _rd->shader_create_from_spirv(spirv);
	if (!_brush_shader.is_valid()) {
		LOG(ERROR, "Failed to create GPU brush shader RID");
		return false;
	}
	_brush_pipeline = _rd->compute_pipeline_create(_brush_shader);
	if (!_brush_pipeline.is_valid()) {
		LOG(ERROR, "Failed to create GPU brush pipeline");
		_rd->free_rid(_brush_shader);
		_brush_shader = RID();
		return false;
	}
	return true;
}

void Terrain3DGpuWorkflow::_ensure_fallback_mask() {
	if (_fallback_mask_texture.is_valid() || !_rd) {
		return;
	}
	Ref<Image> img;
	img.instantiate();
	img->create(1, 1, false, Image::FORMAT_RF);
	img->set_pixel(0, 0, Color(1.f, 0.f, 0.f, 1.f));
	Vector2i dummy_size;
	_fallback_mask_texture = _create_mask_texture(img, dummy_size);
	_fallback_mask_size = dummy_size == Vector2i() ? Vector2i(1, 1) : dummy_size;
}

Terrain3DGpuWorkflow::RegionGpuState &Terrain3DGpuWorkflow::_get_or_create_region_state(const Vector2i &p_region_loc, Terrain3DRegion *p_region) {
	auto found = _region_color_textures.find(p_region_loc);
	if (found != _region_color_textures.end()) {
		return found->second;
	}
	RegionGpuState state;
	Ref<Image> color_map = p_region->get_color_map();
	if (color_map.is_null()) {
		p_region->sanitize_maps();
		color_map = p_region->get_color_map();
	}
	if (color_map.is_valid()) {
		if (p_region->get_region_size() <= 0) {
			LOG(WARN, "Region ", p_region_loc, " has invalid region size; skipping GPU upload");
			auto inserted = _region_color_textures.insert({ p_region_loc, state });
			return inserted.first->second;
		}
		Vector2i map_size = color_map->get_size();
		if (map_size.x <= 0 || map_size.y <= 0) {
			LOG(WARN, "Region ", p_region_loc, " color map size invalid (", map_size, ")");
			auto inserted = _region_color_textures.insert({ p_region_loc, state });
			return inserted.first->second;
		}
		Ref<Image> upload_image = color_map;
		bool needs_copy = color_map->has_mipmaps() || color_map->get_format() != Image::FORMAT_RGBA8;
		if (needs_copy) {
			upload_image = color_map->duplicate();
			if (upload_image.is_null()) {
				LOG(ERROR, "Failed to duplicate color map for region ", p_region_loc);
				auto inserted = _region_color_textures.insert({ p_region_loc, state });
				return inserted.first->second;
			}
			if (upload_image->get_format() != Image::FORMAT_RGBA8) {
				upload_image->convert(Image::FORMAT_RGBA8);
			}
		}
		if (upload_image->has_mipmaps()) {
			upload_image->clear_mipmaps();
		}
		if (!_image_has_pixels(upload_image, "region color map")) {
			auto inserted = _region_color_textures.insert({ p_region_loc, state });
			return inserted.first->second;
		}
		map_size = upload_image->get_size();
		state.size = map_size;
		state.color_texture = _create_texture_from_image(upload_image, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	}
	auto inserted = _region_color_textures.insert({ p_region_loc, state });
	return inserted.first->second;
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
	return _rd->texture_create(fmt, view, data);
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
	p_state.color_texture = RID();
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

bool Terrain3DGpuWorkflow::_dispatch_color_brush(const Terrain3DGpuBrushRequest &p_request, const Terrain3DGpuBrushRegion &p_region_info,
		const RegionGpuState &p_state, const RID &p_mask_texture, const Vector2i &p_mask_size) {
	if (!_rd || !_brush_pipeline.is_valid()) {
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
	RID uniform_set = _rd->uniform_set_create(uniforms, _brush_shader, 0);
	if (!uniform_set.is_valid()) {
		LOG(ERROR, "Failed to create uniform set for GPU brush");
		return false;
	}

	BrushPushConstant push;
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
	push_bytes.resize(sizeof(BrushPushConstant));
	std::memcpy(push_bytes.ptrw(), &push, sizeof(BrushPushConstant));

	int64_t compute_list = _rd->compute_list_begin();
	_rd->compute_list_bind_compute_pipeline(compute_list, _brush_pipeline);
	_rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	_rd->compute_list_set_push_constant(compute_list, push_bytes, push_bytes.size());
	uint32_t group_x = (target_rect.size.x + 7) / 8;
	uint32_t group_y = (target_rect.size.y + 7) / 8;
	_rd->compute_list_dispatch(compute_list, group_x, group_y, 1);
	_rd->compute_list_end();
	_rd->submit();
	_rd->sync();
	_rd->free_rid(uniform_set);
	return true;
}
