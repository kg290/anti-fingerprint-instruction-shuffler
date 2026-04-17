#include "renamer.h"

#include <algorithm>
#include <random>
#include <unordered_set>

namespace afis {
namespace {

void AddCandidate(std::unordered_set<std::string>& symbols, const std::string& token) {
    if (IsIdentifier(token)) {
        symbols.insert(token);
    }
}

std::string GenerateFreshName(std::mt19937_64& rng,
                              std::unordered_set<std::string>& usedNames) {
    static const std::string kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<std::size_t> dist(0, kAlphabet.size() - 1);

    while (true) {
        std::string candidate = "r";
        for (int i = 0; i < 7; ++i) {
            candidate += kAlphabet[dist(rng)];
        }
        if (usedNames.find(candidate) == usedNames.end()) {
            usedNames.insert(candidate);
            return candidate;
        }
    }
}

std::string RenameToken(const std::string& token, const RenameMap& mapping) {
    if (!IsIdentifier(token)) {
        return token;
    }
    auto it = mapping.find(token);
    if (it == mapping.end()) {
        return token;
    }
    return it->second;
}

}  // namespace

RenameResult RenameRegisters(const Program& program, std::uint64_t seed) {
    RenameResult result;

    std::unordered_set<std::string> symbolSet;
    for (const Instruction& inst : program.instructions) {
        if (inst.hasDest) {
            AddCandidate(symbolSet, inst.dest);
        }

        switch (inst.op) {
            case OpCode::Label:
            case OpCode::Goto:
                break;
            case OpCode::Assign:
            case OpCode::UnaryNeg:
            case OpCode::Load:
            case OpCode::Print:
                AddCandidate(symbolSet, inst.arg1);
                break;
            case OpCode::Binary:
            case OpCode::Store:
                AddCandidate(symbolSet, inst.arg1);
                AddCandidate(symbolSet, inst.arg2);
                break;
            case OpCode::Call:
                for (const std::string& arg : inst.callArgs) {
                    AddCandidate(symbolSet, arg);
                }
                break;
            case OpCode::IfGoto:
                AddCandidate(symbolSet, inst.arg1);
                if (!inst.opSymbol.empty()) {
                    AddCandidate(symbolSet, inst.arg2);
                }
                break;
            case OpCode::Nop:
                break;
        }
    }

    std::vector<std::string> symbols(symbolSet.begin(), symbolSet.end());
    std::sort(symbols.begin(), symbols.end());

    std::mt19937_64 rng(seed);
    std::unordered_set<std::string> usedNames(symbolSet.begin(), symbolSet.end());

    for (const std::string& symbol : symbols) {
        result.renameMap[symbol] = GenerateFreshName(rng, usedNames);
    }

    result.program.instructions.reserve(program.instructions.size());

    for (const Instruction& original : program.instructions) {
        Instruction renamed = original;
        if (renamed.hasDest) {
            renamed.dest = RenameToken(renamed.dest, result.renameMap);
        }

        switch (renamed.op) {
            case OpCode::Label:
            case OpCode::Goto:
                break;
            case OpCode::Assign:
            case OpCode::UnaryNeg:
            case OpCode::Load:
            case OpCode::Print:
                renamed.arg1 = RenameToken(renamed.arg1, result.renameMap);
                break;
            case OpCode::Binary:
            case OpCode::Store:
                renamed.arg1 = RenameToken(renamed.arg1, result.renameMap);
                renamed.arg2 = RenameToken(renamed.arg2, result.renameMap);
                break;
            case OpCode::Call:
                for (std::string& arg : renamed.callArgs) {
                    arg = RenameToken(arg, result.renameMap);
                }
                break;
            case OpCode::IfGoto:
                renamed.arg1 = RenameToken(renamed.arg1, result.renameMap);
                if (!renamed.opSymbol.empty()) {
                    renamed.arg2 = RenameToken(renamed.arg2, result.renameMap);
                }
                break;
            case OpCode::Nop:
                break;
        }

        result.program.instructions.push_back(std::move(renamed));
    }

    return result;
}

}  // namespace afis