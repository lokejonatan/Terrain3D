R"SHADER(#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r32f, set = 0, binding = 0) uniform image2D height_map;
layout(r32f, set = 0, binding = 1) uniform readonly image2D brush_mask;

layout(push_constant, std430) uniform HeightBrushParams {
	ivec2 target_origin;
	ivec2 target_size;
	ivec2 texture_size;
	ivec2 mask_size;
	vec2 brush_center;
	float radius;
	float inv_radius;
	float strength;
	float gamma;
	float rotation_cos;
	float rotation_sin;
	float target_height;
	float cursor_height;
	int height_mode;
	int use_alt;
	vec2 padding;
} params;

float sample_mask(vec2 uv) {
	if (params.mask_size.x <= 0 || params.mask_size.y <= 0) {
		return 1.0;
	}
	uv = clamp(uv, vec2(0.0), vec2(1.0));
	ivec2 coords = ivec2(uv * (vec2(params.mask_size) - vec2(1.0)));
	return imageLoad(brush_mask, coords).r;
}

vec2 rotate_uv(vec2 offset) {
	float x = offset.x * params.rotation_cos - offset.y * params.rotation_sin;
	float y = offset.x * params.rotation_sin + offset.y * params.rotation_cos;
	return vec2(x, y);
}

float apply_height(float src, float influence) {
	const int MODE_RAISE = 0;
	const int MODE_LOWER = 1;
	const int MODE_SET = 2;
	if (influence <= 0.0) {
		return src;
	}
	if (params.height_mode == MODE_SET) {
		return mix(src, params.target_height, clamp(influence, 0.0, 1.0));
	}
	bool alt_mode = params.use_alt == 1 && !isnan(params.cursor_height);
	if (params.height_mode == MODE_RAISE) {
		if (alt_mode) {
			float target = params.cursor_height + influence;
			float lower = src;
			float upper = src + influence;
			return clamp(target, lower, upper);
		}
		return src + influence;
	}
	if (params.height_mode == MODE_LOWER) {
		if (alt_mode) {
			float target = params.cursor_height - influence;
			float lower = src - influence;
			float upper = src;
			return clamp(target, lower, upper);
		}
		return src - influence;
	}
	return src;
}

void main() {
	ivec2 local_id = ivec2(gl_GlobalInvocationID.xy);
	if (local_id.x >= params.target_size.x || local_id.y >= params.target_size.y) {
		return;
	}
	ivec2 pixel = params.target_origin + local_id;
	if (pixel.x < 0 || pixel.y < 0 || pixel.x >= params.texture_size.x || pixel.y >= params.texture_size.y) {
		return;
	}

	vec2 center_offset = (vec2(pixel) + vec2(0.5)) - params.brush_center;

	vec2 normalized = rotate_uv(center_offset / (params.radius * 2.0)) + vec2(0.5);
	float mask = pow(max(sample_mask(normalized), 0.0), params.gamma);
	if (mask <= 0.0) {
		return;
	}
	float influence = max(mask * params.strength, 0.0);

	vec4 src_texel = imageLoad(height_map, pixel);
	float result = apply_height(src_texel.r, influence);
	if (result != src_texel.r) {
		imageStore(height_map, pixel, vec4(result, 0.0, 0.0, 1.0));
	}
}
)SHADER"