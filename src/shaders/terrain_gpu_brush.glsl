R"SHADER(#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba8) uniform image2D target_map;
layout(r32f, set = 0, binding = 1) uniform readonly image2D brush_mask;

layout(push_constant, std430) uniform BrushParams {
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
	vec4 color;
	int color_mode;
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
	float dist = length(center_offset) * params.inv_radius;
	if (dist > 1.0) {
		return;
	}

	vec2 normalized = rotate_uv(center_offset / (params.radius * 2.0)) + vec2(0.5);
	float mask = pow(max(sample_mask(normalized), 0.0), params.gamma);
	float falloff = pow(max(1.0 - dist, 0.0), 2.0);
	float influence = clamp(mask * falloff * params.strength, 0.0, 1.0);
	if (influence <= 0.0) {
		return;
	}

	vec4 src = imageLoad(target_map, pixel);
	vec3 target = params.color.rgb;
	if (params.color_mode == 1) {
		target = vec3(1.0);
	}
	vec3 result = mix(src.rgb, target, influence);
	imageStore(target_map, pixel, vec4(result, src.a));
}
)SHADER"
