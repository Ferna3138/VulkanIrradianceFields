#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_GOOGLE_include_directive : enable

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "probeUtil.glsl"
#include "wavefront.glsl"


layout(location = 0) in vec3 pos;

layout(location = 0) flat out int probe_index;
layout(location = 1) out vec4 normal_edge_factor;
layout(location = 2) flat out uint probe_status;
layout(location = 3) out vec3 fragPosition;

layout(set = 1, binding = eGlobals) uniform _GlobalUniforms { GlobalUniforms uni; };

layout(std430, set = 0, binding = eStatus) buffer ProbeStatusSSBO {
  uint probe_statuses[];
};

layout(push_constant) uniform _PushConstantDebug {
  PushConstantDebug pcDebug;
};

void main() {
    vec3 camera_position = vec3(uni.viewInverse[3]);

    probe_index = gl_InstanceIndex;
    probe_status = probe_statuses[ probe_index ];

    const ivec3 probe_grid_indices = probe_index_to_grid_indices(int(probe_index));
    const vec3 probe_position = grid_indices_to_world( probe_grid_indices, probe_index );
    
    gl_Position = uni.projection * uni.view  * vec4( (pos * probe_sphere_scale) + probe_position, 1.0 );

    normal_edge_factor.xyz = normalize( pos );
    normal_edge_factor.w = abs(dot(normal_edge_factor.xyz, normalize(probe_position - camera_position.xyz)));


    vec4 worldPosition = pcDebug.modelMatrix * vec4(pos, 1.0);
    fragPosition = (uni.view * worldPosition).xyz;  // Pass the view-space position

    //gl_Position = uni.projection * vec4(fragPosition, 1.0);
}
