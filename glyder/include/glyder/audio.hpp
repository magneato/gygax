#pragma once

// Audio cues: a scene you can hear, so a pilot can fly with their eyes shut, or keep track of
// what is around them while looking elsewhere.
//
// Every voice is an envelope of four stages (attack, decay, sustain, release) and each stage has
// its own waveform, so a kind of object is recognisable from its first few milliseconds: incoming
// kinetic objects open with a hard square edge, aircraft with a soft triangle, ground vehicles
// with a buzzing saw. Each source repeats at a rate set by how close it is (and, for anything
// incoming, how soon it arrives), is panned by its bearing from your head, and is pitched by its
// elevation: above you sounds higher.
//
// Plain C++: the platforms only move the samples to a speaker (AAudio on Quest) or a file.

#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <glyder/math.hpp>

namespace glyder::audio {

enum class Wave : std::uint8_t { Sine, Triangle, Square, Saw, Noise };

struct Stage {
    Wave wave{Wave::Sine};
    float seconds{0.05f};
};

struct Envelope {
    Stage attack, decay, sustain, release;
    float sustainLevel{0.6f};
    [[nodiscard]] float duration() const { return attack.seconds + decay.seconds + sustain.seconds + release.seconds; }
};

// What a source is, by how a listener should react to it.
enum class Cue : std::uint8_t {
    Threat,    // a kinetic object closing on you: missile, bomb, anything fast and inbound
    Air,       // an aircraft
    Ground,    // a vehicle on the ground or the sea
    Friend,    // one of your own side
    Structure, // something fixed, close by
    Terrain,   // the ground below you (a variometer: higher as it gets closer)
    Focus,     // what you are pointing at
    Event,     // something happened: a handover, a narrative event
};
[[nodiscard]] std::string_view name(Cue c);
[[nodiscard]] const Envelope& envelope(Cue c);
[[nodiscard]] float baseHz(Cue c);

// One source this frame, in the listener's head frame.
struct Source {
    std::uint32_t id{0};
    Cue cue{Cue::Air};
    Vec3 direction; // unit: x right, y up, z back (the render frame's head axes)
    float distance{0};
    float audible{1};   // distance at which it fades out, same units as distance
    float closing{0};   // m/s, positive when approaching
    float timeToGo{-1}; // seconds until closest approach, when closing; < 0 unknown
    float pitch{1};     // extra pitch factor (Terrain uses it for height)
};

// The sound one source makes now: when it repeats and what each repetition sounds like.
struct Voicing {
    float period{1}; // seconds between onsets
    float hz{440};
    float gain{0}; // 0..1
    float pan{0};  // -1 left .. +1 right
};
[[nodiscard]] Voicing voicing(const Source& s);

// One waveform sample at phase [0, 1).
[[nodiscard]] float wave(Wave w, float phase, std::uint32_t& noiseState);

class Mixer {
public:
    explicit Mixer(int sampleRate = 48000);
    [[nodiscard]] int sampleRate() const { return rate_; }

    // The sources this frame (sources not listed fall silent after their current note).
    void update(std::span<const Source> sources);
    // A one-off note, such as an event earcon. `hz2` glides the pitch to a second note.
    void note(Cue cue, float hz, float gain, float pan, float hz2 = 0, float delay = 0);
    // Interleaved stereo float samples, [-1, 1]; frames = out.size() / 2.
    void render(std::span<float> out);
    [[nodiscard]] std::size_t voices() const { return voices_.size(); }
    void setVolume(float v) { volume_ = v; }

private:
    struct Voice {
        Cue cue;
        float hz, hz2, gain, pan;
        float age{0};
        float phase{0};
    };
    struct Track {
        Source source;
        float next{-1}; // seconds until the next onset; < 0: not yet scheduled
        bool live{false};
    };
    void spawn(const Voice& v);
    int rate_;
    float volume_{0.5f};
    std::uint32_t noise_{0x9E3779B9u};
    std::vector<Voice> voices_;
    std::map<std::uint32_t, Track> tracks_;
};

// A RIFF/WAVE file of 16-bit stereo PCM from interleaved float samples.
[[nodiscard]] std::vector<std::uint8_t> wav(std::span<const float> stereo, int sampleRate);

} // namespace glyder::audio
