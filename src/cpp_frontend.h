#ifndef AFIS_CPP_FRONTEND_H
#define AFIS_CPP_FRONTEND_H

#include <string>

#include "ir.h"
#include "llm.h"

namespace afis {

struct CppConversionResult {
    bool success = false;
    Program program;
    std::string irText;
    std::string error;
    bool llmUsed = false;
    std::string llmNote;
};

CppConversionResult ConvertCppFileToIR(const std::string& cppPath,
                                       const GeminiClient* llmClient,
                                       bool preferLlm);

}  // namespace afis

#endif