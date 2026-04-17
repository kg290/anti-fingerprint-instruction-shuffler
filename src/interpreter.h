#ifndef AFIS_INTERPRETER_H
#define AFIS_INTERPRETER_H

#include <string>
#include <unordered_map>
#include <vector>

#include "ir.h"

namespace afis {

struct ExecutionResult {
    bool success = true;
    std::string error;
    std::vector<long long> printedValues;
    std::unordered_map<std::string, long long> registers;
    std::unordered_map<std::string, long long> memory;
};

ExecutionResult ExecuteProgram(const Program& program);
std::string PrintedOutputToText(const std::vector<long long>& values);

}  // namespace afis

#endif