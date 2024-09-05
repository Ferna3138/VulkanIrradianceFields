#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require

#include "wavefront.glsl"



layout(push_constant) uniform _PushConstantRaster {
  PushConstantRaster pcRaster;
};

// clang-format off
// Incoming 
layout(location = 1) in vec3 i_worldPos;
layout(location = 2) in vec3 i_worldNrm;
layout(location = 3) in vec3 i_viewDir;
layout(location = 4) in vec2 i_texCoord;

// Outgoing
layout(location = 0) out vec4 o_normals;
layout(location = 1) out vec4 o_depth;
layout(location = 2) out vec4 o_albedo;
layout(location = 3) out vec4 o_diffuseLight;



layout(buffer_reference, scalar) buffer Vertices {Vertex v[]; }; // Positions of an object
layout(buffer_reference, scalar) buffer Indices {uint i[]; }; // Triangle indices
layout(buffer_reference, scalar) buffer Materials {WaveFrontMaterial m[]; }; // Array of all materials on an object
layout(buffer_reference, scalar) buffer MatIndices {int i[]; }; // Material ID for each triangle

layout(binding = eGlobals) uniform _GlobalUniforms { GlobalUniforms uni; };
layout(binding = eObjDescs, scalar) buffer ObjDesc_ { ObjDesc i[]; } objDesc;
layout(binding = eTextures) uniform sampler2D[] textureSamplers;
// clang-format on


float sign_not_zero(in float k) {
  return (k >= 0.0) ? 1.0 : -1.0;
}


vec2 sign_not_zero2(in vec2 v) {
  return vec2(sign_not_zero(v.x), sign_not_zero(v.y));
}


// Assumes that v is a unit vector. The result is an octahedral vector on the [-1, +1] square.
vec2 oct_encode(in vec3 v) {
  float l1norm = abs(v.x) + abs(v.y) + abs(v.z);
  vec2  result = v.xy * (1.0 / l1norm);
  if(v.z < 0.0) {
    result = (1.0 - abs(result.yx)) * sign_not_zero2(result.xy);
  }
  return result;
}



void main() {
    // Normals
	o_normals = vec4(oct_encode(i_worldNrm), 0.0, 1.0);

    // Depth Map
	vec4 clipPos = uni.viewProj * vec4(i_worldPos, 1.0);
	vec3 ndcPos = clipPos.xyz / clipPos.w;
	float normalizedDepth = (ndcPos.z + 1.0) * 0.5;
	o_depth = vec4(normalizedDepth, normalizedDepth, normalizedDepth, 1.0);



    // Material of the object
    ObjDesc objResource = objDesc.i[pcRaster.objIndex];
    MatIndices matIndices = MatIndices(objResource.materialIndexAddress);
    Materials materials = Materials(objResource.materialAddress);

    int matIndex = matIndices.i[gl_PrimitiveID];
    WaveFrontMaterial mat = materials.m[matIndex];

    vec3 albedo = mat.diffuse;

    vec3 N = normalize(i_worldNrm);

    // Vector toward light
    vec3  L;
    float lightRadius = 10;
    float lightIntensity = pcRaster.lightIntensity;
    if(pcRaster.lightType == 0) {
        vec3  lDir     = pcRaster.lightPosition - i_worldPos;
        float d        = length(lDir);
        float attenuation = max(1.0 - (d / lightRadius), 0.0);
        lightIntensity = pcRaster.lightIntensity / (d * d);
        L              = normalize(lDir);
    }else {
        L = normalize(pcRaster.lightPosition);
    }

    // Compute the diffuse component of the lighting
    float NdotL = max(dot(N, L), 0.0);

    vec3 diffuseTxt;
    vec3 diffuse;
    if (mat.textureId >= 0) {
        int txtOffset = objDesc.i[pcRaster.objIndex].txtOffset;
        uint txtId = txtOffset + mat.textureId;
        diffuseTxt = texture(textureSamplers[nonuniformEXT(txtId)], i_texCoord).xyz;
        diffuse *= diffuseTxt;
    }

    // Specular lighting calculation
    vec3 specular = computeSpecular(mat, i_viewDir, L, N);


    // Albedo
    o_albedo = vec4(diffuseTxt, 1.0);

    
    // Diffuse Light
    vec3 diffuseLight = vec3(lightIntensity * NdotL);
    o_diffuseLight = vec4(diffuseLight, 1.0);

}
