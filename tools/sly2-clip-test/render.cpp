#include <cstdio>
#include <cstdlib>
#include <vector>
#include <vulkan/vulkan.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    VkResult r = (x);                                                          \
    if (r) {                                                                   \
      fprintf(stderr, "%s: %d\n", #x, r);                                      \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "Usage: %s vertex.spv geometry.spv fragment.spv output.rgba\n", argv[0]);
    return 2;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance;
  CHECK(vkCreateInstance(&ici, nullptr, &instance));
  uint32_t n = 0;
  CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
  if (!n) { fprintf(stderr, "No Vulkan device\n"); return 1; }
  std::vector<VkPhysicalDevice> devices(n);
  CHECK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
  VkPhysicalDevice gpu = devices[0];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(gpu, &props);
  printf("GPU %s\n", props.deviceName);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
  std::vector<VkQueueFamilyProperties> qs(n);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, qs.data());
  uint32_t q = 0;
  while (q < n && !(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    ++q;
  if (q == n) { fprintf(stderr, "No graphics queue\n"); return 1; }
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = q;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(gpu, &features);
  if (!features.geometryShader) { fprintf(stderr, "Geometry shaders not supported\n"); return 1; }
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.pEnabledFeatures = &features;
  VkDevice device;
  CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
  VkShaderModule modules[3];
  VkPipelineShaderStageCreateInfo stages[3]{};
  VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_VERTEX_BIT,
                                   VK_SHADER_STAGE_GEOMETRY_BIT,
                                   VK_SHADER_STAGE_FRAGMENT_BIT};
  for (int i = 0; i < 3; ++i) {
    FILE *f = fopen(argv[i + 1], "rb");
    if (!f)
      return 3;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    if (length < 20 || length % 4) { fclose(f); return 3; }
    size_t size = static_cast<size_t>(length);
    rewind(f);
    std::vector<uint32_t> bytes((size + 3) / 4);
    if (fread(bytes.data(), 1, size, f) != size) { fclose(f); return 3; }
    fclose(f);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size;
    mi.pCode = bytes.data();
    CHECK(vkCreateShaderModule(device, &mi, nullptr, &modules[i]));
    stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[i].stage = kinds[i];
    stages[i].module = modules[i];
    stages[i].pName = "main";
  }
  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  ri.attachmentCount = 1;
  ri.pAttachments = &att;
  ri.subpassCount = 1;
  ri.pSubpasses = &sub;
  VkRenderPass rp;
  CHECK(vkCreateRenderPass(device, &ri, nullptr, &rp));
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkPipelineLayout layout;
  CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0, 0, 320, 320, 0, 1};
  VkRect2D scissor{{0, 0}, {320, 320}};
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.pViewports = &viewport;
  vp.scissorCount = 1;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba{};
  ba.colorWriteMask = 15;
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo pi{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.stageCount = 3;
  pi.pStages = stages;
  pi.pVertexInputState = &vi;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pColorBlendState = &cb;
  pi.layout = layout;
  pi.renderPass = rp;
  puts("Creating pipeline");
  fflush(stdout);
  VkPipeline pipeline;
  CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr,
                                  &pipeline));
  puts("Pipeline compiled");
  const uint32_t W = 320, H = 320;
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  auto memoryType = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    return uint32_t(-1);
  };
  VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  im.imageType = VK_IMAGE_TYPE_2D;
  im.format = VK_FORMAT_R8G8B8A8_UNORM;
  im.extent = {W, H, 1};
  im.mipLevels = 1;
  im.arrayLayers = 1;
  im.samples = VK_SAMPLE_COUNT_1_BIT;
  im.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImage image;
  CHECK(vkCreateImage(device, &im, nullptr, &image));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device, image, &mr);
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ma.allocationSize = mr.size;
  ma.memoryTypeIndex = memoryType(mr.memoryTypeBits, 0);
  VkDeviceMemory imageMemory;
  CHECK(vkAllocateMemory(device, &ma, nullptr, &imageMemory));
  CHECK(vkBindImageMemory(device, image, imageMemory, 0));
  VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  iv.image = image;
  iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
  iv.format = im.format;
  iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view;
  CHECK(vkCreateImageView(device, &iv, nullptr, &view));
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = rp;
  fi.attachmentCount = 1;
  fi.pAttachments = &view;
  fi.width = W;
  fi.height = H;
  fi.layers = 1;
  VkFramebuffer fb;
  CHECK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = W * H * 4;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  CHECK(vkCreateBuffer(device, &bi, nullptr, &buffer));
  vkGetBufferMemoryRequirements(device, buffer, &mr);
  ma.allocationSize = mr.size;
  ma.memoryTypeIndex =
      memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bufferMemory;
  CHECK(vkAllocateMemory(device, &ma, nullptr, &bufferMemory));
  CHECK(vkBindBufferMemory(device, buffer, bufferMemory, 0));
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cp.queueFamilyIndex = q;
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(device, &cp, nullptr, &pool));
  VkCommandBufferAllocateInfo ca{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = pool;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  VkCommandBuffer cmd;
  CHECK(vkAllocateCommandBuffers(device, &ca, &cmd));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cmd, &begin));
  VkClearValue clear{};
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = rp;
  rb.framebuffer = fb;
  rb.renderArea = {{0, 0}, {W, H}};
  rb.clearValueCount = 1;
  rb.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdDraw(cmd, 3, 27, 0, 0);
  vkCmdEndRenderPass(cmd);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = iv.subresourceRange;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {W, H, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         buffer, 1, &copy);
  VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer = buffer;
  bb.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                       nullptr);
  CHECK(vkEndCommandBuffer(cmd));
  VkQueue queue;
  vkGetDeviceQueue(device, q, 0, &queue);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
  CHECK(vkQueueWaitIdle(queue));
  void *mapped;
  CHECK(vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
  FILE *out = fopen(argv[4], "wb");
  if (!out)
    return 4;
  if (fwrite(mapped, 1, W * H * 4, out) != W * H * 4) { fclose(out); return 4; }
  puts("PASS: rendered all 27 clipping cases");
  fclose(out);
  vkUnmapMemory(device, bufferMemory);
  vkDestroyCommandPool(device, pool, nullptr);
  vkDestroyFramebuffer(device, fb, nullptr);
  vkDestroyImageView(device, view, nullptr);
  vkDestroyImage(device, image, nullptr);
  vkFreeMemory(device, imageMemory, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, bufferMemory, nullptr);

  vkDestroyPipeline(device, pipeline, nullptr);
  vkDestroyPipelineLayout(device, layout, nullptr);
  vkDestroyRenderPass(device, rp, nullptr);
  for (auto m : modules)
    vkDestroyShaderModule(device, m, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}
