#include "ir.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace afis {

bool IsIdentifier(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    unsigned char first = static_cast<unsigned char>(token[0]);
    if (!(std::isalpha(first) || token[0] == '_')) {
        return false;
    }
    for (char ch : token) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_')) {
            return false;
        }
    }
    return true;
}

bool IsInteger(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::size_t start = 0;
    if (token[0] == '-') {
        if (token.size() == 1) {
            return false;
        }
        start = 1;
    }
    for (std::size_t i = start; i < token.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(token[i]);
        if (!std::isdigit(c)) {
            return false;
        }
    }
    return true;
}

std::string Trim(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

std::vector<std::string> SplitWhitespace(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        out.push_back(token);
    }
    return out;
}

std::string InstructionToString(const Instruction& inst) {
    switch (inst.op) {
        case OpCode::Label:
            return inst.label + ":";
        case OpCode::Assign:
            return inst.dest + " = " + inst.arg1;
        case OpCode::Binary:
            return inst.dest + " = " + inst.arg1 + " " + inst.opSymbol + " " + inst.arg2;
        case OpCode::UnaryNeg:
            return inst.dest + " = - " + inst.arg1;
        case OpCode::Load:
            return "load " + inst.dest + ", " + inst.arg1;
        case OpCode::Store:
            return "store " + inst.arg1 + ", " + inst.arg2;
        case OpCode::Print:
            return "print " + inst.arg1;
        case OpCode::Call: {
            std::string text = "call " + inst.callName;
            for (const std::string& arg : inst.callArgs) {
                text += " " + arg;
            }
            if (inst.hasDest) {
                text += " -> " + inst.dest;
            }
            return text;
        }
        case OpCode::Goto:
            return "goto " + inst.label;
        case OpCode::IfGoto:
            if (inst.opSymbol.empty()) {
                return "if " + inst.arg1 + " goto " + inst.label;
            }
            return "if " + inst.arg1 + " " + inst.opSymbol + " " + inst.arg2 + " goto " + inst.label;
        case OpCode::Nop:
            return "nop";
    }
    return "nop";
}

}  // namespace afis