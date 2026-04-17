#include "llm.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace afis {
namespace {

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

std::unordered_map<std::string, std::string> ParseEnvFile(const std::string& path) {
    std::unordered_map<std::string, std::string> values;

    std::ifstream in(path);
    if (!in) {
        return values;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string text = Trim(line);
        if (text.empty() || text[0] == '#') {
            continue;
        }

        std::size_t eq = text.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key = Trim(text.substr(0, eq));
        std::string value = Trim(text.substr(eq + 1));
        if (key.empty()) {
            continue;
        }

        if (value.size() >= 2) {
            char first = value.front();
            char last = value.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }

        values[key] = value;
    }

    return values;
}

std::string GetValue(const std::unordered_map<std::string, std::string>& fromFile,
                     const char* envKey,
                     const std::string& fileKey,
                     const std::string& defaultValue = "") {
    const char* envVal = std::getenv(envKey);
    if (envVal != nullptr && *envVal != '\0') {
        return std::string(envVal);
    }

    auto it = fromFile.find(fileKey);
    if (it != fromFile.end()) {
        return it->second;
    }

    return defaultValue;
}

std::string NormalizeConfigValue(const std::string& text) {
    std::string value = Trim(text);
    while (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
        value.pop_back();
    }
    return Trim(value);
}

std::string QuoteForCommand(const std::string& text) {
    if (text.find(' ') != std::string::npos || text.find('\t') != std::string::npos) {
        return "\"" + text + "\"";
    }
    return text;
}

int ParseIntSafe(const std::string& text, int fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        int value = std::stoi(text);
        return value > 0 ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);

    for (char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }

    return out;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

bool ExtractDecodedJsonString(const std::string& text,
                              std::size_t startQuote,
                              std::string& decoded,
                              std::size_t* endPos = nullptr) {
    if (startQuote >= text.size() || text[startQuote] != '"') {
        return false;
    }

    decoded.clear();

    for (std::size_t i = startQuote + 1; i < text.size(); ++i) {
        char ch = text[i];

        if (ch == '"') {
            if (endPos != nullptr) {
                *endPos = i;
            }
            return true;
        }

        if (ch != '\\') {
            decoded.push_back(ch);
            continue;
        }

        if (i + 1 >= text.size()) {
            return false;
        }

        char esc = text[++i];
        switch (esc) {
            case '"':
                decoded.push_back('"');
                break;
            case '\\':
                decoded.push_back('\\');
                break;
            case '/':
                decoded.push_back('/');
                break;
            case 'b':
                decoded.push_back('\b');
                break;
            case 'f':
                decoded.push_back('\f');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            case 'u': {
                if (i + 4 >= text.size()) {
                    return false;
                }
                int code = 0;
                for (int k = 0; k < 4; ++k) {
                    int hex = HexValue(text[i + 1 + k]);
                    if (hex < 0) {
                        return false;
                    }
                    code = (code << 4) | hex;
                }
                i += 4;

                if (code >= 32 && code <= 126) {
                    decoded.push_back(static_cast<char>(code));
                } else {
                    decoded.push_back('?');
                }
                break;
            }
            default:
                decoded.push_back(esc);
                break;
        }
    }

    return false;
}

std::string ExtractFirstCandidateText(const std::string& response) {
    std::size_t searchStart = response.find("\"candidates\"");
    if (searchStart == std::string::npos) {
        return "";
    }

    std::size_t textKey = response.find("\"text\"", searchStart);
    while (textKey != std::string::npos) {
        std::size_t colon = response.find(':', textKey);
        if (colon == std::string::npos) {
            return "";
        }

        std::size_t quote = response.find('"', colon + 1);
        if (quote == std::string::npos) {
            return "";
        }

        std::string decoded;
        if (ExtractDecodedJsonString(response, quote, decoded)) {
            return decoded;
        }

        textKey = response.find("\"text\"", textKey + 6);
    }

    return "";
}

std::string ExtractErrorMessage(const std::string& response) {
    std::size_t errPos = response.find("\"error\"");
    if (errPos == std::string::npos) {
        return "";
    }

    std::size_t msgKey = response.find("\"message\"", errPos);
    if (msgKey == std::string::npos) {
        return "";
    }

    std::size_t colon = response.find(':', msgKey);
    if (colon == std::string::npos) {
        return "";
    }

    std::size_t quote = response.find('"', colon + 1);
    if (quote == std::string::npos) {
        return "";
    }

    std::string decoded;
    if (!ExtractDecodedJsonString(response, quote, decoded)) {
        return "";
    }

    return decoded;
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return "";
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

GeminiConfig LoadGeminiConfigFromEnvFile(const std::string& envPath) {
    GeminiConfig config;

    std::unordered_map<std::string, std::string> fileValues = ParseEnvFile(envPath);

    config.apiKey = NormalizeConfigValue(GetValue(fileValues, "GEMINI_API_KEY", "GEMINI_API_KEY", ""));
    config.model = NormalizeConfigValue(
        GetValue(fileValues, "GEMINI_MODEL", "GEMINI_MODEL", "gemini-2.0-flash"));
    config.curlPath = NormalizeConfigValue(
        GetValue(fileValues, "GEMINI_CURL_PATH", "GEMINI_CURL_PATH", "curl"));
    config.timeoutSeconds = ParseIntSafe(
        GetValue(fileValues, "GEMINI_TIMEOUT_SECONDS", "GEMINI_TIMEOUT_SECONDS", "30"), 30);

    return config;
}

std::string StripMarkdownCodeFences(const std::string& text) {
    std::string trimmed = Trim(text);
    if (trimmed.rfind("```", 0) != 0) {
        return trimmed;
    }

    std::size_t firstNewline = trimmed.find('\n');
    if (firstNewline == std::string::npos) {
        return trimmed;
    }

    std::size_t lastFence = trimmed.rfind("```");
    if (lastFence == std::string::npos || lastFence <= firstNewline) {
        return trimmed;
    }

    std::string inside = trimmed.substr(firstNewline + 1, lastFence - firstNewline - 1);
    return Trim(inside);
}

GeminiClient::GeminiClient(GeminiConfig config) : config_(std::move(config)) {}

bool GeminiClient::IsConfigured() const {
    return !config_.apiKey.empty();
}

const GeminiConfig& GeminiClient::Config() const {
    return config_;
}

LlmResult GeminiClient::GenerateText(const std::string& systemPrompt,
                                     const std::string& userPrompt,
                                     double temperature) const {
    LlmResult result;

    if (!IsConfigured()) {
        result.error = "Gemini API key is not configured (set GEMINI_API_KEY in .env).";
        return result;
    }

    std::filesystem::path tempDir;
    try {
        tempDir = std::filesystem::temp_directory_path();
    } catch (...) {
        tempDir = std::filesystem::current_path();
    }

    long long tick = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::string tag = std::to_string(tick);

    std::filesystem::path reqPath = tempDir / ("afis_gemini_req_" + tag + ".json");
    std::filesystem::path respPath = tempDir / ("afis_gemini_resp_" + tag + ".json");

    {
        std::ofstream req(reqPath);
        if (!req) {
            result.error = "Could not create Gemini request file.";
            return result;
        }

        req << "{\n";
        req << "  \"system_instruction\": {\"parts\": [{\"text\": \""
            << JsonEscape(systemPrompt) << "\"}]},\n";
        req << "  \"contents\": [{\"parts\": [{\"text\": \""
            << JsonEscape(userPrompt) << "\"}]}],\n";
        req << "  \"generationConfig\": {\"temperature\": " << temperature << "}\n";
        req << "}\n";
    }

    std::string endpoint = "https://generativelanguage.googleapis.com/v1beta/models/" +
                           config_.model + ":generateContent?key=" + config_.apiKey;

    std::string curlPath = QuoteForCommand(config_.curlPath.empty() ? "curl" : config_.curlPath);
    std::string reqPathQuoted = "\"@" + reqPath.string() + "\"";
    std::string respPathQuoted = "\"" + respPath.string() + "\"";

    std::ostringstream cmd;
    cmd << curlPath
        << " -sS --max-time " << config_.timeoutSeconds
        << " -X POST \"" << endpoint << "\""
        << " -H \"Content-Type: application/json\""
        << " --data-binary " << reqPathQuoted
        << " -o " << respPathQuoted;

    int code = std::system(cmd.str().c_str());
    if (code != 0) {
        std::filesystem::remove(reqPath);
        std::filesystem::remove(respPath);

        std::ostringstream err;
        err << "Gemini API request failed (curl exit code " << code << ").";
        result.error = err.str();
        return result;
    }

    std::string response = ReadWholeFile(respPath);
    result.rawResponse = response;

    std::filesystem::remove(reqPath);
    std::filesystem::remove(respPath);

    if (response.empty()) {
        result.error = "Gemini API returned an empty response.";
        return result;
    }

    std::string text = ExtractFirstCandidateText(response);
    if (!text.empty()) {
        result.ok = true;
        result.text = text;
        return result;
    }

    std::string errMsg = ExtractErrorMessage(response);
    if (!errMsg.empty()) {
        result.error = "Gemini API error: " + errMsg;
        return result;
    }

    result.error = "Could not parse Gemini response text.";
    return result;
}

}  // namespace afis