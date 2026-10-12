#include <glyder/render.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace glyder::render {

namespace {

#include "shaders.inc"

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("vulkan: ") + what + " failed (" + std::to_string(static_cast<int>(r)) + ")");
}

constexpr VkClearColorValue kBlack{{0, 0, 0, 1}};

struct GpuLine {
    float a[3];
    float width;
    float b[3];
    float intensity;
    std::uint8_t colour[4];
};
static_assert(sizeof(GpuLine) == 36);

struct GpuInstance {
    float rows[3][4];
    std::uint8_t tint[4];
    std::uint8_t params[4];
};
static_assert(sizeof(GpuInstance) == 56);

std::uint8_t unorm8(float v) {
    return static_cast<std::uint8_t>(std::lround(clamp(v, 0.0f, 1.0f) * 255.0f));
}

VkShaderModule module(VkDevice device, const std::uint32_t* code, std::size_t words) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule m;
    check(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

enum class Blend { Opaque, Alpha, Additive };

struct PipelineDesc {
    VkShaderModule vs, fs;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    Blend blend{Blend::Opaque};
    bool depthTest{true};
    bool depthWrite{true};
    bool cull{false};
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Gpu

std::unique_ptr<Gpu> Gpu::create(std::span<const char* const> instanceExtensions, bool swapchain, bool validation, std::string* error) {
    auto gpu = std::make_unique<Gpu>();
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "glyder";
        app.applicationVersion = VK_MAKE_VERSION(0, 4, 0);
        app.pEngineName = "glyder";
        app.apiVersion = VK_API_VERSION_1_1;
        std::vector<const char*> layers;
        if (validation) {
            std::uint32_t n = 0;
            vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> props(n);
            vkEnumerateInstanceLayerProperties(&n, props.data());
            for (const auto& p : props)
                if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        ici.ppEnabledLayerNames = layers.data();
        ici.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
        ici.ppEnabledExtensionNames = instanceExtensions.data();
        check(vkCreateInstance(&ici, nullptr, &gpu->instance), "vkCreateInstance");
        gpu->ownsInstance = true;

        std::uint32_t n = 0;
        vkEnumeratePhysicalDevices(gpu->instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devices(n);
        vkEnumeratePhysicalDevices(gpu->instance, &n, devices.data());
        // GLYDER_GPU=<substring> picks a device by name (e.g. "llvmpipe" for a software rasteriser).
        const char* want = std::getenv("GLYDER_GPU");
        int best = -1;
        for (std::uint32_t i = 0; i < n; ++i) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devices[i], &p);
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                 : 1;
            if (want && std::strstr(p.deviceName, want)) score = 10;
            if (best < 0 || score > best) {
                best = score;
                gpu->physical = devices[i];
            }
        }
        if (!gpu->physical) throw std::runtime_error("no Vulkan device");
        vkGetPhysicalDeviceProperties(gpu->physical, &gpu->properties);
        vkGetPhysicalDeviceMemoryProperties(gpu->physical, &gpu->memory);

        std::uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu->physical, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu->physical, &qn, qs.data());
        bool found = false;
        for (std::uint32_t i = 0; i < qn && !found; ++i)
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gpu->queueFamily = i;
                found = true;
            }
        if (!found) throw std::runtime_error("no graphics queue");

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = gpu->queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        multiview.multiview = VK_TRUE;
        const char* swapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &multiview;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = swapchain ? 1u : 0u;
        dci.ppEnabledExtensionNames = swapchain ? &swapchainExt : nullptr;
        check(vkCreateDevice(gpu->physical, &dci, nullptr, &gpu->device), "vkCreateDevice");
        gpu->ownsDevice = true;
        vkGetDeviceQueue(gpu->device, gpu->queueFamily, 0, &gpu->queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gpu->queueFamily;
        check(vkCreateCommandPool(gpu->device, &pci, nullptr, &gpu->pool), "vkCreateCommandPool");
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
    return gpu;
}

std::unique_ptr<Gpu> Gpu::adopt(VkInstance instance, VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily,
                                std::uint32_t queueIndex) {
    auto gpu = std::make_unique<Gpu>();
    gpu->instance = instance;
    gpu->physical = physical;
    gpu->device = device;
    gpu->queueFamily = queueFamily;
    vkGetPhysicalDeviceProperties(physical, &gpu->properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &gpu->memory);
    vkGetDeviceQueue(device, queueFamily, queueIndex, &gpu->queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    check(vkCreateCommandPool(device, &pci, nullptr, &gpu->pool), "vkCreateCommandPool");
    return gpu;
}

Gpu::~Gpu() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (ownsDevice) vkDestroyDevice(device, nullptr);
    }
    if (instance && ownsInstance) vkDestroyInstance(instance, nullptr);
}

std::uint32_t Gpu::memoryType(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) const {
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
    if (fallback)
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & fallback) == fallback) return i;
    throw std::runtime_error("vulkan: no suitable memory type");
}

Buffer createBuffer(Gpu& gpu, VkDeviceSize bytes, VkBufferUsageFlags usage, bool hostVisible) {
    Buffer b;
    b.size = std::max<VkDeviceSize>(bytes, 256);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size;
    bci.usage = usage;
    check(vkCreateBuffer(gpu.device, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        hostVisible ? gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(gpu.device, &mai, nullptr, &b.memory), "vkAllocateMemory");
    vkBindBufferMemory(gpu.device, b.buffer, b.memory, 0);
    if (hostVisible) vkMapMemory(gpu.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return b;
}

void destroy(Gpu& gpu, Buffer& b) {
    if (b.buffer) vkDestroyBuffer(gpu.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(gpu.device, b.memory, nullptr);
    b = {};
}

// ---------------------------------------------------------------------------------------------
// Renderer

Renderer::Renderer(Gpu& gpu, VkFormat colourFormat, std::uint32_t viewCount, VkSampleCountFlagBits samples, VkImageLayout finalLayout,
                   std::uint32_t framesInFlight)
    : gpu_(gpu), colourFormat_(colourFormat), viewCount_(std::clamp(viewCount, 1u, 2u)), samples_(samples) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu_.physical, VK_FORMAT_D32_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depthFormat_ = VK_FORMAT_D24_UNORM_S8_UINT;
    pass_ = makeScenePass(finalLayout);

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                         nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(gpu_.device, &dl, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout_;
    check(vkCreatePipelineLayout(gpu_.device, &pl, nullptr, &layout_), "vkCreatePipelineLayout");
    createPipelines();

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = framesInFlight;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    check(vkCreateDescriptorPool(gpu_.device, &dp, nullptr, &descriptorPool_), "vkCreateDescriptorPool");

    slots_.resize(framesInFlight);
    for (Slot& s : slots_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = gpu_.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(gpu_.device, &cai, &s.cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(gpu_.device, &fci, nullptr, &s.fence), "vkCreateFence");
        s.uniforms = createBuffer(gpu_, sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descriptorPool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout_;
        check(vkAllocateDescriptorSets(gpu_.device, &dai, &s.set), "vkAllocateDescriptorSets");
        VkDescriptorBufferInfo bi{s.uniforms.buffer, 0, sizeof(SceneUniforms)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);
    }

    // The six corners of a line ribbon: (end, side).
    const float corners[12] = {0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1};
    corners_ = createBuffer(gpu_, sizeof corners, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    std::memcpy(corners_.mapped, corners, sizeof corners);

    createPost(finalLayout);
    for (kit::Shape s : kit::allShapes()) addMesh(kit::build(s));
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(gpu_.device);
    for (Slot& s : slots_) {
        destroy(gpu_, s.uniforms);
        destroy(gpu_, s.instances);
        destroy(gpu_, s.lines);
        destroy(gpu_, s.panels);
        destroy(gpu_, s.post);
        vkDestroyFence(gpu_.device, s.fence, nullptr);
        vkFreeCommandBuffers(gpu_.device, gpu_.pool, 1, &s.cmd);
    }
    for (MeshRange& m : meshes_) {
        destroy(gpu_, m.vb);
        destroy(gpu_, m.ib);
    }
    destroy(gpu_, staticVb_);
    destroy(gpu_, staticIb_);
    destroy(gpu_, corners_);
    for (auto& [view, fb] : framebuffers_) vkDestroyFramebuffer(gpu_.device, fb, nullptr);
    for (auto& [view, fb] : postFramebuffers_) vkDestroyFramebuffer(gpu_.device, fb, nullptr);
    destroyIntermediate();
    destroyAttachments();
    vkDestroyPipeline(gpu_.device, post_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, postLayout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, postPool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, postSetLayout_, nullptr);
    vkDestroySampler(gpu_.device, sampler_, nullptr);
    vkDestroyRenderPass(gpu_.device, passToTexture_, nullptr);
    vkDestroyRenderPass(gpu_.device, postPass_, nullptr);
    for (VkPipeline p : {mesh_, sky_, glow_, solid_, panel_, glowUi_, solidUi_, panelUi_})
        if (p) vkDestroyPipeline(gpu_.device, p, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, setLayout_, nullptr);
    vkDestroyRenderPass(gpu_.device, pass_, nullptr);
}

VkRenderPass Renderer::makeScenePass(VkImageLayout finalLayout) {
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkAttachmentDescription> att;
    VkAttachmentDescription colour{};
    colour.format = colourFormat_;
    colour.samples = samples_;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colour.finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : finalLayout;
    att.push_back(colour);
    VkAttachmentDescription depth{};
    depth.format = depthFormat_;
    depth.samples = samples_;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att.push_back(depth);
    if (msaa) {
        VkAttachmentDescription resolve{};
        resolve.format = colourFormat_;
        resolve.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout = finalLayout;
        att.push_back(resolve);
    }
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rref{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pResolveAttachments = msaa ? &rref : nullptr;
    sub.pDepthStencilAttachment = &dref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    const std::uint32_t mask = (1u << viewCount_) - 1u;
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &mask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &mask;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.pNext = &mv;
    rp.attachmentCount = static_cast<std::uint32_t>(att.size());
    rp.pAttachments = att.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VkRenderPass pass;
    check(vkCreateRenderPass(gpu_.device, &rp, nullptr, &pass), "vkCreateRenderPass");
    return pass;
}

void Renderer::createPost(VkImageLayout finalLayout) {
    VkDevice d = gpu_.device;
    passToTexture_ = makeScenePass(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkAttachmentDescription out{};
    out.format = colourFormat_;
    out.samples = VK_SAMPLE_COUNT_1_BIT;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    out.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    out.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.finalLayout = finalLayout;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    const std::uint32_t mask = (1u << viewCount_) - 1u;
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &mask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &mask;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.pNext = &mv;
    rp.attachmentCount = 1;
    rp.pAttachments = &out;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    check(vkCreateRenderPass(d, &rp, nullptr, &postPass_), "vkCreateRenderPass(post)");

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(d, &sci, nullptr, &sampler_), "vkCreateSampler");
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    check(vkCreateDescriptorSetLayout(d, &dl, nullptr, &postSetLayout_), "vkCreateDescriptorSetLayout(post)");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &postSetLayout_;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &postLayout_), "vkCreatePipelineLayout(post)");

    const VkShaderModule vs = module(d, k_post_vert, sizeof(k_post_vert) / 4), fs = module(d, k_post_frag, sizeof(k_post_frag) / 4);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dy;
    gp.layout = postLayout_;
    gp.renderPass = postPass_;
    check(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &post_), "vkCreateGraphicsPipelines(post)");
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);

    const std::uint32_t n = static_cast<std::uint32_t>(slots_.size());
    const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = n;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    check(vkCreateDescriptorPool(d, &dp, nullptr, &postPool_), "vkCreateDescriptorPool(post)");
    for (Slot& sl : slots_) {
        sl.post = createBuffer(gpu_, sizeof(PostParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = postPool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &postSetLayout_;
        check(vkAllocateDescriptorSets(d, &dai, &sl.postSet), "vkAllocateDescriptorSets(post)");
        VkDescriptorBufferInfo bi{sl.post.buffer, 0, sizeof(PostParams)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sl.postSet;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
    }
}

void Renderer::destroyIntermediate() {
    VkDevice d = gpu_.device;
    if (mid_.framebuffer) vkDestroyFramebuffer(d, mid_.framebuffer, nullptr);
    if (mid_.view) vkDestroyImageView(d, mid_.view, nullptr);
    if (mid_.image) vkDestroyImage(d, mid_.image, nullptr);
    if (mid_.memory) vkFreeMemory(d, mid_.memory, nullptr);
    mid_ = {};
}

void Renderer::ensureIntermediate(VkExtent2D extent) {
    if (mid_.image && mid_.extent.width == extent.width && mid_.extent.height == extent.height) return;
    vkDeviceWaitIdle(gpu_.device);
    destroyIntermediate();
    mid_.extent = extent;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = colourFormat_;
    ic.extent = {extent.width, extent.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = viewCount_;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(gpu_.device, &ic, nullptr, &mid_.image), "vkCreateImage(intermediate)");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu_.device, mid_.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu_.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(gpu_.device, &mai, nullptr, &mid_.memory), "vkAllocateMemory(intermediate)");
    vkBindImageMemory(gpu_.device, mid_.image, mid_.memory, 0);
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = mid_.image;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vc.format = colourFormat_;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, viewCount_};
    check(vkCreateImageView(gpu_.device, &vc, nullptr, &mid_.view), "vkCreateImageView(intermediate)");
    std::vector<VkImageView> views;
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        views = {att_.colourView, att_.depthView, mid_.view};
    else
        views = {mid_.view, att_.depthView};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = passToTexture_;
    fci.attachmentCount = static_cast<std::uint32_t>(views.size());
    fci.pAttachments = views.data();
    fci.width = extent.width;
    fci.height = extent.height;
    fci.layers = 1;
    check(vkCreateFramebuffer(gpu_.device, &fci, nullptr, &mid_.framebuffer), "vkCreateFramebuffer(intermediate)");
    VkDescriptorImageInfo ii{sampler_, mid_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (Slot& sl : slots_) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sl.postSet;
        w.dstBinding = 1;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);
    }
}

VkFramebuffer Renderer::postFramebuffer(const Target& t) {
    for (auto& [view, fb] : postFramebuffers_)
        if (view == t.view) return fb;
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = postPass_;
    fci.attachmentCount = 1;
    fci.pAttachments = &t.view;
    fci.width = t.extent.width;
    fci.height = t.extent.height;
    fci.layers = 1;
    VkFramebuffer fb;
    check(vkCreateFramebuffer(gpu_.device, &fci, nullptr, &fb), "vkCreateFramebuffer(post)");
    postFramebuffers_.emplace_back(t.view, fb);
    return fb;
}

void Renderer::createPipelines() {
    VkDevice d = gpu_.device;
    auto build = [&](const PipelineDesc& p) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, p.vs, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, p.fs, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(p.bindings.size());
        vi.pVertexBindingDescriptions = p.bindings.data();
        vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(p.attributes.size());
        vi.pVertexAttributeDescriptions = p.attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = p.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        // Outward faces wind counter-clockwise seen from outside; the y-down projection and Vulkan's
        // framebuffer orientation cancel, so they stay counter-clockwise on screen.
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples_;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = p.depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = p.depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed-Z
        VkPipelineColorBlendAttachmentState cb{};
        cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (p.blend == Blend::Alpha) {
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (p.blend == Blend::Additive) {
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cbs;
        gp.pDynamicState = &dy;
        gp.layout = layout_;
        gp.renderPass = pass_;
        VkPipeline pipe;
        check(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe), "vkCreateGraphicsPipelines");
        return pipe;
    };
#define GLYDER_MODULE(name) module(d, k_##name, sizeof(k_##name) / 4)
    const VkShaderModule meshVs = GLYDER_MODULE(mesh_vert), meshFs = GLYDER_MODULE(mesh_frag), skyVs = GLYDER_MODULE(sky_vert),
                         skyFs = GLYDER_MODULE(sky_frag), lineVs = GLYDER_MODULE(line_vert), glowFs = GLYDER_MODULE(glow_frag),
                         solidFs = GLYDER_MODULE(solid_frag), panelVs = GLYDER_MODULE(panel_vert), panelFs = GLYDER_MODULE(panel_frag);
#undef GLYDER_MODULE

    PipelineDesc mesh{meshVs, meshFs};
    mesh.bindings = {{0, sizeof(kit::Vertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(GpuInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    mesh.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},     {1, 0, VK_FORMAT_R8G8B8A8_SNORM, 12},
                       {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16},      {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                       {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16}, {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                       {6, 1, VK_FORMAT_R8G8B8A8_UNORM, 48},      {7, 1, VK_FORMAT_R8G8B8A8_UNORM, 52}};
    mesh.cull = true;
    mesh_ = build(mesh);

    PipelineDesc sky{skyVs, skyFs};
    sky.depthWrite = false;
    sky_ = build(sky);

    PipelineDesc line{lineVs, glowFs};
    line.bindings = {{0, 8, VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(GpuLine), VK_VERTEX_INPUT_RATE_INSTANCE}};
    line.attributes = {{3, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                       {0, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                       {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
                       {2, 1, VK_FORMAT_R8G8B8A8_UNORM, 32}};
    line.depthWrite = false;
    line.blend = Blend::Additive;
    glow_ = build(line);
    line.depthTest = false;
    glowUi_ = build(line);
    line.depthTest = true;
    line.fs = solidFs;
    line.blend = Blend::Alpha;
    solid_ = build(line);
    line.depthTest = false;
    solidUi_ = build(line);

    PipelineDesc panel{panelVs, panelFs};
    panel.bindings = {{0, sizeof(PanelVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    panel.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
                        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 28},
                        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 44},
                        {4, 0, VK_FORMAT_R8G8B8A8_UNORM, 48}};
    panel.depthWrite = false;
    panel.blend = Blend::Alpha;
    panel_ = build(panel);
    panel.depthTest = false;
    panelUi_ = build(panel);

    for (VkShaderModule m : {meshVs, meshFs, skyVs, skyFs, lineVs, glowFs, solidFs, panelVs, panelFs}) vkDestroyShaderModule(d, m, nullptr);
}

void Renderer::destroyAttachments() {
    VkDevice d = gpu_.device;
    if (att_.colourView) vkDestroyImageView(d, att_.colourView, nullptr);
    if (att_.depthView) vkDestroyImageView(d, att_.depthView, nullptr);
    if (att_.colour) vkDestroyImage(d, att_.colour, nullptr);
    if (att_.depth) vkDestroyImage(d, att_.depth, nullptr);
    if (att_.colourMem) vkFreeMemory(d, att_.colourMem, nullptr);
    if (att_.depthMem) vkFreeMemory(d, att_.depthMem, nullptr);
    att_ = {};
}

void Renderer::ensureAttachments(VkExtent2D extent) {
    if (att_.extent.width == extent.width && att_.extent.height == extent.height && att_.depth) return;
    vkDeviceWaitIdle(gpu_.device);
    for (auto& [view, fb] : framebuffers_) vkDestroyFramebuffer(gpu_.device, fb, nullptr);
    framebuffers_.clear();
    for (auto& [view, fb] : postFramebuffers_) vkDestroyFramebuffer(gpu_.device, fb, nullptr);
    postFramebuffers_.clear();
    destroyIntermediate();
    destroyAttachments();
    att_.extent = extent;
    auto make = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& img, VkDeviceMemory& mem,
                    VkImageView& view) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = format;
        ic.extent = {extent.width, extent.height, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = viewCount_;
        ic.samples = samples_;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        check(vkCreateImage(gpu_.device, &ic, nullptr, &img), "vkCreateImage");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(gpu_.device, img, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        // On a tiler these never leave tile memory: lazily allocated, often never backed at all.
        mai.memoryTypeIndex =
            gpu_.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(gpu_.device, &mai, nullptr, &mem), "vkAllocateMemory");
        vkBindImageMemory(gpu_.device, img, mem, 0);
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = img;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vc.format = format;
        vc.subresourceRange = {aspect, 0, 1, 0, viewCount_};
        check(vkCreateImageView(gpu_.device, &vc, nullptr, &view), "vkCreateImageView");
    };
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        make(colourFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, att_.colour, att_.colourMem, att_.colourView);
    make(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, att_.depth, att_.depthMem, att_.depthView);
}

VkFramebuffer Renderer::framebuffer(const Target& t) {
    for (auto& [view, fb] : framebuffers_)
        if (view == t.view) return fb;
    std::vector<VkImageView> views;
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        views = {att_.colourView, att_.depthView, t.view};
    else
        views = {t.view, att_.depthView};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass_;
    fci.attachmentCount = static_cast<std::uint32_t>(views.size());
    fci.pAttachments = views.data();
    fci.width = t.extent.width;
    fci.height = t.extent.height;
    fci.layers = 1; // multiview addresses the layers
    VkFramebuffer fb;
    check(vkCreateFramebuffer(gpu_.device, &fci, nullptr, &fb), "vkCreateFramebuffer");
    framebuffers_.emplace_back(t.view, fb);
    return fb;
}

MeshId Renderer::addMesh(const kit::Mesh& mesh) {
    MeshRange r;
    r.firstIndex = static_cast<std::uint32_t>(stagedIndices_.size());
    r.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    r.vertexOffset = static_cast<std::int32_t>(stagedVertices_.size());
    stagedVertices_.insert(stagedVertices_.end(), mesh.vertices.begin(), mesh.vertices.end());
    stagedIndices_.insert(stagedIndices_.end(), mesh.indices.begin(), mesh.indices.end());
    meshes_.push_back(std::move(r));
    staticDirty_ = true;
    return static_cast<MeshId>(meshes_.size() - 1);
}

void Renderer::replaceMesh(MeshId id, const kit::Mesh& mesh) {
    if (id >= meshes_.size()) return;
    vkDeviceWaitIdle(gpu_.device);
    MeshRange& r = meshes_[id];
    destroy(gpu_, r.vb);
    destroy(gpu_, r.ib);
    r.dynamic = true;
    r.vb = createBuffer(gpu_, mesh.vertices.size() * sizeof(kit::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    r.ib = createBuffer(gpu_, mesh.indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    if (!mesh.vertices.empty()) std::memcpy(r.vb.mapped, mesh.vertices.data(), mesh.vertices.size() * sizeof(kit::Vertex));
    if (!mesh.indices.empty()) std::memcpy(r.ib.mapped, mesh.indices.data(), mesh.indices.size() * 4);
    r.firstIndex = 0;
    r.vertexOffset = 0;
    r.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
}

void Renderer::uploadStatic() {
    vkDeviceWaitIdle(gpu_.device);
    destroy(gpu_, staticVb_);
    destroy(gpu_, staticIb_);
    staticVb_ = createBuffer(gpu_, stagedVertices_.size() * sizeof(kit::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    staticIb_ = createBuffer(gpu_, stagedIndices_.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    std::memcpy(staticVb_.mapped, stagedVertices_.data(), stagedVertices_.size() * sizeof(kit::Vertex));
    std::memcpy(staticIb_.mapped, stagedIndices_.data(), stagedIndices_.size() * 4);
    staticDirty_ = false;
}

void Renderer::ensure(Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    if (b.buffer && b.size >= bytes) return;
    destroy(gpu_, b);
    b = createBuffer(gpu_, std::max<VkDeviceSize>(bytes * 3 / 2, 64 * 1024), usage, true);
}

void Renderer::render(const FrameData& f, const Target& target, VkSemaphore wait, VkSemaphore signal) {
    if (staticDirty_) uploadStatic();
    ensureAttachments(target.extent);
    Slot& s = slots_[slot_];
    vkWaitForFences(gpu_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(gpu_.device, 1, &s.fence);

    std::memcpy(s.uniforms.mapped, &f.scene, sizeof(SceneUniforms));
    const bool post = f.post != nullptr;
    if (post) {
        ensureIntermediate(target.extent);
        std::memcpy(s.post.mapped, f.post, sizeof(PostParams));
    }

    // Instances, grouped by mesh.
    sorted_.assign(f.items.begin(), f.items.end());
    std::stable_sort(sorted_.begin(), sorted_.end(), [](const DrawItem& a, const DrawItem& b) { return a.mesh < b.mesh; });
    ensure(s.instances, sorted_.size() * sizeof(GpuInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto* inst = static_cast<GpuInstance*>(s.instances.mapped);
    for (std::size_t i = 0; i < sorted_.size(); ++i) {
        const DrawItem& d = sorted_[i];
        GpuInstance& g = inst[i];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) g.rows[r][c] = d.model.m[c][r];
        g.tint[0] = d.tint.r;
        g.tint[1] = d.tint.g;
        g.tint[2] = d.tint.b;
        g.tint[3] = unorm8(d.highlight);
        g.params[0] = unorm8(d.emissive / 4.0f);
        g.params[1] = 255;
        g.params[2] = d.contour ? 255 : 0;
        g.params[3] = 0;
    }

    // Lines: solid strokes then glow, one buffer.
    const std::size_t lineCount = f.solid.size() + f.glow.size() + f.uiSolid.size() + f.uiGlow.size();
    ensure(s.lines, lineCount * sizeof(GpuLine), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto* gl = static_cast<GpuLine*>(s.lines.mapped);
    std::size_t li = 0;
    for (const auto* list : {&f.solid, &f.glow, &f.uiSolid, &f.uiGlow})
        for (const Line& l : *list) {
            GpuLine& g = gl[li++];
            g.a[0] = l.a.x;
            g.a[1] = l.a.y;
            g.a[2] = l.a.z;
            g.width = l.width;
            g.b[0] = l.b.x;
            g.b[1] = l.b.y;
            g.b[2] = l.b.z;
            g.intensity = l.intensity;
            g.colour[0] = l.colour.r;
            g.colour[1] = l.colour.g;
            g.colour[2] = l.colour.b;
            g.colour[3] = l.alpha;
        }
    ensure(s.panels, (f.panels.size() + f.uiPanels.size()) * sizeof(PanelVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!f.panels.empty()) std::memcpy(s.panels.mapped, f.panels.data(), f.panels.size() * sizeof(PanelVertex));
    if (!f.uiPanels.empty())
        std::memcpy(static_cast<PanelVertex*>(s.panels.mapped) + f.panels.size(), f.uiPanels.data(),
                    f.uiPanels.size() * sizeof(PanelVertex));

    VkCommandBuffer cmd = s.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clears[3]{};
    clears[0].color = {{f.clear.x, f.clear.y, f.clear.z, f.clear.w}};
    clears[1].depthStencil = {0.0f, 0}; // reversed-Z: cleared to infinitely far
    clears[2].color = kBlack;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = post ? passToTexture_ : pass_;
    rb.framebuffer = post ? mid_.framebuffer : framebuffer(target);
    rb.renderArea = {{0, 0}, target.extent};
    rb.clearValueCount = samples_ != VK_SAMPLE_COUNT_1_BIT ? 3u : 2u;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0, 0, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0, 1};
    const VkRect2D sc{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &s.set, 0, nullptr);

    std::uint32_t draws = 0;
    std::uint64_t tris = 0;
    if (!sorted_.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 1, 1, &s.instances.buffer, &zero);
        bool staticBound = false;
        for (std::size_t i = 0; i < sorted_.size();) {
            std::size_t j = i;
            while (j < sorted_.size() && sorted_[j].mesh == sorted_[i].mesh) ++j;
            const MeshId id = sorted_[i].mesh;
            if (id < meshes_.size() && meshes_[id].indexCount) {
                const MeshRange& r = meshes_[id];
                if (r.dynamic) {
                    vkCmdBindVertexBuffers(cmd, 0, 1, &r.vb.buffer, &zero);
                    vkCmdBindIndexBuffer(cmd, r.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                    staticBound = false;
                } else if (!staticBound) {
                    vkCmdBindVertexBuffers(cmd, 0, 1, &staticVb_.buffer, &zero);
                    vkCmdBindIndexBuffer(cmd, staticIb_.buffer, 0, VK_INDEX_TYPE_UINT32);
                    staticBound = true;
                }
                const auto count = static_cast<std::uint32_t>(j - i);
                vkCmdDrawIndexed(cmd, r.indexCount, count, r.firstIndex, r.vertexOffset, static_cast<std::uint32_t>(i));
                ++draws;
                tris += static_cast<std::uint64_t>(r.indexCount / 3) * count;
            }
            i = j;
        }
    }
    if (f.drawSky) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        ++draws;
    }
    if (!f.panels.empty()) {
        const VkDeviceSize zero = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, panel_);
        vkCmdBindVertexBuffers(cmd, 0, 1, &s.panels.buffer, &zero);
        vkCmdDraw(cmd, static_cast<std::uint32_t>(f.panels.size()), 1, 0, 0);
        ++draws;
    }
    if (lineCount) {
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &corners_.buffer, &zero);
        vkCmdBindVertexBuffers(cmd, 1, 1, &s.lines.buffer, &zero);
        if (!f.solid.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_);
            vkCmdDraw(cmd, 6, static_cast<std::uint32_t>(f.solid.size()), 0, 0);
            ++draws;
        }
        if (!f.glow.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_);
            vkCmdDraw(cmd, 6, static_cast<std::uint32_t>(f.glow.size()), 0, static_cast<std::uint32_t>(f.solid.size()));
            ++draws;
        }
    }
    // The interface layer, over everything.
    const auto uiLineBase = static_cast<std::uint32_t>(f.solid.size() + f.glow.size());
    if (!f.uiPanels.empty()) {
        const VkDeviceSize zero = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, panelUi_);
        vkCmdBindVertexBuffers(cmd, 0, 1, &s.panels.buffer, &zero);
        vkCmdDraw(cmd, static_cast<std::uint32_t>(f.uiPanels.size()), 1, static_cast<std::uint32_t>(f.panels.size()), 0);
        ++draws;
    }
    if (!f.uiSolid.empty() || !f.uiGlow.empty()) {
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &corners_.buffer, &zero);
        vkCmdBindVertexBuffers(cmd, 1, 1, &s.lines.buffer, &zero);
        if (!f.uiSolid.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solidUi_);
            vkCmdDraw(cmd, 6, static_cast<std::uint32_t>(f.uiSolid.size()), 0, uiLineBase);
            ++draws;
        }
        if (!f.uiGlow.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glowUi_);
            vkCmdDraw(cmd, 6, static_cast<std::uint32_t>(f.uiGlow.size()), 0, uiLineBase + static_cast<std::uint32_t>(f.uiSolid.size()));
            ++draws;
        }
    }
    vkCmdEndRenderPass(cmd);
    if (post) {
        VkRenderPassBeginInfo pb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pb.renderPass = postPass_;
        pb.framebuffer = postFramebuffer(target);
        pb.renderArea = {{0, 0}, target.extent};
        vkCmdBeginRenderPass(cmd, &pb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &s.postSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        ++draws;
    }
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait ? 1u : 0u;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = signal ? 1u : 0u;
    si.pSignalSemaphores = &signal;
    check(vkQueueSubmit(gpu_.queue, 1, &si, s.fence), "vkQueueSubmit");
    slot_ = (slot_ + 1) % static_cast<std::uint32_t>(slots_.size());
    drawCallsLastFrame_ = draws;
    trianglesLastFrame_ = tris;
}

void Renderer::waitIdle() {
    vkDeviceWaitIdle(gpu_.device);
}

// ---------------------------------------------------------------------------------------------
// Offscreen target

OffscreenTarget::OffscreenTarget(Gpu& gpu, VkFormat format, VkExtent2D extent, std::uint32_t layers)
    : gpu_(gpu), extent_(extent), layers_(layers) {
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = format;
    ic.extent = {extent.width, extent.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = layers;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(gpu.device, &ic, nullptr, &image_), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.device, image_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(gpu.device, &mai, nullptr, &memory_), "vkAllocateMemory");
    vkBindImageMemory(gpu.device, image_, memory_, 0);
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = image_;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    check(vkCreateImageView(gpu.device, &vc, nullptr, &view_), "vkCreateImageView");
}

OffscreenTarget::~OffscreenTarget() {
    vkDeviceWaitIdle(gpu_.device);
    vkDestroyImageView(gpu_.device, view_, nullptr);
    vkDestroyImage(gpu_.device, image_, nullptr);
    vkFreeMemory(gpu_.device, memory_, nullptr);
}

std::vector<std::uint8_t> OffscreenTarget::read(std::uint32_t layer) {
    vkQueueWaitIdle(gpu_.queue);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    Buffer staging = createBuffer(gpu_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gpu_.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(gpu_.device, &cai, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, std::min(layer, layers_ - 1), 1};
    region.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(gpu_.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(gpu_.queue);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes));
    std::memcpy(out.data(), staging.mapped, out.size());
    vkFreeCommandBuffers(gpu_.device, gpu_.pool, 1, &cmd);
    destroy(gpu_, staging);
    return out;
}

} // namespace glyder::render
