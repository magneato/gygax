#include <glyder/audio.hpp>

#include <algorithm>
#include <cmath>

namespace glyder::audio {

namespace {
constexpr float kTwoPi = 6.28318530718f;
constexpr std::size_t kMaxVoices = 48;
} // namespace

std::string_view name(Cue c) {
    switch (c) {
    case Cue::Threat: return "threat";
    case Cue::Air: return "air";
    case Cue::Ground: return "ground";
    case Cue::Friend: return "friend";
    case Cue::Structure: return "structure";
    case Cue::Terrain: return "terrain";
    case Cue::Focus: return "focus";
    case Cue::Event: return "event";
    }
    return "air";
}

const Envelope& envelope(Cue c) {
    // Attack, decay, sustain, release: each stage its own waveform.
    static const Envelope threat{{Wave::Square, 0.004f}, {Wave::Saw, 0.03f}, {Wave::Square, 0.035f}, {Wave::Sine, 0.06f}, 0.7f};
    static const Envelope air{{Wave::Triangle, 0.02f}, {Wave::Sine, 0.06f}, {Wave::Sine, 0.08f}, {Wave::Sine, 0.25f}, 0.5f};
    static const Envelope ground{{Wave::Saw, 0.03f}, {Wave::Square, 0.08f}, {Wave::Triangle, 0.12f}, {Wave::Sine, 0.2f}, 0.5f};
    static const Envelope friendly{{Wave::Sine, 0.01f}, {Wave::Triangle, 0.04f}, {Wave::Sine, 0.03f}, {Wave::Sine, 0.15f}, 0.4f};
    static const Envelope structure{{Wave::Noise, 0.002f}, {Wave::Triangle, 0.02f}, {Wave::Triangle, 0.0f}, {Wave::Sine, 0.05f}, 0.3f};
    static const Envelope terrain{{Wave::Sine, 0.04f}, {Wave::Sine, 0.04f}, {Wave::Triangle, 0.14f}, {Wave::Sine, 0.08f}, 0.6f};
    static const Envelope focus{{Wave::Triangle, 0.005f}, {Wave::Square, 0.02f}, {Wave::Triangle, 0.05f}, {Wave::Sine, 0.12f}, 0.5f};
    static const Envelope event{{Wave::Square, 0.005f}, {Wave::Triangle, 0.05f}, {Wave::Sine, 0.1f}, {Wave::Sine, 0.3f}, 0.5f};
    switch (c) {
    case Cue::Threat: return threat;
    case Cue::Air: return air;
    case Cue::Ground: return ground;
    case Cue::Friend: return friendly;
    case Cue::Structure: return structure;
    case Cue::Terrain: return terrain;
    case Cue::Focus: return focus;
    case Cue::Event: return event;
    }
    return air;
}

float baseHz(Cue c) {
    switch (c) {
    case Cue::Threat: return 1320;
    case Cue::Air: return 660;
    case Cue::Ground: return 165;
    case Cue::Friend: return 990;
    case Cue::Structure: return 330;
    case Cue::Terrain: return 220;
    case Cue::Focus: return 523.25f;
    case Cue::Event: return 784;
    }
    return 440;
}

Voicing voicing(const Source& s) {
    Voicing v;
    const float near = std::clamp(1.0f - s.distance / std::max(s.audible, 1e-6f), 0.0f, 1.0f);
    v.gain = near * near;
    // Pan by bearing; a source behind you is a little quieter and a semitone lower, so front and
    // back are told apart.
    v.pan = std::clamp(s.direction.x, -1.0f, 1.0f);
    const bool behind = s.direction.z > 0.2f;
    // Elevation: straight up is an octave above level, straight down an octave below.
    const float elevation = std::asin(std::clamp(s.direction.y, -1.0f, 1.0f)) / 1.5707963f;
    v.hz = baseHz(s.cue) * std::exp2(elevation) * s.pitch * (behind ? 0.944f : 1.0f);
    if (behind) v.gain *= 0.8f;
    switch (s.cue) {
    case Cue::Threat: {
        // Faster as it closes: from twice a second at the edge of hearing to a near-continuous
        // rattle in the last second. Never quiet while inbound.
        const float tgo = s.timeToGo >= 0 ? s.timeToGo : 10.0f;
        v.period = std::clamp(0.06f + tgo * 0.06f, 0.06f, 0.5f) * (0.6f + 0.4f * (1 - near));
        v.gain = std::max(v.gain, s.closing > 0 ? 0.35f : 0.0f);
        break;
    }
    case Cue::Air: v.period = 0.35f + 1.2f * (1 - near); break;
    case Cue::Ground: v.period = 0.5f + 1.5f * (1 - near); break;
    case Cue::Friend: v.period = 1.0f + 1.5f * (1 - near); break;
    case Cue::Structure: v.period = 0.25f + 0.75f * (1 - near); break;
    case Cue::Terrain:
        v.period = 0.3f;
        v.gain = std::max(v.gain, 0.15f);
        break;
    case Cue::Focus:
        v.period = 0.4f;
        v.gain = std::max(v.gain, 0.5f);
        break;
    case Cue::Event: v.period = 1e9f; break;
    }
    return v;
}

float wave(Wave w, float p, std::uint32_t& noise) {
    switch (w) {
    case Wave::Sine: return std::sin(p * kTwoPi);
    case Wave::Triangle: return p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
    case Wave::Square: return p < 0.5f ? 1.0f : -1.0f;
    case Wave::Saw: return 2 * p - 1;
    case Wave::Noise:
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        return static_cast<float>(noise) / 2147483648.0f - 1.0f;
    }
    return 0;
}

Mixer::Mixer(int sampleRate) : rate_(sampleRate) {}

void Mixer::update(std::span<const Source> sources) {
    for (auto& [id, t] : tracks_) t.live = false;
    for (const Source& s : sources) {
        Track& t = tracks_[s.id];
        t.source = s;
        t.live = true;
    }
    std::erase_if(tracks_, [](const auto& kv) { return !kv.second.live; });
}

void Mixer::note(Cue cue, float hz, float gain, float pan, float hz2, float delay) {
    spawn({cue, hz, hz2 > 0 ? hz2 : hz, gain, std::clamp(pan, -1.0f, 1.0f), -delay, 0});
}

void Mixer::spawn(const Voice& v) {
    if (v.gain <= 0.001f) return;
    if (voices_.size() >= kMaxVoices) {
        // Drop the quietest, oldest voice; threats are never the ones dropped while others remain.
        auto victim = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) {
            const bool at = a.cue == Cue::Threat, bt = b.cue == Cue::Threat;
            if (at != bt) return bt;
            return a.gain < b.gain;
        });
        voices_.erase(victim);
    }
    voices_.push_back(v);
}

void Mixer::render(std::span<float> out) {
    const std::size_t frames = out.size() / 2;
    const float dt = 1.0f / static_cast<float>(rate_);
    const float block = static_cast<float>(frames) * dt;
    std::fill(out.begin(), out.end(), 0.0f);

    // Onsets due within this block, placed at their exact sample by a negative age.
    for (auto& [id, t] : tracks_) {
        const Voicing v = voicing(t.source);
        // A new source starts at a point in its period set by its id, so sources that appear
        // together are heard one after another rather than as a chord.
        if (t.next < 0) t.next = std::min(v.period, 2.0f) * static_cast<float>((id * 2654435761u) >> 8) / 16777216.0f;
        // A source that gets closer is heard sooner.
        t.next = std::min(t.next, v.period);
        while (t.next < block) {
            if (v.gain > 0.001f) spawn({t.source.cue, v.hz, v.hz, v.gain, v.pan, -t.next, 0});
            t.next += v.period;
        }
        t.next -= block;
    }

    for (Voice& v : voices_) {
        const Envelope& e = envelope(v.cue);
        const float total = e.duration();
        // Equal-power pan.
        const float angle = (v.pan + 1) * 0.25f * 3.14159265f;
        const float gl = std::cos(angle) * v.gain, gr = std::sin(angle) * v.gain;
        for (std::size_t i = 0; i < frames; ++i) {
            const float age = v.age + static_cast<float>(i) * dt;
            if (age < 0 || age >= total) continue;
            // Stage, its waveform and the envelope level.
            float level;
            Wave w;
            const float a = e.attack.seconds, d = e.decay.seconds, s = e.sustain.seconds;
            if (age < a) {
                level = age / std::max(a, 1e-6f);
                w = e.attack.wave;
            } else if (age < a + d) {
                level = 1 - (1 - e.sustainLevel) * (age - a) / std::max(d, 1e-6f);
                w = e.decay.wave;
            } else if (age < a + d + s) {
                level = e.sustainLevel;
                w = e.sustain.wave;
            } else {
                level = e.sustainLevel * (1 - (age - a - d - s) / std::max(e.release.seconds, 1e-6f));
                w = e.release.wave;
            }
            const float hz = v.hz + (v.hz2 - v.hz) * std::clamp(age / total, 0.0f, 1.0f);
            v.phase += hz * dt;
            v.phase -= std::floor(v.phase);
            const float x = wave(w, v.phase, noise_) * level;
            out[i * 2] += x * gl;
            out[i * 2 + 1] += x * gr;
        }
        v.age += block;
    }
    std::erase_if(voices_, [](const Voice& v) { return v.age >= envelope(v.cue).duration(); });

    // Master: volume and a soft limit, so many sources never clip.
    for (float& x : out) x = std::tanh(x * volume_);
}

std::vector<std::uint8_t> wav(std::span<const float> stereo, int sampleRate) {
    std::vector<std::uint8_t> b;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&](std::uint16_t v) {
        b.push_back(static_cast<std::uint8_t>(v));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto data = static_cast<std::uint32_t>(stereo.size() * 2);
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    u32(36 + data);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    u32(data);
    for (float x : stereo) {
        const auto s = static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
        u16(static_cast<std::uint16_t>(s));
    }
    return b;
}

} // namespace glyder::audio
