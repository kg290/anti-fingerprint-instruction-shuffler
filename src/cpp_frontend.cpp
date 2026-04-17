#include "cpp_frontend.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "parser.h"

namespace afis {
namespace {

struct ExprToken {
    std::string text;
    bool isOperator = false;
    bool isLParen = false;
    bool isRParen = false;
    int precedence = 0;
    bool rightAssociative = false;
};

std::string ReadWholeFile(const std::string& path, bool& ok) {
    std::ifstream in(path);
    if (!in) {
        ok = false;
        return "";
    }

    std::ostringstream out;
    out << in.rdbuf();
    ok = true;
    return out.str();
}

std::string StripComments(const std::string& source) {
    std::string out;
    out.reserve(source.size());

    bool inLineComment = false;
    bool inBlockComment = false;

    for (std::size_t i = 0; i < source.size(); ++i) {
        char ch = source[i];
        char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

        if (inLineComment) {
            if (ch == '\n') {
                inLineComment = false;
                out.push_back(ch);
            }
            continue;
        }

        if (inBlockComment) {
            if (ch == '*' && next == '/') {
                inBlockComment = false;
                ++i;
            }
            continue;
        }

        if (ch == '/' && next == '/') {
            inLineComment = true;
            ++i;
            continue;
        }

        if (ch == '/' && next == '*') {
            inBlockComment = true;
            ++i;
            continue;
        }

        out.push_back(ch);
    }

    return out;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool IsTypePrefix(const std::string& text, std::string& remainder) {
    static const std::vector<std::string> kTypes = {
        "int ", "long ", "long long ", "short ", "double ", "float ", "auto ",
        "unsigned ", "signed "
    };

    for (const std::string& t : kTypes) {
        if (StartsWith(text, t)) {
            remainder = Trim(text.substr(t.size()));
            return true;
        }
    }
    return false;
}

bool ParseCallExpr(const std::string& text,
                   std::string& functionName,
                   std::vector<std::string>& rawArgs) {
    std::string value = Trim(text);
    if (value.empty()) {
        return false;
    }

    std::size_t left = value.find('(');
    std::size_t right = value.rfind(')');
    if (left == std::string::npos || right == std::string::npos || right <= left) {
        return false;
    }
    if (right != value.size() - 1) {
        return false;
    }

    functionName = Trim(value.substr(0, left));
    if (!IsIdentifier(functionName)) {
        return false;
    }

    std::string inside = value.substr(left + 1, right - left - 1);
    rawArgs.clear();

    int depth = 0;
    std::string current;
    for (char ch : inside) {
        if (ch == '(') {
            depth += 1;
            current.push_back(ch);
            continue;
        }
        if (ch == ')') {
            depth -= 1;
            if (depth < 0) {
                return false;
            }
            current.push_back(ch);
            continue;
        }
        if (ch == ',' && depth == 0) {
            std::string arg = Trim(current);
            if (!arg.empty()) {
                rawArgs.push_back(arg);
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    if (depth != 0) {
        return false;
    }

    std::string tail = Trim(current);
    if (!tail.empty()) {
        rawArgs.push_back(tail);
    }

    return true;
}

std::string NextTempName(std::size_t& tempCounter) {
    std::string name = "__t" + std::to_string(tempCounter);
    tempCounter += 1;
    return name;
}

bool TokenizeExpr(const std::string& expr,
                  std::vector<ExprToken>& tokens,
                  std::string& error) {
    tokens.clear();

    for (std::size_t i = 0; i < expr.size();) {
        char ch = expr[i];

        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++i;
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            std::size_t j = i + 1;
            while (j < expr.size()) {
                char cj = expr[j];
                if (!std::isalnum(static_cast<unsigned char>(cj)) && cj != '_') {
                    break;
                }
                ++j;
            }
            tokens.push_back(ExprToken{expr.substr(i, j - i), false, false, false, 0, false});
            i = j;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch))) {
            std::size_t j = i + 1;
            while (j < expr.size() && std::isdigit(static_cast<unsigned char>(expr[j]))) {
                ++j;
            }
            tokens.push_back(ExprToken{expr.substr(i, j - i), false, false, false, 0, false});
            i = j;
            continue;
        }

        if (ch == '(') {
            tokens.push_back(ExprToken{"(", false, true, false, 0, false});
            ++i;
            continue;
        }

        if (ch == ')') {
            tokens.push_back(ExprToken{")", false, false, true, 0, false});
            ++i;
            continue;
        }

        if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%') {
            std::string op(1, ch);
            int precedence = (op == "+" || op == "-") ? 1 : 2;
            tokens.push_back(ExprToken{op, true, false, false, precedence, false});
            ++i;
            continue;
        }

        error = "Unsupported character in expression: " + std::string(1, ch);
        return false;
    }

    return true;
}

bool LowerExpressionToIR(const std::string& expr,
                         std::vector<std::string>& irLines,
                         std::size_t& tempCounter,
                         std::string& resultToken,
                         std::string& error) {
    std::vector<ExprToken> tokens;
    if (!TokenizeExpr(expr, tokens, error)) {
        return false;
    }

    if (tokens.empty()) {
        error = "Expression is empty";
        return false;
    }

    std::vector<ExprToken> output;
    std::vector<ExprToken> ops;

    bool expectOperand = true;

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        ExprToken token = tokens[i];

        if (!token.isOperator && !token.isLParen && !token.isRParen) {
            output.push_back(token);
            expectOperand = false;
            continue;
        }

        if (token.isLParen) {
            ops.push_back(token);
            expectOperand = true;
            continue;
        }

        if (token.isRParen) {
            bool foundLParen = false;
            while (!ops.empty()) {
                ExprToken top = ops.back();
                ops.pop_back();
                if (top.isLParen) {
                    foundLParen = true;
                    break;
                }
                output.push_back(top);
            }
            if (!foundLParen) {
                error = "Mismatched parentheses in expression";
                return false;
            }
            expectOperand = false;
            continue;
        }

        if (token.isOperator) {
            if (token.text == "-" && expectOperand) {
                token.text = "u-";
                token.precedence = 3;
                token.rightAssociative = true;
            }

            while (!ops.empty()) {
                ExprToken top = ops.back();
                if (!top.isOperator) {
                    break;
                }

                bool shouldPop = false;
                if (token.rightAssociative) {
                    shouldPop = token.precedence < top.precedence;
                } else {
                    shouldPop = token.precedence <= top.precedence;
                }

                if (!shouldPop) {
                    break;
                }

                ops.pop_back();
                output.push_back(top);
            }

            ops.push_back(token);
            expectOperand = true;
        }
    }

    while (!ops.empty()) {
        ExprToken top = ops.back();
        ops.pop_back();
        if (top.isLParen || top.isRParen) {
            error = "Mismatched parentheses in expression";
            return false;
        }
        output.push_back(top);
    }

    std::vector<std::string> eval;
    for (const ExprToken& token : output) {
        if (!token.isOperator) {
            eval.push_back(token.text);
            continue;
        }

        if (token.text == "u-") {
            if (eval.empty()) {
                error = "Malformed unary minus expression";
                return false;
            }
            std::string rhs = eval.back();
            eval.pop_back();

            std::string temp = NextTempName(tempCounter);
            irLines.push_back(temp + " = - " + rhs);
            eval.push_back(temp);
            continue;
        }

        if (eval.size() < 2) {
            error = "Malformed binary expression";
            return false;
        }

        std::string rhs = eval.back();
        eval.pop_back();
        std::string lhs = eval.back();
        eval.pop_back();

        std::string temp = NextTempName(tempCounter);
        irLines.push_back(temp + " = " + lhs + " " + token.text + " " + rhs);
        eval.push_back(temp);
    }

    if (eval.size() != 1) {
        error = "Expression lowering failed";
        return false;
    }

    resultToken = eval.back();
    return true;
}

bool BuildCallIR(const std::string& functionName,
                 const std::vector<std::string>& rawArgs,
                 const std::string& dest,
                 std::vector<std::string>& irLines,
                 std::size_t& tempCounter,
                 std::string& error) {
    std::vector<std::string> loweredArgs;
    loweredArgs.reserve(rawArgs.size());

    for (const std::string& raw : rawArgs) {
        std::string lowered;
        if (!LowerExpressionToIR(raw, irLines, tempCounter, lowered, error)) {
            return false;
        }
        loweredArgs.push_back(lowered);
    }

    std::ostringstream call;
    call << "call " << functionName;
    for (const std::string& arg : loweredArgs) {
        call << " " << arg;
    }
    if (!dest.empty()) {
        call << " -> " << dest;
    }

    irLines.push_back(call.str());
    return true;
}

int CountChar(const std::string& text, char needle) {
    int count = 0;
    for (char ch : text) {
        if (ch == needle) {
            count += 1;
        }
    }
    return count;
}

bool DeterministicCppToIR(const std::string& source,
                          Program& program,
                          std::string& irText,
                          std::string& error) {
    std::string noComments = StripComments(source);

    std::istringstream input(noComments);
    std::string line;

    std::vector<std::string> irLines;
    std::size_t tempCounter = 1;
    std::size_t lineNo = 0;

    bool inMain = false;
    int mainBraceDepth = 0;

    while (std::getline(input, line)) {
        lineNo += 1;
        int openBraces = CountChar(line, '{');
        int closeBraces = CountChar(line, '}');

        std::string text = Trim(line);
        if (text.empty()) {
            continue;
        }

        if (!inMain) {
            if (text.find("main(") != std::string::npos || text.find(" main(") != std::string::npos) {
                inMain = true;
                mainBraceDepth += openBraces - closeBraces;
            }
            continue;
        }

        if (text == "{" || text == "}") {
            mainBraceDepth += openBraces - closeBraces;
            if (mainBraceDepth <= 0) {
                break;
            }
            continue;
        }

        if (mainBraceDepth <= 0) {
            break;
        }

        if (StartsWith(text, "#include") || StartsWith(text, "using ")) {
            continue;
        }
        if (text.find("main(") != std::string::npos || text.find(" main(") != std::string::npos) {
            continue;
        }

        if (StartsWith(text, "if ") || StartsWith(text, "if(") ||
            StartsWith(text, "while ") || StartsWith(text, "while(") ||
            StartsWith(text, "for ") || StartsWith(text, "for(") ||
            StartsWith(text, "switch ") || StartsWith(text, "switch(")) {
            std::ostringstream out;
            out << "Deterministic C++ converter does not support control flow at line " << lineNo
                << ". Configure GEMINI_API_KEY in .env for LLM-assisted conversion.";
            error = out.str();
            return false;
        }

        if (!EndsWith(text, ";")) {
            std::ostringstream out;
            out << "Unsupported C++ statement at line " << lineNo << ": missing semicolon";
            error = out.str();
            return false;
        }

        text = Trim(text.substr(0, text.size() - 1));
        if (text.empty()) {
            continue;
        }

        if (StartsWith(text, "return ") || text == "return") {
            continue;
        }

        if (StartsWith(text, "std::cout") || StartsWith(text, "cout")) {
            std::size_t firstShift = text.find("<<");
            if (firstShift == std::string::npos) {
                std::ostringstream out;
                out << "Malformed cout statement at line " << lineNo;
                error = out.str();
                return false;
            }

            std::string chain = text.substr(firstShift + 2);
            std::vector<std::string> parts;
            std::string current;
            for (std::size_t i = 0; i < chain.size(); ++i) {
                if (i + 1 < chain.size() && chain[i] == '<' && chain[i + 1] == '<') {
                    std::string token = Trim(current);
                    if (!token.empty()) {
                        parts.push_back(token);
                    }
                    current.clear();
                    ++i;
                    continue;
                }
                current.push_back(chain[i]);
            }
            std::string tail = Trim(current);
            if (!tail.empty()) {
                parts.push_back(tail);
            }

            for (const std::string& rawPart : parts) {
                std::string part = Trim(rawPart);
                if (part.empty() || part == "endl" || part == "std::endl") {
                    continue;
                }
                if (part.size() >= 2 && part.front() == '"' && part.back() == '"') {
                    continue;
                }

                std::string lowered;
                if (!LowerExpressionToIR(part, irLines, tempCounter, lowered, error)) {
                    std::ostringstream out;
                    out << "Failed to lower cout expression at line " << lineNo << ": " << error;
                    error = out.str();
                    return false;
                }
                irLines.push_back("print " + lowered);
            }

            continue;
        }

        std::string declRemainder;
        bool isDecl = IsTypePrefix(text, declRemainder);
        std::string lhs;
        std::string rhs;

        if (isDecl) {
            std::size_t eq = declRemainder.find('=');
            if (eq == std::string::npos) {
                lhs = Trim(declRemainder);
                rhs = "0";
            } else {
                lhs = Trim(declRemainder.substr(0, eq));
                rhs = Trim(declRemainder.substr(eq + 1));
            }

            if (!IsIdentifier(lhs)) {
                std::ostringstream out;
                out << "Invalid declaration identifier at line " << lineNo << ": " << lhs;
                error = out.str();
                return false;
            }

            if (rhs.empty()) {
                rhs = "0";
            }

            std::string functionName;
            std::vector<std::string> rawArgs;
            if (ParseCallExpr(rhs, functionName, rawArgs)) {
                if (!BuildCallIR(functionName, rawArgs, lhs, irLines, tempCounter, error)) {
                    std::ostringstream out;
                    out << "Failed to lower call declaration at line " << lineNo << ": " << error;
                    error = out.str();
                    return false;
                }
            } else {
                std::string lowered;
                if (!LowerExpressionToIR(rhs, irLines, tempCounter, lowered, error)) {
                    std::ostringstream out;
                    out << "Failed to lower declaration at line " << lineNo << ": " << error;
                    error = out.str();
                    return false;
                }
                irLines.push_back(lhs + " = " + lowered);
            }

            continue;
        }

        std::size_t incPos = text.find("++");
        std::size_t decPos = text.find("--");
        if ((incPos != std::string::npos || decPos != std::string::npos) && text.find('=') == std::string::npos) {
            bool isInc = (incPos != std::string::npos);
            std::string symbol = isInc ? "++" : "--";
            std::size_t pos = isInc ? incPos : decPos;
            std::string base = Trim(text);
            if (pos == 0) {
                base = Trim(base.substr(2));
            } else {
                base = Trim(base.substr(0, pos));
            }

            if (!IsIdentifier(base)) {
                std::ostringstream out;
                out << "Invalid increment/decrement target at line " << lineNo;
                error = out.str();
                return false;
            }

            irLines.push_back(base + " = " + base + (isInc ? " + 1" : " - 1"));
            continue;
        }

        bool handledCompound = false;
        static const std::vector<std::string> kCompoundOps = {"+=", "-=", "*=", "/=", "%="};
        for (const std::string& op : kCompoundOps) {
            std::size_t pos = text.find(op);
            if (pos == std::string::npos) {
                continue;
            }

            lhs = Trim(text.substr(0, pos));
            rhs = Trim(text.substr(pos + op.size()));
            if (!IsIdentifier(lhs) || rhs.empty()) {
                std::ostringstream out;
                out << "Malformed compound assignment at line " << lineNo;
                error = out.str();
                return false;
            }

            std::string lowered;
            if (!LowerExpressionToIR(rhs, irLines, tempCounter, lowered, error)) {
                std::ostringstream out;
                out << "Failed to lower compound assignment at line " << lineNo << ": " << error;
                error = out.str();
                return false;
            }

            std::string opSymbol = op.substr(0, 1);
            irLines.push_back(lhs + " = " + lhs + " " + opSymbol + " " + lowered);
            handledCompound = true;
            break;
        }

        if (handledCompound) {
            continue;
        }

        std::size_t eq = text.find('=');
        if (eq != std::string::npos) {
            lhs = Trim(text.substr(0, eq));
            rhs = Trim(text.substr(eq + 1));
            if (!IsIdentifier(lhs) || rhs.empty()) {
                std::ostringstream out;
                out << "Malformed assignment at line " << lineNo;
                error = out.str();
                return false;
            }

            std::string functionName;
            std::vector<std::string> rawArgs;
            if (ParseCallExpr(rhs, functionName, rawArgs)) {
                if (!BuildCallIR(functionName, rawArgs, lhs, irLines, tempCounter, error)) {
                    std::ostringstream out;
                    out << "Failed to lower call assignment at line " << lineNo << ": " << error;
                    error = out.str();
                    return false;
                }
            } else {
                std::string lowered;
                if (!LowerExpressionToIR(rhs, irLines, tempCounter, lowered, error)) {
                    std::ostringstream out;
                    out << "Failed to lower assignment at line " << lineNo << ": " << error;
                    error = out.str();
                    return false;
                }
                irLines.push_back(lhs + " = " + lowered);
            }
            continue;
        }

        std::string callName;
        std::vector<std::string> callArgs;
        if (ParseCallExpr(text, callName, callArgs)) {
            if (!BuildCallIR(callName, callArgs, "", irLines, tempCounter, error)) {
                std::ostringstream out;
                out << "Failed to lower standalone call at line " << lineNo << ": " << error;
                error = out.str();
                return false;
            }
            continue;
        }

        std::ostringstream out;
        out << "Unsupported C++ statement at line " << lineNo << ": " << text;
        error = out.str();
        return false;
    }

    if (!inMain) {
        error = "No main() function was found in the C++ input.";
        return false;
    }

    if (irLines.empty()) {
        error = "No convertible C++ statements were found in the input.";
        return false;
    }

    std::ostringstream irOut;
    for (std::size_t i = 0; i < irLines.size(); ++i) {
        irOut << irLines[i];
        if (i + 1 < irLines.size()) {
            irOut << "\n";
        }
    }

    std::string generated = irOut.str();
    std::istringstream irInput(generated);
    ParseResult parsed = ParseIR(irInput);
    if (!parsed.errors.empty()) {
        std::ostringstream out;
        out << "Generated IR parse failed: " << parsed.errors[0].message;
        error = out.str();
        return false;
    }

    program = parsed.program;
    irText = generated;
    return true;
}

CppConversionResult TryLlmConversion(const std::string& source,
                                     const GeminiClient* llmClient,
                                     const std::string& existingNote) {
    CppConversionResult result;
    result.llmNote = existingNote;

    if (llmClient == nullptr || !llmClient->IsConfigured()) {
        return result;
    }

    const std::string systemPrompt =
        "You convert small C++ snippets into a simple compiler IR. "
        "Output only IR lines with this grammar: assignments, binary ops, unary neg, "
        "load/store, print, call, labels, goto, if-goto. "
        "No markdown fences and no explanations.";

    std::ostringstream userPrompt;
    userPrompt << "Convert this C++ to IR. Keep semantics equivalent.\n";
    userPrompt << "C++ input:\n" << source << "\n";

    LlmResult llm = llmClient->GenerateText(systemPrompt, userPrompt.str(), 0.1);
    if (!llm.ok) {
        if (!result.llmNote.empty()) {
            result.llmNote += " ";
        }
        result.llmNote += "LLM C++ conversion failed: " + llm.error;
        return result;
    }

    std::string text = StripMarkdownCodeFences(llm.text);
    std::istringstream irInput(text);
    ParseResult parsed = ParseIR(irInput);
    if (!parsed.errors.empty()) {
        if (!result.llmNote.empty()) {
            result.llmNote += " ";
        }
        result.llmNote += "LLM produced invalid IR: " + parsed.errors[0].message;
        return result;
    }

    result.success = true;
    result.llmUsed = true;
    result.program = parsed.program;
    result.irText = text;
    if (!result.llmNote.empty()) {
        result.llmNote += " ";
    }
    result.llmNote += "LLM C++ conversion succeeded.";
    return result;
}

}  // namespace

CppConversionResult ConvertCppFileToIR(const std::string& cppPath,
                                       const GeminiClient* llmClient,
                                       bool preferLlm) {
    CppConversionResult result;

    bool ok = false;
    std::string source = ReadWholeFile(cppPath, ok);
    if (!ok) {
        result.error = "Could not open C++ input file: " + cppPath;
        return result;
    }

    if (preferLlm) {
        CppConversionResult llmResult = TryLlmConversion(source, llmClient, "");
        if (llmResult.success) {
            return llmResult;
        }
        result.llmNote = llmResult.llmNote;
    }

    Program deterministicProgram;
    std::string irText;
    std::string deterministicError;
    if (DeterministicCppToIR(source, deterministicProgram, irText, deterministicError)) {
        result.success = true;
        result.program = std::move(deterministicProgram);
        result.irText = std::move(irText);
        if (!result.llmNote.empty()) {
            result.llmNote += " ";
        }
        result.llmNote += "Deterministic C++ conversion path used.";
        return result;
    }

    if (!preferLlm) {
        std::ostringstream out;
        out << deterministicError;
        if (!result.llmNote.empty()) {
            out << " " << result.llmNote;
        }
        result.error = out.str();
        return result;
    }

    CppConversionResult llmFallback = TryLlmConversion(source, llmClient, result.llmNote);
    if (llmFallback.success) {
        return llmFallback;
    }
    result.llmNote = llmFallback.llmNote;

    std::ostringstream out;
    out << deterministicError;
    if (!result.llmNote.empty()) {
        out << " " << result.llmNote;
    }
    result.error = out.str();
    return result;
}

}  // namespace afis