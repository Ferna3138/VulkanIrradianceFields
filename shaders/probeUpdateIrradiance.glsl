#version 460

#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "host_device.h"
#include "probeUtil.glsl"

layout(rgba16f, set = 0, binding = eIrradianceImage) uniform image2D irradiance_image;



#define EPSILON 0.0001f
int k_read_table[6] = {5, 3, 1, -1, -3, -5};

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;


void main() {
  ivec3 coords = ivec3(gl_GlobalInvocationID.xyz);

  int probe_texture_width  = irradiance_texture_width;
  int probe_texture_height = irradiance_texture_height;
  int probe_side_length    = irradiance_side_length;

  // Early out for 1 pixel border around all image and outside bound pixels.
  if(coords.x >= probe_texture_width || coords.y >= probe_texture_height) {
    return;
  }

  const uint probe_with_border_side = probe_side_length + 2;
  const uint probe_last_pixel       = probe_side_length + 1;

  int probe_index = get_probe_index_from_pixels(coords.xy, int(probe_with_border_side), probe_texture_width);

  // Check if thread is a border pixel
  bool border_pixel = ((gl_GlobalInvocationID.x % probe_with_border_side) == 0) || ((gl_GlobalInvocationID.x % probe_with_border_side) == probe_last_pixel);
  border_pixel = border_pixel || ((gl_GlobalInvocationID.y % probe_with_border_side) == 0) || ((gl_GlobalInvocationID.y % probe_with_border_side) == probe_last_pixel);


  if(!border_pixel) {
    vec4        result              = vec4(0);
    const float energy_conservation = 0.95;

    uint backfaces     = 0;
    uint max_backfaces = uint(probe_rays * 0.1f);

    for(int ray_index = 0; ray_index < probe_rays; ++ray_index) {
      ivec2 sample_position = ivec2(ray_index, probe_index);

      vec3 ray_direction   = normalize(mat3(random_rotation) * spherical_fibonacci(ray_index, probe_rays));
      vec3 texel_direction = oct_decode(
          (coords.xy, probe_side_length));

      float weight = max(0.0, dot(texel_direction, ray_direction));

      vec4 radiance_sample = texelFetch(global_textures[nonuniformEXT(radiance_output_index)], sample_position, 0);

      if(radiance_sample.w < 0.0f && use_backfacing_blending()) {
        ++backfaces;
        if(backfaces >= max_backfaces)
          return;
        continue;
      }

      if(weight >= EPSILON) {
        vec3 radiance = radiance_sample.rgb * energy_conservation;

        // Storing the sum of the weights in alpha temporarily
        result += vec4(radiance * weight, weight);
      }
    }

    if(result.w > EPSILON) {
      result.xyz /= result.w;
      result.w = 0.0f;
    }

    // Read previous frame value
    vec4 previous_value = imageLoad(irradiance_image, coords.xy);

    // Debug inside with color green
    if(show_border_vs_inside()) {
      result = vec4(0, 1, 0, 1);
    }

    if(use_perceptual_encoding()) {
      result.rgb = pow(result.rgb, vec3(1.0f / 5.0f));
    }

    result = mix(result, previous_value, hysteresis);
    imageStore(irradiance_image, coords.xy, result);

    return;
  }

  // Wait for all local threads to have finished to copy the border pixels.
  groupMemoryBarrier();
  barrier();

  // Operate with Border pixels
  // Copy border pixel calculating source pixels.
  const uint probe_pixel_x = gl_GlobalInvocationID.x % probe_with_border_side;
  const uint probe_pixel_y = gl_GlobalInvocationID.y % probe_with_border_side;
  bool       corner_pixel  = (probe_pixel_x == 0 || probe_pixel_x == probe_last_pixel) && (probe_pixel_y == 0 || probe_pixel_y == probe_last_pixel);
  bool       row_pixel = (probe_pixel_x > 0 && probe_pixel_x < probe_last_pixel);

  ivec2 source_pixel_coordinate = coords.xy;

  if(corner_pixel) {
    source_pixel_coordinate.x += probe_pixel_x == 0 ? probe_side_length : -probe_side_length;
    source_pixel_coordinate.y += probe_pixel_y == 0 ? probe_side_length : -probe_side_length;

    if(show_border_type()) {
      source_pixel_coordinate = ivec2(2, 2);
    }
  }
  else if(row_pixel) {
    source_pixel_coordinate.x += k_read_table[probe_pixel_x - 1];
    source_pixel_coordinate.y += (probe_pixel_y > 0) ? -1 : 1;

    if(show_border_type()) {
      source_pixel_coordinate = ivec2(3, 3);
    }
  }
  else {
    source_pixel_coordinate.x += (probe_pixel_x > 0) ? -1 : 1;
    source_pixel_coordinate.y += k_read_table[probe_pixel_y - 1];

    if(show_border_type()) {
      source_pixel_coordinate = ivec2(4, 4);
    }
  }

  vec4 copied_data = imageLoad(irradiance_image, source_pixel_coordinate);

  // Debug border source coordinates
  if(show_border_source_coordinates()) {
    copied_data = vec4(coords.xy, source_pixel_coordinate);
  }

  // Debug border with color red
  if(show_border_vs_inside()) {
    copied_data = vec4(1, 0, 0, 1);
  }

  imageStore(irradiance_image, coords.xy, copied_data);
}