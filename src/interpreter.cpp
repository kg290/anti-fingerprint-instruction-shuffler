#include "interpreter.h"

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace afis {
namespace {

long long ReadValue(const std::string& token,
                    std::unordered_map<std::string, long long>& regs,
                    bool& ok,
                    std::string& error) {
    if (IsInteger(token)) {
        try {
            return std::stoll(token);
        } catch (...) {
            ok = false;
            error = "Invalid integer literal: " + token;
            return 0;
        }
    }

    if (IsIdentifier(token)) {
        auto it = regs.find(token);
        if (it == regs.end()) {
            return 0;
        }
        return it->second;
    }

    ok = false;
    error = "Unsupported value token: " + token;
    return 0;
}

std::string AddressKey(const std::string& token) {
    return token;
}

long long EvaluateBuiltin(const Instruction& inst,
                         const std::vector<long long>& args,
                         bool& ok,
                         std::string& error) {
    const std::string& name = inst.callName;

    if (name == "max") {
        if (args.empty()) {
            ok = false;
            error = "call max requires at least one argument";
            return 0;
        }
        return *std::max_element(args.begin(), args.end());
    }
    if (name == "min") {
        if (args.empty()) {
            ok = false;
            error = "call min requires at least one argument";
            return 0;
        }
        return *std::min_element(args.begin(), args.end());
    }
    if (name == "abs") {
        if (args.size() != 1) {
            ok = false;
            error = "call abs requires exactly one argument";
            return 0;
        }
        return std::llabs(args[0]);
    }
    if (name == "inc") {
        if (args.size() != 1) {
            ok = false;
            error = "call inc requires exactly one argument";
            return 0;
        }
        return args[0] + 1;
    }

    // Default deterministic behavior for unknown functions.
    return std::accumulate(args.begin(), args.end(), 0LL);
}

bool EvaluateBranchCondition(const Instruction& inst,
                            std::unordered_map<std::string, long long>& regs,
                            bool& ok,
                            std::string& error) {
    long long left = ReadValue(inst.arg1, regs, ok, error);
    if (!ok) {
        return false;
    }

    if (inst.opSymbol.empty()) {
        return left != 0;
    }

    long long right = ReadValue(inst.arg2, regs, ok, error);
    if (!ok) {
        return false;
    }

    if (inst.opSymbol == "==") {
        return left == right;
    }
    if (inst.opSymbol == "!=") {
        return left != right;
    }
    if (inst.opSymbol == "<") {
        return left < right;
    }
    if (inst.opSymbol == "<=") {
        return left <= right;
    }
    if (inst.opSymbol == ">") {
        return left > right;
    }
    if (inst.opSymbol == ">=") {
        return left >= right;
    }

    ok = false;
    error = "Unsupported conditional operator: " + inst.opSymbol;
    return false;
}

}  // namespace

ExecutionResult ExecuteProgram(const Program& program) {
    ExecutionResult result;

    std::unordered_map<std::string, std::size_t> labelToPc;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const Instruction& inst = program.instructions[i];
        if (inst.op != OpCode::Label) {
            continue;
        }
        if (inst.label.empty()) {
            result.success = false;
            result.error = "Runtime error: empty label at instruction " + std::to_string(i);
            return result;
        }
        if (labelToPc.find(inst.label) != labelToPc.end()) {
            result.success = false;
            result.error = "Runtime error: duplicate label " + inst.label;
            return result;
        }
        labelToPc[inst.label] = i;
    }

    std::size_t pc = 0;
    std::size_t steps = 0;
    const std::size_t maxSteps = 1000000;

    while (pc < program.instructions.size()) {
        if (steps++ > maxSteps) {
            result.success = false;
            result.error = "Runtime error: step limit exceeded (possible infinite loop)";
            return result;
        }

        const Instruction& inst = program.instructions[pc];

        bool ok = true;
        std::string error;

        switch (inst.op) {
            case OpCode::Label:
                pc += 1;
                break;
            case OpCode::Assign: {
                long long value = ReadValue(inst.arg1, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                result.registers[inst.dest] = value;
                pc += 1;
                break;
            }
            case OpCode::Binary: {
                long long left = ReadValue(inst.arg1, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                long long right = ReadValue(inst.arg2, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }

                long long out = 0;
                if (inst.opSymbol == "+") {
                    out = left + right;
                } else if (inst.opSymbol == "-") {
                    out = left - right;
                } else if (inst.opSymbol == "*") {
                    out = left * right;
                } else if (inst.opSymbol == "/") {
                    if (right == 0) {
                        result.success = false;
                        result.error = "Runtime error at instruction " + std::to_string(pc) +
                                       ": division by zero";
                        return result;
                    }
                    out = left / right;
                } else if (inst.opSymbol == "%") {
                    if (right == 0) {
                        result.success = false;
                        result.error = "Runtime error at instruction " + std::to_string(pc) +
                                       ": modulo by zero";
                        return result;
                    }
                    out = left % right;
                } else if (inst.opSymbol == "&") {
                    out = left & right;
                } else if (inst.opSymbol == "|") {
                    out = left | right;
                } else if (inst.opSymbol == "^") {
                    out = left ^ right;
                } else {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) +
                                   ": unsupported operator " + inst.opSymbol;
                    return result;
                }

                result.registers[inst.dest] = out;
                pc += 1;
                break;
            }
            case OpCode::UnaryNeg: {
                long long value = ReadValue(inst.arg1, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                result.registers[inst.dest] = -value;
                pc += 1;
                break;
            }
            case OpCode::Load: {
                std::string key = AddressKey(inst.arg1);
                auto it = result.memory.find(key);
                long long value = (it == result.memory.end()) ? 0 : it->second;
                result.registers[inst.dest] = value;
                pc += 1;
                break;
            }
            case OpCode::Store: {
                long long value = ReadValue(inst.arg2, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                std::string key = AddressKey(inst.arg1);
                result.memory[key] = value;
                pc += 1;
                break;
            }
            case OpCode::Print: {
                long long value = ReadValue(inst.arg1, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                result.printedValues.push_back(value);
                pc += 1;
                break;
            }
            case OpCode::Call: {
                std::vector<long long> argValues;
                argValues.reserve(inst.callArgs.size());
                for (const std::string& arg : inst.callArgs) {
                    long long value = ReadValue(arg, result.registers, ok, error);
                    if (!ok) {
                        result.success = false;
                        result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                        return result;
                    }
                    argValues.push_back(value);
                }

                long long returnValue = EvaluateBuiltin(inst, argValues, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                if (inst.hasDest) {
                    result.registers[inst.dest] = returnValue;
                }
                pc += 1;
                break;
            }
            case OpCode::Goto: {
                auto it = labelToPc.find(inst.label);
                if (it == labelToPc.end()) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) +
                                   ": unknown label target " + inst.label;
                    return result;
                }
                pc = it->second;
                break;
            }
            case OpCode::IfGoto: {
                bool takeBranch = EvaluateBranchCondition(inst, result.registers, ok, error);
                if (!ok) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) + ": " + error;
                    return result;
                }
                if (!takeBranch) {
                    pc += 1;
                    break;
                }

                auto it = labelToPc.find(inst.label);
                if (it == labelToPc.end()) {
                    result.success = false;
                    result.error = "Runtime error at instruction " + std::to_string(pc) +
                                   ": unknown label target " + inst.label;
                    return result;
                }
                pc = it->second;
                break;
            }
            case OpCode::Nop:
                pc += 1;
                break;
        }
    }

    return result;
}

std::string PrintedOutputToText(const std::vector<long long>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << "\\n";
        }
        out << values[i];
    }
    return out.str();
}

}  // namespace afis