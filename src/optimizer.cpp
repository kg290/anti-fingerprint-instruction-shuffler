#include "optimizer.h"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cfg.h"
#include "dependency.h"

namespace afis {
namespace {

long long EvaluateBinary(long long left, long long right, const std::string& op, bool& ok) {
    ok = true;
    if (op == "+") {
        return left + right;
    }
    if (op == "-") {
        return left - right;
    }
    if (op == "*") {
        return left * right;
    }
    if (op == "/") {
        if (right == 0) {
            ok = false;
            return 0;
        }
        return left / right;
    }
    if (op == "%") {
        if (right == 0) {
            ok = false;
            return 0;
        }
        return left % right;
    }
    if (op == "&") {
        return left & right;
    }
    if (op == "|") {
        return left | right;
    }
    if (op == "^") {
        return left ^ right;
    }
    ok = false;
    return 0;
}

std::string ToIntString(long long value) {
    return std::to_string(value);
}

bool TryParseInt(const std::string& token, long long& value) {
    if (!IsInteger(token)) {
        return false;
    }
    try {
        value = std::stoll(token);
        return true;
    } catch (...) {
        return false;
    }
}

void RecordChange(OptimizationTrace& trace,
                  const std::string& passName,
                  const std::string& before,
                  const std::string& after,
                  const std::string& note,
                  bool removed = false) {
    if (before == after && !removed) {
        return;
    }
    trace.changes.push_back(OptimizationChange{passName, before, after, note, removed});
}

bool IsBlockBoundary(const Instruction& inst) {
    return inst.op == OpCode::Label || inst.op == OpCode::Goto || inst.op == OpCode::IfGoto;
}

struct BlockRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<BlockRange> ComputeOptimizationRanges(const Program& program) {
    std::vector<BlockRange> ranges;
    if (HasExplicitControlFlow(program)) {
        const std::size_t n = program.instructions.size();
        std::unordered_map<std::string, std::size_t> labelToInst;
        for (std::size_t i = 0; i < n; ++i) {
            if (program.instructions[i].op == OpCode::Label) {
                labelToInst[program.instructions[i].label] = i;
            }
        }

        std::set<std::size_t> leaderSet;
        leaderSet.insert(0);
        for (std::size_t i = 0; i < n; ++i) {
            const Instruction& inst = program.instructions[i];
            if (inst.op == OpCode::Label) {
                leaderSet.insert(i);
            }
            if (inst.op == OpCode::Goto || inst.op == OpCode::IfGoto) {
                if (i + 1 < n) {
                    leaderSet.insert(i + 1);
                }
                auto it = labelToInst.find(inst.label);
                if (it != labelToInst.end()) {
                    leaderSet.insert(it->second);
                }
            }
        }

        std::vector<std::size_t> leaders(leaderSet.begin(), leaderSet.end());
        for (std::size_t i = 0; i < leaders.size(); ++i) {
            std::size_t begin = leaders[i];
            std::size_t end = (i + 1 < leaders.size()) ? leaders[i + 1] : n;
            ranges.push_back(BlockRange{begin, end});
        }
        return ranges;
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        if (i != start && IsBlockBoundary(program.instructions[i])) {
            ranges.push_back(BlockRange{start, i});
            start = i;
        }
        if (program.instructions[i].op == OpCode::Goto || program.instructions[i].op == OpCode::IfGoto) {
            ranges.push_back(BlockRange{start, i + 1});
            start = i + 1;
        }
    }
    if (start < program.instructions.size()) {
        ranges.push_back(BlockRange{start, program.instructions.size()});
    }
    if (ranges.empty()) {
        ranges.push_back(BlockRange{0, program.instructions.size()});
    }
    return ranges;
}

template <typename Fn>
Program TransformByRange(const Program& program, Fn&& transformRange) {
    Program out;
    out.instructions.reserve(program.instructions.size());
    std::vector<BlockRange> ranges = ComputeOptimizationRanges(program);
    for (const BlockRange& range : ranges) {
        transformRange(program, range, out);
    }
    return out;
}

std::string ReplaceIfConstant(const std::string& token,
                              const std::unordered_map<std::string, long long>& constants,
                              bool& changed) {
    if (!IsIdentifier(token)) {
        return token;
    }
    auto it = constants.find(token);
    if (it == constants.end()) {
        return token;
    }
    changed = true;
    return ToIntString(it->second);
}

std::string ReplaceIfCopy(const std::string& token,
                          const std::unordered_map<std::string, std::string>& copies,
                          bool& changed) {
    if (!IsIdentifier(token)) {
        return token;
    }
    auto it = copies.find(token);
    if (it == copies.end()) {
        return token;
    }
    changed = true;
    return it->second;
}

void InvalidateCopyMappingsFor(std::unordered_map<std::string, std::string>& copies,
                               const std::string& symbol) {
    if (!IsIdentifier(symbol)) {
        return;
    }
    copies.erase(symbol);
    for (auto it = copies.begin(); it != copies.end();) {
        if (it->second == symbol) {
            it = copies.erase(it);
        } else {
            ++it;
        }
    }
}

std::set<std::string> ComputeExternalReadsForRange(const Program& program,
                                                   const BlockRange& current) {
    std::set<std::string> reads;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        if (i >= current.begin && i < current.end) {
            continue;
        }
        for (const std::string& token : ReadSet(program.instructions[i])) {
            reads.insert(token);
        }
    }
    return reads;
}

}  // namespace

Program ConstantFold(const Program& program, OptimizationTrace& trace) {
    return TransformByRange(program, [&trace](const Program& source, const BlockRange& range, Program& out) {
        for (std::size_t i = range.begin; i < range.end; ++i) {
            Instruction inst = source.instructions[i];
            const std::string before = InstructionToString(inst);

            if (inst.op == OpCode::UnaryNeg) {
                long long value = 0;
                if (TryParseInt(inst.arg1, value)) {
                    inst.op = OpCode::Assign;
                    inst.arg1 = ToIntString(-value);
                    inst.arg2.clear();
                    inst.opSymbol.clear();
                    trace.constantFolds += 1;
                    RecordChange(trace, "Constant Folding", before, InstructionToString(inst),
                                 "Folded unary negation into a constant assignment.");
                }
            } else if (inst.op == OpCode::Binary) {
                long long left = 0;
                long long right = 0;
                if (TryParseInt(inst.arg1, left) && TryParseInt(inst.arg2, right)) {
                    bool ok = false;
                    long long folded = EvaluateBinary(left, right, inst.opSymbol, ok);
                    if (ok) {
                        inst.op = OpCode::Assign;
                        inst.arg1 = ToIntString(folded);
                        inst.arg2.clear();
                        inst.opSymbol.clear();
                        trace.constantFolds += 1;
                        RecordChange(trace, "Constant Folding", before, InstructionToString(inst),
                                     "Folded binary arithmetic into a constant assignment.");
                    }
                }
            }

            out.instructions.push_back(std::move(inst));
        }
    });
}

Program ConstantPropagate(const Program& program, OptimizationTrace& trace) {
    return TransformByRange(program, [&trace](const Program& source, const BlockRange& range, Program& out) {
        std::unordered_map<std::string, long long> constants;

        for (std::size_t i = range.begin; i < range.end; ++i) {
            Instruction inst = source.instructions[i];
            const std::string before = InstructionToString(inst);
            bool changed = false;

            switch (inst.op) {
                case OpCode::Assign:
                    inst.arg1 = ReplaceIfConstant(inst.arg1, constants, changed);
                    break;
                case OpCode::UnaryNeg:
                    inst.arg1 = ReplaceIfConstant(inst.arg1, constants, changed);
                    break;
                case OpCode::Binary:
                    inst.arg1 = ReplaceIfConstant(inst.arg1, constants, changed);
                    inst.arg2 = ReplaceIfConstant(inst.arg2, constants, changed);
                    break;
                case OpCode::Store:
                    inst.arg2 = ReplaceIfConstant(inst.arg2, constants, changed);
                    break;
                case OpCode::Print:
                    inst.arg1 = ReplaceIfConstant(inst.arg1, constants, changed);
                    break;
                case OpCode::Call:
                    for (std::string& arg : inst.callArgs) {
                        arg = ReplaceIfConstant(arg, constants, changed);
                    }
                    break;
                case OpCode::IfGoto:
                    inst.arg1 = ReplaceIfConstant(inst.arg1, constants, changed);
                    if (!inst.opSymbol.empty()) {
                        inst.arg2 = ReplaceIfConstant(inst.arg2, constants, changed);
                    }
                    break;
                case OpCode::Load:
                case OpCode::Label:
                case OpCode::Goto:
                case OpCode::Nop:
                    break;
            }

            if (inst.op == OpCode::UnaryNeg) {
                long long value = 0;
                if (TryParseInt(inst.arg1, value)) {
                    inst.op = OpCode::Assign;
                    inst.arg1 = ToIntString(-value);
                    inst.arg2.clear();
                    inst.opSymbol.clear();
                    changed = true;
                }
            } else if (inst.op == OpCode::Binary) {
                long long left = 0;
                long long right = 0;
                if (TryParseInt(inst.arg1, left) && TryParseInt(inst.arg2, right)) {
                    bool ok = false;
                    long long folded = EvaluateBinary(left, right, inst.opSymbol, ok);
                    if (ok) {
                        inst.op = OpCode::Assign;
                        inst.arg1 = ToIntString(folded);
                        inst.arg2.clear();
                        inst.opSymbol.clear();
                        changed = true;
                    }
                }
            }

            if (changed) {
                trace.constantPropagations += 1;
                RecordChange(trace, "Constant Propagation", before, InstructionToString(inst),
                             "Propagated known constant values into later uses.");
            }

            if (inst.hasDest) {
                constants.erase(inst.dest);
            }

            if (inst.op == OpCode::Assign) {
                long long value = 0;
                if (TryParseInt(inst.arg1, value)) {
                    constants[inst.dest] = value;
                }
            } else if (inst.op == OpCode::UnaryNeg) {
                long long value = 0;
                if (TryParseInt(inst.arg1, value)) {
                    constants[inst.dest] = -value;
                }
            } else if (inst.op == OpCode::Binary) {
                long long left = 0;
                long long right = 0;
                if (TryParseInt(inst.arg1, left) && TryParseInt(inst.arg2, right)) {
                    bool ok = false;
                    long long folded = EvaluateBinary(left, right, inst.opSymbol, ok);
                    if (ok) {
                        constants[inst.dest] = folded;
                    }
                }
            }

            out.instructions.push_back(std::move(inst));
        }
    });
}

Program CopyPropagate(const Program& program, OptimizationTrace& trace) {
    return TransformByRange(program, [&trace](const Program& source, const BlockRange& range, Program& out) {
        std::unordered_map<std::string, std::string> copies;

        for (std::size_t i = range.begin; i < range.end; ++i) {
            Instruction inst = source.instructions[i];
            const std::string before = InstructionToString(inst);
            bool changed = false;

            switch (inst.op) {
                case OpCode::Assign:
                    inst.arg1 = ReplaceIfCopy(inst.arg1, copies, changed);
                    break;
                case OpCode::UnaryNeg:
                    inst.arg1 = ReplaceIfCopy(inst.arg1, copies, changed);
                    break;
                case OpCode::Binary:
                    inst.arg1 = ReplaceIfCopy(inst.arg1, copies, changed);
                    inst.arg2 = ReplaceIfCopy(inst.arg2, copies, changed);
                    break;
                case OpCode::Store:
                    inst.arg2 = ReplaceIfCopy(inst.arg2, copies, changed);
                    break;
                case OpCode::Print:
                    inst.arg1 = ReplaceIfCopy(inst.arg1, copies, changed);
                    break;
                case OpCode::Call:
                    for (std::string& arg : inst.callArgs) {
                        arg = ReplaceIfCopy(arg, copies, changed);
                    }
                    break;
                case OpCode::IfGoto:
                    inst.arg1 = ReplaceIfCopy(inst.arg1, copies, changed);
                    if (!inst.opSymbol.empty()) {
                        inst.arg2 = ReplaceIfCopy(inst.arg2, copies, changed);
                    }
                    break;
                case OpCode::Load:
                case OpCode::Label:
                case OpCode::Goto:
                case OpCode::Nop:
                    break;
            }

            if (changed) {
                trace.copyPropagations += 1;
                RecordChange(trace, "Copy Propagation", before, InstructionToString(inst),
                             "Reused an earlier variable instead of an unnecessary copy.");
            }

            if (inst.hasDest) {
                InvalidateCopyMappingsFor(copies, inst.dest);
            }

            if (inst.op == OpCode::Assign && IsIdentifier(inst.arg1) && IsIdentifier(inst.dest)) {
                std::string root = inst.arg1;
                auto it = copies.find(root);
                if (it != copies.end()) {
                    root = it->second;
                }
                copies[inst.dest] = root;
            }

            out.instructions.push_back(std::move(inst));
        }
    });
}

Program DeadCodeEliminate(const Program& program, OptimizationTrace& trace) {
    return TransformByRange(program, [&trace](const Program& source, const BlockRange& range, Program& out) {
        std::vector<Instruction> kept;
        kept.reserve(range.end - range.begin);
        std::set<std::string> used = ComputeExternalReadsForRange(source, range);

        for (std::size_t idx = range.end; idx > range.begin; --idx) {
            const Instruction& inst = source.instructions[idx - 1];
            const std::string before = InstructionToString(inst);
            bool keep = true;

            if (inst.op == OpCode::Assign || inst.op == OpCode::UnaryNeg || inst.op == OpCode::Binary) {
                if (inst.hasDest && used.find(inst.dest) == used.end()) {
                    keep = false;
                    trace.deadInstructionsRemoved += 1;
                    RecordChange(trace, "Dead Code Elimination", before, "",
                                 "Removed instruction because its result is not used later.", true);
                }
            } else if (inst.op == OpCode::Nop) {
                keep = false;
                trace.deadInstructionsRemoved += 1;
                RecordChange(trace, "Dead Code Elimination", before, "",
                             "Removed no-op instruction.", true);
            }

            if (!keep) {
                continue;
            }

            if (inst.hasDest) {
                used.erase(inst.dest);
            }
            for (const std::string& token : ReadSet(inst)) {
                used.insert(token);
            }

            kept.push_back(inst);
        }

        for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            out.instructions.push_back(*it);
        }
    });
}

OptimizationPipelineResult RunOptimizationPipeline(const Program& program) {
    OptimizationPipelineResult result;
    result.afterConstantFolding = ConstantFold(program, result.trace);
    result.afterConstantPropagation = ConstantPropagate(result.afterConstantFolding, result.trace);
    result.afterCopyPropagation = CopyPropagate(result.afterConstantPropagation, result.trace);
    result.afterDeadCodeElimination =
        DeadCodeEliminate(result.afterCopyPropagation, result.trace);
    return result;
}

}  // namespace afis
