#include "diagram.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <queue>
#include <sstream>
#include <vector>

namespace afis {
namespace {

std::string EscapeDot(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\' || ch == '"' || ch == '{' || ch == '}') {
            out.push_back('\\');
        }
        if (ch == '\n') {
            out += "\\l";
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::string EscapeXml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 32);
    for (char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    if (lines.empty()) {
        lines.push_back(text);
    }
    return lines;
}

bool PrepareParentPath(const std::string& path, std::string& error) {
    try {
        std::filesystem::path output(path);
        std::filesystem::path parent = output.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Could not create diagram directory: ") + ex.what();
        return false;
    } catch (...) {
        error = "Could not create diagram directory.";
        return false;
    }
}

std::string DependencyEdgeReason(const Instruction& first, const Instruction& second) {
    std::vector<std::string> labels;
    const std::vector<std::string> readFirst = ReadSet(first);
    const std::vector<std::string> writeFirst = WriteSet(first);
    const std::vector<std::string> readSecond = ReadSet(second);
    const std::vector<std::string> writeSecond = WriteSet(second);

    auto sharesToken = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
        for (const std::string& left : a) {
            for (const std::string& right : b) {
                if (left == right) {
                    return true;
                }
            }
        }
        return false;
    };

    if (sharesToken(writeFirst, readSecond)) {
        labels.push_back("RAW");
    }
    if (sharesToken(readFirst, writeSecond)) {
        labels.push_back("WAR");
    }
    if (sharesToken(writeFirst, writeSecond)) {
        labels.push_back("WAW");
    }
    if (IsSideEffectingBarrier(first) || IsSideEffectingBarrier(second)) {
        labels.push_back("BARRIER");
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            out << "/";
        }
        out << labels[i];
    }
    return out.str();
}

struct EdgeInfo {
    std::size_t from = 0;
    std::size_t to = 0;
    std::string reason;
};

std::vector<EdgeInfo> BuildReducedDependencyEdges(const Program& program,
                                                 const DependencyGraph& graph) {
    std::vector<EdgeInfo> candidates;
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        for (std::size_t target : graph.edges[i]) {
            candidates.push_back(
                {i, target, DependencyEdgeReason(program.instructions[i], program.instructions[target])});
        }
    }

    std::vector<std::vector<std::size_t>> bySource(program.instructions.size());
    for (std::size_t idx = 0; idx < candidates.size(); ++idx) {
        bySource[candidates[idx].from].push_back(idx);
    }

    auto canReachWithoutEdge =
        [&](std::size_t start, std::size_t goal, std::size_t skipped) -> bool {
            std::vector<bool> visited(program.instructions.size(), false);
            std::queue<std::size_t> pending;
            pending.push(start);
            visited[start] = true;
            while (!pending.empty()) {
                const std::size_t node = pending.front();
                pending.pop();
                for (std::size_t edgeIdx : bySource[node]) {
                    if (edgeIdx == skipped) {
                        continue;
                    }
                    const std::size_t next = candidates[edgeIdx].to;
                    if (next == goal) {
                        return true;
                    }
                    if (!visited[next]) {
                        visited[next] = true;
                        pending.push(next);
                    }
                }
            }
            return false;
        };

    std::vector<EdgeInfo> reduced;
    for (std::size_t idx = 0; idx < candidates.size(); ++idx) {
        const bool pureBarrier = (candidates[idx].reason == "BARRIER");
        if (!pureBarrier || !canReachWithoutEdge(candidates[idx].from, candidates[idx].to, idx)) {
            reduced.push_back(candidates[idx]);
        }
    }
    return reduced;
}

}  // namespace

bool WriteCfgDot(const CFG& cfg, const std::string& path, std::string& error) {
    error.clear();
    if (!PrepareParentPath(path, error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not open CFG dot output file: " + path;
        return false;
    }

    out << "digraph AFIS_CFG {\n";
    out << "  rankdir=TB;\n";
    out << "  node [shape=box, fontname=\"Consolas\"];\n";

    for (const BasicBlock& block : cfg.blocks) {
        std::ostringstream label;
        label << "Block " << block.id;
        if (!block.label.empty()) {
            label << " (" << block.label << ")";
        }
        label << "\\l";
        for (const Instruction& inst : block.instructions) {
            label << InstructionToString(inst) << "\\l";
        }
        out << "  B" << block.id << " [label=\"" << EscapeDot(label.str()) << "\"];\n";
    }

    for (const BasicBlock& block : cfg.blocks) {
        for (std::size_t successor : block.successors) {
            out << "  B" << block.id << " -> B" << successor;
            if (block.fallthroughSuccessor >= 0 &&
                static_cast<std::size_t>(block.fallthroughSuccessor) == successor) {
                out << " [label=\"fallthrough\"]";
            }
            out << ";\n";
        }
    }

    out << "}\n";
    return true;
}

std::string BuildCfgSvgMarkup(const CFG& cfg) {
    const int width = 1200;
    const int margin = 40;
    const int boxWidth = 340;
    const int minBoxHeight = 110;
    const int rowGap = 80;
    const int colGap = 80;

    struct BoxInfo {
        int x = 0;
        int y = 0;
        int w = boxWidth;
        int h = minBoxHeight;
    };

    const std::size_t blockCount = cfg.blocks.size();
    std::vector<int> level(blockCount, std::numeric_limits<int>::max());
    if (blockCount > 0) {
        std::queue<std::size_t> pending;
        level[0] = 0;
        pending.push(0);
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            for (std::size_t succ : cfg.blocks[current].successors) {
                if (succ >= blockCount) {
                    continue;
                }
                if (level[succ] == std::numeric_limits<int>::max()) {
                    level[succ] = level[current] + 1;
                    pending.push(succ);
                }
            }
        }
    }
    for (int& value : level) {
        if (value == std::numeric_limits<int>::max()) {
            value = 0;
        }
    }

    int maxLevel = 0;
    for (int value : level) {
        maxLevel = std::max(maxLevel, value);
    }

    std::vector<std::vector<std::size_t>> levelGroups(static_cast<std::size_t>(maxLevel + 1));
    for (std::size_t i = 0; i < blockCount; ++i) {
        levelGroups[static_cast<std::size_t>(level[i])].push_back(i);
    }

    std::vector<BoxInfo> boxes(blockCount);
    int currentY = margin + 24;
    for (std::size_t levelIdx = 0; levelIdx < levelGroups.size(); ++levelIdx) {
        int rowHeight = minBoxHeight;
        for (std::size_t blockIdx : levelGroups[levelIdx]) {
            rowHeight = std::max(rowHeight,
                                 78 + static_cast<int>(cfg.blocks[blockIdx].instructions.size()) * 18);
        }

        const int count = static_cast<int>(levelGroups[levelIdx].size());
        const int totalWidth = count * boxWidth + std::max(0, count - 1) * colGap;
        const int startX = (width - totalWidth) / 2;
        for (int col = 0; col < count; ++col) {
            const std::size_t blockIdx = levelGroups[levelIdx][static_cast<std::size_t>(col)];
            boxes[blockIdx].x = startX + col * (boxWidth + colGap);
            boxes[blockIdx].y = currentY;
            boxes[blockIdx].h =
                std::max(minBoxHeight, 78 + static_cast<int>(cfg.blocks[blockIdx].instructions.size()) * 18);
        }
        currentY += rowHeight + rowGap;
    }
    const int height = currentY + margin;

    std::ostringstream out;
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\">";
    out << "<defs>"
        << "<linearGradient id=\"cfgBox\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
        << "<stop offset=\"0%\" stop-color=\"#fff5dd\"/>"
        << "<stop offset=\"100%\" stop-color=\"#ffe4ad\"/>"
        << "</linearGradient>"
        << "<marker id=\"cfgArrow\" viewBox=\"0 0 10 10\" refX=\"7\" refY=\"5\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\">"
        << "<path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"#7a4d00\"/>"
        << "</marker>"
        << "</defs>";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#fffdf7\" rx=\"18\" ry=\"18\"/>";
    out << "<text x=\"40\" y=\"28\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"22\" font-weight=\"700\" fill=\"#6b4500\">Control Flow Graph</text>";

    for (std::size_t i = 0; i < cfg.blocks.size(); ++i) {
        const BasicBlock& block = cfg.blocks[i];
        const BoxInfo& box = boxes[i];
        out << "<rect x=\"" << box.x << "\" y=\"" << box.y << "\" width=\"" << box.w
            << "\" height=\"" << box.h
            << "\" rx=\"16\" ry=\"16\" fill=\"url(#cfgBox)\" stroke=\"#7a4d00\" stroke-width=\"2\"/>";
        std::ostringstream title;
        title << "B" << block.id;
        if (!block.label.empty()) {
            title << "  (" << block.label << ")";
        }
        out << "<text x=\"" << (box.x + 18) << "\" y=\"" << (box.y + 28)
            << "\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"20\" font-weight=\"700\" fill=\"#5a3900\">"
            << EscapeXml(title.str()) << "</text>";
        int textY = box.y + 56;
        for (const Instruction& inst : block.instructions) {
            out << "<text x=\"" << (box.x + 18) << "\" y=\"" << textY
                << "\" font-family=\"Consolas, monospace\" font-size=\"15\" fill=\"#6d4f18\">"
                << EscapeXml(InstructionToString(inst)) << "</text>";
            textY += 18;
        }
    }

    std::size_t routingLane = 0;
    for (std::size_t i = 0; i < cfg.blocks.size(); ++i) {
        const BasicBlock& block = cfg.blocks[i];
        const BoxInfo& from = boxes[i];
        for (std::size_t successor : block.successors) {
            const BoxInfo& to = boxes[successor];
            const bool forward = level[successor] > level[i];
            const int fromCx = from.x + from.w / 2;
            const int toCx = to.x + to.w / 2;
            const int fromBottom = from.y + from.h;
            const int toTop = to.y;
            if (forward) {
                const int midY = fromBottom + (toTop - fromBottom) / 2;
                out << "<path d=\"M " << fromCx << " " << fromBottom
                    << " L " << fromCx << " " << midY
                    << " L " << toCx << " " << midY
                    << " L " << toCx << " " << toTop
                    << "\" fill=\"none\" stroke=\"#7a4d00\" stroke-width=\"3\" marker-end=\"url(#cfgArrow)\"/>";
            } else {
                const int laneX = width - margin - 40 - static_cast<int>((routingLane % 5) * 28);
                const int fromY = from.y + from.h / 2;
                const int toY = to.y + to.h / 2;
                out << "<path d=\"M " << (from.x + from.w) << " " << fromY
                    << " L " << laneX << " " << fromY
                    << " L " << laneX << " " << toY
                    << " L " << to.x << " " << toY
                    << "\" fill=\"none\" stroke=\"#7a4d00\" stroke-width=\"3\" marker-end=\"url(#cfgArrow)\"/>";
                routingLane += 1;
            }
            std::string edgeLabel = "";
            if (block.fallthroughSuccessor >= 0 &&
                static_cast<std::size_t>(block.fallthroughSuccessor) == successor) {
                edgeLabel = "fallthrough";
            }
            if (!edgeLabel.empty()) {
                int lx = (fromCx + toCx) / 2;
                int ly = (fromBottom + toTop) / 2 - 8;
                out << "<text x=\"" << lx << "\" y=\"" << ly
                    << "\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"13\" fill=\"#7a4d00\" text-anchor=\"middle\">"
                    << EscapeXml(edgeLabel) << "</text>";
            }
        }
    }

    out << "</svg>";
    return out.str();
}

bool WriteCfgSvg(const CFG& cfg, const std::string& path, std::string& error) {
    error.clear();
    if (!PrepareParentPath(path, error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not open CFG SVG output file: " + path;
        return false;
    }
    out << BuildCfgSvgMarkup(cfg);
    return true;
}

bool WriteDependencyDot(const Program& program,
                        const DependencyGraph& graph,
                        const std::string& path,
                        std::string& error) {
    error.clear();
    if (!PrepareParentPath(path, error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not open dependency dot output file: " + path;
        return false;
    }

    out << "digraph AFIS_DEP {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box, fontname=\"Consolas\"];\n";

    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        std::ostringstream label;
        label << "[" << i << "] " << InstructionToString(program.instructions[i]);
        out << "  N" << i << " [label=\"" << EscapeDot(label.str()) << "\"";
        if (IsSideEffectingBarrier(program.instructions[i])) {
            out << ", style=filled, fillcolor=\"lightgoldenrod1\"";
        }
        out << "];\n";
    }

    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        for (std::size_t target : graph.edges[i]) {
            out << "  N" << i << " -> N" << target;
            std::string label = DependencyEdgeReason(program.instructions[i], program.instructions[target]);
            if (!label.empty()) {
                out << " [label=\"" << EscapeDot(label) << "\"]";
            }
            out << ";\n";
        }
    }

    out << "}\n";
    return true;
}

std::string BuildDependencySvgMarkup(const Program& program, const DependencyGraph& graph) {
    const int margin = 50;
    const int boxWidth = 720;
    const int boxHeight = 70;
    const int rowGap = 34;
    const std::vector<EdgeInfo> reducedEdges = BuildReducedDependencyEdges(program, graph);

    std::size_t laneCount = 0;
    for (const EdgeInfo& edge : reducedEdges) {
        if (edge.to > edge.from + 1) {
            laneCount += 1;
        }
    }
    if (laneCount == 0) {
        laneCount = 1;
    }
    const int laneStartX = margin + boxWidth + 70;
    const int laneGap = 28;
    const int width = laneStartX + static_cast<int>(laneCount) * laneGap + 110;
    const int rows = static_cast<int>(program.instructions.size());
    const int height = margin * 2 + std::max(1, rows) * boxHeight + std::max(0, rows - 1) * rowGap + 20;

    struct BoxInfo {
        int x = 0;
        int y = 0;
    };
    std::vector<BoxInfo> boxes(program.instructions.size());
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        int row = static_cast<int>(i);
        boxes[i].x = margin;
        boxes[i].y = margin + row * (boxHeight + rowGap);
    }

    std::ostringstream out;
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\">";
    out << "<defs>"
        << "<linearGradient id=\"depBox\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
        << "<stop offset=\"0%\" stop-color=\"#eefcf0\"/>"
        << "<stop offset=\"100%\" stop-color=\"#d6f4da\"/>"
        << "</linearGradient>"
        << "<marker id=\"depArrow\" viewBox=\"0 0 10 10\" refX=\"7\" refY=\"5\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\">"
        << "<path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"#1d6b2c\"/>"
        << "</marker>"
        << "</defs>";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#f8fff9\" rx=\"18\" ry=\"18\"/>";
    out << "<text x=\"40\" y=\"28\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"22\" font-weight=\"700\" fill=\"#165225\">Dependency Graph</text>";

    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const BoxInfo& box = boxes[i];
        bool barrier = IsSideEffectingBarrier(program.instructions[i]);
        out << "<rect x=\"" << box.x << "\" y=\"" << box.y << "\" width=\"" << boxWidth
            << "\" height=\"" << boxHeight
            << "\" rx=\"14\" ry=\"14\" fill=\"" << (barrier ? "#fff1c7" : "url(#depBox)")
            << "\" stroke=\"#1d6b2c\" stroke-width=\"2\"/>";
        std::ostringstream title;
        title << "[" << i << "]";
        out << "<text x=\"" << (box.x + 16) << "\" y=\"" << (box.y + 28)
            << "\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"18\" font-weight=\"700\" fill=\"#165225\">"
            << EscapeXml(title.str()) << "</text>";
        out << "<text x=\"" << (box.x + 16) << "\" y=\"" << (box.y + 52)
            << "\" font-family=\"Consolas, monospace\" font-size=\"15\" fill=\"#2b5d35\">"
            << EscapeXml(InstructionToString(program.instructions[i])) << "</text>";
    }

    std::size_t laneIndex = 0;
    for (const EdgeInfo& edge : reducedEdges) {
        const BoxInfo& from = boxes[edge.from];
        const BoxInfo& to = boxes[edge.to];
        const bool pureBarrier = (edge.reason == "BARRIER");
        const std::string dash = pureBarrier ? " stroke-dasharray=\"7 5\"" : "";

        if (edge.to == edge.from + 1) {
            const int x = from.x + boxWidth / 2;
            const int y1 = from.y + boxHeight;
            const int y2 = to.y;
            out << "<path d=\"M " << x << " " << y1
                << " L " << x << " " << y2
                << "\" fill=\"none\" stroke=\"#1d6b2c\" stroke-width=\"2.5\"" << dash
                << " marker-end=\"url(#depArrow)\"/>";
            if (!pureBarrier && !edge.reason.empty()) {
                out << "<text x=\"" << (x + 14) << "\" y=\"" << ((y1 + y2) / 2 - 4)
                    << "\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"12\" fill=\"#165225\">"
                    << EscapeXml(edge.reason) << "</text>";
            }
        } else {
            const int laneX = laneStartX + static_cast<int>(laneIndex++) * laneGap;
            const int fromY = from.y + boxHeight / 2;
            const int toY = to.y + boxHeight / 2;
            out << "<path d=\"M " << (from.x + boxWidth) << " " << fromY
                << " L " << laneX << " " << fromY
                << " L " << laneX << " " << toY
                << " L " << to.x << " " << toY
                << "\" fill=\"none\" stroke=\"#1d6b2c\" stroke-width=\"2.5\"" << dash
                << " marker-end=\"url(#depArrow)\"/>";
            if (!pureBarrier && !edge.reason.empty()) {
                out << "<text x=\"" << (laneX + 8) << "\" y=\"" << (fromY + 14)
                    << "\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"12\" fill=\"#165225\">"
                    << EscapeXml(edge.reason) << "</text>";
            }
        }
    }

    out << "</svg>";
    return out.str();
}

bool WriteDependencySvg(const Program& program,
                        const DependencyGraph& graph,
                        const std::string& path,
                        std::string& error) {
    error.clear();
    if (!PrepareParentPath(path, error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not open dependency SVG output file: " + path;
        return false;
    }
    out << BuildDependencySvgMarkup(program, graph);
    return true;
}

bool WriteWorkflowDot(const std::vector<WorkflowStepDiagram>& steps,
                      const std::string& path,
                      std::string& error) {
    error.clear();
    if (!PrepareParentPath(path, error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not open workflow dot output file: " + path;
        return false;
    }

    out << "digraph AFIS_WORKFLOW {\n";
    out << "  rankdir=LR;\n";
    out << "  graph [fontname=\"Consolas\"];\n";
    out << "  node [shape=box, style=\"rounded,filled\", fillcolor=\"lightcyan\", fontname=\"Consolas\"];\n";
    out << "  edge [fontname=\"Consolas\"];\n";

    for (std::size_t i = 0; i < steps.size(); ++i) {
        std::ostringstream label;
        label << "Step " << (i + 1) << ": " << steps[i].title;
        if (!steps[i].detail.empty()) {
            label << "\\l" << steps[i].detail << "\\l";
        }
        out << "  S" << i << " [label=\"" << EscapeDot(label.str()) << "\"];\n";
    }

    for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
        out << "  S" << i << " -> S" << (i + 1) << ";\n";
    }

    out << "}\n";
    return true;
}

std::string BuildWorkflowSvgMarkup(const std::vector<WorkflowStepDiagram>& steps) {
    const int width = 980;
    const int marginX = 60;
    const int marginY = 40;
    const int boxWidth = 860;
    const int boxHeight = 96;
    const int gap = 42;
    const int arrowGap = 8;
    const int totalHeight = marginY * 2 + static_cast<int>(steps.size()) * boxHeight +
                            static_cast<int>(steps.size() > 0 ? steps.size() - 1 : 0) * gap;

    std::ostringstream out;
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\""
        << totalHeight << "\" viewBox=\"0 0 " << width << " " << totalHeight << "\">";
    out << "<defs>"
        << "<linearGradient id=\"afisBox\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
        << "<stop offset=\"0%\" stop-color=\"#edf7ff\"/>"
        << "<stop offset=\"100%\" stop-color=\"#d7ebff\"/>"
        << "</linearGradient>"
        << "<marker id=\"arrow\" viewBox=\"0 0 10 10\" refX=\"7\" refY=\"5\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\">"
        << "<path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"#2d5f8b\"/>"
        << "</marker>"
        << "</defs>";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#f7fbff\" rx=\"18\" ry=\"18\"/>";

    for (std::size_t i = 0; i < steps.size(); ++i) {
        const int x = marginX;
        const int y = marginY + static_cast<int>(i) * (boxHeight + gap);
        out << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << boxWidth
            << "\" height=\"" << boxHeight
            << "\" rx=\"16\" ry=\"16\" fill=\"url(#afisBox)\" stroke=\"#2d5f8b\" stroke-width=\"2\"/>";
        out << "<text x=\"" << (x + 22) << "\" y=\"" << (y + 30)
            << "\" font-family=\"Segoe UI, Arial, sans-serif\" font-size=\"22\" font-weight=\"700\" fill=\"#10395c\">"
            << EscapeXml("Step " + std::to_string(i + 1) + ": " + steps[i].title) << "</text>";

        std::vector<std::string> detailLines = SplitLines(steps[i].detail);
        int textY = y + 58;
        for (const std::string& line : detailLines) {
            if (line.empty()) {
                continue;
            }
            out << "<text x=\"" << (x + 22) << "\" y=\"" << textY
                << "\" font-family=\"Consolas, monospace\" font-size=\"15\" fill=\"#244a6c\">"
                << EscapeXml(line) << "</text>";
            textY += 18;
        }

        if (i + 1 < steps.size()) {
            const int lineX = x + boxWidth / 2;
            const int lineY1 = y + boxHeight + arrowGap;
            const int lineY2 = y + boxHeight + gap - arrowGap;
            out << "<line x1=\"" << lineX << "\" y1=\"" << lineY1 << "\" x2=\"" << lineX
                << "\" y2=\"" << lineY2
                << "\" stroke=\"#2d5f8b\" stroke-width=\"3\" marker-end=\"url(#arrow)\"/>";
        }
    }

    out << "</svg>";
    return out.str();
}

bool WriteWorkflowSvg(const std::vector<WorkflowStepDiagram>& steps,
                      const std::string& path,
                      std::string& error) {
    error.clear();
    if (!PrepareParentPath(path, error)) {
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not open workflow SVG output file: " + path;
        return false;
    }

    out << BuildWorkflowSvgMarkup(steps);
    return true;
}

}  // namespace afis
