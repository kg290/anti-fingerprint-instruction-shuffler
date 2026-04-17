#ifndef AFIS_SHUFFLER_H
#define AFIS_SHUFFLER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dependency.h"
#include "ir.h"

namespace afis {

struct ShuffleResult {
    Program program;
    std::vector<std::size_t> order;
    bool fallbackUsed = false;
    std::string note;
};

std::uint64_t GenerateRandomSeed();
ShuffleResult RandomizedTopologicalShuffle(const Program& original,
                                           const DependencyGraph& graph,
                                           std::uint64_t seed);

}  // namespace afis

#endif