#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require


#include "host_device.h"
#include "probeUtil.glsl"


layout(std430, set = 0, binding = eStatus) readonly buffer ProbeStatusSSBO {
  uint probe_status[];
};

layout(push_constant) uniform _PushConstantSample {
  PushConstantSample pcSample;
};

layout(set = 1, binding = eGlobals) uniform _GlobalUniforms{ GlobalUniforms uni; };


ivec2 pixel_offsets[] = ivec2[](ivec2(0, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1));

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;



vec3 get_world_position(vec2 screen_uv, float depth, mat4 projectionMatrix, mat4 inverseView) {
  // Convert screen UV coordinates to NDC (Normalized Device Coordinates)
  vec2 ndc = screen_uv * 2.0 - 1.0;

  // Convert depth from [0, 1] to [-1, 1] (clip space depth)
  float clip_depth = depth * 2.0 - 1.0;

  // Create the clip space position vector
  vec4 clipSpacePosition = vec4(ndc, clip_depth, 1.0);

  // Convert from clip space to view space (using inverse of the projection matrix)
  vec4 viewSpacePosition = inverse(projectionMatrix) * clipSpacePosition;

  // Perform perspective division to get view space position
  viewSpacePosition /= viewSpacePosition.w;

  // Convert from view space to world space (using inverse of the view matrix)
  vec4 worldSpacePosition = inverseView * viewSpacePosition;

  return worldSpacePosition.xyz;
}




void main() {
  uint output_resolution_half   = pcSample.output_resolution_half;
  vec3 camera_position          = uni.position;


  ivec3 coords                  = ivec3(gl_GlobalInvocationID.xyz);

  int  resolution_divider = output_resolution_half == 1 ? 2 : 1;

  vec2 screen_uv          = uv_nearest(coords.xy, resolution / resolution_divider);

  float raw_depth                        = 1.0;
  int   chosen_hiresolution_sample_index = 0;
  if(output_resolution_half == 1) {
    float closer_depth = 0.0;
    for(int i = 0; i < 4; ++i) {

      float depth = texelFetch(global_textures[nonuniformEXT(depth_fullscreen_texture_index)], (coords.xy) * 2 + pixel_offsets[i], 0).r;

      if(closer_depth < depth) {
        closer_depth                     = depth;
        chosen_hiresolution_sample_index = i;
      }
    }

    raw_depth = closer_depth;
  } else {
    raw_depth = texelFetch(global_textures[nonuniformEXT(depth_fullscreen_texture_index)], coords.xy, 0).r;
  }
  
  if(raw_depth == 1.0) {
    imageStore(global_images_2d[indirect_output_index], coords.xy, vec4(0, 0, 0, 1));
    return;
  }

  // Manually fetch normals when in low resolution
  vec3 normal = vec3(0);

  if(output_resolution_half == 1) {
    vec2 encoded_normal = texelFetch(global_textures[nonuniformEXT(normal_texture_index)], (coords.xy) * 2 + pixel_offsets[chosen_hiresolution_sample_index], 0).rg;
    normal = normalize(oct_decode(encoded_normal));

    //vec3 texture_normal = texelFetch(global_textures[nonuniformEXT(normal_texture_index)], (coords.xy) * 2 + pixel_offsets[chosen_hiresolution_sample_index], 0).rgb;
    //normal = normalize(texture_normal);
  }
  else {
    vec2 encoded_normal = texelFetch(global_textures[nonuniformEXT(normal_texture_index)], coords.xy, 0).rg;
    normal              = oct_decode(encoded_normal);

  }

  
  


  const vec3 pixel_world_position = get_world_position(screen_uv, raw_depth, uni.projection, uni.viewInverse);
  
  vec3 irradiance = sample_irradiance(pixel_world_position, normal, camera_position);
  imageStore(global_images_2d[indirect_output_index], coords.xy, vec4(irradiance, 1.0));

}
