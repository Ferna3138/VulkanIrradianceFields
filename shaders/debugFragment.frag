#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require

#include "wavefront.glsl"
#include "probeUtil.glsl"


layout (location = 0) flat in int probe_index;
layout (location = 1) in vec4 normal_edge_factor;
layout (location = 2) flat in uint probe_status;
layout (location = 3) in vec3 fragPosition;


layout(location = 0) out vec4 o_color;

layout(set = 1, binding = eGlobals) uniform _GlobalUniforms { GlobalUniforms uni; };


void main() {    

    vec2 uv = get_probe_uv(normal_edge_factor.xyz, probe_index, irradiance_texture_width, irradiance_texture_height, irradiance_side_length);

    vec3 irradiance = textureLod(global_textures[nonuniformEXT(grid_irradiance_output_index)], uv, 0).rgb;

    if ( use_perceptual_encoding() ) {
        irradiance = pow(irradiance, vec3(0.5f * 5.0f));
        irradiance = irradiance * irradiance;
    }

    /*
    // Sample the depth from the G-buffer using the appropriate index
    float gbufferDepth = texture(global_textures[nonuniformEXT(depth_fullscreen_texture_index)], gl_FragCoord.xy / vec2(textureSize(global_textures[nonuniformEXT(depth_fullscreen_texture_index)], 0))).r;
    float z = gbufferDepth * 2.0 - 1.0; 

    vec4 clipSpacePosition = vec4(gl_FragCoord.xy * 2.0 - 1.0, z, 1.0);
    vec4 viewSpacePosition = uni.viewInverse * clipSpacePosition;
    viewSpacePosition /= viewSpacePosition.w;

    // Compute the fragment's depth in view space (assuming modelViewMatrix is provided)
    float fragmentDepth = (uni.view * vec4(fragPosition, 1.0)).z;

    if (fragmentDepth > viewSpacePosition.z) {
        discard;  // Fragment is behind the G-buffer depth, discard it
    }
    */
    

    if ( normal_edge_factor.w < 0.55f ) {
        if (probe_status == PROBE_STATUS_OFF) {
            irradiance = vec3(1,0,0);
        }
        else if (probe_status == PROBE_STATUS_UNINITIALISED) {
            irradiance = vec3(0,0,1);
        }
        else if (probe_status == PROBE_STATUS_ACTIVE) {
            irradiance = vec3(0,1,0);
        }
        else {
            irradiance = vec3(1,1,1);
        }
    }
    
    o_color = vec4(irradiance, 1.0);
}
