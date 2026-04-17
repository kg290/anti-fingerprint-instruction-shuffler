#ifndef AFIS_LLM_H
#define AFIS_LLM_H

#include <string>

namespace afis {

struct GeminiConfig {
    std::string apiKey;
    std::string model = "gemini-2.0-flash";
    std::string curlPath = "curl";
    int timeoutSeconds = 30;
};

struct LlmResult {
    bool ok = false;
    std::string text;
    std::string error;
    std::string rawResponse;
};

GeminiConfig LoadGeminiConfigFromEnvFile(const std::string& envPath);
std::string StripMarkdownCodeFences(const std::string& text);

class GeminiClient {
public:
    explicit GeminiClient(GeminiConfig config);

    bool IsConfigured() const;
    const GeminiConfig& Config() const;

    LlmResult GenerateText(const std::string& systemPrompt,
                           const std::string& userPrompt,
                           double temperature = 0.2) const;

private:
    GeminiConfig config_;
};

}  // namespace afis

#endif