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

struct renderSceneVolume {
  bool gi_show_probes = false;

  
  // Sponza -> Scale - 0.015
  glm::vec3 gi_probe_grid_position{-20.705f, 0.426f, -9.326};
  glm::vec3 gi_probe_spacing{1.726f, 1.098f, 1.124f};
  

  /*
  // Sibenik -> Scale - 1.0
  glm::vec3 gi_probe_grid_position{-20.705f, -14.463f, -7.181};
  glm::vec3 gi_probe_spacing{1.710f, 1.098f, 0.977f};
  */

  
  // Living Room
  /*
  glm::vec3 gi_probe_grid_position{-3.832f, 0.425f, -3.500f};
  glm::vec3 gi_probe_spacing{0.851f, 0.756f, 0.743f};
  */

  float    gi_probe_sphere_scale          = 0.1f;
  float    gi_max_probe_offset            = 0.5f;
  float    gi_self_shadow_bias            = 0.85f;
  float    gi_hysteresis                  = 0.65f;
  bool     gi_debug_border                = false;
  bool     gi_debug_border_type           = false;
  bool     gi_debug_border_source         = false;
  uint32_t gi_total_probes                = 0;
  float    gi_intensity                   = 0.8f;
  bool     gi_use_visibility              = true;
  bool     gi_use_wrap_shading			  = true;
  bool     gi_use_perceptual_encoding     = false;
  bool     gi_use_backface_blending       = true;
  bool     gi_use_probe_offsetting        = true;
  bool     gi_recalculate_offsets         = false;  // When moving grid or changing spaces -> recalculate offsets
  bool     gi_use_probe_status            = false;
  bool     gi_use_half_resolution         = true;
  bool     gi_use_infinite_bounces        = false;
  float    gi_infinite_bounces_multiplier = 0.75f;
  uint32_t gi_per_frame_probes_update     = 1000;
};


class Probe_Volume {
public:
	int  m_currentTextureDebug		= 0;
	bool m_showDebugTextures		= false;

	// Sponza
	uint32_t probe_count_x			= 24;
	uint32_t probe_count_y			= 20;
	uint32_t probe_count_z			= 16;
	


	/*
	// Sibenik
	uint32_t probe_count_x = 24;
	uint32_t probe_count_y = 24;
	uint32_t probe_count_z = 16;
	*/

	
	// Living Room
	/*
	uint32_t probe_count_x = 10;
	uint32_t probe_count_y = 10;
	uint32_t probe_count_z = 10;*/
	

	int32_t per_frame_probe_updates = 0;
	int32_t probe_update_offset     = 0;

	int32_t probe_rays				= 128;

	int32_t irradiance_atlas_width;
	int32_t irradiance_atlas_height;
	int32_t irradiance_probe_size	= 6;  // Irradiance is a 6x6 quad with 1 pixel borders for bilinear filtering, total 8x8

	int32_t visibility_atlas_width;
	int32_t visibility_atlas_height;
	int32_t visibility_probe_size	= 6;

	bool half_resolution_output		= false;

	uint32_t get_total_probes() { return probe_count_x * probe_count_y * probe_count_z; }
	uint32_t get_total_rays() { return probe_rays * probe_count_x * probe_count_y * probe_count_z; }

};