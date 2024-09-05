#pragma once

#include "nvvkhl/appbase_vk.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/memallocator_dma_vk.hpp"
#include "nvvk/resourceallocator_vk.hpp"
#include "shaders/host_device.h"
//#include "ProbeVolume.h"

// #VKRay
#include "nvvk/raytraceKHR_vk.hpp"


struct alignas(16) Indirect_gpu_constants {
  uint32_t radiance_output_index;
  uint32_t grid_irradiance_output_index;
  uint32_t indirect_output_index;
  uint32_t normal_texture_index;

  uint32_t depth_pyramid_texture_index;
  uint32_t depth_fullscreen_texture_index;
  uint32_t grid_visibility_texture_index;
  uint32_t probe_offset_texture_index;

  float   hysteresis;
  float   infinte_bounces_multiplier;
  
  int32_t probe_update_offset;
  int32_t probe_update_count;

  glm::vec3 probe_grid_position;
  float     probe_sphere_scale;

  glm::vec3 probe_spacing;
  float     max_probe_offset;  // [0,0.5] max offset for probes

  glm::vec3 reciprocal_probe_spacing;
  float     self_shadow_bias;

  int32_t  probe_counts[3];
  uint32_t debug_options;

  int32_t irradiance_texture_width;
  int32_t irradiance_texture_height;
  int32_t irradiance_side_length;
  int32_t probe_rays;

  int32_t  visibility_texture_width;
  int32_t  visibility_texture_height;
  int32_t  visibility_side_length;
  uint32_t pad1;

  glm::mat4 random_rotation;
  glm::vec2 resolution;
};  // struct DDGIConstants




