#include <sstream>

#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "obj_loader.h"
#include "stb_image.h"

#include "hello_vulkan.h"
#include "nvh/alignment.hpp"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvk/renderpasses_vk.hpp"
#include "nvvk/shaders_vk.hpp"
#include "nvvk/buffers_vk.hpp"

#include "utility.h"



extern std::vector<std::string> defaultSearchPaths;


//--------------------------------------------------------------------------------------------------
// Keep the handle on the device
// Initialize the tool to do all our allocations: buffers, images
//
void HelloVulkan::setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice, uint32_t queueFamily) {
  AppBaseVk::setup(instance, device, physicalDevice, queueFamily);
  m_alloc.init(instance, device, physicalDevice);
  m_debug.setup(m_device);
  m_offscreenDepthFormat = nvvk::findDepthFormat(physicalDevice);
  m_gBufferDepthFormat   = nvvk::findDepthFormat(physicalDevice);
  m_debugDepthFormat     = nvvk::findDepthFormat(physicalDevice);

  VkQueryPoolCreateInfo queryPoolInfo = {};
  queryPoolInfo.sType                 = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  queryPoolInfo.queryType             = VK_QUERY_TYPE_TIMESTAMP;
  queryPoolInfo.queryCount            = 7;  // Adjust the count as per your needs

  vkCreateQueryPool(m_device, &queryPoolInfo, nullptr, &queryPool);
}



//--------------------------------------------------------------------------------------------------
// Called at each frame to update the camera matrix
//
void HelloVulkan::updateUniformBuffer(const VkCommandBuffer& cmdBuf) {
  // Prepare new UBO contents on host.
  const float    aspectRatio = m_size.width / static_cast<float>(m_size.height);
  GlobalUniforms hostUBO     = {};
  const auto&    view        = CameraManip.getMatrix();
  glm::vec3      pos         = CameraManip.getEye();
  glm::mat4      proj        = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, 0.1f, 1000.0f);
  proj[1][1] *= -1;  // Inverting Y for Vulkan (not needed with perspectiveVK).

  hostUBO.viewProj    = proj * view;
  hostUBO.viewInverse = glm::inverse(view);
  hostUBO.projInverse = glm::inverse(proj);
  hostUBO.view        = view;
  hostUBO.projection  = proj;
  hostUBO.position    = pos;

  // UBO on the device, and what stages access it.
  VkBuffer deviceUBO      = m_bGlobals.buffer;
  auto     uboUsageStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

  // Ensure that the modified UBO is not visible to previous frames.
  VkBufferMemoryBarrier beforeBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  beforeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  beforeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  beforeBarrier.buffer        = deviceUBO;
  beforeBarrier.offset        = 0;
  beforeBarrier.size          = sizeof(hostUBO);
  vkCmdPipelineBarrier(cmdBuf, uboUsageStages, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0,
                       nullptr, 1, &beforeBarrier, 0, nullptr);


  // Schedule the host-to-device upload. (hostUBO is copied into the cmd
  // buffer so it is okay to deallocate when the function returns).
  vkCmdUpdateBuffer(cmdBuf, m_bGlobals.buffer, 0, sizeof(GlobalUniforms), &hostUBO);

  // Making sure the updated UBO will be visible.
  VkBufferMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  afterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  afterBarrier.buffer        = deviceUBO;
  afterBarrier.offset        = 0;
  afterBarrier.size          = sizeof(hostUBO);
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, uboUsageStages, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0,
                       nullptr, 1, &afterBarrier, 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Describing the layout pushed when rendering
//
void HelloVulkan::createDescriptorSetLayout() {
  auto nbTxt = static_cast<uint32_t>(m_textures.size());

  // Camera matrices
  m_descSetLayoutBind.addBinding(SceneBindings::eGlobals, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT);
  // Obj descriptions
  m_descSetLayoutBind.addBinding(SceneBindings::eObjDescs, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
  // Textures
  m_descSetLayoutBind.addBinding(SceneBindings::eTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nbTxt,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);


  m_descSetLayout = m_descSetLayoutBind.createLayout(m_device);
  m_descPool      = m_descSetLayoutBind.createPool(m_device, 1);
  m_descSet       = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Setting up the buffers in the descriptor set
//
void HelloVulkan::updateDescriptorSet() {
  std::vector<VkWriteDescriptorSet> writes;

  // Camera matrices and scene description
  VkDescriptorBufferInfo dbiUnif{m_bGlobals.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, SceneBindings::eGlobals, &dbiUnif));

  VkDescriptorBufferInfo dbiSceneDesc{m_bObjDesc.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, SceneBindings::eObjDescs, &dbiSceneDesc));

  // All texture samplers
  std::vector<VkDescriptorImageInfo> diit;
  for(auto& texture : m_textures) {
    diit.emplace_back(texture.descriptor);
  }
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, SceneBindings::eTextures, diit.data()));

  // Writing the information
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}



//--------------------------------------------------------------------------------------------------
// Creating the pipeline layout
//
void HelloVulkan::createGraphicsPipeline() {
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantRaster)};

  // Creating the Pipeline Layout
  VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  createInfo.setLayoutCount         = 1;
  createInfo.pSetLayouts            = &m_descSetLayout;
  createInfo.pushConstantRangeCount = 1;
  createInfo.pPushConstantRanges    = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &createInfo, nullptr, &m_pipelineLayout);


  // Creating the Pipeline
  std::vector<std::string>                paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_pipelineLayout, m_offscreenRenderPass);
  gpb.depthStencilState.depthTestEnable = true;

  gpb.addShader(nvh::loadFile("spv/vert_shader.vert.spv", true, paths, true), VK_SHADER_STAGE_VERTEX_BIT);
  gpb.addShader(nvh::loadFile("spv/frag_shader.frag.spv", true, paths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  gpb.addBindingDescription({0, sizeof(VertexObj)});
  
  gpb.addAttributeDescriptions({
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, pos))},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, nrm))},
      {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, color))},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, texCoord))},
  });

  std::array<VkPipelineColorBlendAttachmentState, 2> colorBlendAttachments = {};
  for(auto& blendAttachment : colorBlendAttachments) {
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;
  }

  VkPipelineColorBlendStateCreateInfo colorBlending = {};
  colorBlending.sType                               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable                       = VK_FALSE;
  colorBlending.attachmentCount                     = static_cast<uint32_t>(colorBlendAttachments.size());
  colorBlending.pAttachments                        = colorBlendAttachments.data();
  gpb.colorBlendState                               = colorBlending;


  m_graphicsPipeline = gpb.createPipeline();
  m_debug.setObjectName(m_graphicsPipeline, "Graphics");
}


//--------------------------------------------------------------------------------------------------
// Loading the OBJ file and setting up all buffers
//
void HelloVulkan::loadModel(const std::string& filename, glm::mat4 transform, float scaleFactor) {
  LOGI("Loading File:  %s \n", filename.c_str());
  ObjLoader loader;
  loader.loadModel(filename);

  // Converting from Srgb to linear
  for(auto& m : loader.m_materials) {
    m.ambient  = glm::pow(m.ambient, glm::vec3(2.2f));
    m.diffuse  = glm::pow(m.diffuse, glm::vec3(2.2f));
    m.specular = glm::pow(m.specular, glm::vec3(2.2f));
  }

  ObjModel model;
  model.nbIndices  = static_cast<uint32_t>(loader.m_indices.size());
  model.nbVertices = static_cast<uint32_t>(loader.m_vertices.size());

  // Create the buffers on Device and copy vertices, indices and materials
  nvvk::CommandPool  cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer    cmdBuf          = cmdBufGet.createCommandBuffer();
  VkBufferUsageFlags flag            = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VkBufferUsageFlags rayTracingFlags =  // used also for building acceleration structures
      flag | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  model.vertexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rayTracingFlags);
  model.indexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rayTracingFlags);
  model.matColorBuffer = m_alloc.createBuffer(cmdBuf, loader.m_materials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | flag);
  model.matIndexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_matIndx, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | flag);
  // Creates all textures found and find the offset for this model
  auto txtOffset = static_cast<uint32_t>(m_textures.size());
  createTextureImages(cmdBuf, loader.m_textures);
  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();

  std::string objNb = std::to_string(m_objModel.size());
  m_debug.setObjectName(model.vertexBuffer.buffer, (std::string("vertex_" + objNb)));
  m_debug.setObjectName(model.indexBuffer.buffer, (std::string("index_" + objNb)));
  m_debug.setObjectName(model.matColorBuffer.buffer, (std::string("mat_" + objNb)));
  m_debug.setObjectName(model.matIndexBuffer.buffer, (std::string("matIdx_" + objNb)));

  // Keeping transformation matrix of the instance
  ObjInstance instance;

  glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(scaleFactor));
  transform             = transform * scaleMatrix;

  
  instance.transform = transform;
  instance.objIndex  = static_cast<uint32_t>(m_objModel.size());
  m_instances.push_back(instance);

  // Creating information for device access
  ObjDesc desc;
  desc.txtOffset            = txtOffset;
  desc.vertexAddress        = nvvk::getBufferDeviceAddress(m_device, model.vertexBuffer.buffer);
  desc.indexAddress         = nvvk::getBufferDeviceAddress(m_device, model.indexBuffer.buffer);
  desc.materialAddress      = nvvk::getBufferDeviceAddress(m_device, model.matColorBuffer.buffer);
  desc.materialIndexAddress = nvvk::getBufferDeviceAddress(m_device, model.matIndexBuffer.buffer);

  // Keeping the obj host model and device description
  m_objModel.emplace_back(model);
  m_objDesc.emplace_back(desc);
}



void HelloVulkan::loadDebugMesh(const std::string& filename, glm::mat4 transform, float scaleFactor) {
  LOGI("Loading Debug Mesh File:  %s \n", filename.c_str());
  ObjLoader loader;
  loader.loadModel(filename);

  // Converting from Srgb to linear
  for(auto& m : loader.m_materials)
  {
    m.ambient  = glm::pow(m.ambient, glm::vec3(2.2f));
    m.diffuse  = glm::pow(m.diffuse, glm::vec3(2.2f));
    m.specular = glm::pow(m.specular, glm::vec3(2.2f));
  }

  ObjModel model;
  model.nbIndices  = static_cast<uint32_t>(loader.m_indices.size());
  model.nbVertices = static_cast<uint32_t>(loader.m_vertices.size());

  // Create the buffers on Device and copy vertices, indices and materials
  nvvk::CommandPool  cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer    cmdBuf          = cmdBufGet.createCommandBuffer();

  VkBufferUsageFlags flag            = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VkBufferUsageFlags rayTracingFlags =  // used also for building acceleration structures
      flag | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

  model.vertexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rayTracingFlags);
  model.indexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rayTracingFlags);
  model.matColorBuffer = m_alloc.createBuffer(cmdBuf, loader.m_materials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | flag);
  model.matIndexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_matIndx, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | flag);
  // Creates all textures found and find the offset for this model
  auto txtOffset = static_cast<uint32_t>(m_textures.size());
  createTextureImages(cmdBuf, loader.m_textures);
  
  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();

  std::string objNb = std::to_string(m_debugObjModel.size());
  m_debug.setObjectName(model.vertexBuffer.buffer, (std::string("vertex_" + objNb)));
  m_debug.setObjectName(model.indexBuffer.buffer, (std::string("index_" + objNb)));
  m_debug.setObjectName(model.matColorBuffer.buffer, (std::string("mat_" + objNb)));
  m_debug.setObjectName(model.matIndexBuffer.buffer, (std::string("matIdx_" + objNb)));

  // Keeping transformation matrix of the instance
  ObjInstance instance;

  glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(scaleFactor));
  transform             = transform * scaleMatrix;


  instance.transform = transform;
  instance.objIndex  = static_cast<uint32_t>(m_debugObjModel.size());
  m_debugInstances.push_back(instance);

  // Creating information for device access
  ObjDesc desc;
  desc.txtOffset            = txtOffset;
  desc.vertexAddress        = nvvk::getBufferDeviceAddress(m_device, model.vertexBuffer.buffer);
  desc.indexAddress         = nvvk::getBufferDeviceAddress(m_device, model.indexBuffer.buffer);
  desc.materialAddress      = nvvk::getBufferDeviceAddress(m_device, model.matColorBuffer.buffer);
  desc.materialIndexAddress = nvvk::getBufferDeviceAddress(m_device, model.matIndexBuffer.buffer);

  // Keeping the obj host model and device description
  m_debugObjModel.emplace_back(model);
  m_debugObjDesc.emplace_back(desc);
}



//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the camera matrices
// - Buffer is host visible
//
void HelloVulkan::createUniformBuffer() {
  m_bGlobals = m_alloc.createBuffer(sizeof(GlobalUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_debug.setObjectName(m_bGlobals.buffer, "Globals");
}

//--------------------------------------------------------------------------------------------------
// Create a storage buffer containing the description of the scene elements
// - Which geometry is used by which instance
// - Transformation
// - Offset for texture
//
void HelloVulkan::createObjDescriptionBuffer() {
  nvvk::CommandPool cmdGen(m_device, m_graphicsQueueIndex);

  auto cmdBuf = cmdGen.createCommandBuffer();
  m_bObjDesc  = m_alloc.createBuffer(cmdBuf, m_objDesc, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  cmdGen.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();
  m_debug.setObjectName(m_bObjDesc.buffer, "ObjDescs");
}


//--------------------------------------------------------------------------------------------------
// Creating all textures and samplers
//
void HelloVulkan::createTextureImages(const VkCommandBuffer& cmdBuf, const std::vector<std::string>& textures) {
  VkSamplerCreateInfo samplerCreateInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samplerCreateInfo.minFilter  = VK_FILTER_LINEAR;
  samplerCreateInfo.magFilter  = VK_FILTER_LINEAR;
  samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerCreateInfo.maxLod     = FLT_MAX;

  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

  // If no textures are present, create a dummy one to accommodate the pipeline layout
  if(textures.empty() && m_textures.empty()) {
    nvvk::Texture texture;

    std::array<uint8_t, 4> color{255u, 255u, 255u, 255u};
    VkDeviceSize           bufferSize      = sizeof(color);
    auto                   imgSize         = VkExtent2D{1, 1};
    auto                   imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format);

    // Creating the dummy texture
    nvvk::Image           image  = m_alloc.createImage(cmdBuf, bufferSize, color.data(), imageCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
    texture                      = m_alloc.createTexture(image, ivInfo, samplerCreateInfo);

    // The image format must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    nvvk::cmdBarrierImageLayout(cmdBuf, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_textures.push_back(texture);
  }
  else {
    // Uploading all images
    for(const auto& texture : textures) {
      std::stringstream o;
      int               texWidth, texHeight, texChannels;
      o << "media/textures/" << texture;
      std::string txtFile = nvh::findFile(o.str(), defaultSearchPaths, true);

      stbi_uc* stbi_pixels = stbi_load(txtFile.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

      std::array<stbi_uc, 4> color{255u, 0u, 255u, 255u};

      stbi_uc* pixels = stbi_pixels;
      // Handle failure
      if(!stbi_pixels) {
        texWidth = texHeight = 1;
        texChannels          = 4;
        pixels               = reinterpret_cast<stbi_uc*>(color.data());
      }

      VkDeviceSize bufferSize      = static_cast<uint64_t>(texWidth) * texHeight * sizeof(uint8_t) * 4;
      auto         imgSize         = VkExtent2D{(uint32_t)texWidth, (uint32_t)texHeight};
      auto         imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format, VK_IMAGE_USAGE_SAMPLED_BIT, true);

      {
        nvvk::Image image = m_alloc.createImage(cmdBuf, bufferSize, pixels, imageCreateInfo);
        nvvk::cmdGenerateMipmaps(cmdBuf, image.image, format, imgSize, imageCreateInfo.mipLevels);
        VkImageViewCreateInfo ivInfo  = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
        nvvk::Texture         texture = m_alloc.createTexture(image, ivInfo, samplerCreateInfo);

        m_textures.push_back(texture);
      }

      stbi_image_free(stbi_pixels);
    }
  }
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void HelloVulkan::destroyResources() {
  vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);

  m_alloc.destroy(m_bGlobals);
  m_alloc.destroy(m_bObjDesc);
  m_alloc.destroy(m_bIndirectConstants);
  m_alloc.destroy(m_bIndirectStatus);


  for(auto& m : m_objModel) {
    m_alloc.destroy(m.vertexBuffer);
    m_alloc.destroy(m.indexBuffer);
    m_alloc.destroy(m.matColorBuffer);
    m_alloc.destroy(m.matIndexBuffer);
  }
  
  for(auto& m : m_debugObjModel){
    m_alloc.destroy(m.vertexBuffer);
    m_alloc.destroy(m.indexBuffer);
    m_alloc.destroy(m.matColorBuffer);
    m_alloc.destroy(m.matIndexBuffer);
  }


  for(auto& t : m_textures) {
    m_alloc.destroy(t);
  }
  


  m_alloc.destroy(m_radianceTexture);
  m_alloc.destroy(m_irradianceTexture);
  m_alloc.destroy(m_offsetsTexture);
  m_alloc.destroy(m_visibilityTexture);
  m_alloc.destroy(m_indirectTexture);
  
  
  for(auto& sample : m_globalTextureSamplers) {
    vkDestroySampler(m_device, sample, nullptr);
  }


  // G Buffer
  m_alloc.destroy(m_gBufferNormals);
  m_alloc.destroy(m_gBufferDepth);
  m_alloc.destroy(m_gBufferAlbedo);
  m_alloc.destroy(m_gBufferDiffuse);
  vkDestroyPipeline(m_device, m_gBufferPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_gBufferPipelineLayout, nullptr);
  vkDestroyRenderPass(m_device, m_gBufferRenderPass, nullptr);
  vkDestroyFramebuffer(m_device, m_gBufferFramebuffer, nullptr);


  //#Post
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_offscreenDepth);
  vkDestroyPipeline(m_device, m_postPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_postPipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_postDescPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_postDescSetLayout, nullptr);
  vkDestroyRenderPass(m_device, m_offscreenRenderPass, nullptr);
  vkDestroyFramebuffer(m_device, m_offscreenFramebuffer, nullptr);

  // Indirect
  vkDestroyPipeline(m_device, m_IndirectPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_IndirectPipelineLayout, nullptr);
  // Compute
  vkDestroyPipeline(m_device, m_probeOffsetsPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_probeOffsetsPipelineLayout, nullptr);
  vkDestroyPipeline(m_device, m_probeStatusPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_probeStatusPipelineLayout, nullptr);
  vkDestroyPipeline(m_device, m_probeUpdateIrradiancePipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_probeUpdateIrradiancePipelineLayout, nullptr);
  vkDestroyPipeline(m_device, m_probeUpdateVisibilityPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_probeUpdateVisibilityPipelineLayout, nullptr);
  vkDestroyPipeline(m_device, m_sampleIrradiancePipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_sampleIrradiancePipelineLayout, nullptr);

  vkDestroyRenderPass(m_device, m_IndirectRenderPass, nullptr);
  vkDestroyFramebuffer(m_device, m_IndirectFramebuffer, nullptr);
  m_alloc.destroy(m_IndirectSBTBuffer);



  // Debug
  m_alloc.destroy(m_debugTexture);
  m_alloc.destroy(m_debugDepth);
  vkDestroyPipeline(m_device, m_debugPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_debugPipelineLayout, nullptr);
  vkDestroyRenderPass(m_device, m_debugRenderPass, nullptr);
  vkDestroyFramebuffer(m_device, m_debugFramebuffer, nullptr);


  // #VKRay
  m_rtBuilder.destroy();
  vkDestroyPipeline(m_device, m_rtPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_rtPipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_rtDescPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_rtDescSetLayout, nullptr);
  m_alloc.destroy(m_rtSBTBuffer);


  
  m_alloc.deinit();
}


//--------------------------------------------------------------------------------------------------
// Drawing the scene in raster mode
//
void HelloVulkan::rasterize(const VkCommandBuffer& cmdBuf) {
  VkDeviceSize offset{0};

  m_debug.beginLabel(cmdBuf, "Rasterize");

  // Dynamic Viewport
  setViewport(cmdBuf);

  // Drawing all triangles
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);


  for(const HelloVulkan::ObjInstance& inst : m_instances) {
    auto& model            = m_objModel[inst.objIndex];
    m_pcRaster.objIndex    = inst.objIndex;  // Telling which object is drawn
    m_pcRaster.modelMatrix = inst.transform;

    vkCmdPushConstants(cmdBuf, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantRaster), &m_pcRaster);
    vkCmdBindVertexBuffers(cmdBuf, 0, 1, &model.vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmdBuf, model.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmdBuf, model.nbIndices, 1, 0, 0, 0);
  }
  m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void HelloVulkan::onResize(int /*w*/, int /*h*/) {
  createOffscreenRender();
  
  createGBufferRender();

  updatePostDescriptorSet();
  updateRtDescriptorSet();
}


//////////////////////////////////////////////////////////////////////////
// Post-processing
//////////////////////////////////////////////////////////////////////////


//--------------------------------------------------------------------------------------------------
// Creating an offscreen frame buffer and the associated render pass
//

void HelloVulkan::createOffscreenRender() {
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_offscreenDepth);

  // Creating the color image
  {
    auto        colorCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_offscreenColorFormat,
                                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                                  | VK_IMAGE_USAGE_STORAGE_BIT);
    nvvk::Image image           = m_alloc.createImage(colorCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, colorCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_offscreenColor                        = m_alloc.createTexture(image, ivInfo, sampler);
    m_offscreenColor.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the depth buffer
  {
    auto depthCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_offscreenDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    nvvk::Image image = m_alloc.createImage(depthCreateInfo);
    VkImageViewCreateInfo depthStencilView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthStencilView.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    depthStencilView.format           = m_offscreenDepthFormat;
    depthStencilView.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthStencilView.image            = image.image;

    m_offscreenDepth = m_alloc.createTexture(image, depthStencilView);
  }


  // Setting the image layout for both color and depth
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenColor.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Creating a render pass for the offscreen
  if(!m_offscreenRenderPass) {
    m_offscreenRenderPass = nvvk::createRenderPass (m_device, {m_offscreenColorFormat}, m_offscreenDepthFormat, 1, true, true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  }

  // Creating the framebuffer for offscreen
  std::vector<VkImageView> attachments = {m_offscreenColor.descriptor.imageView,
                                          m_offscreenDepth.descriptor.imageView};

  vkDestroyFramebuffer(m_device, m_offscreenFramebuffer, nullptr);
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass      = m_offscreenRenderPass;
  info.attachmentCount = static_cast<uint32_t>(attachments.size());
  info.pAttachments    = attachments.data();
  info.width           = m_size.width;
  info.height          = m_size.height;
  info.layers          = 1;
  vkCreateFramebuffer(m_device, &info, nullptr, &m_offscreenFramebuffer);
}



//--------------------------------------------------------------------------------------------------
// The pipeline is how things are rendered, which shaders, type of primitives, depth test and more
//
void HelloVulkan::createPostPipeline() {
  // Push constants in the fragment shader
  //VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantPost)};

  std::vector<VkDescriptorSetLayout> postDescSetLayouts2 = {m_rtDescSetLayout, m_descSetLayout, m_postDescSetLayout};
  // Creating the pipeline layout
  VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  createInfo.setLayoutCount         = postDescSetLayouts2.size();
  createInfo.pSetLayouts            = postDescSetLayouts2.data();
  createInfo.pushConstantRangeCount = 1;
  createInfo.pPushConstantRanges    = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &createInfo, nullptr, &m_postPipelineLayout);

  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_indirectTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Pipeline: completely generic, no vertices
  nvvk::GraphicsPipelineGeneratorCombined pipelineGenerator(m_device, m_postPipelineLayout, m_renderPass);
  pipelineGenerator.addShader(nvh::loadFile("spv/passthrough.vert.spv", true, defaultSearchPaths, true), VK_SHADER_STAGE_VERTEX_BIT);
  pipelineGenerator.addShader(nvh::loadFile("spv/post.frag.spv", true, defaultSearchPaths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  pipelineGenerator.rasterizationState.cullMode = VK_CULL_MODE_NONE;
  m_postPipeline                                = pipelineGenerator.createPipeline();
  m_debug.setObjectName(m_postPipeline, "post");
}

//--------------------------------------------------------------------------------------------------
// The descriptor layout is the description of the data that is passed to the vertex or the
// fragment program.
//
void HelloVulkan::createPostDescriptor() {
  m_postDescSetLayoutBind.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_postDescSetLayoutBind.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_postDescSetLayoutBind.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);

  //VkDescriptorImageInfo imageInfo{{}, m_indirectTexture.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};
  //VkDescriptorImageInfo debugImageInfo{{}, m_debugColor.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};


  // Add the indirect texture
  m_postDescSetLayout = m_postDescSetLayoutBind.createLayout(m_device);
  m_postDescPool      = m_postDescSetLayoutBind.createPool(m_device);
  m_postDescSet       = nvvk::allocateDescriptorSet(m_device, m_postDescPool, m_postDescSetLayout);

  //std::vector<VkWriteDescriptorSet> writes;
  //writes.emplace_back(m_postDescSetLayoutBind.makeWrite(m_postDescSet, 1, &imageInfo));
}


//--------------------------------------------------------------------------------------------------
// Update the output
//
void HelloVulkan::updatePostDescriptorSet() {
  VkWriteDescriptorSet writeDescriptorSets[3];
  writeDescriptorSets[0] = m_postDescSetLayoutBind.makeWrite(m_postDescSet, 0, &m_offscreenColor.descriptor);
  // Descriptor info for indirect texture
  writeDescriptorSets[1] = m_postDescSetLayoutBind.makeWrite(m_postDescSet, 1, &m_indirectTexture.descriptor);

  writeDescriptorSets[2] = m_postDescSetLayoutBind.makeWrite(m_postDescSet, 2, &m_debugTexture.descriptor);
  //writeDescriptorSets[2] = m_postDescSetLayoutBind.makeWrite(m_postDescSet, 2, &m_debugColor.descriptor);
  // Update both descriptors at once
  vkUpdateDescriptorSets(m_device, 3, writeDescriptorSets, 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Draw a full screen quad with the attached image
//
void HelloVulkan::drawPost(VkCommandBuffer cmdBuf, bool useIndirect, bool showProbes) {
  m_debug.beginLabel(cmdBuf, "Post");

  
  pcPost.indirect_enabled = useIndirect == true ? 1 : 0;
  pcPost.debug_enabled    = showProbes == true ? 1 : 0;
  pcPost.debug_texture    = volume.m_currentTextureDebug;
  pcPost.show_textures    = volume.m_showDebugTextures == true ? 1 : 0;
  setViewport(cmdBuf);

  auto aspectRatio = static_cast<float>(m_size.width) / static_cast<float>(m_size.height);

  pcPost.aspectRatio = aspectRatio;

  std::vector<VkDescriptorSet> descSets{m_rtDescSet, m_descSet, m_postDescSet};

  vkCmdPushConstants(cmdBuf, m_postPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantPost), &pcPost);
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipelineLayout, 0, descSets.size(), descSets.data(), 0, nullptr);
  vkCmdDraw(cmdBuf, 3, 1, 0, 0);

  m_debug.endLabel(cmdBuf);
}



//////////////////////////////////////////////////////////////////////////
// G Buffer
//////////////////////////////////////////////////////////////////////////
void HelloVulkan::createGBufferRender()
{
  // Destroying existing resources
  m_alloc.destroy(m_gBufferNormals);
  m_alloc.destroy(m_gBufferDepthTexture);
  m_alloc.destroy(m_gBufferAlbedo);
  m_alloc.destroy(m_gBufferDiffuse);
  m_alloc.destroy(m_gBufferDepth);

  // Creating the normal image (first colour attachment)
  {
    auto                  normalsCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_gBufferNormalFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    nvvk::Image           image             = m_alloc.createImage(normalsCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, normalsCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_gBufferNormals                        = m_alloc.createTexture(image, ivInfo, sampler);
    m_gBufferNormals.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the depth image (second colour attachment)
  {
    auto                  depthTextureCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_gBufferDepthTextureFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    nvvk::Image           image  = m_alloc.createImage(depthTextureCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, depthTextureCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_gBufferDepthTexture                  = m_alloc.createTexture(image, ivInfo, sampler);
    m_gBufferDepthTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the albedo image (third colour attachment)
  {
    auto                  albedoCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_gBufferAlbedoFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    nvvk::Image           image            = m_alloc.createImage(albedoCreateInfo);
    VkImageViewCreateInfo ivInfo           = nvvk::makeImageViewCreateInfo(image.image, albedoCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_gBufferAlbedo                              = m_alloc.createTexture(image, ivInfo, sampler);
    m_gBufferAlbedo.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the diffuse image (forth colour attachment)
  {
    auto                  diffuseCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_gBufferDiffuseFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    nvvk::Image           image             = m_alloc.createImage(diffuseCreateInfo);
    VkImageViewCreateInfo ivInfo            = nvvk::makeImageViewCreateInfo(image.image, diffuseCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_gBufferDiffuse                             = m_alloc.createTexture(image, ivInfo, sampler);
    m_gBufferDiffuse.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the depth buffer
  {
    auto depthCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_gBufferDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    nvvk::Image image = m_alloc.createImage(depthCreateInfo);

    VkImageViewCreateInfo depthStencilView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthStencilView.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    depthStencilView.format           = m_gBufferDepthFormat;
    depthStencilView.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthStencilView.image            = image.image;

    m_gBufferDepth = m_alloc.createTexture(image, depthStencilView);
  }

  // Setting the image layout for all attachments
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_gBufferNormals.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_gBufferDepthTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_gBufferAlbedo.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_gBufferDiffuse.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    nvvk::cmdBarrierImageLayout(cmdBuf, m_gBufferDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Creating a render pass for the offscreen
  if(!m_gBufferRenderPass)
  {
    std::vector<VkAttachmentDescription> attachments = {
        // Colour Images
        {0, m_gBufferNormalFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL},
        {0, m_gBufferDepthTextureFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL},
        {0, m_gBufferAlbedoFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL},
        {0, m_gBufferDiffuseFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL},

         // Depth Buffer
        {0, m_gBufferDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}
    };
    VkAttachmentReference colorRefs[] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                         {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                         {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                         {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};

    VkAttachmentReference depthRef = {4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass    = {};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 4;  // Updated to 4 for the added attachments
    subpass.pColorAttachments       = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments    = attachments.data();
    renderPassInfo.subpassCount    = 1;
    renderPassInfo.pSubpasses      = &subpass;

    vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_gBufferRenderPass);
  }

  // Creating the framebuffer for offscreen
  std::vector<VkImageView> fbAttachments = {m_gBufferNormals.descriptor.imageView, 
                                            m_gBufferDepthTexture.descriptor.imageView,
                                            m_gBufferAlbedo.descriptor.imageView,
                                            m_gBufferDiffuse.descriptor.imageView,
                                            m_gBufferDepth.descriptor.imageView};

  vkDestroyFramebuffer(m_device, m_gBufferFramebuffer, nullptr);
  VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebufferInfo.renderPass      = m_gBufferRenderPass;
  framebufferInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
  framebufferInfo.pAttachments    = fbAttachments.data();
  framebufferInfo.width           = m_size.width;
  framebufferInfo.height          = m_size.height;
  framebufferInfo.layers          = 1;
  vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_gBufferFramebuffer);
}



void HelloVulkan::gBufferBegin(const VkCommandBuffer& cmdBuf) {
    VkDeviceSize offset{0};

    m_debug.beginLabel(cmdBuf, "GBuffer");

      // Dynamic Viewport
    setViewport(cmdBuf);

      
      // Drawing all triangles
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gBufferPipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gBufferPipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    for(const HelloVulkan::ObjInstance& inst : m_instances) {
        auto& model            = m_objModel[inst.objIndex];
        m_pcRaster.objIndex    = inst.objIndex;  // Telling which object is drawn
        m_pcRaster.modelMatrix = inst.transform;

        vkCmdPushConstants(cmdBuf, m_gBufferPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantRaster), &m_pcRaster);
        vkCmdBindVertexBuffers(cmdBuf, 0, 1, &model.vertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmdBuf, model.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmdBuf, model.nbIndices, 1, 0, 0, 0);
    }
      
    m_debug.endLabel(cmdBuf);
}

void HelloVulkan::createGBufferPipeline()
{
    VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                              sizeof(PushConstantRaster)};

    // Creating the Pipeline Layout
    VkPipelineLayoutCreateInfo gBufferCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    gBufferCreateInfo.setLayoutCount         = 1;
    gBufferCreateInfo.pSetLayouts            = &m_descSetLayout;
    gBufferCreateInfo.pushConstantRangeCount = 1;
    gBufferCreateInfo.pPushConstantRanges    = &pushConstantRanges;
    vkCreatePipelineLayout(m_device, &gBufferCreateInfo, nullptr, &m_gBufferPipelineLayout);

    // Creating the Pipeline
    std::vector<std::string>                paths = defaultSearchPaths;
    nvvk::GraphicsPipelineGeneratorCombined gBufferGpb(m_device, m_gBufferPipelineLayout, m_gBufferRenderPass);
    gBufferGpb.depthStencilState.depthTestEnable = true;
    gBufferGpb.addShader(nvh::loadFile("spv/gBufferVertex.vert.spv", true, paths, true), VK_SHADER_STAGE_VERTEX_BIT);
    gBufferGpb.addShader(nvh::loadFile("spv/gBufferFragment.frag.spv", true, paths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
    gBufferGpb.addBindingDescription({0, sizeof(VertexObj)});
    
    gBufferGpb.addAttributeDescriptions({
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, pos))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, nrm))},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, color))},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, texCoord))},
    });

    // Define color blend attachment states for the two color attachments
    std::array<VkPipelineColorBlendAttachmentState, 3> colorBlendAttachments = {};
    for(auto& blendAttachment : colorBlendAttachments) {
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_FALSE;
        gBufferGpb.addBlendAttachmentState(blendAttachment);
    }

    // Create color blend state create info with the correct number of attachments
    VkPipelineColorBlendStateCreateInfo gBufferColorBlending = {};
    gBufferColorBlending.sType                               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    gBufferColorBlending.logicOpEnable                       = VK_FALSE;
    gBufferColorBlending.attachmentCount                     = static_cast<uint32_t>(colorBlendAttachments.size());
    gBufferColorBlending.pAttachments                        = colorBlendAttachments.data();
    

    gBufferGpb.colorBlendState = gBufferColorBlending;

    m_gBufferPipeline = gBufferGpb.createPipeline();

    m_debug.setObjectName(m_gBufferPipeline, "GBuffer");


    m_globalTextures.push_back(m_gBufferNormals);
    m_globalTextures.push_back(m_gBufferDepthTexture);
    m_globalTextures.push_back(m_indirectTexture);
    m_globalTextures.push_back(m_gBufferAlbedo);
    m_globalTextures.push_back(m_gBufferDiffuse);
}





//////////////////////////////////////////////////////////////////////////
// Ray Tracing
//////////////////////////////////////////////////////////////////////////
// #VKRay
void HelloVulkan::initRayTracing() {
  // Requesting ray tracing properties
  VkPhysicalDeviceProperties2 prop2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  prop2.pNext = &m_rtProperties;
  vkGetPhysicalDeviceProperties2(m_physicalDevice, &prop2);

  m_rtBuilder.setup(m_device, &m_alloc, m_graphicsQueueIndex);
}

//--------------------------------------------------------------------------------------------------
// Convert an OBJ model into the ray tracing geometry used to build the BLAS
//
auto HelloVulkan::objectToVkGeometryKHR(const ObjModel& model) {
  // BLAS builder requires raw device addresses.
  VkDeviceAddress vertexAddress = nvvk::getBufferDeviceAddress(m_device, model.vertexBuffer.buffer);
  VkDeviceAddress indexAddress  = nvvk::getBufferDeviceAddress(m_device, model.indexBuffer.buffer);

  uint32_t maxPrimitiveCount = model.nbIndices / 3;

  // Describe buffer as array of VertexObj.
  VkAccelerationStructureGeometryTrianglesDataKHR triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
  triangles.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;  // vec3 vertex position data.
  triangles.vertexData.deviceAddress = vertexAddress;
  triangles.vertexStride             = sizeof(VertexObj);
  // Describe index data (32-bit unsigned int)
  triangles.indexType               = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = indexAddress;
  // Indicate identity transform by setting transformData to null device pointer.
  //triangles.transformData = {};
  triangles.maxVertex = model.nbVertices - 1;

  // Identify the above data as containing opaque triangles.
  VkAccelerationStructureGeometryKHR asGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  asGeom.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  asGeom.flags              = VK_GEOMETRY_OPAQUE_BIT_KHR;
  asGeom.geometry.triangles = triangles;

  // The entire array will be used to build the BLAS.
  VkAccelerationStructureBuildRangeInfoKHR offset;
  offset.firstVertex     = 0;
  offset.primitiveCount  = maxPrimitiveCount;
  offset.primitiveOffset = 0;
  offset.transformOffset = 0;

  // Our blas is made from only one geometry, but could be made of many geometries
  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);

  return input;
}

//--------------------------------------------------------------------------------------------------
//
//
void HelloVulkan::createBottomLevelAS() {
  // BLAS - Storing each primitive in a geometry
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
  allBlas.reserve(m_objModel.size());
  for(const auto& obj : m_objModel) {
    auto blas = objectToVkGeometryKHR(obj);

    // We could add more geometry in each BLAS, but we add only one for now
    allBlas.emplace_back(blas);
  }
  m_rtBuilder.buildBlas(allBlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
}

//--------------------------------------------------------------------------------------------------
//
//
void HelloVulkan::createTopLevelAS() {
  std::vector<VkAccelerationStructureInstanceKHR> tlas;
  tlas.reserve(m_instances.size());
  for(const HelloVulkan::ObjInstance& inst : m_instances) {
    VkAccelerationStructureInstanceKHR rayInst{};
    rayInst.transform                      = nvvk::toTransformMatrixKHR(inst.transform);  // Position of the instance
    rayInst.instanceCustomIndex            = inst.objIndex;                               // gl_InstanceCustomIndexEXT
    rayInst.accelerationStructureReference = m_rtBuilder.getBlasDeviceAddress(inst.objIndex);
    rayInst.flags                          = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    rayInst.mask                           = 0xFF;       //  Only be hit if rayMask & instance.mask != 0
    rayInst.instanceShaderBindingTableRecordOffset = 0;  // We will use the same hit group for all objects
    tlas.emplace_back(rayInst);
  }
  m_rtBuilder.buildTlas(tlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
}

//--------------------------------------------------------------------------------------------------
// This descriptor set holds the Acceleration structure and the output image
//
void HelloVulkan::createRtDescriptorSet() {
  // Top-level acceleration structure, usable by both the ray generation and the closest hit (to shoot shadow rays)
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);  // TLAS
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eOutImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR);  // Output image

  // Indirect
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                   VK_SHADER_STAGE_VERTEX_BIT |VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT); // Constants
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eStatus, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT);  // Probe Status buffer

  m_rtDescSetLayoutBind.addBinding(RtxBindings::eStorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   static_cast<uint32_t>(m_storageImages.size()),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT);  // Global Images 2D

  m_rtDescSetLayoutBind.addBinding(RtxBindings::eGlobalTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(m_globalTextures.size()),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT);  // Global Textures
  
  // Binded Images for Probe Update
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eIrradianceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT);  // Irradiance image
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eVisibilityImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT);  // Visibility image


  m_rtDescPool      = m_rtDescSetLayoutBind.createPool(m_device);
  m_rtDescSetLayout = m_rtDescSetLayoutBind.createLayout(m_device);


  VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocateInfo.descriptorPool     = m_rtDescPool;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts        = &m_rtDescSetLayout;
  vkAllocateDescriptorSets(m_device, &allocateInfo, &m_rtDescSet);


  VkAccelerationStructureKHR tlas = m_rtBuilder.getAccelerationStructure();
  VkWriteDescriptorSetAccelerationStructureKHR descASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  descASInfo.accelerationStructureCount = 1;
  descASInfo.pAccelerationStructures    = &tlas;

  VkDescriptorImageInfo imageInfo{{}, m_offscreenColor.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};



  // Prepare buffer info for Indirect buffers
  VkDescriptorBufferInfo constantsBufferInfo{};
  constantsBufferInfo.buffer = m_bIndirectConstants.buffer;  // Assuming m_constantsBuffer is a VkBuffer handle for the constants buffer
  constantsBufferInfo.offset = 0;
  constantsBufferInfo.range  = VK_WHOLE_SIZE;

  
  VkDescriptorBufferInfo statusBufferInfo{};
  statusBufferInfo.buffer = m_bIndirectStatus.buffer;  // Assuming m_statusBuffer is a VkBuffer handle for the status buffer
  statusBufferInfo.offset = 0;
  statusBufferInfo.range  = VK_WHOLE_SIZE;

  // Global Images 2D
  std::vector<VkDescriptorImageInfo> imageInfos(m_storageImages.size());
  m_storageImageViews.resize(m_storageImages.size());

  for(size_t i = 0; i < m_storageImages.size(); ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_storageImages[i].image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = m_storageImages[i].format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if(vkCreateImageView(m_device, &viewInfo, nullptr, &m_storageImageViews[i]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture image view!");
    }

    imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[i].imageView   = m_storageImageViews[i];
    imageInfos[i].sampler     = VK_NULL_HANDLE;
  }

  // Global Textures
  std::vector<VkDescriptorImageInfo> globalTextureInfos(m_globalTextures.size());
  m_globalTexturesView.resize(m_globalTextures.size());
  m_globalTextureSamplers.resize(m_globalTextures.size());

  for(size_t i = 0; i < m_globalTextures.size(); ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_globalTextures[i].image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = m_globalTextures[i].format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if(vkCreateImageView(m_device, &viewInfo, nullptr, &m_globalTexturesView[i]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture image view!");
    }

    // Create or reuse a sampler for global textures
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable        = VK_TRUE;
    samplerInfo.maxAnisotropy           = 16;
    samplerInfo.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable           = VK_FALSE;
    samplerInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias              = 0.0f;
    samplerInfo.minLod                  = 0.0f;
    samplerInfo.maxLod                  = 0.0f;

    if(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_globalTextureSamplers[i]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture sampler!");
    }

    globalTextureInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    globalTextureInfos[i].imageView   = m_globalTexturesView[i];
    globalTextureInfos[i].sampler     = m_globalTextureSamplers[i];
  }

  // Binded images
  VkDescriptorImageInfo irradianceImageInfo{{}, m_irradianceTexture.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo visibilityImageInfo{{}, m_visibilityTexture.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};
  

  // Writes
  std::vector<VkWriteDescriptorSet> writes;
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eTlas, &descASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eOutImage, &imageInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eConstants, &constantsBufferInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eStatus, &statusBufferInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eIrradianceImage, &irradianceImageInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eVisibilityImage, &visibilityImageInfo));
  
  // Global Images 2D
  VkWriteDescriptorSet writeStorageImages = {};
  writeStorageImages.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writeStorageImages.dstSet               = m_rtDescSet;
  writeStorageImages.dstBinding           = RtxBindings::eStorageImages;
  writeStorageImages.dstArrayElement      = 0;
  writeStorageImages.descriptorType       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writeStorageImages.descriptorCount      = static_cast<uint32_t>(m_storageImages.size());
  writeStorageImages.pImageInfo           = imageInfos.data();
  writes.emplace_back(writeStorageImages);

   // Global Textures
  VkWriteDescriptorSet writeGlobalTextures = {};
  writeGlobalTextures.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writeGlobalTextures.dstSet               = m_rtDescSet;
  writeGlobalTextures.dstBinding           = RtxBindings::eGlobalTextures;
  writeGlobalTextures.dstArrayElement      = 0;
  writeGlobalTextures.descriptorType       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writeGlobalTextures.descriptorCount      = static_cast<uint32_t>(m_globalTextures.size());
  writeGlobalTextures.pImageInfo           = globalTextureInfos.data();
  writes.emplace_back(writeGlobalTextures);

  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Writes the output image to the descriptor set
// - Required when changing resolution
//
void HelloVulkan::updateRtDescriptorSet() {
  // (1) Output buffer
  VkDescriptorImageInfo imageInfo{{}, m_offscreenColor.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet  wds = m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eOutImage, &imageInfo);
  vkUpdateDescriptorSets(m_device, 1, &wds, 0, nullptr);
}




//--------------------------------------------------------------------------------------------------
// Pipeline for the ray tracer: all shaders, raygen, chit, miss
//
void HelloVulkan::createRtPipeline() {
    enum StageIndices {
    eRaygen,
    eMiss,
    eMiss2,
    eClosestHit,
    eShaderGroupCount
  };

  // All stages
  std::array<VkPipelineShaderStageCreateInfo, eShaderGroupCount> stages{};
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.pName = "main";  // All the same entry point
  // Raygen
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rgen.spv", true, defaultSearchPaths, true));
  stage.stage     = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  stages[eRaygen] = stage;
  // Miss
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rmiss.spv", true, defaultSearchPaths, true));
  stage.stage   = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss] = stage;
  // The second miss shader is invoked when a shadow ray misses the geometry. It simply indicates that no occlusion has been found
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytraceShadow.rmiss.spv", true, defaultSearchPaths, true));
  stage.stage    = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss2] = stage;
  // Hit Group - Closest Hit
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rchit.spv", true, defaultSearchPaths, true));
  stage.stage         = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  stages[eClosestHit] = stage;


  // Shader groups
  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  // Raygen
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  m_rtShaderGroups.push_back(group);

  // Miss
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  m_rtShaderGroups.push_back(group);

  // Shadow Miss
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss2;
  m_rtShaderGroups.push_back(group);

  // closest hit shader
  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.closestHitShader = eClosestHit;
  m_rtShaderGroups.push_back(group);

  // Push constant: we want to be able to update constants used by the shaders
  VkPushConstantRange pushConstant{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                                   0, sizeof(PushConstantRay)};


  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
  pipelineLayoutCreateInfo.pPushConstantRanges    = &pushConstant;

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<VkDescriptorSetLayout> rtDescSetLayouts = {m_rtDescSetLayout, m_descSetLayout};
  pipelineLayoutCreateInfo.setLayoutCount             = static_cast<uint32_t>(rtDescSetLayouts.size());
  pipelineLayoutCreateInfo.pSetLayouts                = rtDescSetLayouts.data();

  vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_rtPipelineLayout);


  // Assemble the shader stages and recursion depth info into the ray tracing pipeline
  VkRayTracingPipelineCreateInfoKHR rayPipelineInfo{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  rayPipelineInfo.stageCount = static_cast<uint32_t>(stages.size());  // Stages are shaders
  rayPipelineInfo.pStages    = stages.data();

  // In this case, m_rtShaderGroups.size() == 4: we have one raygen group,
  // two miss shader groups, and one hit group.
  rayPipelineInfo.groupCount = static_cast<uint32_t>(m_rtShaderGroups.size());
  rayPipelineInfo.pGroups    = m_rtShaderGroups.data();
  rayPipelineInfo.maxPipelineRayRecursionDepth = 2;  // Ray depth
  rayPipelineInfo.layout                       = m_rtPipelineLayout;

  vkCreateRayTracingPipelinesKHR(m_device, {}, {}, 1, &rayPipelineInfo, nullptr, &m_rtPipeline);


  // Spec only guarantees 1 level of "recursion". Check for that sad possibility here.
  if(m_rtProperties.maxRayRecursionDepth <= 1) {
    throw std::runtime_error("Device fails to support ray recursion (m_rtProperties.maxRayRecursionDepth <= 1)");
  }

  for(auto& s : stages)
    vkDestroyShaderModule(m_device, s.module, nullptr);

}


//--------------------------------------------------------------------------------------------------
// The Shader Binding Table (SBT)
// - getting all shader handles and write them in a SBT buffer
// - Besides exception, this could be always done like this
//
void HelloVulkan::createRtShaderBindingTable() {
  uint32_t missCount{2};
  uint32_t hitCount{1};
  auto     handleCount = 1 + missCount + hitCount;
  uint32_t handleSize  = m_rtProperties.shaderGroupHandleSize;

  // The SBT (buffer) need to have starting groups to be aligned and handles in the group to be aligned.
  uint32_t handleSizeAligned = nvh::align_up(handleSize, m_rtProperties.shaderGroupHandleAlignment);

  m_rgenRegion.stride = nvh::align_up(handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);
  m_rgenRegion.size = m_rgenRegion.stride;  // The size member of pRayGenShaderBindingTable must be equal to its stride member
  m_missRegion.stride = handleSizeAligned;
  m_missRegion.size   = nvh::align_up(missCount * handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);
  m_hitRegion.stride  = handleSizeAligned;
  m_hitRegion.size    = nvh::align_up(hitCount * handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);

  // Get the shader group handles
  uint32_t             dataSize = handleCount * handleSize;
  std::vector<uint8_t> handles(dataSize);
  auto result = vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, handleCount, dataSize, handles.data());
  assert(result == VK_SUCCESS);

  // Allocate a buffer for storing the SBT.
  VkDeviceSize sbtSize = m_rgenRegion.size + m_missRegion.size + m_hitRegion.size + m_callRegion.size;
  m_rtSBTBuffer        = m_alloc.createBuffer(sbtSize,
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                                  | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  m_debug.setObjectName(m_rtSBTBuffer.buffer, std::string("SBT"));  // Give it a debug name for NSight.

  // Find the SBT addresses of each group
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, m_rtSBTBuffer.buffer};
  VkDeviceAddress           sbtAddress = vkGetBufferDeviceAddress(m_device, &info);
  m_rgenRegion.deviceAddress           = sbtAddress;
  m_missRegion.deviceAddress           = sbtAddress + m_rgenRegion.size;
  m_hitRegion.deviceAddress            = sbtAddress + m_rgenRegion.size + m_missRegion.size;

  // Helper to retrieve the handle data
  auto getHandle = [&](int i) { return handles.data() + i * handleSize; };

  // Map the SBT buffer and write in the handles.
  auto*    pSBTBuffer = reinterpret_cast<uint8_t*>(m_alloc.map(m_rtSBTBuffer));
  uint8_t* pData{nullptr};
  uint32_t handleIdx{0};
  // Raygen
  pData = pSBTBuffer;
  memcpy(pData, getHandle(handleIdx++), handleSize);
  // Miss
  pData = pSBTBuffer + m_rgenRegion.size;
  for(uint32_t c = 0; c < missCount; c++) {
    memcpy(pData, getHandle(handleIdx++), handleSize);
    pData += m_missRegion.stride;
  }
  // Hit
  pData = pSBTBuffer + m_rgenRegion.size + m_missRegion.size;
  for(uint32_t c = 0; c < hitCount; c++) {
    memcpy(pData, getHandle(handleIdx++), handleSize);
    pData += m_hitRegion.stride;
  }

  m_alloc.unmap(m_rtSBTBuffer);
  m_alloc.finalizeAndReleaseStaging();
}

//--------------------------------------------------------------------------------------------------
// Ray Tracing the scene
//
void HelloVulkan::raytrace(const VkCommandBuffer& cmdBuf, const glm::vec4& clearColor) {
  m_debug.beginLabel(cmdBuf, "Ray trace");
  // Initializing push constant values
  m_pcRay.clearColor     = clearColor;
  m_pcRay.lightPosition  = m_pcRaster.lightPosition;
  m_pcRay.lightIntensity = m_pcRaster.lightIntensity;
  m_pcRay.lightType      = m_pcRaster.lightType;

  

  std::vector<VkDescriptorSet> descSets{m_rtDescSet, m_descSet};
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipelineLayout, 0,
                          (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
  vkCmdPushConstants(cmdBuf, m_rtPipelineLayout,
                     VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                     0, sizeof(PushConstantRay), &m_pcRay);


  vkCmdTraceRaysKHR(cmdBuf, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion, m_size.width, m_size.height, 1);


  m_debug.endLabel(cmdBuf);

}



//////////////////////////////////////////////////////////////////////////
// Irradiance Field
//////////////////////////////////////////////////////////////////////////

void HelloVulkan::prepareIndirectComponents(renderSceneVolume& scene) {
  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer   cmdBuf = cmdBufGet.createCommandBuffer();

  volume.per_frame_probe_updates = scene.gi_per_frame_probes_update;
  //volume.per_frame_probe_updates = 0;
  const uint32_t num_probes      = volume.get_total_probes();
  scene.gi_total_probes     = num_probes;

  volume.half_resolution_output = scene.gi_use_half_resolution;
  
  // Create Buffers
  createIndirectConstantsBuffer();
  createIndirectStatusBuffer();


  // Texture creation
  //-----------------
  // Radiance Texture
  const uint32_t num_rays = volume.probe_rays;
  auto           radianceCreateInfo = nvvk::makeImage2DCreateInfo({num_rays, num_probes}, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
  m_radianceImage  = m_alloc.createImage(radianceCreateInfo);
  nvvk::cmdBarrierImageLayout(cmdBuf, m_radianceImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  //m_radianceImage = createStorageImage(cmdBuf, m_device, m_physicalDevice, num_rays, num_probes, VK_FORMAT_R16G16B16A16_SFLOAT);
  VkImageViewCreateInfo radianceIvInfo = nvvk::makeImageViewCreateInfo(m_radianceImage.image, radianceCreateInfo);
  VkSamplerCreateInfo radianceSampler { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  m_radianceTexture                        = m_alloc.createTexture(m_radianceImage, radianceIvInfo, radianceSampler);
  m_radianceTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  m_storageImages.push_back(m_radianceTexture);   // Global Images array
  m_globalTextures.push_back(m_radianceTexture);    // Global Textures array


  //----------------------
  // Probe offsets texture
  auto offsetsCreateInfo = nvvk::makeImage2DCreateInfo({volume.probe_count_x * volume.probe_count_y, volume.probe_count_z}, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
  m_offsetsImage = createStorageImage(cmdBuf, m_device, m_physicalDevice, volume.probe_count_x * volume.probe_count_y, volume.probe_count_z, VK_FORMAT_R16G16B16A16_SFLOAT);
  VkImageViewCreateInfo offsetsIvInfo = nvvk::makeImageViewCreateInfo(m_offsetsImage.image, offsetsCreateInfo);
  VkSamplerCreateInfo offsetsSampler { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  m_offsetsTexture                        = m_alloc.createTexture(m_offsetsImage, offsetsIvInfo, offsetsSampler);
  m_offsetsTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  m_storageImages.push_back(m_offsetsTexture);   // Global Images array
  m_globalTextures.push_back(m_offsetsTexture);     // Global Textures array


  //----------------------
  // Irradiance Texture 6x6 plus 2 additional pixel border to allow interpolation
  const int octahedral_irradiance_size = volume.irradiance_probe_size + 2;
  volume.irradiance_atlas_width        = (octahedral_irradiance_size * volume.probe_count_x * volume.probe_count_y);
  volume.irradiance_atlas_height       = (octahedral_irradiance_size * volume.probe_count_z);
  //m_irradianceImage = createStorageImage(cmdBuf, m_device, m_physicalDevice, irradiance_atlas_width, irradiance_atlas_height, VK_FORMAT_R16G16B16A16_SFLOAT);
  auto irradianceCreateInfo = nvvk::makeImage2DCreateInfo(
      {static_cast<uint32_t>(volume.irradiance_atlas_width), static_cast<uint32_t>(volume.irradiance_atlas_height)},
      VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
  m_irradianceImage = m_alloc.createImage(irradianceCreateInfo);
  nvvk::cmdBarrierImageLayout(cmdBuf, m_irradianceImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageViewCreateInfo irradianceIvInfo = nvvk::makeImageViewCreateInfo(m_irradianceImage.image, irradianceCreateInfo);
  VkSamplerCreateInfo   irradianceSampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  m_irradianceTexture                       = m_alloc.createTexture(m_irradianceImage, irradianceIvInfo, irradianceSampler);
  m_irradianceTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  
  m_globalTextures.push_back(m_irradianceTexture);  // Global Textures array


  // Visibility Texture
  const int octahedral_visibility_size     = volume.visibility_probe_size + 2;
  volume.visibility_atlas_width        = (octahedral_visibility_size * volume.probe_count_x * volume.probe_count_y);
  volume.visibility_atlas_height           = (octahedral_visibility_size * volume.probe_count_z);
  //m_visibilityImage = createStorageImage(cmdBuf, m_device, m_physicalDevice, visibility_atlas_width, visibility_atlas_height, VK_FORMAT_R16G16_SFLOAT);
  auto visibilityCreateInfo = nvvk::makeImage2DCreateInfo(
      {static_cast<uint32_t>(volume.visibility_atlas_width), static_cast<uint32_t>(volume.visibility_atlas_height)},
      VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
  m_visibilityImage = m_alloc.createImage(visibilityCreateInfo);
  nvvk::cmdBarrierImageLayout(cmdBuf, m_visibilityImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageViewCreateInfo visibilityIvInfo = nvvk::makeImageViewCreateInfo(m_visibilityImage.image, visibilityCreateInfo);
  VkSamplerCreateInfo   visibilitySampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  m_visibilityTexture = m_alloc.createTexture(m_visibilityImage, visibilityIvInfo, visibilitySampler);
  m_visibilityTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  m_globalTextures.push_back(m_visibilityTexture);      // Global Textures array
  

  // Indirect Texture
  uint32_t adjusted_width  = scene.gi_use_half_resolution ? m_size.width / 2 : m_size.width;
  uint32_t adjusted_height = scene.gi_use_half_resolution ? m_size.height / 2 : m_size.height;
  //m_indirectImage = createStorageImage(cmdBuf, m_device, m_physicalDevice, adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT);
  auto indirectCreateInfo = nvvk::makeImage2DCreateInfo({adjusted_width, adjusted_height}, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
  m_indirectImage = m_alloc.createImage(indirectCreateInfo);

  nvvk::cmdBarrierImageLayout(cmdBuf, m_indirectImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageViewCreateInfo indirectIvInfo = nvvk::makeImageViewCreateInfo(m_indirectImage.image, indirectCreateInfo);
  VkSamplerCreateInfo   indirectSampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  m_indirectTexture = m_alloc.createTexture(m_indirectImage, indirectIvInfo, indirectSampler);
  m_indirectTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  
  m_storageImages.push_back(m_indirectTexture);  // Global Images array
}




void HelloVulkan::IndirectBegin(const VkCommandBuffer& cmdBuf, const glm::vec4& clearColor, renderSceneVolume& scene) {
  m_debug.beginLabel(cmdBuf, "Indirect Begin");

  vkCmdResetQueryPool(cmdBuf, queryPool, 0, 7);

  vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 0);


  // Initializing push constant values
  m_pcRay.clearColor     = clearColor;
  m_pcRay.lightPosition  = m_pcRaster.lightPosition;
  m_pcRay.lightIntensity = m_pcRaster.lightIntensity;
  m_pcRay.lightType      = m_pcRaster.lightType;

  // Sample Irradiance Push Constant
  m_pcSampleIrradiance.output_resolution_half = (scene.gi_use_half_resolution == true) ? 1 : 0;

  uint32_t   first_frame;
  static int offsets_calculations_count = 24;

  if(scene.gi_recalculate_offsets) {
    offsets_calculations_count = 24;
  }

  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_radianceTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Ray Tracing
  std::vector<VkDescriptorSet> descSets{m_rtDescSet, m_descSet};
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_IndirectPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_IndirectPipelineLayout, 0,
                          (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
  vkCmdPushConstants(cmdBuf, m_IndirectPipelineLayout,
                     VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                     0, sizeof(PushConstantRay), &m_pcRay);
  const uint32_t probe_count = offsets_calculations_count >= 0 ? volume.get_total_probes() : volume.per_frame_probe_updates;
  //const uint32_t probe_count = volume.get_total_probes();
  vkCmdTraceRaysKHR(cmdBuf, &m_IndirectRgenRegion, &m_IndirectMissRegion, &m_IndirectHitRegion, &m_IndirectCallRegion,
                    volume.probe_rays, volume.get_total_probes(), 1);

  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_radianceTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    genCmdBuf.submitAndWait(cmdBuf);
  }

  vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 1);
  
  m_debug.endLabel(cmdBuf);
  


  
  if(offsets_calculations_count >= 0) {
    --offsets_calculations_count;
    first_frame = offsets_calculations_count == 23 ? 1 : 0;

    m_pcProbeOffsets.first_frame = first_frame;

    m_debug.beginLabel(cmdBuf, "Offsets Compute Begin");

    {
      nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
      auto              cmdBuf = genCmdBuf.createCommandBuffer();
      nvvk::cmdBarrierImageLayout(cmdBuf, m_offsetsTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
      genCmdBuf.submitAndWait(cmdBuf);
    }

    // Probe Offsets Compute Pipeline
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeOffsetsPipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeOffsetsPipelineLayout, 0,
                            (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
    vkCmdPushConstants(cmdBuf, m_probeOffsetsPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstantOffset),
                       &m_pcProbeOffsets);
    vkCmdDispatch(cmdBuf, glm::ceil(probe_count / 32.0f), 1, 1);

      vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 2);


    m_debug.endLabel(cmdBuf);
  }





  m_debug.beginLabel(cmdBuf, "Status Compute Begin");

  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_radianceTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Probe Status
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeStatusPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeStatusPipelineLayout, 0,
                          (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
  first_frame = 0;
  m_pcProbeStatus.first_frame = first_frame;
  vkCmdPushConstants(cmdBuf, m_probeStatusPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstantStatus), &m_pcProbeStatus);
  vkCmdDispatch(cmdBuf, glm::ceil(probe_count / 32.0f), 1, 1);


    vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 3);
  m_debug.endLabel(cmdBuf);


  m_debug.beginLabel(cmdBuf, "Irradiance Compute Begin");

  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_irradianceTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Probe Update Irradiance
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeUpdateIrradiancePipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeUpdateIrradiancePipelineLayout, 0, (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
  vkCmdPushConstants(cmdBuf, m_probeUpdateIrradiancePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(PushConstantOffset), &m_pcProbeOffsets);
  vkCmdDispatch(cmdBuf, glm::ceil(volume.irradiance_atlas_width / 8.0f), glm::ceil(volume.irradiance_atlas_height / 8.0f), 1);
  
    vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 4);
  
  m_debug.endLabel(cmdBuf);





  m_debug.beginLabel(cmdBuf, "Visibility Compute Begin");

  // Probe Update Visibility
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_visibilityTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeUpdateVisibilityPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_probeUpdateVisibilityPipelineLayout, 0, (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
  vkCmdPushConstants(cmdBuf, m_probeUpdateVisibilityPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(PushConstantOffset), &m_pcProbeOffsets);
  vkCmdDispatch(cmdBuf, glm::ceil(volume.visibility_atlas_width / 8.0f), glm::ceil(volume.visibility_atlas_height / 8.0f), 1);
  
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_irradianceTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_visibilityTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }
    vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 5);

  m_debug.endLabel(cmdBuf);
  


  m_debug.beginLabel(cmdBuf, "Sample Compute Begin");
 
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_indirectTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }
  const float resolution_divider = scene.gi_use_half_resolution ? 0.5f : 1.0f;


  // Sample Irradiance
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_sampleIrradiancePipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_sampleIrradiancePipelineLayout, 0, (uint32_t)descSets.size(), descSets.data(), 0, nullptr);
  vkCmdPushConstants(cmdBuf, m_sampleIrradiancePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstantSample), &m_pcSampleIrradiance);
  vkCmdDispatch(cmdBuf, m_size.width * resolution_divider, m_size.height * resolution_divider, 1);
  
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_indirectTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    genCmdBuf.submitAndWait(cmdBuf);
  }

    vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, 6);

  m_debug.endLabel(cmdBuf);


  /*
    // Retrieve results
    uint64_t timestamps[7] = {};  // Array size must match the number of timestamps written
    vkGetQueryPoolResults(m_device, queryPool, 0, 7, sizeof(timestamps), timestamps, sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    // Convert to time
    double timestampPeriod = 1e-9;  // Typically in nanoseconds, but adjust based on your device's timestamp period

    // Compute the time taken for each section
    double timeRayTracing    = (timestamps[1] - timestamps[0]) * timestampPeriod;
    double timeOffsetCompute = (timestamps[2] - timestamps[1]) * timestampPeriod;
    double timeStatusCompute = (timestamps[3] - timestamps[2]) * timestampPeriod;
    double timeIrradiance    = (timestamps[4] - timestamps[3]) * timestampPeriod;
    double timeVisibility    = (timestamps[5] - timestamps[4]) * timestampPeriod;
    double timeSampleCompute = (timestamps[6] - timestamps[5]) * timestampPeriod;

    std::cout << "Ray Tracing Time: " << timeRayTracing << " seconds" << std::endl;
    std::cout << "Offsets Compute Time: " << timeOffsetCompute << " seconds" << std::endl;
    std::cout << "Status Compute Time: " << timeStatusCompute << " seconds" << std::endl;
    std::cout << "Irradiance Compute Time: " << timeIrradiance << " seconds" << std::endl;
    std::cout << "Visibility Compute Time: " << timeVisibility << " seconds" << std::endl;
    std::cout << "Sample Compute Time: " << timeSampleCompute << " seconds" << std::endl;
    */
    
}


void HelloVulkan::createIndirectPipeline() {
  enum StageIndices {
    eRaygen,
    eMiss,
    eClosestHit,
    eShaderGroupCount
  };

  // All stages
  std::array<VkPipelineShaderStageCreateInfo, eShaderGroupCount> IndirectStages{};
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.pName = "main";  // All the same entry point

  // Indirect Raygen
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytraceProbes.rgen.spv", true, defaultSearchPaths, true));
  stage.stage             = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  IndirectStages[eRaygen] = stage;

  // Indirect Miss
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytraceProbes.rmiss.spv", true, defaultSearchPaths, true));
  stage.stage           = VK_SHADER_STAGE_MISS_BIT_KHR;
  IndirectStages[eMiss] = stage;

  // Indirect Closest Hit
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytraceProbes.rchit.spv", true, defaultSearchPaths, true));
  stage.stage                 = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  IndirectStages[eClosestHit] = stage;

  // Shader groups
  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader           = VK_SHADER_UNUSED_KHR;
  group.closestHitShader       = VK_SHADER_UNUSED_KHR;
  group.generalShader          = VK_SHADER_UNUSED_KHR;
  group.intersectionShader     = VK_SHADER_UNUSED_KHR;

  // Indirect Raygen
  group.type              = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  m_IndirectShaderGroups.push_back(group);

  // Indirect Miss
  group.type              = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  m_IndirectShaderGroups.push_back(group);

  // Indirect Closest Hit
  group.type                 = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader        = VK_SHADER_UNUSED_KHR;
  group.closestHitShader     = eClosestHit;
  m_IndirectShaderGroups.push_back(group);


  // Push constant: we want to be able to update constants used by the shaders
  VkPushConstantRange pushConstant{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                                   0, sizeof(PushConstantRay)};

  VkPipelineLayoutCreateInfo indirectPipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  indirectPipelineLayoutCreateInfo.pushConstantRangeCount = 1;
  indirectPipelineLayoutCreateInfo.pPushConstantRanges    = &pushConstant;

  // Descriptor sets: specific to Indirect
  std::vector<VkDescriptorSetLayout> indirectDescSetLayouts = {m_rtDescSetLayout, m_descSetLayout};
  indirectPipelineLayoutCreateInfo.setLayoutCount           = static_cast<uint32_t>(indirectDescSetLayouts.size());
  indirectPipelineLayoutCreateInfo.pSetLayouts              = indirectDescSetLayouts.data();

  vkCreatePipelineLayout(m_device, &indirectPipelineLayoutCreateInfo, nullptr, &m_IndirectPipelineLayout);

  // Assemble the shader stages and recursion depth info into the Indirect ray tracing pipeline
  VkRayTracingPipelineCreateInfoKHR indirectPipelineInfo{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
  indirectPipelineInfo.stageCount               = static_cast<uint32_t>(IndirectStages.size());  // Stages are shaders
  indirectPipelineInfo.pStages                  = IndirectStages.data();
  indirectPipelineInfo.groupCount                   = static_cast<uint32_t>(m_IndirectShaderGroups.size());
  indirectPipelineInfo.pGroups                      = m_IndirectShaderGroups.data();
  indirectPipelineInfo.maxPipelineRayRecursionDepth = 2;  // Ray depth
  indirectPipelineInfo.layout                       = m_IndirectPipelineLayout;

  vkCreateRayTracingPipelinesKHR(m_device, {}, {}, 1, &indirectPipelineInfo, nullptr, &m_IndirectPipeline);

  
  // Spec only guarantees 1 level of "recursion". Check for that sad possibility here.
  if(m_rtProperties.maxRayRecursionDepth <= 1) {
    throw std::runtime_error("Device fails to support ray recursion (m_IndirectProperties.maxRayRecursionDepth <= 1)");
  }
  
  for(auto& s : IndirectStages) {
    vkDestroyShaderModule(m_device, s.module, nullptr);
  }



  // Compute Pipelines
  VkPushConstantRange pushConstantOffset{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, sizeof(PushConstantOffset)};

  VkPushConstantRange pushConstantSample{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
                                         0, sizeof(PushConstantSample)};

  
  createComputePipeline("spv/probeOffsets.glsl.spv", indirectDescSetLayouts, m_probeOffsetsPipelineLayout,
                        m_probeOffsetsPipeline, &pushConstantOffset, sizeof(pushConstantOffset));

  createComputePipeline("spv/probeStatus.glsl.spv", indirectDescSetLayouts, m_probeStatusPipelineLayout,
                        m_probeStatusPipeline, &pushConstantOffset, sizeof(pushConstantOffset));

  createComputePipeline("spv/probeUpdateIrradiance.glsl.spv", indirectDescSetLayouts, m_probeUpdateIrradiancePipelineLayout,
                        m_probeUpdateIrradiancePipeline, &pushConstant, sizeof(pushConstant));

  createComputePipeline("spv/probeUpdateVisibility.glsl.spv", indirectDescSetLayouts, m_probeUpdateVisibilityPipelineLayout,
                        m_probeUpdateVisibilityPipeline, &pushConstant, sizeof(pushConstant));
  
  createComputePipeline("spv/sampleIrradiance.glsl.spv", indirectDescSetLayouts, m_sampleIrradiancePipelineLayout,
                        m_sampleIrradiancePipeline, &pushConstantSample, sizeof(pushConstantSample));
  
}


void HelloVulkan::createComputePipeline(const std::string&    shaderPath,
                                        std::vector<VkDescriptorSetLayout> IndirectDescSetLayouts,
                                        VkPipelineLayout&     pipelineLayout,
                                        VkPipeline&           pipeline,
                                        const void*           pushConstants,
                                        uint32_t              pushConstantsSize) {

  // Compile compute shader and package as stage.
  VkShaderModule computeShader = nvvk::createShaderModule(m_device, nvh::loadFile(shaderPath, true, defaultSearchPaths, true));
  VkPipelineShaderStageCreateInfo stageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
  stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stageInfo.module = computeShader;
  stageInfo.pName  = "main";

  // Set up push constant and pipeline layout.
  VkPushConstantRange        pushCRange = { VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantsSize };
  
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount         = static_cast<uint32_t>(IndirectDescSetLayouts.size());
  layoutInfo.pSetLayouts            = IndirectDescSetLayouts.data();
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges    = &pushCRange;
  vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipelineLayout);

  // Create compute pipeline.
  VkComputePipelineCreateInfo pipelineInfo { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
  pipelineInfo.stage  = stageInfo;
  pipelineInfo.layout = pipelineLayout;
  vkCreateComputePipelines(m_device, {}, 1, &pipelineInfo, nullptr, &pipeline);

  vkDestroyShaderModule(m_device, computeShader, nullptr);
}

//---------------------------------
// Buffers

void HelloVulkan::createIndirectConstantsBuffer() {
  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer   cmdBuf = cmdBufGet.createCommandBuffer();

  using Usage      = VkBufferUsageFlagBits;
  m_bIndirectConstants = m_alloc.createBuffer(sizeof(Indirect_gpu_constants),
                                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_debug.setObjectName(m_bIndirectConstants.buffer, "IndirectConstantsBuffer");

}

void HelloVulkan::createIndirectStatusBuffer(){
  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer   cmdBuf = cmdBufGet.createCommandBuffer();

  const uint32_t num_probes = volume.get_total_probes();
  using Usage   = VkBufferUsageFlagBits;
  m_bIndirectStatus = m_alloc.createBuffer(sizeof(uint32_t) * num_probes,
                                                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_debug.setObjectName(m_bIndirectStatus.buffer, "IndirectStausBuffer");
}

void HelloVulkan::updateIndirectConstantsBuffer(const VkCommandBuffer& cmdBuf, renderSceneVolume& scene) {
  Indirect_gpu_constants hostIndirectConstBuffer = {};
  
  hostIndirectConstBuffer.radiance_output_index             = 0;
  hostIndirectConstBuffer.grid_irradiance_output_index      = 2;
  hostIndirectConstBuffer.indirect_output_index             = 2;
  hostIndirectConstBuffer.normal_texture_index              = 4;
  
  hostIndirectConstBuffer.depth_pyramid_texture_index       = 6;
  hostIndirectConstBuffer.depth_fullscreen_texture_index    = 5;
  hostIndirectConstBuffer.grid_visibility_texture_index     = 3;
  hostIndirectConstBuffer.probe_offset_texture_index        = 1;

  hostIndirectConstBuffer.hysteresis                        = scene.gi_hysteresis;
  hostIndirectConstBuffer.infinte_bounces_multiplier        = scene.gi_infinite_bounces_multiplier;
  hostIndirectConstBuffer.probe_update_offset               = volume.probe_update_offset;
  hostIndirectConstBuffer.probe_update_count                = volume.per_frame_probe_updates;
  //hostIndirectConstBuffer.probe_update_count                = 0;

  hostIndirectConstBuffer.probe_grid_position               = scene.gi_probe_grid_position;
  hostIndirectConstBuffer.probe_sphere_scale                = scene.gi_probe_sphere_scale;

  hostIndirectConstBuffer.probe_spacing                     = scene.gi_probe_spacing;
  hostIndirectConstBuffer.max_probe_offset                  = scene.gi_max_probe_offset;

  hostIndirectConstBuffer.reciprocal_probe_spacing          = {1.f / scene.gi_probe_spacing.x, 1.f / scene.gi_probe_spacing.y, 1.f / scene.gi_probe_spacing.z};
  hostIndirectConstBuffer.self_shadow_bias                  = scene.gi_self_shadow_bias;

  hostIndirectConstBuffer.probe_counts[0]                   = volume.probe_count_x;
  hostIndirectConstBuffer.probe_counts[1]                   = volume.probe_count_y;
  hostIndirectConstBuffer.probe_counts[2]                   = volume.probe_count_z;

  // Debug options for probes
  hostIndirectConstBuffer.debug_options                     = ((scene.gi_debug_border ? 1 : 0)) 
                                                            | ((scene.gi_debug_border_type ? 1 : 0) << 1)
                                                            | ((scene.gi_debug_border_source ? 1 : 0) << 2) 
                                                            | ((scene.gi_use_visibility ? 1 : 0) << 3)
                                                            | ((scene.gi_use_wrap_shading ? 1 : 0) << 4) 
                                                            | ((scene.gi_use_perceptual_encoding ? 1 : 0) << 5)
                                                            | ((scene.gi_use_backface_blending ? 1 : 0) << 6) 
                                                            | ((scene.gi_use_probe_offsetting ? 1 : 0) << 7)
                                                            | ((scene.gi_use_probe_status ? 1 : 0) << 8) 
                                                            | ((scene.gi_use_infinite_bounces ? 1 : 0) << 9);

  // Irradiance - Visibility size settings
  hostIndirectConstBuffer.irradiance_texture_width          = volume.irradiance_atlas_width;
  hostIndirectConstBuffer.irradiance_texture_height         = volume.irradiance_atlas_height;
  hostIndirectConstBuffer.irradiance_side_length            = volume.irradiance_probe_size;

  hostIndirectConstBuffer.probe_rays                        = volume.probe_rays;

  hostIndirectConstBuffer.visibility_texture_width          = volume.visibility_atlas_width;
  hostIndirectConstBuffer.visibility_texture_height         = volume.visibility_atlas_height;
  hostIndirectConstBuffer.visibility_side_length            = volume.visibility_probe_size;


  hostIndirectConstBuffer.probe_update_offset               = volume.probe_update_offset;
  hostIndirectConstBuffer.probe_update_count                = volume.per_frame_probe_updates;
  //hostIndirectConstBuffer.probe_update_count                = 0;
  

  // Rotations
  const float rotation_scaler                               = 0.001f; 
  hostIndirectConstBuffer.random_rotation                   = Util::glms_euler_xyz(glm::vec3(Util::randomFloat(-1.0f, 1.0f) * rotation_scaler, 
                                                              Util::randomFloat(-1, 1) * rotation_scaler, Util::randomFloat(-1, 1) * rotation_scaler));

  // Resolution
  hostIndirectConstBuffer.resolution                        = glm::vec2(m_size.width, m_size.height);

  const uint32_t num_probes                                 = volume.probe_count_x * volume.probe_count_y * volume.probe_count_z;
  volume.probe_update_offset = (volume.probe_update_offset + volume.per_frame_probe_updates) % num_probes;
  volume.per_frame_probe_updates    = scene.gi_per_frame_probes_update;





  // Constants Buffer
  VkBuffer deviceIndirectConstBuffer = m_bIndirectConstants.buffer;
  auto     indirectConstUsageStages  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;


  // Ensure that the modified UBO is not visible to previous frames.
  VkBufferMemoryBarrier beforeBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  beforeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  beforeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  beforeBarrier.buffer        = deviceIndirectConstBuffer;
  beforeBarrier.offset        = 0;
  beforeBarrier.size          = sizeof(hostIndirectConstBuffer);

  vkCmdPipelineBarrier(cmdBuf, indirectConstUsageStages, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_DEVICE_GROUP_BIT,
                       0,
                       nullptr, 1, &beforeBarrier, 0, nullptr);


  // Schedule the host-to-device upload. (hostIndirectConstants is copied into the cmd
  // buffer so it is okay to deallocate when the function returns).
  vkCmdUpdateBuffer(cmdBuf, m_bIndirectConstants.buffer, 0, sizeof(Indirect_gpu_constants), &hostIndirectConstBuffer);
  
  // Making sure the updated UBO will be visible.
  VkBufferMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  afterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  afterBarrier.buffer        = deviceIndirectConstBuffer;
  afterBarrier.offset        = 0;
  afterBarrier.size          = sizeof(hostIndirectConstBuffer);
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, indirectConstUsageStages, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0, nullptr, 1, &afterBarrier, 0, nullptr);
                       
}





// Shader Binding Table
void HelloVulkan::createIndirectShaderBindingTable() {
  uint32_t missCount{1};
  uint32_t hitCount{1};
  auto     handleCount = 1 + missCount + hitCount;
  uint32_t handleSize  = m_rtProperties.shaderGroupHandleSize;

  // The SBT (buffer) need to have starting groups to be aligned and handles in the group to be aligned.
  uint32_t handleSizeAligned = nvh::align_up(handleSize, m_rtProperties.shaderGroupHandleAlignment);

  m_IndirectRgenRegion.stride = nvh::align_up(handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);
  m_IndirectRgenRegion.size = m_IndirectRgenRegion.stride;  // The size member of pRayGenShaderBindingTable must be equal to its stride member
  m_IndirectMissRegion.stride = handleSizeAligned;
  m_IndirectMissRegion.size   = nvh::align_up(missCount * handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);
  m_IndirectHitRegion.stride  = handleSizeAligned;
  m_IndirectHitRegion.size    = nvh::align_up(hitCount * handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);

  // Get the shader group handles
  uint32_t             dataSize = handleCount * handleSize;
  std::vector<uint8_t> handles(dataSize);
  auto result = vkGetRayTracingShaderGroupHandlesKHR(m_device, m_IndirectPipeline, 0, handleCount, dataSize, handles.data());
  assert(result == VK_SUCCESS);

  // Allocate a buffer for storing the SBT.
  VkDeviceSize sbtSize      = m_IndirectRgenRegion.size + m_IndirectMissRegion.size + m_IndirectHitRegion.size + m_IndirectCallRegion.size;
  m_IndirectSBTBuffer       = m_alloc.createBuffer(sbtSize,VK_BUFFER_USAGE_TRANSFER_SRC_BIT 
                                                  | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT 
                                                  | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 
                                                  | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  m_debug.setObjectName(m_IndirectSBTBuffer.buffer, std::string("SBT"));  // Give it a debug name for NSight.

  // Find the SBT addresses of each group
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, m_IndirectSBTBuffer.buffer};
  VkDeviceAddress           sbtAddress = vkGetBufferDeviceAddress(m_device, &info);
  m_IndirectRgenRegion.deviceAddress   = sbtAddress;
  m_IndirectMissRegion.deviceAddress   = sbtAddress + m_IndirectRgenRegion.size;
  m_IndirectHitRegion.deviceAddress    = sbtAddress + m_IndirectRgenRegion.size + m_IndirectMissRegion.size;

  // Helper to retrieve the handle data
  auto getHandle = [&](int i) { return handles.data() + i * handleSize; };

  // Map the SBT buffer and write in the handles.
  auto*    pSBTBuffer = reinterpret_cast<uint8_t*>(m_alloc.map(m_IndirectSBTBuffer));
  uint8_t* pData{nullptr};
  uint32_t handleIdx{0};
  // Raygen
  pData = pSBTBuffer;
  memcpy(pData, getHandle(handleIdx++), handleSize);
  // Miss
  pData = pSBTBuffer + m_IndirectRgenRegion.size;
  for(uint32_t c = 0; c < missCount; c++) {
    memcpy(pData, getHandle(handleIdx++), handleSize);
    pData += m_IndirectMissRegion.stride;
  }
  // Hit
  pData = pSBTBuffer + m_IndirectRgenRegion.size + m_missRegion.size;
  for(uint32_t c = 0; c < hitCount; c++) {
    memcpy(pData, getHandle(handleIdx++), handleSize);
    pData += m_IndirectHitRegion.stride;
  }

  m_alloc.unmap(m_IndirectSBTBuffer);
  m_alloc.finalizeAndReleaseStaging();
}




nvvk::Image HelloVulkan::createStorageImage(const VkCommandBuffer& cmdBuf,
                                            VkDevice               device,
                                            VkPhysicalDevice       physicalDevice,
                                            uint32_t               width,
                                            uint32_t               height,
                                            VkFormat               format) {
  nvvk::Image image;

  std::array<uint8_t, 4> color{255u, 255u, 255u, 255u};
  VkDeviceSize           bufferSize = sizeof(color);

  // Image creation
  VkImageCreateInfo imageCreateInfo{};
  imageCreateInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageCreateInfo.imageType     = VK_IMAGE_TYPE_2D;
  imageCreateInfo.extent.width  = width;
  imageCreateInfo.extent.height = height;
  imageCreateInfo.extent.depth  = 1;
  imageCreateInfo.mipLevels     = 1;
  imageCreateInfo.arrayLayers   = 1;
  imageCreateInfo.format        = format;
  imageCreateInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageCreateInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageCreateInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
  imageCreateInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

  image                        = m_alloc.createImage(cmdBuf, bufferSize, color.data(), imageCreateInfo);
  image.format = format;  // Store the format


  VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);

  // Transition image layout to VK_IMAGE_LAYOUT_GENERAL
  nvvk::cmdBarrierImageLayout(cmdBuf, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

  return image;
}



void HelloVulkan::createDebugRender() {
  m_alloc.destroy(m_debugTexture);
  m_alloc.destroy(m_debugDepth);

  // Creating the normal image
  {
    auto colorCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_debugTextureFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    nvvk::Image           image           = m_alloc.createImage(colorCreateInfo);
    VkImageViewCreateInfo ivInfo          = nvvk::makeImageViewCreateInfo(image.image, colorCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_debugTexture                        = m_alloc.createTexture(image, ivInfo, sampler);
    m_debugTexture.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }


  // Creating the depth buffer
  {
    auto depthCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_debugDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    nvvk::Image image = m_alloc.createImage(depthCreateInfo);

    VkImageViewCreateInfo depthStencilView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthStencilView.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    depthStencilView.format           = m_debugDepthFormat;
    depthStencilView.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthStencilView.image            = image.image;

    m_debugDepth = m_alloc.createTexture(image, depthStencilView);
  }

  // Setting the image layout for all attachments
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_debugTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_debugDepth.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Creating a render pass for the offscreen
  if(!m_debugRenderPass) {
    m_debugRenderPass = nvvk::createRenderPass(m_device, {m_debugTextureFormat}, m_debugDepthFormat, 1, true, true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  }

  // Creating the framebuffer for offscreen
  std::vector<VkImageView> attachments = {m_debugTexture.descriptor.imageView, m_debugDepth.descriptor.imageView};
  


  vkDestroyFramebuffer(m_device, m_debugFramebuffer, nullptr);
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass      = m_debugRenderPass;
  info.attachmentCount = static_cast<uint32_t>(attachments.size());
  info.pAttachments    = attachments.data();
  info.width           = m_size.width;
  info.height          = m_size.height;
  info.layers          = 1;
  vkCreateFramebuffer(m_device, &info, nullptr, &m_debugFramebuffer);
}

void HelloVulkan::drawDebug(VkCommandBuffer cmdBuf) {
  VkDeviceSize offset{0};

  m_debug.beginLabel(cmdBuf, "Debug");

  // Dynamic Viewport
  setViewport(cmdBuf);

  std::vector<VkDescriptorSet> descSets{m_rtDescSet, m_descSet};

  // Drawing all triangles
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipelineLayout, 0, (uint32_t)descSets.size(),
                          descSets.data(), 0, nullptr);

  for(const HelloVulkan::ObjInstance& inst : m_debugInstances) {
    auto& model            = m_debugObjModel[inst.objIndex];
    m_pcDebug.objIndex    = inst.objIndex;  // Telling which object is drawn
    m_pcDebug.modelMatrix = inst.transform;
    

    vkCmdPushConstants(cmdBuf, m_debugPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantDebug), &m_pcDebug);
    vkCmdBindVertexBuffers(cmdBuf, 0, 1, &model.vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmdBuf, model.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmdBuf, model.nbIndices, volume.get_total_probes(), 0, 0, 0);
  }

  m_debug.endLabel(cmdBuf);
}


void HelloVulkan::createDebugPipeline() {
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantDebug)};

  std::vector<VkDescriptorSetLayout> debugSetLayouts = {m_rtDescSetLayout, m_descSetLayout};

  // Creating the Pipeline Layout
  VkPipelineLayoutCreateInfo debugCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  debugCreateInfo.setLayoutCount           = static_cast<uint32_t>(debugSetLayouts.size());
  debugCreateInfo.pSetLayouts              = debugSetLayouts.data();
  debugCreateInfo.pushConstantRangeCount   = 1;
  debugCreateInfo.pPushConstantRanges      = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &debugCreateInfo, nullptr, &m_debugPipelineLayout);


  // Creating the Pipeline
  std::vector<std::string>                paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined debugGpb(m_device, m_debugPipelineLayout, m_debugRenderPass);
  debugGpb.depthStencilState.depthTestEnable = VK_TRUE;
  debugGpb.depthStencilState.depthWriteEnable = VK_TRUE;
  debugGpb.depthStencilState.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;


  debugGpb.addShader(nvh::loadFile("spv/debugVertex.vert.spv", true, paths, true), VK_SHADER_STAGE_VERTEX_BIT);
  debugGpb.addShader(nvh::loadFile("spv/debugFragment.frag.spv", true, paths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  debugGpb.addBindingDescription({0, sizeof(VertexObj)});

  debugGpb.addAttributeDescriptions({
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, pos))},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, nrm))},
      {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, color))},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, texCoord))},
  });

  // Define color blend attachment states for the two color attachments

  std::array<VkPipelineColorBlendAttachmentState, 2> colorBlendAttachments = {};
  for(auto& blendAttachment : colorBlendAttachments) {
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;
  }

  // Create color blend state create info with the correct number of attachments
  VkPipelineColorBlendStateCreateInfo debugColorBlending = {};
  debugColorBlending.sType                                 = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  debugColorBlending.logicOpEnable                         = VK_FALSE;
  debugColorBlending.attachmentCount                       = static_cast<uint32_t>(colorBlendAttachments.size());
  debugColorBlending.pAttachments                          = colorBlendAttachments.data();

  debugGpb.colorBlendState = debugColorBlending;

  m_debugPipeline = debugGpb.createPipeline();

  m_debug.setObjectName(m_debugPipeline, "Debug");
}


/*
void HelloVulkan::createDebugPipeline()
{
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantRaster)};

  std::vector<VkDescriptorSetLayout> debugSetLayouts = {m_rtDescSetLayout, m_descSetLayout};

  // Creating the Pipeline Layout
  VkPipelineLayoutCreateInfo debugCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  debugCreateInfo.setLayoutCount         = static_cast<uint32_t>(debugSetLayouts.size());
  debugCreateInfo.pSetLayouts            = debugSetLayouts.data();
  debugCreateInfo.pushConstantRangeCount = 1;
  debugCreateInfo.pPushConstantRanges    = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &debugCreateInfo, nullptr, &m_debugPipelineLayout);

  // Creating the Pipeline
  std::vector<std::string> paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined debugGpb(m_device, m_debugPipelineLayout, m_offscreenRenderPass);  // Use m_offscreenRenderPass
  debugGpb.depthStencilState.depthTestEnable  = VK_TRUE;
  debugGpb.depthStencilState.depthWriteEnable = VK_TRUE;
  debugGpb.depthStencilState.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

  debugGpb.addShader(nvh::loadFile("spv/debugVertex.vert.spv", true, paths, true), VK_SHADER_STAGE_VERTEX_BIT);
  debugGpb.addShader(nvh::loadFile("spv/debugFragment.frag.spv", true, paths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  debugGpb.addBindingDescription({0, sizeof(VertexObj)});

  debugGpb.addAttributeDescriptions({
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, pos))},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, nrm))},
      {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, color))},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, texCoord))},
  });

  std::array<VkPipelineColorBlendAttachmentState, 2> colorBlendAttachments = {};
  for(auto& blendAttachment : colorBlendAttachments)
  {
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;
  }

  VkPipelineColorBlendStateCreateInfo debugColorBlending = {};
  debugColorBlending.sType                               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  debugColorBlending.logicOpEnable                       = VK_FALSE;
  debugColorBlending.attachmentCount                     = static_cast<uint32_t>(colorBlendAttachments.size());
  debugColorBlending.pAttachments                        = colorBlendAttachments.data();

  debugGpb.colorBlendState = debugColorBlending;

  m_debugPipeline = debugGpb.createPipeline();

  m_debug.setObjectName(m_debugPipeline, "Debug");
}*/