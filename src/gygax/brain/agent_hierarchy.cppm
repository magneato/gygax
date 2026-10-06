module;
#include <format>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gygax/core/log.hpp>

export module gygax.brain.hierarchy;

export namespace gygax::brain {

class MotherAgent {
public:
    [[nodiscard]] std::string getDirective() const {
        return "Create a webpage inspired by the movie TRON. It must contain an interactive 2.5D Robotron-like game "
               "using the Daggorath-style 3D neon vector graphics engine, supporting z-axis shooting across depth layers. "
               "The layout must use glowing cyan, magenta, and green borders with retro-grid lines.";
    }

    [[nodiscard]] std::vector<std::string> requiredMarkers() const { return {"background-color", "#00ffcc", "#ff00ff", "canvas"}; }
};

class FatherAgent {
public:
    std::string superviseLayout(const std::string& rawHtml, const MotherAgent& mother) {
        std::string checked = rawHtml;
        std::vector<std::string> missing;
        for (const auto& marker : mother.requiredMarkers()) {
            if (checked.find(marker) == std::string::npos) missing.push_back(marker);
        }
        if (!missing.empty()) {
            std::string patch = "<style>";
            for (const auto& m : missing) {
                if (m == "background-color") patch += "body{background-color:#020208}";
            }
            patch += "</style>";
            if (const auto head = checked.find("</head>"); head != std::string::npos) checked.insert(head, patch);
            log::warn("father", "layout missing {} required markers; patched", missing.size());
        }
        return checked;
    }

    std::string superviseContent(const std::vector<std::string>& sections, const MotherAgent&) {
        std::ostringstream out;
        out << "<div class='instructions-container'><h2>SYSTEM PROTOCOLS</h2>";
        for (const auto& section : sections) out << "<div class='protocol-section'>" << section << "</div>";
        out << "</div>";
        return out.str();
    }
};

class ChildLayoutAgent {
public:
    [[nodiscard]] std::string generateHTML() const {
        return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Gygax Grid Portal</title>
<style>
body{background-color:#020208;color:#00ffcc;font-family:'Courier New',monospace;margin:0;padding:20px;display:flex;flex-direction:column;align-items:center}
h1{color:#ff00ff;text-shadow:0 0 10px #ff00ff;text-align:center}
.main-container{display:flex;gap:25px;width:100%;max-width:1200px;justify-content:center;flex-wrap:wrap}
.game-panel{border:2px solid #ff00ff;box-shadow:0 0 15px #ff00ff;background-color:#050512;padding:10px;border-radius:8px}
canvas{background-color:#000;display:block;border:1px solid #00ffcc}
.sidebar{width:320px;display:flex;flex-direction:column;gap:15px}
.info-box{border:2px solid #00ffcc;box-shadow:0 0 10px #00ffcc;background-color:#051212;padding:15px;border-radius:8px;font-size:.9em}
.info-box h2,.info-box h3{color:#ffff00;margin-top:0;border-bottom:1px dashed #ffff00;padding-bottom:5px}
.protocol-section{border-left:3px solid #ff00ff;padding-left:10px;margin-bottom:15px}
</style>
</head>
<body>
<h1>GYGAX // GRID PORTAL</h1>
<div class="main-container">
<div class="game-panel"><canvas id="gameCanvas" width="600" height="600"></canvas></div>
<div class="sidebar">
<div class="info-box" id="agent-text-content"><!-- Injected text content --></div>
</div>
</div>
<script>
const c=document.getElementById('gameCanvas'),g=c.getContext('2d');
let t=0;
function frame(){g.fillStyle='#000';g.fillRect(0,0,600,600);g.strokeStyle='#00ffcc';
for(let i=0;i<12;i++){const d=(i+(t%1))/12,o=300*d;g.strokeRect(300-o,300-o,2*o,2*o);}
t+=0.01;requestAnimationFrame(frame);}
frame();
</script>
</body>
</html>
)HTML";
    }
};

class ChildContentAgent {
public:
    [[nodiscard]] std::vector<std::string> gatherSections() const {
        return {
            "<h3>HIERARCHY</h3><p>Mother sets the directive, Father supervises, Children produce layout and content.</p>",
            "<h3>CONTROLS</h3><p>The grid renders continuously; the agent swarm regenerates it on each directive.</p>",
        };
    }
};

std::pair<std::string, std::string> CoordinateWebPage() {
    MotherAgent mother;
    FatherAgent father;
    ChildLayoutAgent layoutChild;
    ChildContentAgent contentChild;

    std::string html = father.superviseLayout(layoutChild.generateHTML(), mother);
    const std::string content = father.superviseContent(contentChild.gatherSections(), mother);
    const std::string target = "<!-- Injected text content -->";
    if (const auto at = html.find(target); at != std::string::npos) html.replace(at, target.size(), content);
    log::debug("hierarchy", "constructed page: {} bytes", html.size());
    return {std::move(html), content};
}

}
