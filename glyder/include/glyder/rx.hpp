#pragma once

// Vision correction from a spectacle prescription.
//
// A headset's lenses form a virtual image at a fixed distance (about 1.3 m on a Quest 3S). What
// software can do about a prescription, and how well:
//
//   PD       exact. The virtual cameras are placed at your measured pupillary distance, so depth
//            and scale are right for your eyes.
//   Prism    exact. A prism displaces what one eye sees; glyder turns that eye's view by the same
//            angle, so binocular vision is as comfortable as through your glasses. Free: it is a
//            change to the projection, not a filter.
//   Sphere,  partial. Light cannot be refocused by a flat display, but the blur your eye adds at
//   cylinder the headset's focal distance is predictable: an ellipse set by your sphere, cylinder
//            and axis, your pupil and how much you can still accommodate. glyder sharpens the
//            image along those axes before you see it (a directional pre-compensation), which
//            restores contrast for mild prescriptions, roughly within +-1.5 D, at some cost in
//            dynamic range. Beyond that, wear glasses with the headset's spacer or lens inserts;
//            glyder says so when it sees such a prescription.
//
// Notation accepted (one eye per line, any order, case-insensitive):
//   OD -1.25 -0.50 x 180 1.0 BI      OS: SPH -1.00 CYL -0.75 AXIS 90 PRISM 0.5 BU
//   RIGHT +0.75 / LEFT -0.25 DS      PD 63    ADD +2.00    AGE 52
// Prism bases: BI/BO/BU/BD (in, out, up, down), several per eye allowed.

#include <optional>
#include <string>
#include <string_view>

namespace glyder::rx {

struct Eye {
    float sphere{0};   // dioptres
    float cylinder{0}; // dioptres (either sign convention; normalised to minus cylinder)
    float axis{180};   // degrees 1..180
    float prismOut{0}; // prism dioptres, base out (+) / base in (-)
    float prismUp{0};  // prism dioptres, base up (+) / base down (-)
};

struct Prescription {
    Eye right, left; // OD, OS
    float pd{63};    // mm, both eyes
    float add{0};    // near addition, dioptres
    int age{0};      // 0: unknown
    [[nodiscard]] bool empty() const;
};

[[nodiscard]] std::optional<Prescription> parse(std::string_view text, std::string* error = nullptr);
[[nodiscard]] std::string format(const Prescription& p);

// The headset's optics and viewing conditions.
struct Display {
    float focusDistance{1.3f}; // metres to the virtual image
    float pupil{4.5f};         // mm, eye pupil in the headset
    float pixelsPerDegree{19}; // at the centre of the eye buffer
};

struct EyeCorrection {
    // Turn the eye's view by these angles (radians) to emulate prism: positive yaw turns the view
    // left (content moves right), positive pitch turns it up (content moves down).
    float yaw{0}, pitch{0};
    // Residual blur ellipse at the display (radians, full width), after the best accommodation.
    float blurMajor{0}, blurMinor{0};
    // Radians: the major blur axis in prescription (TABO) terms, counter-clockwise from 3 o'clock as
    // an examiner facing the wearer sees it. In the wearer's image (x right, y down) that is the
    // direction (cos axis, sin axis).
    float axis{0};
    // Pre-compensation: Gaussian sigma in pixels and gain along each axis; zero gain is no filter.
    float sigmaMajor{0}, sigmaMinor{0}, gainMajor{0}, gainMinor{0};
};

struct Correction {
    EyeCorrection eye[2];       // [0] left (OS), [1] right (OD): OpenXR's view order
    float ipd{0.063f};          // metres
    bool filter{false};         // any eye needs pre-compensation
    bool beyondSoftware{false}; // a prescription software cannot meaningfully correct
    std::string advice;         // one line for the user
};

// `strength` 0..1 scales the pre-compensation (the user tunes it against an acuity chart).
[[nodiscard]] Correction compute(const Prescription& p, const Display& d = {}, float strength = 0.7f);

// The pre-compensation gain for a blur of Gaussian sigma `sigmaPixels` at a strength 0..1.
[[nodiscard]] float gainFor(float sigmaPixels, float strength);

// Average amplitude of accommodation (dioptres) for an age, Hofstetter; with an ADD, from that.
[[nodiscard]] float accommodation(const Prescription& p);

} // namespace glyder::rx
