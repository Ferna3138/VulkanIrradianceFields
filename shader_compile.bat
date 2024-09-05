:: Ray Tracing Files
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\raytraceProbes.rgen -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\raytraceProbes.rgen.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\raytraceProbes.rchit -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\raytraceProbes.rchit.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\raytraceProbes.rmiss -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\raytraceProbes.rmiss.spv


C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\raytrace.rgen -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\raytrace.rgen.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\raytrace.rchit -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\raytrace.rchit.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\raytrace.rmiss -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\raytrace.rmiss.spv


:: Compute Shaders
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 -fshader-stage=compute D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\probeOffsets.glsl -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\probeOffsets.glsl.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 -fshader-stage=compute D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\probeStatus.glsl -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\probeStatus.glsl.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 -fshader-stage=compute D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\probeUpdateIrradiance.glsl -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\probeUpdateIrradiance.glsl.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 -fshader-stage=compute D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\probeUpdateVisibility.glsl -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\probeUpdateVisibility.glsl.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 -fshader-stage=compute D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\sampleIrradiance.glsl -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\sampleIrradiance.glsl.spv

:: GBuffer Files
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\gBufferVertex.vert -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\gBufferVertex.vert.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\gBufferFragment.frag -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\gBufferFragment.frag.spv

::Probe Debug
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\debugVertex.vert -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\debugVertex.vert.spv
C:\VulkanSDK\1.3.275.0\Bin\glslc.exe --target-env=vulkan1.2 D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\shaders\debugFragment.frag -o D:\RayTracing\NVPro\vk_raytracing_tutorial_KHR\ray_tracing__simple\spv\debugFragment.frag.spv
