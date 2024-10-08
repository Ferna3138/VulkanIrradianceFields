#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_GOOGLE_include_directive : enable

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require

#include "raycommon.glsl"
#include "wavefront.glsl"
#include "probeUtil.glsl"

hitAttributeEXT vec2 attribs;


// clang-format off
layout(location = 0) rayPayloadInEXT ProbeRayPayload prd;
layout(location = 1) rayPayloadEXT bool isShadowed;

layout(buffer_reference, scalar) buffer Vertices {Vertex v[]; }; // Positions of an object
layout(buffer_reference, scalar) buffer Indices {ivec3 i[]; }; // Triangle indices
layout(buffer_reference, scalar) buffer Materials {WaveFrontMaterial m[]; }; // Array of all materials on an object
layout(buffer_reference, scalar) buffer MatIndices {int i[]; }; // Material ID for each triangle
layout(set = 0, binding = eTlas) uniform accelerationStructureEXT topLevelAS;


layout(set = 1, binding = eGlobals) uniform _GlobalUniforms { GlobalUniforms uni; };
layout(set = 1, binding = eObjDescs, scalar) buffer ObjDesc_ { ObjDesc i[]; } objDesc;
layout(set = 1, binding = eTextures) uniform sampler2D textureSamplers[];

layout(push_constant) uniform _PushConstantRay { PushConstantRay pcRay; };
// clang-format on


hitAttributeEXT vec2 barycentric_weights;

float attenuation_square_falloff(vec3 position_to_light, float light_inverse_radius) {
    const float distance_square = dot(position_to_light, position_to_light);
    const float factor = distance_square * light_inverse_radius * light_inverse_radius;
    const float smoothFactor = max(1.0 - factor * factor, 0.0);
    return (smoothFactor * smoothFactor) / max(distance_square, 1e-4);
}

void main() {
    vec3 radiance = vec3(0);
    float distance = 0.0f;
    if (gl_HitKindEXT == gl_HitKindBackFacingTriangleEXT) {
        // Track backfacing rays with negative distance
        distance = gl_RayTminEXT + gl_HitTEXT;
        distance *= -0.2;        
    }
    else {
        // Object data
        ObjDesc    objResource = objDesc.i[gl_InstanceCustomIndexEXT];
        MatIndices matIndices  = MatIndices(objResource.materialIndexAddress);
        Materials  materials   = Materials(objResource.materialAddress);
        Indices    indices     = Indices(objResource.indexAddress);
        Vertices   vertices    = Vertices(objResource.vertexAddress);
  
        // Indices of the triangle
        ivec3 ind = indices.i[gl_PrimitiveID];
  
        // Vertex of the triangle
        Vertex v0 = vertices.v[ind.x];
        Vertex v1 = vertices.v[ind.y];
        Vertex v2 = vertices.v[ind.z];

        const vec3 barycentrics = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

        // Computing the coordinates of the hit position
        const vec3 pos      = v0.pos * barycentrics.x + v1.pos * barycentrics.y + v2.pos * barycentrics.z;
        const vec3 worldPos = vec3(gl_ObjectToWorldEXT * vec4(pos, 1.0));  // Transforming the position to world space

        // Computing the normal at hit position
        const vec3 nrm      = v0.nrm * barycentrics.x + v1.nrm * barycentrics.y + v2.nrm * barycentrics.z;
        const vec3 worldNrm = normalize(vec3(nrm * gl_WorldToObjectEXT));  // Transforming the normal to world space
        

        // Vector toward the light
        vec3  L;
        float lightIntensity = pcRay.lightIntensity;
        float lightDistance  = 100000.0;
        // Point light
        if(pcRay.lightType == 0) {
            vec3 lDir      = pcRay.lightPosition - worldPos;
            lightDistance  = length(lDir);
            lightIntensity = pcRay.lightIntensity / (lightDistance * lightDistance);
            L              = normalize(lDir);
        }
        else {  // Directional light
            L = normalize(pcRay.lightPosition);
        }

        // Material of the object
        int               matIdx = matIndices.i[gl_PrimitiveID];
        WaveFrontMaterial mat    = materials.m[matIdx];


        // Diffuse
        vec3 diffuse = computeDiffuse(mat, L, worldNrm);
        if(mat.textureId >= 0) {
            uint txtId    = mat.textureId + objDesc.i[gl_InstanceCustomIndexEXT].txtOffset;
            vec2 texCoord = v0.texCoord * barycentrics.x + v1.texCoord * barycentrics.y + v2.texCoord * barycentrics.z;
            diffuse *= texture(textureSamplers[nonuniformEXT(txtId)], texCoord).xyz;
        }


        vec3 origin = uni.position;

        vec3 hitValue = vec3(lightIntensity * (diffuse));

        // infinite bounces
        if ( use_infinite_bounces() ) {
            hitValue += hitValue * sample_irradiance( pos, worldNrm, origin ) * infinite_bounces_multiplier;
        }

        radiance = hitValue;
        distance = gl_RayTminEXT + gl_HitTEXT;
    }

    prd.radiance = radiance;
    prd.distance = distance;
}

