# Vulkan Ray Traced Irradiance Fields

The main algorithm in this project is Irradiance Fields which is a probe based hybrid rendering approach for computing global illumination. It extends pre-computed light probes approaches, which are commonly used in real-time rendering, however, these probe-based solutions are not able to resolve visibility and occlusion of dynamic scene elements at runtime.

To address these problems, we enhance the algorithm with ray tracing and extensive caching, which leverages hardware acceleration to provide a fast and efficient update rate, making it suitable for dynamic scenes. 

This project is based on the Nvidia Vulkan tutorial Engine which is necessary to run the application. The engine can be downloaded from: https://nvpro-samples.github.io/vk_raytracing_tutorial_KHR/
