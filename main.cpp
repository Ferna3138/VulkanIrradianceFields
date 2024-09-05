#include <array>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include "imgui/imgui_helper.h"

#include "hello_vulkan.h"
#include "Gpu_Constants.h"


#include "imgui/imgui_camera_widget.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvpsystem.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/context_vk.hpp"

#include <iostream>

//////////////////////////////////////////////////////////////////////////
#define UNUSED(x) (void)(x)
//////////////////////////////////////////////////////////////////////////

// Default search path for shaders
std::vector<std::string> defaultSearchPaths;


// GLFW Callback functions
static void onErrorCallback(int error, const char* description){
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Extra UI
void renderUI(HelloVulkan& helloVk, renderSceneVolume& scene)
{
  ImGuiH::CameraWidget();
  if(ImGui::CollapsingHeader("Light")) {
    ImGui::RadioButton("Point", &helloVk.m_pcRaster.lightType, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Infinite", &helloVk.m_pcRaster.lightType, 1);

    ImGui::SliderFloat3("Position", &helloVk.m_pcRaster.lightPosition.x, -20.f, 40.f);
    ImGui::SliderFloat("Intensity", &helloVk.m_pcRaster.lightIntensity, 0.f, 5000.f); 
  }

  if(ImGui::CollapsingHeader("Irradiance Field")) {
    scene.gi_recalculate_offsets = false;
      
    if(ImGui::SliderFloat3("Probe Grid Position", &scene.gi_probe_grid_position.x, -100.f, 100.f, "%2.3f")) {
      scene.gi_recalculate_offsets = true;
    }

    ImGui::Checkbox("Use Infinite Bounces", &scene.gi_use_infinite_bounces);
    ImGui::SliderFloat("Infinite bounces multiplier", &scene.gi_infinite_bounces_multiplier, 0.0f, 1.0f);

    if(ImGui::SliderFloat3("Probe Spacing", &scene.gi_probe_spacing.x, 0.f, 10.f, "%2.3f")) {
      scene.gi_recalculate_offsets = true;
    }  


    ImGui::SliderFloat("Hysteresis", &scene.gi_hysteresis, 0.0f, 1.0f);
    
    if(ImGui::SliderFloat("Max Probe Offset", &scene.gi_max_probe_offset, 0.0f, 0.5f)){
      scene.gi_recalculate_offsets = true;
    }

    ImGui::SliderFloat("Sampling self shadow bias", &scene.gi_self_shadow_bias, 0.0f, 1.0f);
    ImGui::SliderFloat("Probe Sphere Scale", &scene.gi_probe_sphere_scale, 0.0f, 10.0f);
    ImGui::Checkbox("Show debug probes", &scene.gi_show_probes);
    ImGui::Checkbox("Use Visibility", &scene.gi_use_visibility);
    ImGui::Checkbox("Use Wrap Shading", &scene.gi_use_wrap_shading);
    ImGui::Checkbox("Use Perceptual Encoding", &scene.gi_use_perceptual_encoding);
    ImGui::Checkbox("Use Backface Blending", &scene.gi_use_backface_blending);
    ImGui::Checkbox("Use Probe Offsetting", &scene.gi_use_probe_offsetting);
    ImGui::Checkbox("Use Probe Status", &scene.gi_use_probe_status);

    ImGui::Checkbox("Debug border vs inside", &scene.gi_debug_border);
    ImGui::Checkbox("Debug border type (corner, row, column)", &scene.gi_debug_border_type);
    ImGui::Checkbox("Debug border source pixels", &scene.gi_debug_border_source);
  }

  if(ImGui::CollapsingHeader("Debug Textures")){
    ImGui::Checkbox("Show Debug Textures", &helloVk.volume.m_showDebugTextures);
    ImGui::SliderInt("Current Texture", &helloVk.volume.m_currentTextureDebug, 0, 8);
  }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
static int const SAMPLE_WIDTH  = 1280;
static int const SAMPLE_HEIGHT = 720;


//--------------------------------------------------------------------------------------------------
// Application Entry
//
int main(int argc, char** argv) {
  UNUSED(argc);

  // Setup GLFW window
  glfwSetErrorCallback(onErrorCallback);
  if(!glfwInit()) {
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(SAMPLE_WIDTH, SAMPLE_HEIGHT, PROJECT_NAME, nullptr, nullptr);


  // Setup camera
  CameraManip.setWindowSize(SAMPLE_WIDTH, SAMPLE_HEIGHT);
  CameraManip.setLookat(glm::vec3(1.3, 1.8, -0.5), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));

  // Setup Vulkan
  if(!glfwVulkanSupported()) {
    printf("GLFW: Vulkan Not Supported\n");
    return 1;
  }

  // setup some basic things for the sample, logging file for example
  NVPSystem system(PROJECT_NAME);

  // Search path for shaders and other media
  defaultSearchPaths = {
      NVPSystem::exePath() + PROJECT_RELDIRECTORY,
      NVPSystem::exePath() + PROJECT_RELDIRECTORY "..",
      std::string(PROJECT_NAME),
  };

  // Vulkan required extensions
  assert(glfwVulkanSupported() == 1);
  uint32_t count{0};
  auto     reqExtensions = glfwGetRequiredInstanceExtensions(&count);

  // Requesting Vulkan extensions and layers
  nvvk::ContextCreateInfo contextInfo;
  contextInfo.setVersion(1, 2);                       // Using Vulkan 1.2
  for(uint32_t ext_id = 0; ext_id < count; ext_id++)  // Adding required extensions (surface, win32, linux, ..)
    contextInfo.addInstanceExtension(reqExtensions[ext_id]);
  contextInfo.addInstanceLayer("VK_LAYER_LUNARG_monitor", true);              // FPS in titlebar
  contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, true);  // Allow debug names
  contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);            // Enabling ability to present rendering

  // #VKRay: Activate the ray tracing extension
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  contextInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false, &accelFeature);  // To build acceleration structures
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  contextInfo.addDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, false, &rtPipelineFeature);  // To use vkCmdTraceRaysKHR
  contextInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);  // Required by ray tracing pipeline

  // Creating Vulkan base application
  nvvk::Context vkctx{};
  vkctx.initInstance(contextInfo);
  // Find all compatible devices
  auto compatibleDevices = vkctx.getCompatibleDevices(contextInfo);
  assert(!compatibleDevices.empty());
  // Use a compatible device
  vkctx.initDevice(compatibleDevices[0], contextInfo);

  // Create example
  HelloVulkan helloVk;
  renderSceneVolume scene;

  // Window need to be opened to get the surface on which to draw
  const VkSurfaceKHR surface = helloVk.getVkSurface(vkctx.m_instance, window);
  vkctx.setGCTQueueWithPresent(surface);

  helloVk.setup(vkctx.m_instance, vkctx.m_device, vkctx.m_physicalDevice, vkctx.m_queueGCT.familyIndex);
  helloVk.createSwapchain(surface, SAMPLE_WIDTH, SAMPLE_HEIGHT);
  helloVk.createDepthBuffer();
  helloVk.createRenderPass();
  helloVk.createFrameBuffers();

  // Setup Imgui
  helloVk.initGUI(0);  // Using sub-pass 0

  // Creation of the example
  helloVk.loadModel(nvh::findFile("media/scenes/sponza.obj", defaultSearchPaths, true), glm::mat4(1), 0.015f);
  //helloVk.loadModel(nvh::findFile("media/scenes/CornellBox-Original.obj", defaultSearchPaths, true), glm::mat4(1), 4.0f);
  //helloVk.loadModel(nvh::findFile("media/scenes/sibenik/sibenik.obj", defaultSearchPaths, true), glm::mat4(1), 1.0f);
  //helloVk.loadModel(nvh::findFile("media/scenes/Living_Room_2.obj", defaultSearchPaths, true), glm::mat4(1), 2.0f);
  
  helloVk.loadDebugMesh(nvh::findFile("media/scenes/sphere.obj", defaultSearchPaths, true), glm::mat4(1), 1.0f);

  // Probes are loaded here
  helloVk.prepareIndirectComponents(scene);

  helloVk.createOffscreenRender();
  helloVk.createDescriptorSetLayout();
  helloVk.createGraphicsPipeline();
  helloVk.createUniformBuffer();
  helloVk.createObjDescriptionBuffer();
  helloVk.updateDescriptorSet();

  // G Buffer Normals
  helloVk.createGBufferRender();
  helloVk.createGBufferPipeline();

  // #VKRay
  helloVk.initRayTracing();
  helloVk.createBottomLevelAS();
  helloVk.createTopLevelAS();
  helloVk.createRtDescriptorSet();
  helloVk.createRtPipeline();
  helloVk.createRtShaderBindingTable();

  helloVk.createIndirectPipeline();
  helloVk.createIndirectShaderBindingTable();
  
    // Debug
  helloVk.createDebugRender();
  helloVk.createDebugPipeline();
  
  helloVk.createPostDescriptor();
  helloVk.createPostPipeline();
  helloVk.updatePostDescriptorSet();


  glm::vec4 clearColor   = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
  glm::vec4 clearColor2   = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
  glm::vec4 clearColor3   = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
  bool      useRaytracer = true;
  bool      useIndirect  = true;


  helloVk.setupGlfwCallbacks(window);
  ImGui_ImplGlfw_InitForVulkan(window, true);



  // Main loop
  while(!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    if(helloVk.isMinimized())
      continue;

    // Start the Dear ImGui frame
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();


    // Show UI window.
    if(helloVk.showGui()) {
      ImGuiH::Panel::Begin();
      ImGui::ColorEdit3("Clear color", reinterpret_cast<float*>(&clearColor));
      ImGui::Checkbox("Ray Tracer mode", &useRaytracer);  // Switch between raster and ray tracing
      ImGui::Checkbox("Indirect lighting", &useIndirect);

      renderUI(helloVk, scene);
      ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
      ImGuiH::Control::Info("", "", "(F10) Toggle Pane", ImGuiH::Control::Flags::Disabled);
      ImGuiH::Panel::End();
    }


    // Start rendering the scene
    helloVk.prepareFrame();

    // Start command buffer of this frame
    auto                   curFrame = helloVk.getCurFrame();
    const VkCommandBuffer& cmdBuf   = helloVk.getCommandBuffers()[curFrame];

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    // Updating camera buffer
    helloVk.updateUniformBuffer(cmdBuf);
    helloVk.updateIndirectConstantsBuffer(cmdBuf, scene);




    // Clearing screen
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}};
    clearValues[1].depthStencil = {1.0f, 0};

    std::array<VkClearValue, 2> clearValues2{};
    clearValues2[0].color        = {{clearColor2[0], clearColor2[1], clearColor2[2], clearColor2[3]}};
    clearValues2[1].depthStencil = {1.0f, 0};

    std::array<VkClearValue, 5> clearValuesGBuffer{};
    clearValuesGBuffer[0].color  = {{clearColor3[0], clearColor3[1], clearColor3[2], clearColor3[3]}};
    clearValuesGBuffer[1].color  = {{clearColor3[0], clearColor3[1], clearColor3[2], clearColor3[3]}};
    clearValuesGBuffer[2].color  = {{clearColor3[0], clearColor3[1], clearColor3[2], clearColor3[3]}};
    clearValuesGBuffer[3].color  = {{clearColor3[0], clearColor3[1], clearColor3[2], clearColor3[3]}};
    clearValuesGBuffer[4].depthStencil = {1.0f, 0};
   
    
    // Normals GBuffer
    {
      VkRenderPassBeginInfo gBufferRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      gBufferRenderPassBeginInfo.renderPass      = helloVk.m_gBufferRenderPass;   // The render pass created earlier
      gBufferRenderPassBeginInfo.framebuffer     = helloVk.m_gBufferFramebuffer;  // The framebuffer created earlier
      gBufferRenderPassBeginInfo.renderArea      = {{0, 0}, helloVk.getSize()};   // The area to render to
      gBufferRenderPassBeginInfo.clearValueCount = static_cast<uint32_t>(clearValuesGBuffer.size());
      gBufferRenderPassBeginInfo.pClearValues    = clearValuesGBuffer.data();  // The clear values (for color and depth)
      
      vkCmdBeginRenderPass(cmdBuf, &gBufferRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
      helloVk.gBufferBegin(cmdBuf);
      vkCmdEndRenderPass(cmdBuf);
    }


    
    // Indirect Pass
    {
      VkRenderPassBeginInfo indirectPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      indirectPassBeginInfo.clearValueCount = 2;
      indirectPassBeginInfo.pClearValues    = clearValues.data();
      indirectPassBeginInfo.renderPass      = helloVk.m_IndirectRenderPass;
      indirectPassBeginInfo.framebuffer     = helloVk.m_IndirectFramebuffer;
      indirectPassBeginInfo.renderArea      = {{0, 0}, helloVk.getSize()};
      if(useIndirect) {
        helloVk.IndirectBegin(cmdBuf, clearColor, scene);
      }
    }



    // Offscreen render pass
    {
      VkRenderPassBeginInfo offscreenRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      offscreenRenderPassBeginInfo.clearValueCount = 2;
      offscreenRenderPassBeginInfo.pClearValues    = clearValues.data();
      offscreenRenderPassBeginInfo.renderPass      = helloVk.m_offscreenRenderPass;
      offscreenRenderPassBeginInfo.framebuffer     = helloVk.m_offscreenFramebuffer;
      offscreenRenderPassBeginInfo.renderArea      = {{0, 0}, helloVk.getSize()};

      // Rendering Scene
      if(useRaytracer) {
        helloVk.raytrace(cmdBuf, clearColor);

      } else {
        vkCmdBeginRenderPass(cmdBuf, &offscreenRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        helloVk.rasterize(cmdBuf);

        vkCmdEndRenderPass(cmdBuf);
      }
    }

    
    // Debug Pass
    {
      VkRenderPassBeginInfo debugRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      debugRenderPassBeginInfo.clearValueCount = 2;  
      debugRenderPassBeginInfo.pClearValues    = clearValues2.data();
      debugRenderPassBeginInfo.renderPass      = helloVk.m_debugRenderPass;
      debugRenderPassBeginInfo.framebuffer = helloVk.m_debugFramebuffer;  // Make sure this uses the same depth attachment
      debugRenderPassBeginInfo.renderArea = {{0, 0}, helloVk.getSize()};

      if(scene.gi_show_probes)
      {
        vkCmdBeginRenderPass(cmdBuf, &debugRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        helloVk.drawDebug(cmdBuf);
        vkCmdEndRenderPass(cmdBuf);
      }
    }


    
    // 2nd rendering pass: tone mapper, UI
    {
      VkRenderPassBeginInfo postRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      postRenderPassBeginInfo.clearValueCount = 2;
      postRenderPassBeginInfo.pClearValues    = clearValues.data();
      postRenderPassBeginInfo.renderPass      = helloVk.getRenderPass();
      postRenderPassBeginInfo.framebuffer     = helloVk.getFramebuffers()[curFrame];
      postRenderPassBeginInfo.renderArea      = {{0, 0}, helloVk.getSize()};

      // Rendering tonemapper
      vkCmdBeginRenderPass(cmdBuf, &postRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
      helloVk.drawPost(cmdBuf, useIndirect, scene.gi_show_probes);
      // Rendering UI
      ImGui::Render();
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdBuf);
      vkCmdEndRenderPass(cmdBuf);
    }
    
    


    // Submit for display
    vkEndCommandBuffer(cmdBuf);
    helloVk.submitFrame();
  }

  // Cleanup
  vkDeviceWaitIdle(helloVk.getDevice());

  helloVk.destroyResources();
  helloVk.destroy();
  vkctx.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
