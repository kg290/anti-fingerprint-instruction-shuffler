#ifndef AFIS_CFG_H
#define AFIS_CFG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir.h"

namespace afis {

struct BasicBlock {
    std::size_t id = 0;
    std::string label;
    bool labelWasGenerated = false;
    std::vector<Instruction> instructions;
    std::vector<std::size_t> successors;
    int fallthroughSuccessor = -1;
};

struct CFG {
    std::vector<BasicBlock> blocks;
    std::unordered_map<std::string, std::size_t> labelToBlock;
};

struct BranchValidationResult {
    bool success = true;
    std::string error;
    std::size_t duplicateLabelCount = 0;
    std::size_t missingTargetCount = 0;
};

struct BlockReorderStats {
    std::size_t blockCount = 0;
    std::size_t movedBlockSlots = 0;
    std::size_t branchFixupsInserted = 0;
    std::vector<std::size_t> order;
};

bool IsTerminator(const Instruction& inst);
bool HasExplicitControlFlow(const Program& program);
BranchValidationResult ValidateBranches(const Program& program);
bool BuildCFG(const Program& program, CFG& cfg, std::string& error);
Program ReorderBasicBlocks(const CFG& cfg, std::uint64_t seed, BlockReorderStats& stats);
std::size_t CountMovedBlockSlots(const std::vector<std::size_t>& order);

}  // namespace afis

#endif