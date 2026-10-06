#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gygax::inference::atari {

// ATARI ("Agent Tokenized/Terse Agent Relay Interface") is a compact
// alternative to the orchestrator's default JSON tool-calling protocol
// (`{"tool": "...", "input": "..."}` / `{"answer": "..."}`), designed to cut
// per-turn token cost. A frame looks like:
//
//   ATARI::CALL(t=m0;i=6*7)
//   ATARI::RESULT(t=m0;o=42)
//   ATARI::DONE(a=42)
//
// Frame keys are fixed single letters defined by the grammar itself (t=tool,
// i=input, o=output, e=error, a=answer) and never need to be declared. Tool
// *names*, which repeat every step of a multi-step run and are the dominant
// cost once the grammar itself is this terse, are interned through a Schema:
// the first reference to a tool emits one `ATARI::SCHEMA(code=name;...)`
// frame assigning it a short code (c0, c1, ...), and every later CALL/RESULT
// in the same run spends 2-3 characters instead of the full name. Schema is
// a prefix tree (trie) over the assigned codes, so resolving a code back to
// its name is O(code length), not a linear scan: the "tree caching" this
// protocol is built around.
//
// Values may contain any bytes except an unescaped ';', ')' or '\\', which
// must be backslash-escaped; '=' needs no escaping (only the first '=' after
// a key is significant).

class Schema {
public:
    Schema();

    // Returns the code for `name`, assigning a new one (c0, c1, ...) and
    // marking it undeclared (queued for the next declareFrame() call) if this
    // is the first time `name` is seen.
    std::string intern(const std::string& name);

    // O(code length) code -> name lookup.
    [[nodiscard]] std::optional<std::string> resolve(const std::string& code) const;

    // True if `name` has already been interned (regardless of whether the
    // resulting SCHEMA frame has been declared to the peer yet).
    [[nodiscard]] bool contains(const std::string& name) const;

    // Renders `ATARI::SCHEMA(code=name;...)` for every code interned since
    // the last call, or an empty string if nothing is pending. Consumes the
    // pending list, so calling it twice in a row without an intervening
    // intern() gives "" the second time.
    std::string declareFrame();

    // Parses a peer's SCHEMA frame (as produced by declareFrame()) and merges
    // its code -> name pairs into this schema. Returns false, with *error
    // set, if `frame` is not a well-formed SCHEMA frame; a code that
    // conflicts with an existing different mapping is also rejected.
    bool mergeSchemaFrame(const std::string& frame, std::string* error = nullptr);

    [[nodiscard]] std::size_t size() const { return nameToCode_.size(); }

private:
    struct Node {
        std::map<char, std::unique_ptr<Node>> children;
        std::optional<std::string> name;
    };

    Node root_;
    std::map<std::string, std::string> nameToCode_;
    std::vector<std::pair<std::string, std::string>> pending_;
    std::size_t nextCode_ = 0;
};

enum class FrameType { Call, Result, Error, Done, Schema, Unknown };

struct Frame {
    FrameType type = FrameType::Unknown;
    std::map<std::string, std::string> fields;

    [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const;
};

std::string encodeCall(const std::string& toolCode, const std::string& input);
std::string encodeResult(const std::string& toolCode, const std::string& output);
std::string encodeError(const std::string& toolCode, const std::string& message);
std::string encodeDone(const std::string& answer);

// Finds and parses the first well-formed `ATARI::TYPE(...)` frame in `text`.
// FrameType::Unknown (with no fields) if none is found, so callers can fall
// back to treating `text` as a plain-text answer.
Frame parse(const std::string& text);

}
