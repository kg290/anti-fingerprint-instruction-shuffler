#ifndef AFIS_OPTIMIZER_H
#define AFIS_OPTIMIZER_H

#include <cstddef>
#include <string>
#include <vector>

#include "ir.h"

namespace afis {

struct OptimizationChange {
    std::string passName;
    std::string before;
    std::string after;
    std::string note;
    bool removed = false;
};

struct OptimizationTrace {
    std::size_t constantFolds = 0;
    std::size_t constantPropagations = 0;
    std::size_t copyPropagations = 0;
    std::size_t deadInstructionsRemoved = 0;
    std::vector<OptimizationChange> changes;
};

struct OptimizationPipelineResult {
    Program afterConstantFolding;
    Program afterConstantPropagation;
    Program afterCopyPropagation;
    Program afterDeadCodeElimination;
    OptimizationTrace trace;
};

Program ConstantFold(const Program& program, OptimizationTrace& trace);
Program ConstantPropagate(const Program& program, OptimizationTrace& trace);
Program CopyPropagate(const Program& program, OptimizationTrace& trace);
Program DeadCodeEliminate(const Program& program, OptimizationTrace& trace);
OptimizationPipelineResult RunOptimizationPipeline(const Program& program);

}  // namespace afis

#endif
