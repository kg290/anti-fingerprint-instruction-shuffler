#include "parser.h"

#include <fstream>
#include <string>
#include <unordered_set>

namespace afis {
namespace {

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool IsBinaryOperator(const std::string& op) {
    static const std::unordered_set<std::string> kOps = {
        "+", "-", "*", "/", "%", "&", "|", "^"
    };
    return kOps.find(op) != kOps.end();
}

bool IsConditionOperator(const std::string& op) {
    static const std::unordered_set<std::string> kOps = {
        "==", "!=", "<", ">", "<=", ">="
    };
    return kOps.find(op) != kOps.end();
}

void AddError(ParseResult& result, std::size_t line, const std::string& message) {
    result.errors.push_back(ParseError{line, message});
}

}  // namespace

ParseResult ParseIR(std::istream& input) {
    ParseResult result;
    std::string line;
    std::size_t lineNo = 0;

    while (std::getline(input, line)) {
        ++lineNo;
        std::string text = Trim(line);
        if (text.empty() || StartsWith(text, "#") || StartsWith(text, "//")) {
            continue;
        }

        Instruction inst;
        inst.originalIndex = result.program.instructions.size();
        inst.originalText = text;

        if (!text.empty() && text.back() == ':') {
            std::string label = Trim(text.substr(0, text.size() - 1));
            if (!IsIdentifier(label)) {
                AddError(result, lineNo, "Invalid label name: " + label);
                continue;
            }
            inst.op = OpCode::Label;
            inst.label = label;
            result.program.instructions.push_back(inst);
            continue;
        }

        if (StartsWith(text, "goto ")) {
            std::string target = Trim(text.substr(5));
            if (!IsIdentifier(target)) {
                AddError(result, lineNo, "Invalid goto target label: " + target);
                continue;
            }
            inst.op = OpCode::Goto;
            inst.label = target;
            result.program.instructions.push_back(inst);
            continue;
        }

        if (StartsWith(text, "if ")) {
            std::string body = Trim(text.substr(3));
            std::size_t gotoPos = body.find(" goto ");
            if (gotoPos == std::string::npos) {
                AddError(result, lineNo,
                         "Malformed conditional branch. Expected: if x goto L or if a < b goto L");
                continue;
            }

            std::string condText = Trim(body.substr(0, gotoPos));
            std::string target = Trim(body.substr(gotoPos + 6));

            if (condText.empty()) {
                AddError(result, lineNo, "Conditional expression is empty");
                continue;
            }
            if (!IsIdentifier(target)) {
                AddError(result, lineNo, "Invalid conditional branch target label: " + target);
                continue;
            }

            std::vector<std::string> tokens = SplitWhitespace(condText);
            if (tokens.size() == 1) {
                inst.op = OpCode::IfGoto;
                inst.arg1 = tokens[0];
                inst.label = target;
                result.program.instructions.push_back(inst);
                continue;
            }
            if (tokens.size() == 3 && IsConditionOperator(tokens[1])) {
                inst.op = OpCode::IfGoto;
                inst.arg1 = tokens[0];
                inst.opSymbol = tokens[1];
                inst.arg2 = tokens[2];
                inst.label = target;
                result.program.instructions.push_back(inst);
                continue;
            }

            AddError(result, lineNo,
                     "Unsupported conditional format. Use: if x goto L or if a < b goto L");
            continue;
        }

        if (text == "nop") {
            inst.op = OpCode::Nop;
            result.program.instructions.push_back(inst);
            continue;
        }

        if (StartsWith(text, "load ")) {
            std::string body = Trim(text.substr(5));
            std::size_t comma = body.find(',');
            if (comma == std::string::npos) {
                AddError(result, lineNo, "Malformed load. Expected: load dst, addr");
                continue;
            }
            inst.op = OpCode::Load;
            inst.dest = Trim(body.substr(0, comma));
            inst.hasDest = true;
            inst.arg1 = Trim(body.substr(comma + 1));
            if (!IsIdentifier(inst.dest)) {
                AddError(result, lineNo, "Invalid load destination identifier: " + inst.dest);
                continue;
            }
            if (inst.arg1.empty()) {
                AddError(result, lineNo, "Load address operand is empty");
                continue;
            }
            result.program.instructions.push_back(inst);
            continue;
        }

        if (StartsWith(text, "store ")) {
            std::string body = Trim(text.substr(6));
            std::size_t comma = body.find(',');
            if (comma == std::string::npos) {
                AddError(result, lineNo, "Malformed store. Expected: store addr, src");
                continue;
            }
            inst.op = OpCode::Store;
            inst.arg1 = Trim(body.substr(0, comma));
            inst.arg2 = Trim(body.substr(comma + 1));
            if (inst.arg1.empty() || inst.arg2.empty()) {
                AddError(result, lineNo, "Store operands cannot be empty");
                continue;
            }
            result.program.instructions.push_back(inst);
            continue;
        }

        if (StartsWith(text, "print ")) {
            inst.op = OpCode::Print;
            inst.arg1 = Trim(text.substr(6));
            if (inst.arg1.empty()) {
                AddError(result, lineNo, "Print operand is empty");
                continue;
            }
            result.program.instructions.push_back(inst);
            continue;
        }

        if (StartsWith(text, "call ")) {
            std::string body = Trim(text.substr(5));
            std::string left = body;
            std::string dest;

            std::size_t arrow = body.find("->");
            if (arrow != std::string::npos) {
                left = Trim(body.substr(0, arrow));
                dest = Trim(body.substr(arrow + 2));
                if (dest.empty()) {
                    AddError(result, lineNo, "Call destination is empty after ->");
                    continue;
                }
                if (!IsIdentifier(dest)) {
                    AddError(result, lineNo, "Invalid call destination identifier: " + dest);
                    continue;
                }
            }

            std::vector<std::string> tokens = SplitWhitespace(left);
            if (tokens.empty()) {
                AddError(result, lineNo, "Malformed call. Expected: call fn arg1 arg2 -> dst");
                continue;
            }

            inst.op = OpCode::Call;
            inst.callName = tokens[0];
            if (!IsIdentifier(inst.callName)) {
                AddError(result, lineNo, "Invalid call function name: " + inst.callName);
                continue;
            }

            for (std::size_t i = 1; i < tokens.size(); ++i) {
                inst.callArgs.push_back(tokens[i]);
            }
            if (!dest.empty()) {
                inst.hasDest = true;
                inst.dest = dest;
            }
            result.program.instructions.push_back(inst);
            continue;
        }

        std::size_t eq = text.find('=');
        if (eq != std::string::npos) {
            std::string lhs = Trim(text.substr(0, eq));
            std::string rhs = Trim(text.substr(eq + 1));

            if (!IsIdentifier(lhs)) {
                AddError(result, lineNo, "Invalid assignment destination identifier: " + lhs);
                continue;
            }
            if (rhs.empty()) {
                AddError(result, lineNo, "Right-hand side is empty");
                continue;
            }

            std::vector<std::string> rhsTokens = SplitWhitespace(rhs);
            inst.dest = lhs;
            inst.hasDest = true;

            if (rhsTokens.size() == 1) {
                inst.op = OpCode::Assign;
                inst.arg1 = rhsTokens[0];
                result.program.instructions.push_back(inst);
                continue;
            }
            if (rhsTokens.size() == 2 && rhsTokens[0] == "-") {
                inst.op = OpCode::UnaryNeg;
                inst.arg1 = rhsTokens[1];
                result.program.instructions.push_back(inst);
                continue;
            }
            if (rhsTokens.size() == 3 && IsBinaryOperator(rhsTokens[1])) {
                inst.op = OpCode::Binary;
                inst.arg1 = rhsTokens[0];
                inst.opSymbol = rhsTokens[1];
                inst.arg2 = rhsTokens[2];
                result.program.instructions.push_back(inst);
                continue;
            }

            AddError(result, lineNo,
                     "Unsupported assignment format. Examples: a = b, a = b + c, a = - b");
            continue;
        }

        AddError(result, lineNo,
                 "Unsupported instruction format. Use assignment/load/store/print/call/nop");
    }

    return result;
}

ParseResult ParseIRFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        ParseResult result;
        result.errors.push_back(ParseError{0, "Could not open input file: " + path});
        return result;
    }
    return ParseIR(input);
}

}  // namespace afis