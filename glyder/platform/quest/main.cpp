// glyder on Meta Quest: OpenXR on Vulkan in a NativeActivity. No Java, no engine.
//
// The frame loop is the textbook low-latency one: xrWaitFrame paces us to the display, poses are
// located at the frame's predicted display time, both eyes are drawn in one multiview pass
// straight into the runtime's swapchain, and the frame is handed back with the poses it was drawn
// for. The display runs at the highest refresh rate the headset offers (120 Hz on a Quest 3S) with
// the CPU and GPU held at sustained-high performance.

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <sys/system_properties.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <vulkan/vulkan.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <glyder/app.hpp>
#include <glyder/net.hpp>
#include <glyder/render.hpp>
#include <glyder/version.hpp>

using namespace glyder;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "glyder", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "glyder", __VA_ARGS__)

namespace {

// The speaker: AAudio pulls from a single-producer, single-consumer ring that the frame loop keeps
// about 40 ms ahead, so the sound follows the device's own clock and never waits on a frame.
class Speaker {
public:
    bool open(int rate) {
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(b, 2);
        AAudioStreamBuilder_setSampleRate(b, rate);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setDataCallback(b, &Speaker::pull, this);
        const bool ok = AAudioStreamBuilder_openStream(b, &stream_) == AAUDIO_OK;
        AAudioStreamBuilder_delete(b);
        if (!ok) return false;
        rate_ = AAudioStream_getSampleRate(stream_);
        return AAudioStream_requestStart(stream_) == AAUDIO_OK;
    }
    void close() {
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
    }
    // Frames to add now to stay about 40 ms ahead of the device.
    [[nodiscard]] std::size_t wanted() const {
        const std::size_t target = static_cast<std::size_t>(rate_) / 25;
        const std::size_t have = (head_.load() - tail_.load()) / 2;
        return have < target ? target - have : 0;
    }
    void push(const float* stereo, std::size_t frames) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < frames * 2; ++i, ++h) ring_[h % ring_.size()] = stereo[i];
        head_.store(h, std::memory_order_release);
    }
    [[nodiscard]] int rate() const { return rate_; }

private:
    static aaudio_data_callback_result_t pull(AAudioStream*, void* user, void* data, std::int32_t frames) {
        auto* s = static_cast<Speaker*>(user);
        auto* out = static_cast<float*>(data);
        std::size_t t = s->tail_.load(std::memory_order_relaxed);
        const std::size_t h = s->head_.load(std::memory_order_acquire);
        for (std::int32_t i = 0; i < frames * 2; ++i) out[i] = t < h ? s->ring_[t++ % s->ring_.size()] : 0.0f;
        s->tail_.store(t, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    AAudioStream* stream_{nullptr};
    int rate_{48000};
    std::array<float, 48000> ring_{}; // half a second of stereo
    std::atomic<std::size_t> head_{0}, tail_{0};
};

class CameraCaptureHook {
public:
    ~CameraCaptureHook() { stop(); }

    bool start() {
        if (device_) return true;
        failed_.store(false);
        manager_ = ACameraManager_create();
        if (!manager_) {
            LOGE("camera hook: ACameraManager_create failed");
            return false;
        }
        ACameraIdList* ids = nullptr;
        if (ACameraManager_getCameraIdList(manager_, &ids) != ACAMERA_OK || !ids || ids->numCameras == 0) {
            LOGE("camera hook: no camera devices available");
            if (ids) ACameraManager_deleteCameraIdList(ids);
            stop();
            return false;
        }
        ACameraDevice_StateCallbacks deviceCallbacks{};
        deviceCallbacks.context = this;
        deviceCallbacks.onDisconnected = &CameraCaptureHook::disconnected;
        deviceCallbacks.onError = &CameraCaptureHook::error;
        camera_status_t opened = ACAMERA_ERROR_CAMERA_DISCONNECTED;
        for (int i = 0; i < ids->numCameras && opened != ACAMERA_OK; ++i)
            opened = ACameraManager_openCamera(manager_, ids->cameraIds[i], &deviceCallbacks, &device_);
        ACameraManager_deleteCameraIdList(ids);
        if (opened != ACAMERA_OK || !device_) {
            LOGE("camera hook: unable to open headset camera (status %d); grant camera permission and try again", opened);
            stop();
            return false;
        }
        if (AImageReader_new(640, 480, AIMAGE_FORMAT_YUV_420_888, 2, &reader_) != AMEDIA_OK ||
            AImageReader_setImageListener(reader_, &imageListener_) != AMEDIA_OK) {
            LOGE("camera hook: could not create image reader");
            stop();
            return false;
        }
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(reader_, &window) != AMEDIA_OK || !window) {
            LOGE("camera hook: could not get image reader window");
            stop();
            return false;
        }
        ACaptureSessionOutputContainer* outputs = nullptr;
        ACaptureSessionOutput* output = nullptr;
        ACameraOutputTarget* target = nullptr;
        bool setup = ACaptureSessionOutputContainer_create(&outputs) == ACAMERA_OK &&
                     ACaptureSessionOutput_create(window, &output) == ACAMERA_OK &&
                     ACaptureSessionOutputContainer_add(outputs, output) == ACAMERA_OK;
        ACameraCaptureSession_stateCallbacks sessionCallbacks{};
        sessionCallbacks.context = this;
        if (setup) setup = ACameraDevice_createCaptureSession(device_, outputs, &sessionCallbacks, &session_) == ACAMERA_OK;
        if (setup) setup = ACameraOutputTarget_create(window, &target) == ACAMERA_OK;
        if (setup) setup = ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &request_) == ACAMERA_OK;
        if (setup) setup = ACaptureRequest_addTarget(request_, target) == ACAMERA_OK;
        if (setup) setup = ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr) == ACAMERA_OK;
        if (target) ACameraOutputTarget_free(target);
        if (output) ACaptureSessionOutput_free(output);
        if (outputs) ACaptureSessionOutputContainer_free(outputs);
        if (!setup) {
            LOGE("camera hook: could not start capture session");
            stop();
            return false;
        }
        LOGI("experimental camera capture started; frames are counted locally and not saved or recognized");
        return true;
    }

    [[nodiscard]] bool failed() const { return failed_.load(); }

    void stop() {
        if (session_) {
            ACameraCaptureSession_stopRepeating(session_);
            ACameraCaptureSession_close(session_);
            session_ = nullptr;
        }
        if (request_) {
            ACaptureRequest_free(request_);
            request_ = nullptr;
        }
        if (device_) {
            ACameraDevice_close(device_);
            device_ = nullptr;
        }
        if (reader_) {
            AImageReader_setImageListener(reader_, nullptr);
            AImageReader_delete(reader_);
            reader_ = nullptr;
        }
        if (manager_) {
            ACameraManager_delete(manager_);
            manager_ = nullptr;
        }
    }

private:
    static void disconnected(void* context, ACameraDevice*) {
        auto* self = static_cast<CameraCaptureHook*>(context);
        LOGE("camera hook: headset camera disconnected");
        self->failed_.store(true);
    }
    static void error(void* context, ACameraDevice*, int errorCode) {
        auto* self = static_cast<CameraCaptureHook*>(context);
        LOGE("camera hook: camera error %d", errorCode);
        self->failed_.store(true);
    }
    static void imageAvailable(void* context, AImageReader* reader) {
        auto* self = static_cast<CameraCaptureHook*>(context);
        AImage* image = nullptr;
        if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;
        std::int32_t width = 0, height = 0;
        AImage_getWidth(image, &width);
        AImage_getHeight(image, &height);
        const std::uint64_t frame = self->frames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frame == 1) LOGI("camera hook: receiving %dx%d frames; no image data is retained", width, height);
        AImage_delete(image);
    }
    static inline AImageReader_ImageListener imageListener_{nullptr, &CameraCaptureHook::imageAvailable};
    ACameraManager* manager_{nullptr};
    ACameraDevice* device_{nullptr};
    ACameraCaptureSession* session_{nullptr};
    AImageReader* reader_{nullptr};
    ACaptureRequest* request_{nullptr};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<bool> failed_{false};
};

bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    LOGE("%s failed: %d", what, static_cast<int>(r));
    return false;
}

template <class F> F proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(instance, name, &f);
    return reinterpret_cast<F>(f);
}

Pose toPose(const XrPosef& p) {
    return {{p.position.x, p.position.y, p.position.z}, {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}};
}

struct Quest {
    android_app* android{nullptr};
    bool resumed{false};

    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace stage{XR_NULL_HANDLE}, view{XR_NULL_HANDLE};
    XrSessionState state{XR_SESSION_STATE_UNKNOWN};
    bool sessionRunning{false}, exitRequested{false};
    bool hasRefreshRate{false}, hasPerfSettings{false}, hasLocalFloor{false}, hasPassthrough{false};
    bool passthroughActive{false}, alphaBlendSupported{false};
    XrPassthroughFB passthrough{XR_NULL_HANDLE};
    XrPassthroughLayerFB passthroughLayer{XR_NULL_HANDLE};
    PFN_xrCreatePassthroughFB createPassthrough{nullptr};
    PFN_xrDestroyPassthroughFB destroyPassthrough{nullptr};
    PFN_xrPassthroughStartFB startPassthrough{nullptr};
    PFN_xrPassthroughPauseFB pausePassthrough{nullptr};
    PFN_xrCreatePassthroughLayerFB createPassthroughLayer{nullptr};
    PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayer{nullptr};
    PFN_xrPassthroughLayerPauseFB pausePassthroughLayer{nullptr};
    PFN_xrPassthroughLayerResumeFB resumePassthroughLayer{nullptr};

    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkan2KHR> images;
    std::vector<VkImageView> imageViews;
    std::uint32_t width{0}, height{0};
    VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};

    XrActionSet actions{XR_NULL_HANDLE};
    XrAction aim{}, grip{}, trigger{}, squeeze{}, stick{}, primary{}, secondary{}, stickClick{}, haptic{};
    XrPath hand[2]{};
    XrSpace aimSpace[2]{}, gripSpace[2]{};

    std::unique_ptr<render::Gpu> gpu;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<App> app;
    Speaker speaker;
    CameraCaptureHook camera;
    bool cameraOptIn{false}, cameraActive{false}, cameraAttempted{false};
    std::vector<float> audioBlock;
    render::MeshId terrainMesh{0};
    double lastFrame{0};

    bool init();
    bool initVulkan();
    bool initSession();
    bool initActions();
    bool initSwapchain();
    void pollEvents();
    void frame();
    void shutdown();
};

bool Quest::init() {
    // The Khronos loader needs the Java VM and the activity before anything else.
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initLoader));
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = android->activity->vm;
        li.applicationContext = android->activity->clazz;
        initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li));
    }

    std::uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    auto has = [&](const char* name) {
        return std::any_of(props.begin(), props.end(),
                           [&](const XrExtensionProperties& p) { return std::strcmp(p.extensionName, name) == 0; });
    };
    std::vector<const char*> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    for (const char* e : exts)
        if (!has(e)) {
            LOGE("runtime lacks %s", e);
            return false;
        }
    if ((hasRefreshRate = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if ((hasPerfSettings = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))) exts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if ((hasLocalFloor = has(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME))) exts.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    if ((hasPassthrough = has(XR_FB_PASSTHROUGH_EXTENSION_NAME))) exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR android_{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_.applicationVM = android->activity->vm;
    android_.applicationActivity = android->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_;
    std::strncpy(ci.applicationInfo.applicationName, "glyder", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 400;
    std::strncpy(ci.applicationInfo.engineName, "glyder", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 400;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    if (!ok(xrCreateInstance(&ci, &instance), "xrCreateInstance")) return false;

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(instance, &si, &system), "xrGetSystem")) return false;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, system, &sp);
    LOGI("glyder %s \"%s\" on %s", kVersion, kReleaseName, sp.systemName);
    return initVulkan() && initSession() && initActions() && initSwapchain();
}

bool Quest::initVulkan() {
    auto getReqs = proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR");
    auto createInstance = proc<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
    auto getDevice = proc<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR");
    auto createDevice = proc<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR");
    if (!getReqs || !createInstance || !getDevice || !createDevice) return false;
    XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!ok(getReqs(instance, system, &reqs), "xrGetVulkanGraphicsRequirements2KHR")) return false;

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "glyder";
    appInfo.applicationVersion = 400;
    appInfo.pEngineName = "glyder";
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    XrVulkanInstanceCreateInfoKHR xci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xci.systemId = system;
    xci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xci.vulkanCreateInfo = &ici;
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkResult vr = VK_SUCCESS;
    if (!ok(createInstance(instance, &xci, &vkInstance, &vr), "xrCreateVulkanInstanceKHR") || vr != VK_SUCCESS) return false;

    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = system;
    gi.vulkanInstance = vkInstance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    if (!ok(getDevice(instance, &gi, &physical), "xrGetVulkanGraphicsDevice2KHR")) return false;

    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qs.data());
    std::uint32_t family = 0;
    for (std::uint32_t i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    multiview.multiview = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &multiview;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = system;
    xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = physical;
    xdci.vulkanCreateInfo = &dci;
    VkDevice device = VK_NULL_HANDLE;
    if (!ok(createDevice(instance, &xdci, &device, &vr), "xrCreateVulkanDeviceKHR") || vr != VK_SUCCESS) return false;
    gpu = render::Gpu::adopt(vkInstance, physical, device, family, 0);
    LOGI("Vulkan on %s", gpu->name().c_str());
    return true;
}

bool Quest::initSession() {
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = gpu->instance;
    binding.physicalDevice = gpu->physical;
    binding.device = gpu->device;
    binding.queueFamilyIndex = gpu->queueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    if (!ok(xrCreateSession(instance, &sci, &session), "xrCreateSession")) return false;
    if (hasPassthrough) {
        createPassthrough = proc<PFN_xrCreatePassthroughFB>(instance, "xrCreatePassthroughFB");
        destroyPassthrough = proc<PFN_xrDestroyPassthroughFB>(instance, "xrDestroyPassthroughFB");
        startPassthrough = proc<PFN_xrPassthroughStartFB>(instance, "xrPassthroughStartFB");
        pausePassthrough = proc<PFN_xrPassthroughPauseFB>(instance, "xrPassthroughPauseFB");
        createPassthroughLayer = proc<PFN_xrCreatePassthroughLayerFB>(instance, "xrCreatePassthroughLayerFB");
        destroyPassthroughLayer = proc<PFN_xrDestroyPassthroughLayerFB>(instance, "xrDestroyPassthroughLayerFB");
        pausePassthroughLayer = proc<PFN_xrPassthroughLayerPauseFB>(instance, "xrPassthroughLayerPauseFB");
        resumePassthroughLayer = proc<PFN_xrPassthroughLayerResumeFB>(instance, "xrPassthroughLayerResumeFB");
        if (createPassthrough && destroyPassthrough && startPassthrough && pausePassthrough && createPassthroughLayer &&
            destroyPassthroughLayer && pausePassthroughLayer && resumePassthroughLayer) {
            XrPassthroughCreateInfoFB passthroughInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
            passthroughInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            if (XR_SUCCEEDED(createPassthrough(session, &passthroughInfo, &passthrough))) {
                XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
                layerInfo.passthrough = passthrough;
                layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
                if (XR_SUCCEEDED(createPassthroughLayer(session, &layerInfo, &passthroughLayer))) {
                    pausePassthroughLayer(passthroughLayer);
                    pausePassthrough(passthrough);
                    std::uint32_t modeCount = 0;
                    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                       0, &modeCount, nullptr))) {
                        std::vector<XrEnvironmentBlendMode> modes(modeCount);
                        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                          modeCount, &modeCount, modes.data()))) {
                            alphaBlendSupported = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != modes.end();
                        }
                    }
                    if (!alphaBlendSupported) {
                        LOGI("passthrough available, but alpha blend mode is unsupported");
                        pausePassthroughLayer(passthroughLayer);
                        pausePassthrough(passthrough);
                    } else {
                        LOGI("Quest passthrough available; enable AR MODE in the menu");
                    }
                } else {
                    destroyPassthrough(passthrough);
                    passthrough = XR_NULL_HANDLE;
                }
            } else {
                LOGI("Quest passthrough could not start; use base/standard mode");
            }
        }
    }

    // Floor-level origin where the pilot stands: LOCAL_FLOOR when offered, else STAGE, else LOCAL
    // dropped to a typical standing eye height.
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = hasLocalFloor ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT : XR_REFERENCE_SPACE_TYPE_STAGE;
    if (!XR_SUCCEEDED(xrCreateReferenceSpace(session, &rs, &stage))) {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rs.poseInReferenceSpace.position.y = -1.6f;
        if (!ok(xrCreateReferenceSpace(session, &rs, &stage), "xrCreateReferenceSpace")) return false;
    }
    XrReferenceSpaceCreateInfo vs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vs.poseInReferenceSpace.orientation.w = 1;
    return ok(xrCreateReferenceSpace(session, &vs, &view), "xrCreateReferenceSpace(view)");
}

bool Quest::initActions() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "glyder");
    std::strcpy(asci.localizedActionSetName, "glyder");
    if (!ok(xrCreateActionSet(instance, &asci, &actions), "xrCreateActionSet")) return false;
    xrStringToPath(instance, "/user/hand/left", &hand[0]);
    xrStringToPath(instance, "/user/hand/right", &hand[1]);
    auto make = [&](XrAction& a, const char* name, XrActionType type) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, name);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = hand;
        return ok(xrCreateAction(actions, &ci, &a), name);
    };
    if (!make(aim, "aim", XR_ACTION_TYPE_POSE_INPUT) || !make(grip, "grip", XR_ACTION_TYPE_POSE_INPUT) ||
        !make(trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT) || !make(squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make(stick, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT) || !make(primary, "primary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(secondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT) || !make(stickClick, "stickclick", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT))
        return false;

    auto suggest = [&](const char* profile, std::vector<std::pair<XrAction, const char*>> list) {
        std::vector<XrActionSuggestedBinding> b;
        for (auto& [a, path] : list) {
            XrPath p;
            xrStringToPath(instance, path, &p);
            b.push_back({a, p});
        }
        XrPath prof;
        xrStringToPath(instance, profile, &prof);
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = prof;
        s.countSuggestedBindings = static_cast<std::uint32_t>(b.size());
        s.suggestedBindings = b.data();
        return XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance, &s));
    };
    // Touch controllers (the Quest 3S's Touch Plus answer to this profile too).
    suggest("/interaction_profiles/oculus/touch_controller", {{aim, "/user/hand/left/input/aim/pose"},
                                                              {aim, "/user/hand/right/input/aim/pose"},
                                                              {grip, "/user/hand/left/input/grip/pose"},
                                                              {grip, "/user/hand/right/input/grip/pose"},
                                                              {trigger, "/user/hand/left/input/trigger/value"},
                                                              {trigger, "/user/hand/right/input/trigger/value"},
                                                              {squeeze, "/user/hand/left/input/squeeze/value"},
                                                              {squeeze, "/user/hand/right/input/squeeze/value"},
                                                              {stick, "/user/hand/left/input/thumbstick"},
                                                              {stick, "/user/hand/right/input/thumbstick"},
                                                              {primary, "/user/hand/left/input/x/click"},
                                                              {primary, "/user/hand/right/input/a/click"},
                                                              {secondary, "/user/hand/left/input/y/click"},
                                                              {secondary, "/user/hand/right/input/b/click"},
                                                              {stickClick, "/user/hand/left/input/thumbstick/click"},
                                                              {stickClick, "/user/hand/right/input/thumbstick/click"},
                                                              {haptic, "/user/hand/left/output/haptic"},
                                                              {haptic, "/user/hand/right/output/haptic"}});
    // Anything else: point and select.
    suggest("/interaction_profiles/khr/simple_controller", {{aim, "/user/hand/left/input/aim/pose"},
                                                            {aim, "/user/hand/right/input/aim/pose"},
                                                            {grip, "/user/hand/left/input/grip/pose"},
                                                            {grip, "/user/hand/right/input/grip/pose"},
                                                            {trigger, "/user/hand/left/input/select/click"},
                                                            {trigger, "/user/hand/right/input/select/click"},
                                                            {secondary, "/user/hand/left/input/menu/click"},
                                                            {secondary, "/user/hand/right/input/menu/click"},
                                                            {haptic, "/user/hand/left/output/haptic"},
                                                            {haptic, "/user/hand/right/output/haptic"}});

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.subactionPath = hand[h];
        ci.poseInActionSpace.orientation.w = 1;
        ci.action = aim;
        xrCreateActionSpace(session, &ci, &aimSpace[h]);
        ci.action = grip;
        xrCreateActionSpace(session, &ci, &gripSpace[h]);
    }
    XrSessionActionSetsAttachInfo att{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    att.countActionSets = 1;
    att.actionSets = &actions;
    return ok(xrAttachSessionActionSets(session, &att), "xrAttachSessionActionSets");
}

bool Quest::initSwapchain() {
    std::uint32_t n = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, views.data());
    if (n != 2) return false;
    width = views[0].recommendedImageRectWidth;
    height = views[0].recommendedImageRectHeight;

    std::uint32_t fn = 0;
    xrEnumerateSwapchainFormats(session, 0, &fn, nullptr);
    std::vector<std::int64_t> formats(fn);
    xrEnumerateSwapchainFormats(session, fn, &fn, formats.data());
    format = static_cast<VkFormat>(formats.empty() ? VK_FORMAT_R8G8B8A8_SRGB : formats[0]);
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(want)) != formats.end()) {
            format = want;
            break;
        }

    // One swapchain, two array layers: multiview draws both eyes in one pass.
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = format;
    sci.sampleCount = 1;
    sci.width = width;
    sci.height = height;
    sci.faceCount = 1;
    sci.arraySize = 2;
    sci.mipCount = 1;
    if (!ok(xrCreateSwapchain(session, &sci, &swapchain), "xrCreateSwapchain")) return false;
    std::uint32_t in = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &in, nullptr);
    images.assign(in, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(swapchain, in, &in, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    for (const auto& img : images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
        VkImageView v;
        vkCreateImageView(gpu->device, &vci, nullptr, &v);
        imageViews.push_back(v);
    }
    LOGI("eye buffers %ux%u x2, format %d, %u images", width, height, static_cast<int>(format), in);

    renderer = std::make_unique<render::Renderer>(*gpu, format, 2, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 3);
    terrainMesh = renderer->addMesh({});
    AppOptions ao;
    ao.clientName = "quest";
    ao.storageDir = android->activity->internalDataPath ? android->activity->internalDataPath : "";
    ao.terrainMesh = terrainMesh;
    app = std::make_unique<App>(ao);
    // `adb shell setprop debug.glyder.base 1` starts in base mode; the menu toggle is remembered after that.
    char baseProp[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.glyder.base", baseProp) > 0 && baseProp[0] == '1') app->setBaseMode(true);
    char arProp[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.glyder.ar", arProp) > 0 && arProp[0] == '1') app->setArMode(true);
    char cameraProp[PROP_VALUE_MAX] = {};
    cameraOptIn = __system_property_get("debug.glyder.camera", cameraProp) > 0 && cameraProp[0] == '1';
    if (!speaker.open(app->audioRate())) LOGE("no audio output; audio cues will be silent");
    return true;
}

void Quest::pollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: exitRequested = true; break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            state = e->state;
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (ok(xrBeginSession(session, &bi), "xrBeginSession")) {
                    sessionRunning = true;
                    if (hasRefreshRate) {
                        auto enumerate = proc<PFN_xrEnumerateDisplayRefreshRatesFB>(instance, "xrEnumerateDisplayRefreshRatesFB");
                        auto request = proc<PFN_xrRequestDisplayRefreshRateFB>(instance, "xrRequestDisplayRefreshRateFB");
                        std::uint32_t rn = 0;
                        if (enumerate && request && XR_SUCCEEDED(enumerate(session, 0, &rn, nullptr)) && rn) {
                            std::vector<float> rates(rn);
                            enumerate(session, rn, &rn, rates.data());
                            const float best = *std::max_element(rates.begin(), rates.end());
                            request(session, best);
                            LOGI("display %.0f Hz", static_cast<double>(best));
                        }
                    }
                    if (hasPerfSettings) {
                        auto perf = proc<PFN_xrPerfSettingsSetPerformanceLevelEXT>(instance, "xrPerfSettingsSetPerformanceLevelEXT");
                        if (perf) {
                            perf(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                            perf(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                        }
                    }
                }
            } else if (state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session);
                sessionRunning = false;
                camera.stop();
                cameraActive = false;
                cameraAttempted = false;
                if (passthroughActive) {
                    pausePassthroughLayer(passthroughLayer);
                    pausePassthrough(passthrough);
                    passthroughActive = false;
                }
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                exitRequested = true;
            }
            break;
        }
        default: break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void Quest::frame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!XR_SUCCEEDED(xrWaitFrame(session, &wi, &fs))) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!XR_SUCCEEDED(xrBeginFrame(session, &bi))) return;

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerPassthroughFB passthroughComposition{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};

    if (fs.shouldRender) {
        const double t = net::now();
        Input in;
        in.time = t;
        in.dt = lastFrame > 0 ? static_cast<float>(t - lastFrame) : 0.011f;
        lastFrame = t;
        in.passthroughAvailable = alphaBlendSupported && passthroughLayer && passthrough;
        const XrTime when = fs.predictedDisplayTime;

        if (state == XR_SESSION_STATE_FOCUSED) {
            XrActiveActionSet active{actions, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            xrSyncActions(session, &sync);
        }
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(view, stage, when, &head);
        in.head = toPose(head.pose);
        for (int h = 0; h < 2; ++h) {
            Hand& hd = in.hands[h];
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(aimSpace[h], stage, when, &loc);
            hd.active =
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            hd.aim = toPose(loc.pose);
            XrSpaceLocation gl{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(gripSpace[h], stage, when, &gl);
            hd.grip = toPose(gl.pose);
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.subactionPath = hand[h];
            XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
            gi.action = trigger;
            xrGetActionStateFloat(session, &gi, &f);
            hd.trigger = f.currentState;
            gi.action = squeeze;
            xrGetActionStateFloat(session, &gi, &f);
            hd.squeeze = f.currentState;
            XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
            gi.action = stick;
            xrGetActionStateVector2f(session, &gi, &v);
            hd.stick = {v.currentState.x, v.currentState.y};
            XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
            gi.action = primary;
            xrGetActionStateBoolean(session, &gi, &b);
            hd.primary = b.currentState;
            gi.action = secondary;
            xrGetActionStateBoolean(session, &gi, &b);
            hd.secondary = b.currentState;
            gi.action = stickClick;
            xrGetActionStateBoolean(session, &gi, &b);
            hd.stickClick = b.currentState;
        }

        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = when;
        vli.space = stage;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t vn = 0;
        xrLocateViews(session, &vli, &vs, 2, &vn, views);
        if (vn == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            EyeView eyes[2];
            for (int i = 0; i < 2; ++i) {
                eyes[i].pose = toPose(views[i].pose);
                eyes[i].tanLeft = std::tan(views[i].fov.angleLeft);
                eyes[i].tanRight = std::tan(views[i].fov.angleRight);
                eyes[i].tanUp = std::tan(views[i].fov.angleUp);
                eyes[i].tanDown = std::tan(views[i].fov.angleDown);
            }
            const render::FrameData fd = app->frame(in, eyes, width, height);
            const bool wantCamera = cameraOptIn && app->arMode() && !app->baseMode();
            if (wantCamera && !cameraAttempted) {
                cameraAttempted = true;
                cameraActive = camera.start();
            } else if (!wantCamera && cameraAttempted) {
                camera.stop();
                cameraActive = false;
                cameraAttempted = false;
            } else if (cameraActive && camera.failed()) {
                camera.stop();
                cameraActive = false;
            }
            if (auto mesh = app->takeTerrainMesh()) renderer->replaceMesh(terrainMesh, *mesh);
            if (const std::size_t n = speaker.wanted(); n > 0) {
                audioBlock.resize(n * 2);
                app->audio(audioBlock);
                speaker.push(audioBlock.data(), n);
            }

            std::uint32_t index = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xrAcquireSwapchainImage(swapchain, &ai, &index);
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(swapchain, &wait);
            renderer->render(fd, {images[index].image, imageViews[index], {width, height}});
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(swapchain, &ri);

            // The layer carries the poses the runtime gave us, not the PD- and prism-adjusted
            // ones we drew with: the compositor must keep a prescription's prism, not undo it.
            for (int i = 0; i < 2; ++i) {
                pv[i].pose = views[i].pose;
                pv[i].fov = views[i].fov;
                pv[i].subImage.swapchain = swapchain;
                pv[i].subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)}};
                pv[i].subImage.imageArrayIndex = static_cast<std::uint32_t>(i);
            }
            const bool usePassthrough = app->arMode() && !app->baseMode() && passthroughLayer && alphaBlendSupported;
            if (usePassthrough != passthroughActive) {
                if (usePassthrough) {
                    if (XR_SUCCEEDED(startPassthrough(passthrough)) && XR_SUCCEEDED(resumePassthroughLayer(passthroughLayer)))
                        passthroughActive = true;
                    else {
                        LOGE("could not resume passthrough layer");
                        app->setArMode(false);
                    }
                } else {
                    pausePassthroughLayer(passthroughLayer);
                    pausePassthrough(passthrough);
                    passthroughActive = false;
                }
            }
            layer.space = stage;
            layer.layerFlags = usePassthrough && passthroughActive ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            layer.viewCount = 2;
            layer.views = pv;
            if (usePassthrough && passthroughActive) {
                passthroughComposition.space = stage;
                passthroughComposition.layerHandle = passthroughLayer;
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthroughComposition));
            }
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));

            for (int h = 0; h < 2; ++h) {
                const Haptic hp = app->haptic(h);
                if (hp.amplitude <= 0) continue;
                XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
                vib.amplitude = hp.amplitude;
                vib.duration = static_cast<XrDuration>(hp.seconds * 1e9);
                vib.frequency = XR_FREQUENCY_UNSPECIFIED;
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                hi.action = haptic;
                hi.subactionPath = hand[h];
                xrApplyHapticFeedback(session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
            }
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = passthroughActive ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<std::uint32_t>(layers.size());
    ei.layers = layers.data();
    xrEndFrame(session, &ei);
}

void Quest::shutdown() {
    camera.stop();
    speaker.close();
    if (renderer) renderer->waitIdle();
    app.reset();
    renderer.reset();
    if (gpu)
        for (VkImageView v : imageViews) vkDestroyImageView(gpu->device, v, nullptr);
    imageViews.clear();
    if (swapchain) xrDestroySwapchain(swapchain);
    if (passthroughLayer && destroyPassthroughLayer) destroyPassthroughLayer(passthroughLayer);
    if (passthrough && destroyPassthrough) destroyPassthrough(passthrough);
    if (actions) xrDestroyActionSet(actions);
    if (view) xrDestroySpace(view);
    if (stage) xrDestroySpace(stage);
    if (session) xrDestroySession(session);
    // The runtime made the Vulkan objects through us; we destroy them before the instance.
    if (gpu) {
        vkDestroyCommandPool(gpu->device, gpu->pool, nullptr);
        gpu->pool = VK_NULL_HANDLE;
        vkDestroyDevice(gpu->device, nullptr);
        gpu->device = VK_NULL_HANDLE;
        vkDestroyInstance(gpu->instance, nullptr);
        gpu->instance = VK_NULL_HANDLE;
    }
    gpu.reset();
    if (instance) xrDestroyInstance(instance);
}

void onCommand(android_app* a, std::int32_t cmd) {
    auto* q = static_cast<Quest*>(a->userData);
    switch (cmd) {
    case APP_CMD_RESUME: q->resumed = true; break;
    case APP_CMD_PAUSE: q->resumed = false; break;
    case APP_CMD_DESTROY: q->exitRequested = true; break;
    default: break;
    }
}

} // namespace

void android_main(android_app* android) {
    JNIEnv* env = nullptr;
    android->activity->vm->AttachCurrentThread(&env, nullptr);
    Quest q;
    q.android = android;
    android->userData = &q;
    android->onAppCmd = onCommand;
    if (!q.init()) {
        LOGE("could not start OpenXR; is this a Quest?");
        q.shutdown();
        ANativeActivity_finish(android->activity);
        android->activity->vm->DetachCurrentThread();
        return;
    }
    while (!android->destroyRequested) {
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            // Block while there is nothing to draw; never block while a session runs.
            const int timeout = (!q.resumed && !q.sessionRunning && !q.exitRequested) ? -1 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) < 0) break;
            if (source) source->process(android, source);
            if (android->destroyRequested) break;
        }
        q.pollEvents();
        if (q.exitRequested) {
            ANativeActivity_finish(android->activity);
            if (!q.sessionRunning) break;
        }
        if (q.sessionRunning) q.frame();
    }
    q.shutdown();
    android->activity->vm->DetachCurrentThread();
}
