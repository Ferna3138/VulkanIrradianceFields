#pragma once

#include "nvvkhl/appbase_vk.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/memallocator_dma_vk.hpp"
#include "nvvk/resourceallocator_vk.hpp"
#include "shaders/host_device.h"
//#include "ProbeVolume.h"

#include "Gpu_Constants.h"
#include "Probe_Volume.h"


// #VKRay
#include "nvvk/raytraceKHR_vk.hpp"

//--------------------------------------------------------------------------------------------------
// Simple rasterizer of OBJ objects
// - Each OBJ loaded are stored in an `ObjModel` and referenced by a `ObjInstance`
// - It is possible to have many `ObjInstance` referencing the same `ObjModel`
// - Rendering is done in an offscreen framebuffer
// - The image of the framebuffer is displayed in post-process in a full-screen quad
//
class HelloVulkan : public nvvkhl::AppBaseVk {

public:
  void setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice, uint32_t queueFamily) override;
  void createDescriptorSetLayout();
  void createGraphicsPipeline();
  void loadModel(const std::string& filename, glm::mat4 transform = glm::mat4(1), float scaleFactor = 1);
  void updateDescriptorSet();
  void createUniformBuffer();
  void createObjDescriptionBuffer();
  void createTextureImages(const VkCommandBuffer& cmdBuf, const std::vector<std::string>& textures);
  void updateUniformBuffer(const VkCommandBuffer& cmdBuf);
  void onResize(int /*w*/, int /*h*/) override;
  void destroyResources();
  void rasterize(const VkCommandBuffer& cmdBuff);

  // The OBJ model
  struct ObjModel {
    uint32_t     nbIndices{0};
    uint32_t     nbVertices{0};
    nvvk::Buffer vertexBuffer;    // Device buffer of all 'Vertex'
    nvvk::Buffer indexBuffer;     // Device buffer of the indices forming triangles
    nvvk::Buffer matColorBuffer;  // Device buffer of array of 'Wavefront material'
    nvvk::Buffer matIndexBuffer;  // Device buffer of array of 'Wavefront material'
  };

  struct ObjInstance {
    glm::mat4 transform;    // Matrix of the instance
    glm::mat4 invTransform = glm::inverse(transform);
    uint32_t  objIndex{0};  // Model index reference
  };


  // Information pushed at each draw call
  PushConstantRaster m_pcRaster{
      {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},  // Identity matrix
      {-4.0f, 1.f, 0.3f},                                 // light position
      0,                                                 // instance Id
      150.f,                                             // light intensity
      0                                                  // light type
  };

  PushConstantDebug m_pcDebug{
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},  // Identity matrix
        0                                                  // instance Id
  };


  // Array of objects and instances in the scene
  std::vector<ObjModel>    m_objModel;   // Model on host
  std::vector<ObjDesc>     m_objDesc;    // Model description for device access
  std::vector<ObjInstance> m_instances;  // Scene model instances

  // Graphic pipeline
  VkPipelineLayout            m_pipelineLayout;
  VkPipeline                  m_graphicsPipeline;
  nvvk::DescriptorSetBindings m_descSetLayoutBind;
  VkDescriptorPool            m_descPool;
  VkDescriptorSetLayout       m_descSetLayout;
  VkDescriptorSet             m_descSet;

  nvvk::Buffer m_bGlobals;  // Device-Host of the camera matrices
  nvvk::Buffer m_bObjDesc;  // Device buffer of the OBJ descriptions


  std::vector<nvvk::Texture> m_textures;  // vector of all textures of the scene


  nvvk::ResourceAllocatorDma m_alloc;  // Allocator for buffer, images, acceleration structures
  nvvk::DebugUtil            m_debug;  // Utility to name objects


  // #Post - Draw the rendered image on a quad using a tonemapper
  void createOffscreenRender();
  void createPostPipeline();
  void createPostDescriptor();
  void updatePostDescriptorSet();
  void drawPost(VkCommandBuffer cmdBuf, bool useIndirect, bool showProbes);

  PushConstantPost pcPost{};

  nvvk::DescriptorSetBindings m_postDescSetLayoutBind;
  VkDescriptorPool            m_postDescPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout       m_postDescSetLayout{VK_NULL_HANDLE};
  VkDescriptorSet             m_postDescSet{VK_NULL_HANDLE};
  VkPipeline                  m_postPipeline{VK_NULL_HANDLE};
  VkPipelineLayout            m_postPipelineLayout{VK_NULL_HANDLE};
  VkRenderPass                m_offscreenRenderPass{VK_NULL_HANDLE};
  VkFramebuffer               m_offscreenFramebuffer{VK_NULL_HANDLE};
  nvvk::Texture               m_offscreenColor;
  nvvk::Texture               m_offscreenDepth;
  VkFormat                    m_offscreenColorFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat                    m_offscreenDepthFormat{VK_FORMAT_X8_D24_UNORM_PACK32};





  //////////////////////////////////////////////////////////////////////////
  // Ray Tracing
  //////////////////////////////////////////////////////////////////////////

  void initRayTracing();
  auto objectToVkGeometryKHR(const ObjModel& model);
  void createBottomLevelAS();
  void createTopLevelAS();
  void createRtDescriptorSet();
  void updateRtDescriptorSet();
  void createRtPipeline();
  void createRtShaderBindingTable();
  void raytrace(const VkCommandBuffer& cmdBuf, const glm::vec4& clearColor);


  VkPhysicalDeviceRayTracingPipelinePropertiesKHR   m_rtProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  nvvk::RaytracingBuilderKHR                        m_rtBuilder;
  nvvk::DescriptorSetBindings                       m_rtDescSetLayoutBind;
  VkDescriptorPool                                  m_rtDescPool;
  VkDescriptorSetLayout                             m_rtDescSetLayout;
  VkDescriptorSet                                   m_rtDescSet;
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_rtShaderGroups;
  VkPipelineLayout                                  m_rtPipelineLayout;
  VkPipeline                                        m_rtPipeline;

  nvvk::Buffer                    m_rtSBTBuffer;
  VkStridedDeviceAddressRegionKHR m_rgenRegion{};
  VkStridedDeviceAddressRegionKHR m_missRegion{};
  VkStridedDeviceAddressRegionKHR m_hitRegion{};
  VkStridedDeviceAddressRegionKHR m_callRegion{};

  // Push constant for ray tracer
  PushConstantRay m_pcRay{};



  //////////////////////////////////////////////////////////////////////////
  // Irradiance Fields
  //////////////////////////////////////////////////////////////////////////
  Probe_Volume volume;

  VkRenderPass  m_IndirectRenderPass{VK_NULL_HANDLE};
  VkFramebuffer m_IndirectFramebuffer{VK_NULL_HANDLE};


  void createIndirectPipeline();
  void createIndirectShaderBindingTable();
  void createComputePipeline(const std::string& shaderPath,
                             std::vector<VkDescriptorSetLayout> IndirectDescSetLayouts,
                             VkPipelineLayout& pipelineLayout,
                             VkPipeline& pipeline,
                             const void* pushConstants,
                             uint32_t pushConstantsSize);


  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_IndirectShaderGroups;
  VkPipelineLayout                                  m_IndirectPipelineLayout;
  VkPipeline                                        m_IndirectPipeline;

  nvvk::Buffer                    m_IndirectSBTBuffer;
  VkStridedDeviceAddressRegionKHR m_IndirectRgenRegion{};
  VkStridedDeviceAddressRegionKHR m_IndirectMissRegion{};
  VkStridedDeviceAddressRegionKHR m_IndirectHitRegion{};
  VkStridedDeviceAddressRegionKHR m_IndirectCallRegion{};

  // Push constant for ray tracer
  PushConstantOffset m_pcProbeOffsets{};
  PushConstantStatus m_pcProbeStatus{};
  PushConstantSample m_pcSampleIrradiance{};

  // Textures Vector
  std::vector<nvvk::Texture> m_storageImages;
  std::vector<VkImageView>   m_storageImageViews;
  
  std::vector<nvvk::Texture> m_globalTextures;
  std::vector<VkImageView> m_globalTexturesView;
  std::vector<VkSampler> m_globalTextureSamplers;


  nvvk::Image              m_indirectImage;
  nvvk::Texture            m_indirectTexture;

  nvvk::Image              m_radianceImage;
  nvvk::Texture            m_radianceTexture;

  nvvk::Image              m_offsetsImage;
  nvvk::Texture            m_offsetsTexture;
  
  nvvk::Image              m_irradianceImage;
  nvvk::Texture            m_irradianceTexture;
  
  nvvk::Image              m_visibilityImage;
  nvvk::Texture            m_visibilityTexture;

  nvvk::Image createStorageImage(const VkCommandBuffer& cmdBuf, VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, VkFormat format);

  // Buffers
  nvvk::Buffer m_bIndirectConstants;
  nvvk::Buffer m_bIndirectStatus;

  // Compute Pipelines
  VkPipelineLayout m_probeOffsetsPipelineLayout;
  VkPipeline       m_probeOffsetsPipeline;

  VkPipelineLayout m_probeStatusPipelineLayout;
  VkPipeline       m_probeStatusPipeline;

  VkPipelineLayout m_probeUpdateIrradiancePipelineLayout;
  VkPipeline       m_probeUpdateIrradiancePipeline;
  
  VkPipelineLayout m_probeUpdateVisibilityPipelineLayout;
  VkPipeline       m_probeUpdateVisibilityPipeline;

  VkPipelineLayout m_sampleIrradiancePipelineLayout;
  VkPipeline       m_sampleIrradiancePipeline;

  void createIndirectConstantsBuffer();
  void createIndirectStatusBuffer();

  void updateIndirectConstantsBuffer(const VkCommandBuffer& cmdBuf, renderSceneVolume& scene);

  void IndirectBegin(const VkCommandBuffer& cmdBuf, const glm::vec4& clearColor, renderSceneVolume& scene);

  void prepareIndirectComponents(renderSceneVolume& scene);
  

  //////////////////////////////////////////////////////////////////////////
  // G Buffer
  //////////////////////////////////////////////////////////////////////////
  VkPipelineLayout m_gBufferPipelineLayout;
  VkPipeline       m_gBufferPipeline;
  VkRenderPass     m_gBufferRenderPass{VK_NULL_HANDLE};
  VkFramebuffer    m_gBufferFramebuffer{VK_NULL_HANDLE};

  void createGBufferRender();
  void gBufferBegin(const VkCommandBuffer& cmdBuff);
  void createGBufferPipeline();

  nvvk::Texture m_gBufferNormals;
  nvvk::Texture m_gBufferDepthTexture;
  nvvk::Texture m_gBufferAlbedo;
  nvvk::Texture m_gBufferDiffuse;
  nvvk::Texture m_gBufferDepth;

  VkFormat      m_gBufferNormalFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat      m_gBufferDepthTextureFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat      m_gBufferAlbedoFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat      m_gBufferDiffuseFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat      m_gBufferDepthFormat{VK_FORMAT_X8_D24_UNORM_PACK32};




  //////////////////////////////////////////////////////////////////////////
  // Debug Pass
  //////////////////////////////////////////////////////////////////////////
  void loadDebugMesh(const std::string& filename, glm::mat4 transform = glm::mat4(1), float scaleFactor = 1);
  std::vector<ObjModel>    m_debugObjModel;   // Model on host
  std::vector<ObjDesc>     m_debugObjDesc;    // Model description for device access
  std::vector<ObjInstance> m_debugInstances;  // Scene model instances


  VkPipelineLayout m_debugPipelineLayout;
  VkPipeline       m_debugPipeline;
  VkRenderPass     m_debugRenderPass{VK_NULL_HANDLE};
  VkFramebuffer    m_debugFramebuffer{VK_NULL_HANDLE};

  void createDebugRender();
  void createDebugPipeline();
  void drawDebug(VkCommandBuffer cmdBuf);

  nvvk::Texture m_debugTexture;
  nvvk::Texture m_debugDepth;
  VkFormat      m_debugTextureFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat      m_debugDepthFormat{VK_FORMAT_X8_D24_UNORM_PACK32};
  




  VkQueryPool queryPool;
};