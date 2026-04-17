#include "substitution.h"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

namespace afis {
namespace {

bool IsSafeOperand(const std::string& token) {
    return IsIdentifier(token) || IsInteger(token);
}

bool IsCandidateInstruction(const Instruction& inst) {
    if (inst.op != OpCode::Binary || !inst.hasDest) {
        return false;
    }

    if (inst.opSymbol != "+" && inst.opSymbol != "-") {
        return false;
    }

    if (!IsIdentifier(inst.dest)) {
        return false;
    }

    return IsSafeOperand(inst.arg1) && IsSafeOperand(inst.arg2);
}

std::set<std::size_t> ToIdSet(const std::vector<std::size_t>& ids) {
    return std::set<std::size_t>(ids.begin(), ids.end());
}

}  // namespace

std::vector<SubstitutionCandidate> FindSafeSubstitutionCandidates(const Program& program) {
    std::vector<SubstitutionCandidate> out;
    out.reserve(program.instructions.size());

    std::size_t candidateId = 1;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const Instruction& inst = program.instructions[i];
        if (!IsCandidateInstruction(inst)) {
            continue;
        }

        SubstitutionCandidate cand;
        cand.candidateId = candidateId++;
        cand.instructionIndex = i;
        cand.instruction = inst;
        out.push_back(std::move(cand));
    }

    return out;
}

std::string BuildSubstitutionSelectionPrompt(const Program& program,
                                             const std::vector<SubstitutionCandidate>& candidates) {
    (void)program;

    std::ostringstream out;
    out << "Select safe candidate IDs for deterministic substitution in this IR.\\n";
    out << "Rewrite pattern: dst = a OP b -> dst = a ; dst = dst OP b\\n";
    out << "Only choose if instruction is pure arithmetic and safe in isolation.\\n";
    out << "Return ONLY comma-separated candidate IDs (example: 1,3,7).\\n";
    out << "Return an empty line if none should be applied.\\n\\n";
    out << "Candidates:\\n";

    for (const SubstitutionCandidate& cand : candidates) {
        out << "[" << cand.candidateId << "] " << InstructionToString(cand.instruction) << "\\n";
    }

    return out.str();
}

std::vector<std::size_t> ParseApprovedCandidateIds(const std::string& text) {
    std::vector<std::size_t> ids;

    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }

        std::size_t start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
        }

        try {
            std::size_t id = static_cast<std::size_t>(std::stoull(text.substr(start, i - start)));
            ids.push_back(id);
        } catch (...) {
            // Ignore malformed numeric fragments.
        }
    }

    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

Program ApplyApprovedSubstitutions(const Program& input,
                                   const std::vector<SubstitutionCandidate>& candidates,
                                   const std::vector<std::size_t>& approvedIds,
                                   SubstitutionStats& stats) {
    stats = SubstitutionStats{};
    stats.candidateCount = candidates.size();

    std::set<std::size_t> approvedSet = ToIdSet(approvedIds);
    stats.llmApprovedCount = approvedSet.size();

    std::unordered_set<std::size_t> validApproved;
    for (const SubstitutionCandidate& cand : candidates) {
        if (approvedSet.find(cand.candidateId) != approvedSet.end()) {
            validApproved.insert(cand.instructionIndex);
        }
    }

    Program out;
    out.instructions.reserve(input.instructions.size() + validApproved.size());

    const std::size_t kSynthetic = std::numeric_limits<std::size_t>::max();

    for (std::size_t i = 0; i < input.instructions.size(); ++i) {
        const Instruction& inst = input.instructions[i];

        if (validApproved.find(i) == validApproved.end()) {
            out.instructions.push_back(inst);
            continue;
        }

        Instruction first;
        first.op = OpCode::Assign;
        first.dest = inst.dest;
        first.hasDest = true;
        first.arg1 = inst.arg1;
        first.originalIndex = inst.originalIndex;
        first.originalText = first.dest + " = " + first.arg1;

        Instruction second = inst;
        second.arg1 = inst.dest;
        second.originalIndex = kSynthetic;
        second.originalText = InstructionToString(second);

        out.instructions.push_back(std::move(first));
        out.instructions.push_back(std::move(second));
        stats.appliedCount += 1;
    }

    return out;
}

}  // namespace afis