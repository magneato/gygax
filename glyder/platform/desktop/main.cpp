// glyder on a desktop: the same experience as the headset in a window (or headless, for captures),
// with the mouse and keyboard standing in for the controllers.
//
//   glyder --demo                         the Construct, built in
//   glyder --connect 192.168.1.20 --code 482913
//   glyder --capture shot.png --demo --script "2 drop 3; 6 out"   headless, scripted
//
// Mouse: point and click (trigger); right-drag turns the table or looks around; wheel zooms.
// Keys: WASD left stick, arrows right stick, Space = A, Esc/Backspace = B (jack out), Tab = menu.

#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glyder/app.hpp>
#include <glyder/image.hpp>
#include <glyder/render.hpp>
#include <glyder/version.hpp>

using namespace glyder;

namespace {

struct Options {
    AppOptions app;
    int width{1600}, height{900};
    bool stereo{false};
    std::string capture; // headless: write the last frame (or a frame sequence with %05d) here
    double seconds{8};   // headless: how long to run
    int fps{60};
    std::string script; // "t action [args]; ..." for headless captures
    float fovDeg{68};
    // Accessibility: applied at start (and remembered, as the menu does).
    std::string colour; // typical, tritan, protan, deutan
    bool preview{false}, uncorrected{false}, cues{false}, blind{false}, base{false};
    std::string audio; // headless: write the audio cues to this WAV, in step with the frames
};

void applyAccess(App& app, const Options& o) {
    if (!o.colour.empty() || o.preview || o.uncorrected)
        app.setColourVision(colour::fromToken(o.colour, app.colourVision()), !o.uncorrected, o.preview);
    if (o.cues) app.setAudioCues(true);
    if (o.blind) app.setBlindMode(true);
    if (o.base) app.setBaseMode(true);
}

struct Cue {
    double t;
    std::string action;
    std::vector<std::string> args;
};

std::vector<Cue> parseScript(const std::string& s) {
    std::vector<Cue> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        std::istringstream is(item);
        Cue c;
        if (!(is >> c.t >> c.action)) continue;
        std::string a;
        while (is >> a) c.args.push_back(a);
        out.push_back(c);
    }
    return out;
}

void usage() {
    std::printf(
        "glyder %s \"%s\" (desktop)\n\n"
        "  --demo                 start in the Construct demo world\n"
        "  --connect HOST[:PORT]  connect to a Gygax host (default port %u)\n"
        "  --code CODE            its pairing code\n"
        "  --host HOST            also look for a host here (networks without broadcast)\n"
        "  --size WxH             window size (default 1600x900)\n"
        "  --stereo               both eyes side by side\n"
        "  --capture FILE.png     headless: render offscreen and save (use %%05d for a sequence)\n"
        "  --capture FILE.rgba    headless: every frame as raw RGBA to a file or pipe, for an encoder\n"
        "  --seconds S            headless run time (default 8)\n"
        "  --fps N                headless frame rate (default 60)\n"
        "  --script \"T ACTION..;\" headless cues: drop ID|SHAPE | out | head X Y Z YAW PITCH | point X Y | click | menu | scene NAME | stick H X Y\n"
        "                         colour MODE [preview] [raw] | cues on|off | blind on|off | base on|off\n"
        "  --rx \"OD ..\"           a prescription to apply (as written on it)\n"
        "  --colour MODE          colour vision: typical, tritan (no blue cones), protan (no red), deutan (no green)\n"
        "  --preview              show what that viewer sees (for viewers with typical colour vision)\n"
        "  --raw                  with --preview: without the correction\n"
        "  --cues                 audio cues for what is around you\n"
        "  --blind                blind mode: cues for everything, earcons, pointing sounds the node\n"
        "  --base                 base mode: plain view, sound first, no AR/passthrough; vision correction and colour kept\n"
        "  --audio FILE.wav       headless: write the sound, in step with the frames\n",
        kVersion, kReleaseName, wire::kDefaultPort);
}

// Desktop "head": a seated viewer looking at the table, turned by right-drag.
struct Viewer {
    float yaw{0}, pitch{-0.32f};
    Vec3 pos{0, 1.45f, 0.25f};
    [[nodiscard]] Pose pose() const { return {pos, axisAngle({0, 1, 0}, yaw) * axisAngle({1, 0, 0}, pitch)}; }
};

std::vector<EyeView> eyesFor(const Pose& head, float fovDeg, float aspect, bool stereo) {
    const float t = std::tan(fovDeg * kDeg * 0.5f);
    std::vector<EyeView> eyes;
    if (!stereo) {
        eyes.push_back({head, -t * aspect, t * aspect, t, -t});
        return eyes;
    }
    const float half = 0.032f;
    for (int i = 0; i < 2; ++i) {
        EyeView e{head, -t * aspect * 0.5f * 2.0f, t * aspect * 0.5f * 2.0f, t, -t};
        e.pose.pos = head.pos + head.right() * (i == 0 ? -half : half);
        eyes.push_back(e);
    }
    return eyes;
}

Pose rayThrough(const Pose& head, float fovDeg, float aspect, float nx, float ny) {
    // nx, ny in -1..1 (y down on screen).
    const float t = std::tan(fovDeg * kDeg * 0.5f);
    const Vec3 dirCam = normalize(Vec3{nx * t * aspect, -ny * t, -1});
    const Vec3 d = rotate(head.q, dirCam);
    // A pose whose -z is d.
    const Vec3 f = d, upHint{0, 1, 0};
    Vec3 r = normalize(cross(f, upHint));
    if (length(r) < 0.5f) r = {1, 0, 0};
    const Vec3 u = cross(r, f);
    Mat4 m;
    m.m[0][0] = r.x, m.m[0][1] = r.y, m.m[0][2] = r.z;
    m.m[1][0] = u.x, m.m[1][1] = u.y, m.m[1][2] = u.z;
    m.m[2][0] = -f.x, m.m[2][1] = -f.y, m.m[2][2] = -f.z;
    // Matrix to quaternion.
    const float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m.m[1][2] - m.m[2][1]) / s, (m.m[2][0] - m.m[0][2]) / s, (m.m[0][1] - m.m[1][0]) / s, 0.25f * s};
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q = {0.25f * s, (m.m[1][0] + m.m[0][1]) / s, (m.m[2][0] + m.m[0][2]) / s, (m.m[1][2] - m.m[2][1]) / s};
    } else if (m.m[1][1] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q = {(m.m[1][0] + m.m[0][1]) / s, 0.25f * s, (m.m[2][1] + m.m[1][2]) / s, (m.m[2][0] - m.m[0][2]) / s};
    } else {
        const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q = {(m.m[2][0] + m.m[0][2]) / s, (m.m[2][1] + m.m[1][2]) / s, 0.25f * s, (m.m[0][1] - m.m[1][0]) / s};
    }
    return {head.pos, normalize(q)};
}

// ---------------------------------------------------------------------------------------------
// Headless

int headless(const Options& o) {
    std::string error;
    auto gpu = render::Gpu::create({}, false, std::getenv("GLYDER_VALIDATION") != nullptr, &error);
    if (!gpu) {
        std::fprintf(stderr, "glyder: %s\n", error.c_str());
        return 1;
    }
    const std::uint32_t views = o.stereo ? 2u : 1u;
    const std::uint32_t w = static_cast<std::uint32_t>(o.stereo ? o.width / 2 : o.width), h = static_cast<std::uint32_t>(o.height);
    render::Renderer renderer(*gpu, VK_FORMAT_R8G8B8A8_SRGB, views, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    render::OffscreenTarget target(*gpu, VK_FORMAT_R8G8B8A8_SRGB, {w, h}, views);
    AppOptions ao = o.app;
    ao.terrainMesh = renderer.addMesh({});
    ao.rememberAccess = false; // a capture says what it wants; it never changes what a user chose
    App app(ao);
    applyAccess(app, o);
    auto cues = parseScript(o.script);
    // Raw frames to a file or pipe (an encoder reading a FIFO): fast enough to keep real time.
    const bool rawFrames = o.capture.size() > 5 && o.capture.ends_with(".rgba");
    std::FILE* rawOut = rawFrames ? std::fopen(o.capture.c_str(), "wb") : nullptr;
    if (rawFrames && !rawOut) {
        std::fprintf(stderr, "glyder: cannot write %s\n", o.capture.c_str());
        return 1;
    }
    std::vector<float> track;
    Viewer viewer;
    Input in;
    in.desktop = true;
    float px = 0, py = 0;
    bool click = false;
    Vec2 sticks[2];
    const double start = net::now();
    const int frames = static_cast<int>(o.seconds * o.fps);
    const bool sequence = o.capture.find('%') != std::string::npos;
    double last = start;
    for (int f = 0; f < frames; ++f) {
        const double due = start + static_cast<double>(f) / o.fps;
        const double now0 = net::now();
        if (due > now0) std::this_thread::sleep_for(std::chrono::duration<double>(due - now0));
        const double t = net::now();
        // Cues are timed by frame, so each lands at its second of the video however fast frames render.
        const double rel = static_cast<double>(f) / o.fps;
        in = {};
        in.desktop = true;
        in.time = t;
        in.dt = static_cast<float>(t - last);
        last = t;
        in.hands[0].active = in.hands[1].active = false;
        for (auto it = cues.begin(); it != cues.end();) {
            if (it->t > rel) {
                ++it;
                continue;
            }
            const auto& a = it->args;
            auto num = [&](std::size_t i, float d = 0) { return i < a.size() ? std::strtof(a[i].c_str(), nullptr) : d; };
            if (it->action == "drop" && !a.empty()) {
                // A node id, or a kit shape name: the first drivable node of that kind.
                const bool numeric = std::isdigit(static_cast<unsigned char>(a[0][0]));
                const std::uint32_t id = numeric ? static_cast<std::uint32_t>(std::stoul(a[0])) : app.find(kit::shapeFromName(a[0]));
                if (id) app.select(id);
            } else if (it->action == "out")
                app.jackOut();
            else if (it->action == "head") {
                viewer.pos = {num(0), num(1, 1.45f), num(2, 0.25f)};
                viewer.yaw = num(3) * kDeg;
                viewer.pitch = num(4) * kDeg;
            } else if (it->action == "point") {
                px = num(0);
                py = num(1);
            } else if (it->action == "click") {
                click = true;
            } else if (it->action == "menu") {
                in.hands[0].secondary = true;
            } else if (it->action == "stick") {
                const int hand = std::clamp(static_cast<int>(num(0)), 0, 1);
                sticks[hand] = {num(1), num(2)};
            } else if (it->action == "colour" && !a.empty()) {
                const bool preview = std::find(a.begin(), a.end(), "preview") != a.end();
                const bool raw = std::find(a.begin(), a.end(), "raw") != a.end();
                app.setColourVision(colour::fromToken(a[0]), !raw, preview);
            } else if (it->action == "cues" && !a.empty()) {
                app.setAudioCues(a[0] == "on");
            } else if (it->action == "blind" && !a.empty()) {
                app.setBlindMode(a[0] == "on");
            } else if (it->action == "base" && !a.empty()) {
                app.setBaseMode(a[0] == "on");
            } else if (it->action == "scene" && !a.empty()) {
                std::string name = a[0];
                for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                app.goScene(name);
            }
            it = cues.erase(it);
        }
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        in.hands[0].stick = sticks[0];
        in.hands[1].stick = sticks[1];
        in.head = viewer.pose();
        in.pointer = rayThrough(in.head, o.fovDeg, aspect, px, py);
        if (click) {
            in.hands[1].trigger = 1;
            click = false;
        }
        const auto eyes = eyesFor(in.head, o.fovDeg, aspect, o.stereo);
        const render::FrameData fd = app.frame(in, eyes, w, h);
        if (!o.audio.empty()) {
            // Exactly as many samples as this frame lasts, so the track stays in step with the video.
            const auto samplesDue = static_cast<std::size_t>(std::llround(static_cast<double>(f + 1) * app.audioRate() / o.fps));
            std::vector<float> block((samplesDue - track.size() / 2) * 2);
            app.audio(block);
            track.insert(track.end(), block.begin(), block.end());
        }
        if (auto mesh = app.takeTerrainMesh()) renderer.replaceMesh(ao.terrainMesh, *mesh);
        renderer.render(fd, target.target());
        if (rawOut) {
            const std::vector<std::uint8_t> frame = target.read(0);
            if (std::fwrite(frame.data(), 1, frame.size(), rawOut) != frame.size()) {
                std::fprintf(stderr, "glyder: the frame reader went away\n");
                break;
            }
        } else if (sequence || f == frames - 1) {
            std::vector<std::uint8_t> px0 = target.read(0);
            std::vector<std::uint8_t> out;
            int ow = static_cast<int>(w);
            if (o.stereo) {
                const auto px1 = target.read(1);
                out.resize(static_cast<std::size_t>(w) * 2 * h * 4);
                for (std::uint32_t y = 0; y < h; ++y) {
                    std::memcpy(&out[(static_cast<std::size_t>(y) * w * 2) * 4], &px0[static_cast<std::size_t>(y) * w * 4], w * 4);
                    std::memcpy(&out[(static_cast<std::size_t>(y) * w * 2 + w) * 4], &px1[static_cast<std::size_t>(y) * w * 4], w * 4);
                }
                ow *= 2;
            } else {
                out = std::move(px0);
            }
            char name[1024];
            std::snprintf(name, sizeof name, o.capture.c_str(), f);
            image::writePng(sequence ? name : o.capture, ow, static_cast<int>(h), out);
        }
    }
    if (rawOut) std::fclose(rawOut);
    if (!o.audio.empty()) {
        const auto bytes = audio::wav(track, app.audioRate());
        std::FILE* af = std::fopen(o.audio.c_str(), "wb");
        if (!af || std::fwrite(bytes.data(), 1, bytes.size(), af) != bytes.size())
            std::fprintf(stderr, "glyder: cannot write %s\n", o.audio.c_str());
        if (af) std::fclose(af);
    }
    std::printf("glyder: %d frames, %s, last frame %u draws %llu triangles on %s\n", frames, app.mode().c_str(),
                renderer.drawCallsLastFrame(), static_cast<unsigned long long>(renderer.trianglesLastFrame()), gpu->name().c_str());
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Window

int windowed(const Options& o) {
    int screenIndex = 0;
    xcb_connection_t* conn = xcb_connect(nullptr, &screenIndex);
    if (xcb_connection_has_error(conn)) {
        std::fprintf(stderr, "glyder: no X display (use --capture for headless)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenIndex; ++i) xcb_screen_next(&it);
    xcb_screen_t* screen = it.data;
    const xcb_window_t window = xcb_generate_id(conn);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t values[2] = {screen->black_pixel, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                                                              XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                                              XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, static_cast<std::uint16_t>(o.width),
                      static_cast<std::uint16_t>(o.height), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    auto atom = [&](const char* name) {
        xcb_intern_atom_reply_t* r =
            xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, static_cast<std::uint16_t>(std::strlen(name)), name), nullptr);
        const xcb_atom_t a = r ? r->atom : 0;
        std::free(r);
        return a;
    };
    const xcb_atom_t wmProtocols = atom("WM_PROTOCOLS"), wmDelete = atom("WM_DELETE_WINDOW");
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window, wmProtocols, 4, 32, 1, &wmDelete);
    xcb_map_window(conn, window);
    xcb_flush(conn);

    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    std::string error;
    auto gpu = render::Gpu::create(exts, true, std::getenv("GLYDER_VALIDATION") != nullptr, &error);
    if (!gpu) {
        std::fprintf(stderr, "glyder: %s\n", error.c_str());
        return 1;
    }
    VkXcbSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
    sci.connection = conn;
    sci.window = window;
    VkSurfaceKHR surface;
    vkCreateXcbSurfaceKHR(gpu->instance, &sci, nullptr, &surface);
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu->physical, gpu->queueFamily, surface, &supported);
    if (!supported) {
        std::fprintf(stderr, "glyder: the GPU cannot present to this window\n");
        return 1;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    VkExtent2D extent{static_cast<std::uint32_t>(o.width), static_cast<std::uint32_t>(o.height)};
    auto makeSwapchain = [&] {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu->physical, surface, &caps);
        if (caps.currentExtent.width != 0xFFFFFFFFu) extent = caps.currentExtent;
        std::uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu->physical, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu->physical, surface, &n, formats.data());
        VkSurfaceFormatKHR fmt = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) fmt = f;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = std::max(caps.minImageCount, 2u);
        ci.imageFormat = fmt.format;
        ci.imageColorSpace = fmt.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain;
        VkSwapchainKHR fresh;
        vkCreateSwapchainKHR(gpu->device, &ci, nullptr, &fresh);
        if (swapchain) vkDestroySwapchainKHR(gpu->device, swapchain, nullptr);
        swapchain = fresh;
        vkGetSwapchainImagesKHR(gpu->device, swapchain, &n, nullptr);
        images.resize(n);
        vkGetSwapchainImagesKHR(gpu->device, swapchain, &n, images.data());
    };
    makeSwapchain();

    const std::uint32_t views = o.stereo ? 2u : 1u;
    render::Renderer renderer(*gpu, VK_FORMAT_R8G8B8A8_SRGB, views, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    std::unique_ptr<render::OffscreenTarget> target;
    VkExtent2D targetExtent{};
    AppOptions ao = o.app;
    ao.terrainMesh = renderer.addMesh({});
    App app(ao);
    applyAccess(app, o);

    VkSemaphore acquired, blitted;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(gpu->device, &sem, nullptr, &acquired);
    vkCreateSemaphore(gpu->device, &sem, nullptr, &blitted);
    VkCommandBuffer blitCmd;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gpu->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(gpu->device, &cai, &blitCmd);
    VkFence blitFence;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(gpu->device, &fci, nullptr, &blitFence);

    Viewer viewer;
    bool keys[256]{};
    bool running = true, rightDrag = false, leftDown = false;
    int mx = o.width / 2, my = o.height / 2, lastX = mx, lastY = my;
    float wheel = 0;
    double last = net::now();
    std::string title;
    while (running && !app.quitRequested()) {
        Input in;
        in.desktop = true;
        in.time = net::now();
        in.dt = static_cast<float>(in.time - last);
        last = in.time;
        bool tab = false, escape = false, space = false;
        while (xcb_generic_event_t* ev = xcb_poll_for_event(conn)) {
            switch (ev->response_type & 0x7F) {
            case XCB_KEY_PRESS: {
                const auto* k = reinterpret_cast<xcb_key_press_event_t*>(ev);
                keys[k->detail] = true;
                if (k->detail == 23) tab = true;
                if (k->detail == 9 || k->detail == 22) escape = true;
                if (k->detail == 65) space = true;
                // Digits for the pairing keypad (evdev keycodes 10..19 are 1..9, 0).
                if (k->detail >= 10 && k->detail <= 19) in.typed += static_cast<char>(k->detail == 19 ? '0' : '1' + (k->detail - 10));
                if (k->detail == 22) in.typed += '\b';
                if (k->detail == 36) in.typed += '\n';
                break;
            }
            case XCB_KEY_RELEASE: keys[reinterpret_cast<xcb_key_release_event_t*>(ev)->detail] = false; break;
            case XCB_BUTTON_PRESS: {
                const auto* b = reinterpret_cast<xcb_button_press_event_t*>(ev);
                if (b->detail == 1) leftDown = true;
                if (b->detail == 3) rightDrag = true;
                if (b->detail == 4) wheel += 1;
                if (b->detail == 5) wheel -= 1;
                lastX = b->event_x;
                lastY = b->event_y;
                break;
            }
            case XCB_BUTTON_RELEASE: {
                const auto* b = reinterpret_cast<xcb_button_release_event_t*>(ev);
                if (b->detail == 1) leftDown = false;
                if (b->detail == 3) rightDrag = false;
                break;
            }
            case XCB_MOTION_NOTIFY: {
                const auto* m = reinterpret_cast<xcb_motion_notify_event_t*>(ev);
                mx = m->event_x;
                my = m->event_y;
                if (rightDrag) {
                    viewer.yaw -= static_cast<float>(mx - lastX) * 0.004f;
                    viewer.pitch = clamp(viewer.pitch - static_cast<float>(my - lastY) * 0.004f, -1.4f, 1.4f);
                }
                lastX = mx;
                lastY = my;
                break;
            }
            case XCB_CONFIGURE_NOTIFY: break;
            case XCB_CLIENT_MESSAGE:
                if (reinterpret_cast<xcb_client_message_event_t*>(ev)->data.data32[0] == wmDelete) running = false;
                break;
            default: break;
            }
            std::free(ev);
        }
        auto key = [&](int code) { return keys[code] ? 1.0f : 0.0f; };
        in.hands[0].stick = {key(40) - key(38), key(25) - key(39)};
        in.hands[1].stick = {key(114) - key(113), key(111) - key(116) + wheel * 4.0f};
        wheel *= 0.6f;
        if (std::fabs(wheel) < 0.05f) wheel = 0;
        in.hands[0].secondary = tab;
        in.hands[1].secondary = escape;
        in.hands[1].primary = space;
        in.hands[1].trigger = leftDown ? 1.0f : 0.0f;
        in.head = viewer.pose();
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));
        in.pointer = rayThrough(in.head, o.fovDeg, aspect, 2.0f * static_cast<float>(mx) / static_cast<float>(extent.width) - 1.0f,
                                2.0f * static_cast<float>(my) / static_cast<float>(extent.height) - 1.0f);

        const VkExtent2D want{o.stereo ? extent.width / 2 : extent.width, extent.height};
        if (!target || targetExtent.width != want.width || targetExtent.height != want.height) {
            renderer.waitIdle();
            target = std::make_unique<render::OffscreenTarget>(*gpu, VK_FORMAT_R8G8B8A8_SRGB, want, views);
            targetExtent = want;
        }
        const auto eyes = eyesFor(in.head, o.fovDeg, static_cast<float>(want.width) / static_cast<float>(want.height), o.stereo);
        const render::FrameData fd = app.frame(in, eyes, want.width, want.height);
        if (auto mesh = app.takeTerrainMesh()) renderer.replaceMesh(ao.terrainMesh, *mesh);

        std::uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(gpu->device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(gpu->device);
            makeSwapchain();
            if (r == VK_ERROR_OUT_OF_DATE_KHR) continue;
        }
        renderer.render(fd, target->target());

        vkWaitForFences(gpu->device, 1, &blitFence, VK_TRUE, UINT64_MAX);
        vkResetFences(gpu->device, 1, &blitFence);
        vkResetCommandBuffer(blitCmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(blitCmd, &bi);
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = images[index];
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkMemoryBarrier rendered{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        rendered.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        rendered.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(blitCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &rendered, 0, nullptr, 1, &toDst);
        for (std::uint32_t v = 0; v < views; ++v) {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, v, 1};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(want.width), static_cast<std::int32_t>(want.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {static_cast<std::int32_t>(v * want.width), 0, 0};
            blit.dstOffsets[1] = {static_cast<std::int32_t>((v + 1) * want.width), static_cast<std::int32_t>(want.height), 1};
            vkCmdBlitImage(blitCmd, target->target().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        }
        VkImageMemoryBarrier toPresent = toDst;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(blitCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toPresent);
        vkEndCommandBuffer(blitCmd);
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &blitCmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &blitted;
        vkQueueSubmit(gpu->queue, 1, &si, blitFence);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &blitted;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(gpu->queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(gpu->device);
            makeSwapchain();
        }
        const std::string cap = app.caption();
        if (cap != title) {
            title = cap;
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                                static_cast<std::uint32_t>(title.size()), title.c_str());
            xcb_flush(conn);
        }
    }
    vkDeviceWaitIdle(gpu->device);
    target.reset();
    vkDestroyFence(gpu->device, blitFence, nullptr);
    vkDestroySemaphore(gpu->device, acquired, nullptr);
    vkDestroySemaphore(gpu->device, blitted, nullptr);
    vkDestroySwapchainKHR(gpu->device, swapchain, nullptr);
    vkDestroySurfaceKHR(gpu->instance, surface, nullptr);
    xcb_disconnect(conn);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    o.app.clientName = "glyder-desktop";
    const char* home = std::getenv("HOME");
    o.app.storageDir = std::string(home ? home : ".") + "/.config/glyder";
    std::string rxText;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--demo")
            o.app.demo = true;
        else if (a == "--connect")
            o.app.connect = next();
        else if (a == "--code")
            o.app.secret = next();
        else if (a == "--host")
            o.app.hosts.push_back(next());
        else if (a == "--stereo")
            o.stereo = true;
        else if (a == "--capture")
            o.capture = next();
        else if (a == "--seconds")
            o.seconds = std::strtod(next().c_str(), nullptr);
        else if (a == "--fps")
            o.fps = std::max(1, std::atoi(next().c_str()));
        else if (a == "--script")
            o.script = next();
        else if (a == "--rx")
            rxText = next();
        else if (a == "--colour" || a == "--color")
            o.colour = next();
        else if (a == "--preview")
            o.preview = true;
        else if (a == "--raw")
            o.uncorrected = true;
        else if (a == "--cues")
            o.cues = true;
        else if (a == "--base")
            o.base = true;
        else if (a == "--blind")
            o.blind = true;
        else if (a == "--audio")
            o.audio = next();
        else if (a == "--fov")
            o.fovDeg = std::strtof(next().c_str(), nullptr);
        else if (a == "--size") {
            const std::string s = next();
            std::sscanf(s.c_str(), "%dx%d", &o.width, &o.height);
        } else if (a == "--version") {
            std::printf("glyder %s \"%s\"\n", kVersion, kReleaseName);
            return 0;
        } else {
            usage();
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(o.app.storageDir, ec);
    if (!rxText.empty()) {
        std::string error;
        auto p = rx::parse(rxText, &error);
        if (!p) {
            std::fprintf(stderr, "glyder: prescription: %s\n", error.c_str());
            return 2;
        }
        std::FILE* f = std::fopen((o.app.storageDir + "/prescription.txt").c_str(), "w");
        if (f) {
            std::fputs(rx::format(*p).c_str(), f);
            std::fclose(f);
        }
    }
    return o.capture.empty() ? windowed(o) : headless(o);
}
