#include "cfg.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace afis {
namespace {

void AppendSuccessor(BasicBlock& block, std::size_t successor) {
    if (std::find(block.successors.begin(), block.successors.end(), successor) == block.successors.end()) {
        block.successors.push_back(successor);
    }
}

int LastNonLabelIndex(const std::vector<Instruction>& instructions) {
    for (int i = static_cast<int>(instructions.size()) - 1; i >= 0; --i) {
        if (instructions[i].op != OpCode::Label) {
            return i;
        }
    }
    return -1;
}

std::string MakeGeneratedLabel(std::size_t blockId,
                               std::unordered_set<std::string>& usedLabels) {
    std::size_t attempt = 0;
    while (true) {
        std::string candidate = "__bb_" + std::to_string(blockId);
        if (attempt > 0) {
            candidate += "_" + std::to_string(attempt);
        }
        if (usedLabels.find(candidate) == usedLabels.end()) {
            usedLabels.insert(candidate);
            return candidate;
        }
        attempt += 1;
    }
}

}  // namespace

bool IsTerminator(const Instruction& inst) {
    return inst.op == OpCode::Goto || inst.op == OpCode::IfGoto;
}

bool HasExplicitControlFlow(const Program& program) {
    for (const Instruction& inst : program.instructions) {
        if (inst.op == OpCode::Label || inst.op == OpCode::Goto || inst.op == OpCode::IfGoto) {
            return true;
        }
    }
    return false;
}

BranchValidationResult ValidateBranches(const Program& program) {
    BranchValidationResult result;
    std::unordered_map<std::string, std::size_t> labelCounts;

    for (const Instruction& inst : program.instructions) {
        if (inst.op == OpCode::Label) {
            labelCounts[inst.label] += 1;
        }
    }

    std::vector<std::string> duplicateLabels;
    for (const auto& kv : labelCounts) {
        if (kv.second > 1) {
            result.duplicateLabelCount += 1;
            duplicateLabels.push_back(kv.first);
        }
    }

    std::vector<std::string> missingTargets;
    for (const Instruction& inst : program.instructions) {
        if (inst.op != OpCode::Goto && inst.op != OpCode::IfGoto) {
            continue;
        }
        if (labelCounts.find(inst.label) == labelCounts.end()) {
            result.missingTargetCount += 1;
            missingTargets.push_back(inst.label);
        }
    }

    result.success = (result.duplicateLabelCount == 0 && result.missingTargetCount == 0);
    if (!result.success) {
        std::ostringstream out;
        if (result.duplicateLabelCount > 0) {
            out << "Duplicate labels: ";
            for (std::size_t i = 0; i < duplicateLabels.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << duplicateLabels[i];
            }
            if (result.missingTargetCount > 0) {
                out << ". ";
            }
        }
        if (result.missingTargetCount > 0) {
            out << "Missing branch targets: ";
            for (std::size_t i = 0; i < missingTargets.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << missingTargets[i];
            }
            out << ".";
        }
        result.error = out.str();
    }

    return result;
}

bool BuildCFG(const Program& program, CFG& cfg, std::string& error) {
    cfg.blocks.clear();
    cfg.labelToBlock.clear();
    error.clear();

    if (program.instructions.empty()) {
        return true;
    }

    BranchValidationResult branchValidation = ValidateBranches(program);
    if (!branchValidation.success) {
        error = branchValidation.error;
        return false;
    }

    const std::size_t n = program.instructions.size();
    std::unordered_map<std::string, std::size_t> labelToInst;
    for (std::size_t i = 0; i < n; ++i) {
        const Instruction& inst = program.instructions[i];
        if (inst.op == OpCode::Label) {
            labelToInst[inst.label] = i;
        }
    }

    std::set<std::size_t> leaderSet;
    leaderSet.insert(0);

    for (std::size_t i = 0; i < n; ++i) {
        const Instruction& inst = program.instructions[i];

        if (inst.op == OpCode::Label) {
            leaderSet.insert(i);
        }

        if (IsTerminator(inst)) {
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
    std::vector<std::size_t> instructionToBlock(n, 0);

    for (std::size_t b = 0; b < leaders.size(); ++b) {
        std::size_t start = leaders[b];
        std::size_t end = (b + 1 < leaders.size()) ? leaders[b + 1] : n;

        BasicBlock block;
        block.id = b;
        block.instructions.reserve(end - start);
        for (std::size_t idx = start; idx < end; ++idx) {
            block.instructions.push_back(program.instructions[idx]);
            instructionToBlock[idx] = b;
        }

        cfg.blocks.push_back(std::move(block));
    }

    std::unordered_set<std::string> usedLabels;
    for (const Instruction& inst : program.instructions) {
        if (inst.op == OpCode::Label) {
            usedLabels.insert(inst.label);
        }
    }

    for (BasicBlock& block : cfg.blocks) {
        std::size_t firstNonLabel = 0;
        while (firstNonLabel < block.instructions.size() &&
               block.instructions[firstNonLabel].op == OpCode::Label) {
            if (block.label.empty()) {
                block.label = block.instructions[firstNonLabel].label;
            }
            firstNonLabel += 1;
        }

        if (!block.label.empty()) {
            continue;
        }

        Instruction generated;
        generated.op = OpCode::Label;
        generated.label = MakeGeneratedLabel(block.id, usedLabels);
        generated.originalIndex = std::numeric_limits<std::size_t>::max();
        generated.originalText = generated.label + ":";

        block.instructions.insert(block.instructions.begin(), generated);
        block.label = generated.label;
        block.labelWasGenerated = true;
    }

    for (const BasicBlock& block : cfg.blocks) {
        for (const Instruction& inst : block.instructions) {
            if (inst.op != OpCode::Label) {
                break;
            }
            cfg.labelToBlock[inst.label] = block.id;
        }
    }

    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
        BasicBlock& block = cfg.blocks[b];
        block.successors.clear();
        block.fallthroughSuccessor = -1;

        int tailIndex = LastNonLabelIndex(block.instructions);
        if (tailIndex < 0) {
            if (b + 1 < cfg.blocks.size()) {
                AppendSuccessor(block, b + 1);
                block.fallthroughSuccessor = static_cast<int>(b + 1);
            }
            continue;
        }

        const Instruction& tail = block.instructions[tailIndex];
        if (tail.op == OpCode::Goto) {
            auto it = cfg.labelToBlock.find(tail.label);
            if (it == cfg.labelToBlock.end()) {
                error = "Unknown goto target label: " + tail.label;
                return false;
            }
            AppendSuccessor(block, it->second);
            continue;
        }

        if (tail.op == OpCode::IfGoto) {
            auto it = cfg.labelToBlock.find(tail.label);
            if (it == cfg.labelToBlock.end()) {
                error = "Unknown conditional target label: " + tail.label;
                return false;
            }
            AppendSuccessor(block, it->second);
            if (b + 1 < cfg.blocks.size()) {
                AppendSuccessor(block, b + 1);
                block.fallthroughSuccessor = static_cast<int>(b + 1);
            }
            continue;
        }

        if (b + 1 < cfg.blocks.size()) {
            AppendSuccessor(block, b + 1);
            block.fallthroughSuccessor = static_cast<int>(b + 1);
        }
    }

    return true;
}

std::size_t CountMovedBlockSlots(const std::vector<std::size_t>& order) {
    std::size_t moved = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] != i) {
            moved += 1;
        }
    }
    return moved;
}

Program ReorderBasicBlocks(const CFG& cfg, std::uint64_t seed, BlockReorderStats& stats) {
    Program output;
    stats = BlockReorderStats{};

    const std::size_t n = cfg.blocks.size();
    stats.blockCount = n;

    if (n == 0) {
        return output;
    }

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);

    if (n > 1) {
        std::mt19937_64 rng(seed);
        std::shuffle(order.begin() + 1, order.end(), rng);
    }

    stats.movedBlockSlots = CountMovedBlockSlots(order);
    stats.order = order;

    bool needsExitLabel = false;
    for (std::size_t pos = 0; pos < n; ++pos) {
        const BasicBlock& block = cfg.blocks[order[pos]];
        if (!block.successors.empty()) {
            continue;
        }
        if (pos + 1 < n) {
            needsExitLabel = true;
            break;
        }
    }

    std::string syntheticExitLabel;
    if (needsExitLabel) {
        std::unordered_set<std::string> used;
        for (const auto& kv : cfg.labelToBlock) {
            used.insert(kv.first);
        }
        std::size_t attempt = 0;
        while (true) {
            syntheticExitLabel = "__afis_exit";
            if (attempt > 0) {
                syntheticExitLabel += "_" + std::to_string(attempt);
            }
            if (used.find(syntheticExitLabel) == used.end()) {
                break;
            }
            attempt += 1;
        }
    }

    for (std::size_t pos = 0; pos < n; ++pos) {
        const BasicBlock& block = cfg.blocks[order[pos]];
        std::vector<Instruction> instructions = block.instructions;

        const bool hasNext = (pos + 1 < n);
        const std::size_t nextBlockId = hasNext ? order[pos + 1] : n;

        if (block.successors.empty() && hasNext) {
            Instruction exitJump;
            exitJump.op = OpCode::Goto;
            exitJump.label = syntheticExitLabel;
            exitJump.originalIndex = std::numeric_limits<std::size_t>::max();
            exitJump.originalText = "goto " + syntheticExitLabel;
            instructions.push_back(exitJump);
            stats.branchFixupsInserted += 1;
        }

        if (block.fallthroughSuccessor >= 0) {
            std::size_t expectedFallthrough = static_cast<std::size_t>(block.fallthroughSuccessor);
            if (!hasNext || nextBlockId != expectedFallthrough) {
                Instruction fixup;
                fixup.op = OpCode::Goto;
                fixup.label = cfg.blocks[expectedFallthrough].label;
                fixup.originalIndex = std::numeric_limits<std::size_t>::max();
                fixup.originalText = "goto " + fixup.label;
                instructions.push_back(fixup);
                stats.branchFixupsInserted += 1;
            }
        }

        output.instructions.insert(output.instructions.end(), instructions.begin(), instructions.end());
    }

    if (needsExitLabel) {
        Instruction endLabel;
        endLabel.op = OpCode::Label;
        endLabel.label = syntheticExitLabel;
        endLabel.originalIndex = std::numeric_limits<std::size_t>::max();
        endLabel.originalText = syntheticExitLabel + ":";
        output.instructions.push_back(endLabel);
    }

    return output;
}

}  // namespace afis