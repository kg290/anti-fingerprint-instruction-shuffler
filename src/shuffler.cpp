#include "shuffler.h"

#include <chrono>
#include <random>

namespace afis {

namespace {

std::uint64_t Mix64(std::uint64_t x) {
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return x;
}

}  // namespace

std::uint64_t GenerateRandomSeed() {
    std::random_device rd;
    std::uint64_t seed = 0;

    // Mix entropy from random_device, clock, and stack address. This avoids
    // repeated seeds on runtimes where random_device is deterministic.
    std::uint64_t rd1 = static_cast<std::uint64_t>(rd());
    std::uint64_t rd2 = static_cast<std::uint64_t>(rd());
    std::uint64_t timeNow = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uint64_t addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&seed));

    seed ^= Mix64((rd1 << 32U) ^ rd2);
    seed ^= Mix64(timeNow);
    seed ^= Mix64(addr);
    seed ^= 0xA24BAED4963EE407ULL;

    if (seed == 0) {
        seed = 0x9E3779B97F4A7C15ULL;
    }
    return seed;
}

ShuffleResult RandomizedTopologicalShuffle(const Program& original,
                                           const DependencyGraph& graph,
                                           std::uint64_t seed) {
    const std::size_t n = original.instructions.size();
    ShuffleResult result;
    result.order.clear();

    if (n == 0) {
        result.program = original;
        result.note = "No instructions to shuffle";
        return result;
    }

    std::vector<std::size_t> indegree = graph.indegree;
    std::vector<std::size_t> ready;
    ready.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }

    std::mt19937_64 rng(seed);
    std::vector<std::size_t> order;
    order.reserve(n);

    while (!ready.empty()) {
        std::uniform_int_distribution<std::size_t> dist(0, ready.size() - 1);
        std::size_t pickIndex = dist(rng);
        std::size_t node = ready[pickIndex];

        ready[pickIndex] = ready.back();
        ready.pop_back();

        order.push_back(node);

        for (std::size_t target : graph.edges[node]) {
            if (indegree[target] == 0) {
                continue;
            }
            indegree[target] -= 1;
            if (indegree[target] == 0) {
                ready.push_back(target);
            }
        }
    }

    if (order.size() != n) {
        // Should not happen for a graph formed from forward-only constraints.
        result.fallbackUsed = true;
        result.note = "Cycle detected in dependency graph. Falling back to original order.";
        result.program = original;
        result.order.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            result.order[i] = i;
        }
        return result;
    }

    result.program.instructions.reserve(n);
    for (std::size_t idx : order) {
        result.program.instructions.push_back(original.instructions[idx]);
    }
    result.order = std::move(order);
    result.note = "Randomized topological scheduling completed";
    return result;
}

}  // namespace afis