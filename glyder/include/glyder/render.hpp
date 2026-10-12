#pragma once

// glyder's Vulkan renderer: the same code draws a Quest's eye buffers, the desktop window and the
// headless captures used for tests and documentation.
//
// One render pass per frame, both eyes at once with multiview, 4x MSAA resolved on tile into the
// target, transient lazily-allocated colour and depth so a tiled mobile GPU never writes them to
// memory, reversed-Z with an infinite far plane. Opaque kit instances first (sorted by mesh, one
// instanced draw each), then the sky only where nothing was drawn, then panels, crisp strokes and
// additive glow lines.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <glyder/draw.hpp>
#include <glyder/kit.hpp>
#include <glyder/math.hpp>

namespace glyder::render {

struct Gpu {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{0};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    VkCommandPool pool{VK_NULL_HANDLE};
    bool ownsInstance{false}, ownsDevice{false};

    // Create an instance and device for the desktop window or headless use. `instanceExtensions`
    // adds e.g. the surface extensions; `swapchain` enables VK_KHR_swapchain on the device.
    static std::unique_ptr<Gpu> create(std::span<const char* const> instanceExtensions, bool swapchain, bool validation,
                                       std::string* error);
    // Wrap an instance and device someone else created (OpenXR's XR_KHR_vulkan_enable2).
    static std::unique_ptr<Gpu> adopt(VkInstance instance, VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily,
                                      std::uint32_t queueIndex);
    ~Gpu();

    [[nodiscard]] std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback = 0) const;
    [[nodiscard]] std::string name() const { return properties.deviceName; }
};

struct Buffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void* mapped{nullptr};
    VkDeviceSize size{0};
};

struct Target {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE}; // a 2D array view with `layers` layers
    VkExtent2D extent{};
};

class Renderer {
public:
    // `finalLayout` is the layout the target is left in: COLOR_ATTACHMENT_OPTIMAL for OpenXR,
    // TRANSFER_SRC_OPTIMAL for the desktop blit and headless readback.
    Renderer(Gpu& gpu, VkFormat colourFormat, std::uint32_t viewCount, VkSampleCountFlagBits samples, VkImageLayout finalLayout,
             std::uint32_t framesInFlight = 2);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshId addMesh(const kit::Mesh& mesh);
    void replaceMesh(MeshId id, const kit::Mesh& mesh);

    // Record and submit one frame. Waits only for the frame slot it reuses. `wait` and `signal` are
    // optional semaphores for a presentation engine.
    void render(const FrameData& frame, const Target& target, VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    void waitIdle();

    [[nodiscard]] std::uint32_t viewCount() const { return viewCount_; }
    [[nodiscard]] VkFormat colourFormat() const { return colourFormat_; }
    [[nodiscard]] std::size_t meshCount() const { return meshes_.size(); }
    [[nodiscard]] std::uint64_t trianglesLastFrame() const { return trianglesLastFrame_; }
    [[nodiscard]] std::uint32_t drawCallsLastFrame() const { return drawCallsLastFrame_; }

private:
    struct MeshRange {
        std::uint32_t firstIndex{0}, indexCount{0};
        std::int32_t vertexOffset{0};
        bool dynamic{false};
        Buffer vb, ib; // only for replaced (dynamic) meshes
    };
    struct Slot {
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        Buffer uniforms, instances, lines, panels, post;
        VkDescriptorSet set{VK_NULL_HANDLE}, postSet{VK_NULL_HANDLE};
    };
    struct Attachments {
        VkExtent2D extent{};
        VkImage colour{VK_NULL_HANDLE}, depth{VK_NULL_HANDLE};
        VkDeviceMemory colourMem{VK_NULL_HANDLE}, depthMem{VK_NULL_HANDLE};
        VkImageView colourView{VK_NULL_HANDLE}, depthView{VK_NULL_HANDLE};
    };

    VkRenderPass makeScenePass(VkImageLayout finalLayout);
    void createPost(VkImageLayout finalLayout);
    void ensureIntermediate(VkExtent2D extent);
    void destroyIntermediate();
    void createPipelines();
    void ensureAttachments(VkExtent2D extent);
    void destroyAttachments();
    VkFramebuffer framebuffer(const Target& t);
    VkFramebuffer postFramebuffer(const Target& t);
    void ensure(Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage);
    void uploadStatic();

    Gpu& gpu_;
    VkFormat colourFormat_;
    VkFormat depthFormat_{VK_FORMAT_D32_SFLOAT};
    std::uint32_t viewCount_;
    VkSampleCountFlagBits samples_;
    VkRenderPass pass_{VK_NULL_HANDLE};          // straight into the target
    VkRenderPass passToTexture_{VK_NULL_HANDLE}; // into the intermediate, for the post pass
    VkRenderPass postPass_{VK_NULL_HANDLE};
    VkDescriptorSetLayout postSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout postLayout_{VK_NULL_HANDLE};
    VkPipeline post_{VK_NULL_HANDLE};
    VkDescriptorPool postPool_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    struct Intermediate {
        VkExtent2D extent{};
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkFramebuffer framebuffer{VK_NULL_HANDLE};
    } mid_;
    std::vector<std::pair<VkImageView, VkFramebuffer>> postFramebuffers_;
    VkDescriptorSetLayout setLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline mesh_{VK_NULL_HANDLE}, sky_{VK_NULL_HANDLE}, glow_{VK_NULL_HANDLE}, solid_{VK_NULL_HANDLE}, panel_{VK_NULL_HANDLE};
    VkPipeline glowUi_{VK_NULL_HANDLE}, solidUi_{VK_NULL_HANDLE}, panelUi_{VK_NULL_HANDLE}; // no depth test
    std::vector<Slot> slots_;
    std::uint32_t slot_{0};
    Attachments att_;
    std::vector<std::pair<VkImageView, VkFramebuffer>> framebuffers_;
    // All static meshes share one vertex and one index buffer.
    std::vector<kit::Vertex> stagedVertices_;
    std::vector<std::uint32_t> stagedIndices_;
    bool staticDirty_{true};
    Buffer staticVb_, staticIb_, corners_;
    std::vector<MeshRange> meshes_;
    std::vector<DrawItem> sorted_;
    std::uint64_t trianglesLastFrame_{0};
    std::uint32_t drawCallsLastFrame_{0};
};

// Offscreen target for headless rendering and the desktop window's blit source.
class OffscreenTarget {
public:
    OffscreenTarget(Gpu& gpu, VkFormat format, VkExtent2D extent, std::uint32_t layers);
    ~OffscreenTarget();
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;
    [[nodiscard]] Target target() const { return {image_, view_, extent_}; }
    // Copy one layer back as tightly packed RGBA8 (the renderer must have finished the frame).
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint32_t layer);

private:
    Gpu& gpu_;
    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkExtent2D extent_;
    std::uint32_t layers_;
};

void destroy(Gpu& gpu, Buffer& b);
Buffer createBuffer(Gpu& gpu, VkDeviceSize bytes, VkBufferUsageFlags usage, bool hostVisible);

} // namespace glyder::render
