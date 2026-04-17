#ifndef AFIS_DEPENDENCY_H
#define AFIS_DEPENDENCY_H

#include <cstddef>
#include <string>
#include <vector>

#include "ir.h"

namespace afis {

struct DependencyGraph {
    std::vector<std::vector<std::size_t>> edges;
    std::vector<std::size_t> indegree;
};

std::vector<std::string> ReadSet(const Instruction& inst);
std::vector<std::string> WriteSet(const Instruction& inst);
bool IsSideEffectOperation(const Instruction& inst);
bool IsSideEffectingBarrier(const Instruction& inst);
DependencyGraph BuildDependencyGraph(const Program& program);

}  // namespace afis

#endif