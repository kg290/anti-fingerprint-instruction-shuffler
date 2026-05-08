#ifndef AFIS_FINGERPRINT_H
#define AFIS_FINGERPRINT_H

#include <cstddef>
#include <cstdint>

namespace afis {

struct FingerprintMetrics {
    std::size_t movedInstructionSlots = 0;
    double reorderRatio = 0.0;
    std::size_t movedBlockSlots = 0;
    double blockReorderRatio = 0.0;
    std::size_t renamedSymbols = 0;
    std::uint64_t originalHash = 0;
    std::uint64_t transformedHash = 0;
    double diversificationScore = 0.0;
};

FingerprintMetrics BuildFingerprintMetrics(std::size_t movedInstructionSlots,
                                           double reorderRatio,
                                           std::size_t movedBlockSlots,
                                           double blockReorderRatio,
                                           std::size_t renamedSymbols,
                                           std::uint64_t originalHash,
                                           std::uint64_t transformedHash);

}  // namespace afis

#endif
