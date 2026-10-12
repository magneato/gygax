// glyder_gallery: render every kit model headless into a contact sheet (PNG), and export the kit
// as glTF. Used for the docs and to judge the models by eye.
//
//   glyder_gallery [--out kit.png] [--glb kit.glb] [--tile 480x360] [--columns 8] [--shape NAME]

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <glyder/draw.hpp>
#include <glyder/image.hpp>
#include <glyder/kit.hpp>
#include <glyder/render.hpp>

using namespace glyder;

int main(int argc, char** argv) {
    std::string out = "kit.png", glb, only;
    bool icon = false;
    std::uint32_t tw = 480, th = 360;
    int columns = 8;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--out")
            out = next();
        else if (a == "--glb")
            glb = next();
        else if (a == "--columns")
            columns = std::max(1, std::atoi(next().c_str()));
        else if (a == "--shape")
            only = next();
        else if (a == "--icon") {
            icon = true;
            only = next();
        } else if (a == "--tile") {
            const std::string t = next();
            std::sscanf(t.c_str(), "%ux%u", &tw, &th);
        } else {
            std::fprintf(stderr, "usage: glyder_gallery [--out kit.png] [--glb kit.glb] [--tile WxH] [--columns N] [--shape NAME]\n");
            return 2;
        }
    }
    if (!glb.empty()) {
        const auto bytes = kit::exportGlb();
        std::ofstream(glb, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::printf("wrote %s (%zu bytes, %zu meshes)\n", glb.c_str(), bytes.size(), kit::allShapes().size());
    }

    std::string error;
    auto gpu = render::Gpu::create({}, false, std::getenv("GLYDER_VALIDATION") != nullptr, &error);
    if (!gpu) {
        std::fprintf(stderr, "glyder_gallery: %s\n", error.c_str());
        return 1;
    }
    render::Renderer renderer(*gpu, VK_FORMAT_R8G8B8A8_SRGB, 1, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    render::OffscreenTarget tile(*gpu, VK_FORMAT_R8G8B8A8_SRGB, {tw, th}, 1);

    // A plinth for everything to stand on.
    kit::Builder pb;
    pb.color(hex(0x30343B));
    pb.at({}, axisAngle({0, 1, 0}, -kPi / 2), [&] { pb.lathe(std::array<Vec2, 3>{{{-0.04f, 0.0f}, {-0.04f, 1.0f}, {0.0f, 1.0f}}}, 48); });
    const render::MeshId plinth = renderer.addMesh(pb.finish());

    std::vector<kit::Shape> shapes;
    for (kit::Shape s : kit::allShapes())
        if (only.empty() || kit::name(s) == only) shapes.push_back(s);
    if (shapes.empty()) {
        std::fprintf(stderr, "no such shape: %s\n", only.c_str());
        return 2;
    }
    const int cols = std::min(columns, static_cast<int>(shapes.size()));
    const int rows = (static_cast<int>(shapes.size()) + cols - 1) / cols;
    const std::size_t sw = static_cast<std::size_t>(cols) * tw, sh = static_cast<std::size_t>(rows) * th;
    std::vector<std::uint8_t> sheet(sw * sh * 4, 0);

    for (std::size_t n = 0; n < shapes.size(); ++n) {
        const kit::Shape shape = shapes[n];
        const kit::Mesh mesh = kit::build(shape);
        const Vec3 c = mesh.bounds.centre();
        const float r = std::max(mesh.bounds.radius(), 0.05f);
        // Models stand on their lowest point; the plinth sits just below.
        const float floorZ = mesh.bounds.min.z;
        const kit::Domain dom = kit::domain(shape);
        const bool flying = dom == kit::Domain::Air || dom == kit::Domain::Space;
        const float lift = flying ? r * 0.35f : 0.0f;

        render::SceneUniforms scene;
        render::applyEnvironment(scene, render::Environment::Studio);
        const Vec3 target = toRender(Vec3{c.x, c.y, c.z + lift});
        const float az = 38.0f * kDeg, el = 20.0f * kDeg;
        const float dist = r * 3.15f;
        const Vec3 eye = target + toRender(Vec3{std::cos(az) * std::cos(el), std::sin(az) * std::cos(el), std::sin(el)}) * dist;
        const Mat4 view = lookAt(eye, target, {0, 1, 0});
        const Mat4 proj = perspective(34.0f * kDeg, static_cast<float>(tw) / static_cast<float>(th), r * 0.05f);
        scene.views[0] = scene.views[1] = render::viewData(view, proj);

        if (icon) {
            // An icon: the model alone, larger, from a little above, rotated for drama.
            const Vec3 iconEye = target + toRender(normalize(Vec3{0.62f, -0.5f, 0.6f})) * (r * 3.7f);
            scene.views[0] = scene.views[1] =
                render::viewData(lookAt(iconEye, target, {0, 1, 0}), perspective(34.0f * kDeg, 1.0f, r * 0.05f));
            render::applyEnvironment(scene, render::Environment::Night);
            scene.ambientSky = {0.18f, 0.22f, 0.3f, 1.0f};
            scene.sunColor = {1.1f, 1.15f, 1.25f, 0.6f};
        }
        std::vector<render::DrawItem> items;
        render::DrawItem model;
        model.mesh = static_cast<render::MeshId>(shape);
        model.model = worldToRender() * translation({0, 0, lift});
        model.tint = kit::palette::teams[1];
        model.emissive = 1.6f;
        items.push_back(model);
        render::DrawItem base;
        base.mesh = plinth;
        const float pr = std::max(mesh.bounds.size().x, mesh.bounds.size().y) * 0.5f;
        base.model = worldToRender() * compose({c.x, c.y, floorZ - 0.04f * pr}, Quat{}, {pr, pr, pr});
        if (!icon) items.push_back(base);

        // Soft contact shadow and the label.
        std::vector<render::PanelVertex> panels;
        render::PanelStyle shadow;
        shadow.fill = {0, 0, 0};
        shadow.fillAlpha = flying ? 90 : 150;
        const Vec2 half{mesh.bounds.size().x * 0.5f, mesh.bounds.size().y * 0.5f};
        shadow.radius = std::min(half.x, half.y);
        shadow.softness = r * 0.15f;
        if (!icon)
            render::addPanel(panels, toRender({c.x, c.y, floorZ + 0.002f * r}), toRender({1, 0, 0}), toRender({0, 1, 0}), half, shadow);

        std::vector<render::Line> strokes;
        const Mat4 camWorld = rigidInverse(view);
        const Vec3 right = camWorld.transformDir({1, 0, 0}), up = camWorld.transformDir({0, 1, 0}), fwd = camWorld.transformDir({0, 0, -1});
        const float d = dist * 0.12f;
        const float halfH = std::tan(17.0f * kDeg) * d, halfW = halfH * static_cast<float>(tw) / static_cast<float>(th);
        const Vec3 corner = eye + fwd * d - right * (halfW * 0.9f) - up * (halfH * 0.86f);
        render::text(strokes, kit::name(shape), corner, right, up, halfH * 0.075f, {220, 222, 226}, render::Align::Left, 0.1f);
        const kit::Mesh info = kit::build(shape);
        char stats[64];
        std::snprintf(stats, sizeof stats, "%zu TRIS  %.1f M", info.triangles(),
                      static_cast<double>(std::max(mesh.bounds.size().x, std::max(mesh.bounds.size().y, mesh.bounds.size().z))));
        render::text(strokes, stats, corner + right * (halfW * 1.8f), right, up, halfH * 0.055f, {150, 156, 164}, render::Align::Right,
                     0.1f);

        render::FrameData frame;
        frame.scene = scene;
        frame.items = items;
        frame.panels = panels;
        if (!icon) frame.solid = strokes;
        renderer.render(frame, tile.target());
        const auto px = tile.read(0);
        const std::size_t ox = (n % static_cast<std::size_t>(cols)) * tw, oy = (n / static_cast<std::size_t>(cols)) * th;
        for (std::uint32_t y = 0; y < th; ++y)
            std::memcpy(&sheet[((oy + y) * sw + ox) * 4], &px[static_cast<std::size_t>(y) * tw * 4], static_cast<std::size_t>(tw) * 4);
    }
    if (!image::writePng(out, static_cast<int>(sw), static_cast<int>(sh), sheet)) {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%zux%zu, %zu models, %s)\n", out.c_str(), sw, sh, shapes.size(), gpu->name().c_str());
    return 0;
}
