// src/orchestrator/model_engines.hpp
#pragma once
#include <memory>
#include <string>

#include "llm/llm.hpp"
#include "orchestrator/model_router.hpp"

namespace voice_agent {

// ========== LocalLlamaEngine ==========
// 把现有 LLM（llama.cpp）包装成 IModelEngine。
// 这是"最小侵入"的接法：不重写推理，只加一层路由需要的接口。
class LocalLlamaEngine : public IModelEngine {
public:
    // 持有外部 LLM（不拥有生命周期，由 Orchestrator/GUI 管理）
    explicit LocalLlamaEngine(std::shared_ptr<LLM> llm, bool supports_tools = true);

    std::string name() const override { return name_; }
    bool available() const override;
    bool supports_tools() const override { return supports_tools_; }

    bool apply_profile(const LLMGenerationProfile& p) override;
    LLMGenerationProfile profile() const override;

    void generate_stream(const std::string& prompt,
                         const std::function<void(const std::string&)>& on_token,
                         std::shared_ptr<class CancelToken> cancel) override;

    void stop() override;

private:
    std::shared_ptr<LLM> llm_;
    std::string name_;
    bool supports_tools_;
    LLMGenerationProfile current_;
};

}  // namespace voice_agent
