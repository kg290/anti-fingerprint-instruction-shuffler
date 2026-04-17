#ifndef AFIS_SUBSTITUTION_H
#define AFIS_SUBSTITUTION_H

#include <cstddef>
#include <string>
#include <vector>

#include "ir.h"

namespace afis {

struct SubstitutionCandidate {
    std::size_t candidateId = 0;
    std::size_t instructionIndex = 0;
    Instruction instruction;
};

struct SubstitutionStats {
    std::size_t candidateCount = 0;
    std::size_t llmApprovedCount = 0;
    std::size_t appliedCount = 0;
};

std::vector<SubstitutionCandidate> FindSafeSubstitutionCandidates(const Program& program);
std::string BuildSubstitutionSelectionPrompt(const Program& program,
                                             const std::vector<SubstitutionCandidate>& candidates);
std::vector<std::size_t> ParseApprovedCandidateIds(const std::string& text);
Program ApplyApprovedSubstitutions(const Program& input,
                                   const std::vector<SubstitutionCandidate>& candidates,
                                   const std::vector<std::size_t>& approvedIds,
                                   SubstitutionStats& stats);

}  // namespace afis

#endif