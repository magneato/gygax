#pragma once

// The glyder experience, independent of any platform: the lobby, pairing, the overview of a world,
// dropping into nodes, the HUD, menus and vision correction. The Quest layer and the desktop
// viewer feed it head and hand poses and draw what it returns; everything here is plain C++ and
// runs in tests without a GPU or a headset.
//
// Modes:
//   Lobby     the Construct: a white void listing the Gygax hosts on the network
//   Overview  a host's world at map-table scale; grab, turn and zoom it; point at a node
//   Ride      inside a node at 1:1. The view switches the instant you choose the node; control
//             passes to you when the node reaches a good stopping point, and the HUD says what it
//             is finishing until then. B jacks you out, instantly, and autonomy resumes.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glyder/client.hpp>
#include <glyder/draw.hpp>
#include <glyder/handling.hpp>
#include <glyder/kit.hpp>
#include <glyder/rx.hpp>

namespace glyder {

class DemoWorld;
class RxServer;

struct Pose {
    Vec3 pos;
    Quat q;
    [[nodiscard]] Vec3 forward() const { return rotate(q, {0, 0, -1}); }
    [[nodiscard]] Vec3 right() const { return rotate(q, {1, 0, 0}); }
    [[nodiscard]] Vec3 up() const { return rotate(q, {0, 1, 0}); }
};

struct Hand {
    bool active{false};
    Pose aim, grip;
    float trigger{0}, squeeze{0};
    Vec2 stick;
    bool primary{false};   // A / X
    bool secondary{false}; // B / Y
    bool stickClick{false};
};

struct Input {
    double time{0}; // net::now()
    float dt{0};    // seconds since the last frame
    Pose head;      // stage space (y up, metres)
    Hand hands[2];  // [0] left, [1] right
    bool desktop{false};
    // Desktop: the pointer ray when the mouse is over the window (replaces the right hand's aim).
    std::optional<Pose> pointer;
    std::string typed; // desktop: characters typed this frame
};

// One eye: its pose in stage space and its field of view as tangents (OpenXR convention:
// left and down negative).
struct EyeView {
    Pose pose;
    float tanLeft{-1}, tanRight{1}, tanUp{1}, tanDown{-1};
};

struct Haptic {
    float amplitude{0};
    float seconds{0};
};

struct AppOptions {
    std::string clientName{"glyder"};
    std::string storageDir;         // where the prescription and paired hosts are kept
    std::vector<std::string> hosts; // extra addresses to ask during discovery
    std::string connect;            // connect to this address at once
    std::string secret;             // its pairing code
    bool demo{false};               // start in the Construct demo
    render::MeshId terrainMesh{0};  // a mesh slot the platform reserved for terrain
    float nearZ{0.05f};
};

class App {
public:
    explicit App(AppOptions options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Advance one frame and build what to draw. Eye poses come in as tracked; the app applies the
    // pilot's PD and prism to them before projecting.
    // `width` and `height` are one eye buffer's size, for the vision pre-compensation.
    render::FrameData frame(const Input& in, std::span<const EyeView> eyes, std::uint32_t width = 0, std::uint32_t height = 0);

    // A new terrain mesh for the reserved slot, when one has arrived.
    [[nodiscard]] std::optional<kit::Mesh> takeTerrainMesh();
    [[nodiscard]] Haptic haptic(int hand) const { return haptics_[hand]; }
    [[nodiscard]] const rx::Correction& vision() const { return correction_; }
    [[nodiscard]] bool visionFilter() const { return rxEnabled_ && correction_.filter; }
    [[nodiscard]] bool quitRequested() const { return quit_; }
    // Desktop: a one-line description of what the app is doing, for the window title.
    [[nodiscard]] std::string caption() const;
    // Scripted control for captures and tests.
    void select(std::uint32_t entity);
    // The first drivable node of this kit shape, or 0.
    [[nodiscard]] std::uint32_t find(kit::Shape shape) const;
    void jackOut();
    [[nodiscard]] std::uint32_t riding() const { return ride_.entity; }
    [[nodiscard]] std::string mode() const;

private:
    enum class Mode { Lobby, Keypad, Connecting, Overview, Ride };
    enum class Overlay { None, Menu, Vision };

    struct Button {
        std::string id;
        Vec3 c, right, up;
        Vec2 half;
    };

    struct Ride {
        std::uint32_t entity{0};
        bool control{false};
        std::string status; // "FINISHING A BREAK TURN"
        float eta{0};
        double since{0}; // when we dropped in
        double controlAt{0};
        Pose anchor; // stage pose the node's eyepoint maps to
        Handling handling;
        Motion predicted;
        bool havePrediction{false};
        kit::Shape shape{kit::Shape::Marker};
    };

    // Frame stages.
    void connectTo(const net::Address& a, const std::string& secret, const std::string& label);
    void poll(const Input& in);
    void lobby(const Input& in);
    void keypad(const Input& in);
    void overview(const Input& in);
    void ride(const Input& in);
    void menu(const Input& in);
    void visionPanel(const Input& in);
    void hud(const Input& in);
    void controllers(const Input& in);
    void rain(const Input& in);
    void drawWorld(const Input& in, const Mat4& renderFromWorld, float worldScale, bool overview);
    void tableGestures(const Input& in);
    void enterRide(std::uint32_t id, const Input& in);
    void leaveRide(const std::string& why);
    void toast(std::string text, double seconds = 4.0);
    static void subdivide(std::vector<render::Line>& lines, Vec3 eye);
    [[nodiscard]] Mat4 tableTransform() const;
    [[nodiscard]] Mat4 rideTransform(const Input& in);

    // Immediate-mode UI on world-space panels, hit by the pointer rays.
    bool button(const Input& in, const std::string& id, Vec3 c, Vec3 right, Vec3 up, Vec2 half, const std::string& label,
                bool active = false, bool enabled = true);
    void label(const std::string& text, Vec3 origin, Vec3 right, Vec3 up, float height, Rgb8 colour,
               render::Align align = render::Align::Left, std::uint8_t alpha = 255);
    void card(Vec3 c, Vec3 right, Vec3 up, Vec2 half, Rgb8 fill, std::uint8_t alpha, Rgb8 border, float glow = 0.0f);
    [[nodiscard]] Pose pointer(const Input& in, int hand) const;
    [[nodiscard]] bool pressed(const Input& in, int hand) const; // trigger went down this frame
    void loadState();
    void saveState() const;
    void applyRx();

    AppOptions options_;
    std::unique_ptr<Client> client_;
    std::unique_ptr<DemoWorld> demo_;
    std::unique_ptr<RxServer> rxServer_;
    Mode mode_{Mode::Lobby};
    Overlay overlay_{Overlay::None};
    std::string connectingTo_;
    net::Address pendingHost_;
    std::string code_;
    std::map<std::string, std::string> paired_; // host address -> pairing code
    std::optional<wire::World> world_;
    std::optional<kit::Mesh> terrainMesh_;
    std::optional<wire::Heightfield> terrain_;
    bool haveTerrain_{false};

    // Frame output.
    render::SceneUniforms scene_;
    render::PostParams post_{};
    std::vector<render::DrawItem> items_;
    std::vector<render::PanelVertex> panels_;
    std::vector<render::Line> solid_, glow_;
    std::vector<render::PanelVertex> uiPanels_;
    std::vector<render::Line> uiSolid_, uiGlow_;
    bool ui_{false}; // route labels, cards and buttons to the interface layer
    struct UiLayer {
        explicit UiLayer(App& a) : app(a), was(a.ui_) { a.ui_ = true; }
        ~UiLayer() { app.ui_ = was; }
        App& app;
        bool was;
    };
    [[nodiscard]] std::vector<render::PanelVertex>& panelsOut() { return ui_ ? uiPanels_ : panels_; }
    [[nodiscard]] std::vector<render::Line>& solidOut() { return ui_ ? uiSolid_ : solid_; }
    [[nodiscard]] std::vector<render::Line>& glowOut() { return ui_ ? uiGlow_ : glow_; }
    std::vector<wire::EntityState> entities_;
    Haptic haptics_[2];

    // Input edges.
    bool triggerWas_[2]{false, false}, primaryWas_[2]{false, false}, secondaryWas_[2]{false, false};
    bool menuWas_{false};
    std::string hot_, hotWas_;
    std::string clicked_;

    // Overview.
    Vec3 focus_; // world point at the table's centre
    float tableScale_{0.0002f};
    float tableYaw_{0};
    Vec3 tableCentre_{0, 0.9f, -0.9f};
    bool grabbing_[2]{false, false};
    Pose grabStart_[2];
    Vec3 grabFocus_;
    float grabScale_{1}, grabYaw_{0};
    float grabDistance_{1};
    std::uint32_t hovered_{0};
    std::map<std::uint32_t, std::deque<std::pair<double, Vec3>>> trails_;

    Ride ride_;
    double rainAt_{-10};
    bool rainOut_{false};
    struct Toast {
        std::string text;
        double until;
    };
    std::deque<Toast> toasts_;
    std::deque<wire::Event> events_;
    double eventAt_{0};

    // Comfort.
    bool horizonLock_{true}, vignette_{true};
    float vignetteAmount_{0};

    // Vision.
    rx::Prescription rx_;
    bool rxEnabled_{true};
    float rxStrength_{0.7f};
    bool chart_{false};
    rx::Correction correction_;
    std::string rxMessage_;

    Pose menuAt_, visionAt_;
    float hudYaw_{0};
    bool hudYawSet_{false};
    bool menuPlaced_{false}, visionPlaced_{false};
    bool quit_{false};
    double startTime_{-1};
};

} // namespace glyder
