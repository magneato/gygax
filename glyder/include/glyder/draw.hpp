#pragma once

// Draw lists: what glyder's app logic hands the renderer each frame. Vulkan-free, so the whole
// experience (scene, HUD, drop-in, menus) is portable and unit-testable without a GPU.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glyder/kit.hpp>
#include <glyder/math.hpp>

namespace glyder::render {

// Per-frame scene constants (matches common.glsl's std140 block).
struct ViewData {
    Mat4 viewProj, invViewProj;
    Vec4 eye;
};
struct SceneUniforms {
    ViewData views[2];
    Vec4 sunDir{0.3f, 0.8f, 0.2f, 1.0f};
    Vec4 sunColor{1, 1, 1, 0.2f};
    Vec4 skyZenith{0.1f, 0.2f, 0.5f, 0.0f};
    Vec4 skyHorizon{0.6f, 0.7f, 0.8f, 0.0001f};
    Vec4 groundColor{0.2f, 0.18f, 0.15f, 0.0f};
    Vec4 moon{0, 1, 0, 0};
    Vec4 contour{25.0f, 0.0f, 0.0f, 1.0f};
    Vec4 contourColor{0.3f, 0.5f, 0.7f, 0.0f};
    Vec4 ambientSky{0.2f, 0.22f, 0.25f, 1.0f}; // w: exposure
    Vec4 ambientGround{0.08f, 0.07f, 0.06f, 0.0f};
    Vec4 worldUp{0, 1, 0, 0};
};
static_assert(sizeof(SceneUniforms) == 2 * 144 + 11 * 16);

using MeshId = std::uint32_t; // kit shapes occupy ids 0 .. kit::Shape::Count - 1

struct DrawItem {
    MeshId mesh{0};
    Mat4 model; // model to render frame
    Rgb8 tint{kit::palette::teams[0]};
    float highlight{0};  // 0..1 selection halo
    float emissive{1};   // emitter gain, 0..4
    bool contour{false}; // draw world-height contour lines (terrain)
};

struct Line {
    Vec3 a, b;   // render frame
    float width; // render metres
    float intensity;
    Rgb8 colour;
    std::uint8_t alpha{255};
};

struct PanelVertex {
    Vec3 pos;
    Vec4 local; // xy offset from the centre, zw half size (metres)
    Vec4 shape; // corner radius, border width, softness, glow
    std::uint8_t fill[4];
    std::uint8_t border[4];
};
static_assert(sizeof(PanelVertex) == 52);

struct PanelStyle {
    float radius{0.02f}, border{0.0f}, softness{0.0f}, glow{0.0f};
    Rgb8 fill{0, 0, 0};
    std::uint8_t fillAlpha{200};
    Rgb8 borderColour{255, 255, 255};
    std::uint8_t borderAlpha{255};
};
// Append a rectangle centred at `c` spanning `right` and `up` (unit axes) with half size `half`.
void addPanel(std::vector<PanelVertex>& out, Vec3 c, Vec3 right, Vec3 up, Vec2 half, const PanelStyle& style);

// Vision pre-compensation for one eye (see shaders/post.frag and glyder/rx.hpp).
struct PostEye {
    Vec4 axes;    // major blur axis (x, y), Gaussian sigma major and minor, in pixels
    Vec4 gains;   // gain major, gain minor, foveal radius (tangent), enabled (1)
    Vec4 frustum; // tan left, tan width, tan up, tan height
    Vec4 texel;   // 1 / width, 1 / height
};
struct PostParams {
    PostEye eye[2];
};
static_assert(sizeof(PostParams) == 128);

struct FrameData {
    SceneUniforms scene;
    std::span<const DrawItem> items;
    std::span<const PanelVertex> panels;
    std::span<const Line> solid; // strokes: text, HUD, outlines (alpha blended)
    std::span<const Line> glow;  // lasers, tracers, rings, trails (additive)
    // The interface layer, drawn last and never hidden by the world: HUD, menus, panels.
    std::span<const PanelVertex> uiPanels;
    std::span<const Line> uiSolid;
    std::span<const Line> uiGlow;
    bool drawSky{true};
    Vec4 clear{0, 0, 0, 1};
    const PostParams* post{nullptr}; // non-null: run the vision pre-compensation pass
};

// Lighting moods. Night is OHFLOCK's moonlit desert; Construct is the white void glyder's lobby
// lives in; Studio is the kit gallery's backdrop.
enum class Environment { Night, Dusk, Day, Construct, Studio };
// Fill the lighting half of the scene constants (views are the caller's). `metresPerUnit` scales
// fog and contours for the god view's miniature world.
void applyEnvironment(SceneUniforms& s, Environment e, float renderPerWorld = 1.0f, float groundRenderY = 0.0f);
// Build both eyes' view data from view and projection matrices.
[[nodiscard]] ViewData viewData(const Mat4& view, const Mat4& proj);

enum class Align { Left, Centre, Right };

// Glyder's stroke font: uppercase, digits and punctuation drawn as line segments on an 8 x 12
// grid, so text is resolution independent and razor sharp at any distance in a headset.
// Appends strokes for `text` with its baseline starting at `origin` (moved by `align`), along the
// unit axes `right` and `up`, `height` render metres tall (cap height).
void text(std::vector<Line>& out, std::string_view text, Vec3 origin, Vec3 right, Vec3 up, float height, Rgb8 colour,
          Align align = Align::Left, float weight = 0.11f, std::uint8_t alpha = 255);
[[nodiscard]] float textWidth(std::string_view text, float height);

} // namespace glyder::render
