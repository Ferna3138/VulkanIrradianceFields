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

namespace Util{
    // Function to generate a random float between min and max
    float randomFloat(float min, float max) {
        return min + static_cast<float>(rand()) / (static_cast<float>(RAND_MAX / (max - min)));
    }

    // Function to generate a random unit vector (axis of rotation)
    glm::vec3 randomUnitVector() {
        float theta  = randomFloat(0.0f, 2.0f * glm::pi<float>());  // Random angle in radians
        float z      = randomFloat(-1.0f, 1.0f);                    // Random z coordinate between -1 and 1
        float radius = sqrt(1.0f - z * z);                          // Radius in the x-y plane

        float x = radius * cos(theta);
        float y = radius * sin(theta);

        return glm::vec3(x, y, z);
    }

    // Function to generate a random rotation matrix
    glm::mat4 randomRotationMatrix() {
        glm::vec3 axis  = randomUnitVector();
        float     angle = randomFloat(0.0f, 2.0f * glm::pi<float>());  // Random angle in radians
        return glm::rotate(glm::mat4(1.0f), angle, axis);
    }


    void glm_euler_xyz2(glm::vec3 angles, glm::mat4 dest) {
        float cx, cy, cz, sx, sy, sz, czsx, cxcz, sysz;

        sx = sinf(angles.x);
        cx = cosf(angles.x);
        sy = sinf(angles.y);
        cy = cosf(angles.y);
        sz = sinf(angles.z);
        cz = cosf(angles.z);

        czsx = cz * sx;
        cxcz = cx * cz;
        sysz = sy * sz;

        dest[0][0] = cy * cz;
        dest[0][1] = czsx * sy + cx * sz;
        dest[0][2] = -cxcz * sy + sx * sz;
        dest[1][0] = -cy * sz;
        dest[1][1] = cxcz - sx * sysz;
        dest[1][2] = czsx + cx * sysz;
        dest[2][0] = sy;
        dest[2][1] = -cy * sx;
        dest[2][2] = cx * cy;
        dest[0][3] = 0.0f;
        dest[1][3] = 0.0f;
        dest[2][3] = 0.0f;
        dest[3][0] = 0.0f;
        dest[3][1] = 0.0f;
        dest[3][2] = 0.0f;
        dest[3][3] = 1.0f;
    }


    glm::mat4 glms_euler_xyz(glm::vec3 angles) {
        glm::mat4 dest = glm::mat4(1.0f);
        glm_euler_xyz2(angles, dest);
        return dest;
    }
};