#include "fingerprint.h"

namespace afis {

FingerprintMetrics BuildFingerprintMetrics(std::size_t movedInstructionSlots,
                                           double reorderRatio,
                                           std::size_t movedBlockSlots,
                                           double blockReorderRatio,
                                           std::size_t renamedSymbols,
                                           std::uint64_t originalHash,
                                           std::uint64_t transformedHash) {
    FingerprintMetrics metrics;
    metrics.movedInstructionSlots = movedInstructionSlots;
    metrics.reorderRatio = reorderRatio;
    metrics.movedBlockSlots = movedBlockSlots;
    metrics.blockReorderRatio = blockReorderRatio;
    metrics.renamedSymbols = renamedSymbols;
    metrics.originalHash = originalHash;
    metrics.transformedHash = transformedHash;
    const double hashComponent =
        static_cast<double>((originalHash ^ transformedHash) & 0xFFFFULL) / 65535.0;
    metrics.diversificationScore =
        static_cast<double>(movedInstructionSlots) +
        (2.0 * static_cast<double>(movedBlockSlots)) +
        (0.25 * reorderRatio) +
        (0.50 * blockReorderRatio) +
        static_cast<double>(renamedSymbols) +
        hashComponent;
    return metrics;
}

}  // namespace afis
