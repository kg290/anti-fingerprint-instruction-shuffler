#ifndef AFIS_IR_H
#define AFIS_IR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace afis {

enum class OpCode {
    Label,
    Assign,
    Binary,
    UnaryNeg,
    Load,
    Store,
    Print,
    Call,
    Goto,
    IfGoto,
    Nop
};

struct Instruction {
    OpCode op = OpCode::Nop;

    // Destination register/variable for ops that produce a value.
    std::string dest;
    bool hasDest = false;

    // Generic operands for assignment/unary/binary/load/store/print.
    std::string arg1;
    std::string arg2;
    std::string opSymbol;

    // Call-specific metadata.
    std::string callName;
    std::vector<std::string> callArgs;

    // Label definition and branch target metadata.
    std::string label;

    std::size_t originalIndex = 0;
    std::string originalText;
};

struct Program {
    std::vector<Instruction> instructions;
};

struct ParseError {
    std::size_t line = 0;
    std::string message;
};

struct ParseResult {
    Program program;
    std::vector<ParseError> errors;
};

using RenameMap = std::unordered_map<std::string, std::string>;

bool IsIdentifier(const std::string& token);
bool IsInteger(const std::string& token);
std::string Trim(const std::string& text);
std::vector<std::string> SplitWhitespace(const std::string& text);
std::string InstructionToString(const Instruction& inst);

}  // namespace afis

#endif