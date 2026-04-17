#include "dependency.h"

#include <unordered_set>

namespace afis {
namespace {

void AddIfIdentifier(std::vector<std::string>& out, const std::string& token) {
    if (IsIdentifier(token)) {
        out.push_back(token);
    }
}

bool Intersects(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    std::unordered_set<std::string> setA(a.begin(), a.end());
    for (const std::string& token : b) {
        if (setA.find(token) != setA.end()) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> ReadSet(const Instruction& inst) {
    std::vector<std::string> reads;
    switch (inst.op) {
        case OpCode::Label:
            break;
        case OpCode::Assign:
            AddIfIdentifier(reads, inst.arg1);
            break;
        case OpCode::Binary:
            AddIfIdentifier(reads, inst.arg1);
            AddIfIdentifier(reads, inst.arg2);
            break;
        case OpCode::UnaryNeg:
            AddIfIdentifier(reads, inst.arg1);
            break;
        case OpCode::Load:
            AddIfIdentifier(reads, inst.arg1);
            break;
        case OpCode::Store:
            AddIfIdentifier(reads, inst.arg1);
            AddIfIdentifier(reads, inst.arg2);
            break;
        case OpCode::Print:
            AddIfIdentifier(reads, inst.arg1);
            break;
        case OpCode::Call:
            for (const std::string& arg : inst.callArgs) {
                AddIfIdentifier(reads, arg);
            }
            break;
        case OpCode::Goto:
            break;
        case OpCode::IfGoto:
            AddIfIdentifier(reads, inst.arg1);
            if (!inst.opSymbol.empty()) {
                AddIfIdentifier(reads, inst.arg2);
            }
            break;
        case OpCode::Nop:
            break;
    }
    return reads;
}

std::vector<std::string> WriteSet(const Instruction& inst) {
    std::vector<std::string> writes;
    if (inst.hasDest) {
        AddIfIdentifier(writes, inst.dest);
    }
    return writes;
}

bool IsSideEffectOperation(const Instruction& inst) {
    switch (inst.op) {
        case OpCode::Load:
        case OpCode::Store:
        case OpCode::Print:
        case OpCode::Call:
            return true;
        case OpCode::Label:
        case OpCode::Assign:
        case OpCode::Binary:
        case OpCode::UnaryNeg:
        case OpCode::Goto:
        case OpCode::IfGoto:
        case OpCode::Nop:
            return false;
    }
    return false;
}

bool IsSideEffectingBarrier(const Instruction& inst) {
    switch (inst.op) {
        case OpCode::Label:
        case OpCode::Goto:
        case OpCode::IfGoto:
            return true;
        case OpCode::Load:
        case OpCode::Store:
        case OpCode::Print:
        case OpCode::Call:
            return true;
        case OpCode::Assign:
        case OpCode::Binary:
        case OpCode::UnaryNeg:
        case OpCode::Nop:
            return false;
    }
    return false;
}

DependencyGraph BuildDependencyGraph(const Program& program) {
    const std::size_t n = program.instructions.size();
    DependencyGraph graph;
    graph.edges.assign(n, {});
    graph.indegree.assign(n, 0);

    std::vector<std::vector<std::string>> reads(n);
    std::vector<std::vector<std::string>> writes(n);
    std::vector<bool> barrier(n, false);

    for (std::size_t i = 0; i < n; ++i) {
        reads[i] = ReadSet(program.instructions[i]);
        writes[i] = WriteSet(program.instructions[i]);
        barrier[i] = IsSideEffectingBarrier(program.instructions[i]);
    }

    std::vector<std::vector<bool>> hasEdge(n, std::vector<bool>(n, false));

    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            bool needsEdge = false;

            // Data hazards: RAW, WAR, WAW.
            if (Intersects(writes[i], reads[j]) ||
                Intersects(reads[i], writes[j]) ||
                Intersects(writes[i], writes[j])) {
                needsEdge = true;
            }

            // Conservative side-effect barrier: no motion across memory/I/O/call instructions.
            if (barrier[i] || barrier[j]) {
                needsEdge = true;
            }

            if (needsEdge && !hasEdge[i][j]) {
                hasEdge[i][j] = true;
                graph.edges[i].push_back(j);
                graph.indegree[j] += 1;
            }
        }
    }

    return graph;
}

}  // namespace afis