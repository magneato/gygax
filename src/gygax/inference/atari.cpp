#include <gygax/inference/atari.hpp>

#include <array>
#include <string_view>
#include <utility>

namespace gygax::inference::atari {

namespace {

std::string escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\' || c == ';' || c == ')') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            out.push_back(value[++i]);
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

constexpr std::array<std::pair<std::string_view, FrameType>, 5> kFrameNames{{
    {"CALL", FrameType::Call},
    {"RESULT", FrameType::Result},
    {"ERROR", FrameType::Error},
    {"DONE", FrameType::Done},
    {"SCHEMA", FrameType::Schema},
}};

std::string_view frameName(FrameType type) {
    for (const auto& [name, t] : kFrameNames)
        if (t == type) return name;
    return "";
}

std::string encodeFrame(FrameType type, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "ATARI::";
    out += frameName(type);
    out.push_back('(');
    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) out.push_back(';');
        first = false;
        out += key;
        out.push_back('=');
        out += escape(value);
    }
    out.push_back(')');
    return out;
}

// Finds the closing, unescaped ')' for the field list starting at `open + 1`.
std::optional<std::size_t> findClose(const std::string& text, std::size_t open) {
    bool escaped = false;
    for (std::size_t i = open + 1; i < text.size(); ++i) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (text[i] == '\\')
            escaped = true;
        else if (text[i] == ')')
            return i;
    }
    return std::nullopt;
}

std::map<std::string, std::string> parseFields(std::string_view body) {
    std::map<std::string, std::string> fields;
    std::size_t start = 0;
    while (start <= body.size()) {
        std::size_t end = start;
        bool escaped = false;
        while (end < body.size()) {
            if (escaped) {
                escaped = false;
            } else if (body[end] == '\\') {
                escaped = true;
            } else if (body[end] == ';') {
                break;
            }
            ++end;
        }
        const auto segment = body.substr(start, end - start);
        if (const auto eq = segment.find('='); eq != std::string_view::npos && !segment.empty()) {
            fields[std::string(segment.substr(0, eq))] = unescape(segment.substr(eq + 1));
        }
        if (end >= body.size()) break;
        start = end + 1;
    }
    return fields;
}

}

std::string Frame::get(const std::string& key, const std::string& fallback) const {
    const auto it = fields.find(key);
    return it == fields.end() ? fallback : it->second;
}

Schema::Schema() = default;

std::string Schema::intern(const std::string& name) {
    if (const auto it = nameToCode_.find(name); it != nameToCode_.end()) return it->second;
    const std::string code = "c" + std::to_string(nextCode_++);
    Node* node = &root_;
    for (const char c : code) {
        auto& child = node->children[c];
        if (!child) child = std::make_unique<Node>();
        node = child.get();
    }
    node->name = name;
    nameToCode_[name] = code;
    pending_.emplace_back(code, name);
    return code;
}

std::optional<std::string> Schema::resolve(const std::string& code) const {
    const Node* node = &root_;
    for (const char c : code) {
        const auto it = node->children.find(c);
        if (it == node->children.end()) return std::nullopt;
        node = it->second.get();
    }
    return node->name;
}

bool Schema::contains(const std::string& name) const {
    return nameToCode_.contains(name);
}

std::string Schema::declareFrame() {
    if (pending_.empty()) return {};
    const std::string out = encodeFrame(FrameType::Schema, pending_);
    pending_.clear();
    return out;
}

bool Schema::mergeSchemaFrame(const std::string& frame, std::string* error) {
    const Frame parsed = parse(frame);
    if (parsed.type != FrameType::Schema) {
        if (error != nullptr) *error = "not a SCHEMA frame";
        return false;
    }
    for (const auto& [code, name] : parsed.fields) {
        if (const auto existing = resolve(code); existing && *existing != name) {
            if (error != nullptr) {
                error->clear();
                error->append("code '")
                    .append(code)
                    .append("' already maps to '")
                    .append(*existing)
                    .append("', not '")
                    .append(name)
                    .append("'");
            }
            return false;
        }
        Node* node = &root_;
        for (const char c : code) {
            auto& child = node->children[c];
            if (!child) child = std::make_unique<Node>();
            node = child.get();
        }
        node->name = name;
        nameToCode_[name] = code;
    }
    return true;
}

std::string encodeCall(const std::string& toolCode, const std::string& input) {
    return encodeFrame(FrameType::Call, {{"t", toolCode}, {"i", input}});
}

std::string encodeResult(const std::string& toolCode, const std::string& output) {
    return encodeFrame(FrameType::Result, {{"t", toolCode}, {"o", output}});
}

std::string encodeError(const std::string& toolCode, const std::string& message) {
    return encodeFrame(FrameType::Error, {{"t", toolCode}, {"e", message}});
}

std::string encodeDone(const std::string& answer) {
    return encodeFrame(FrameType::Done, {{"a", answer}});
}

Frame parse(const std::string& text) {
    constexpr std::string_view kMarker = "ATARI::";
    std::size_t at = text.find(kMarker);
    while (at != std::string::npos) {
        const std::size_t nameStart = at + kMarker.size();
        const std::size_t open = text.find('(', nameStart);
        if (open == std::string::npos) {
            at = text.find(kMarker, nameStart);
            continue;
        }
        const std::string_view candidate = std::string_view(text).substr(nameStart, open - nameStart);
        FrameType type = FrameType::Unknown;
        for (const auto& [name, t] : kFrameNames) {
            if (candidate == name) {
                type = t;
                break;
            }
        }
        if (type != FrameType::Unknown) {
            if (const auto close = findClose(text, open)) {
                Frame frame;
                frame.type = type;
                frame.fields = parseFields(std::string_view(text).substr(open + 1, *close - open - 1));
                return frame;
            }
        }
        at = text.find(kMarker, nameStart);
    }
    return {};
}

}
