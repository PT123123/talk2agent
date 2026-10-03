// src/orchestrator/model_engines.cpp
#include "model_engines.hpp"
#include "util/log.hpp"

namespace voice_agent {

LocalLlamaEngine::LocalLlamaEngine(std::shared_ptr<LLM> llm, bool supports_tools)
    : llm_(std::move(llm)),
      name_(llm_ ? ("llama:" + llm_->provider_label()) : "llama:none"),
      supports_tools_(supports_tools) {
    if (llm_) {
        current_.temperature = llm_->temperature();
        current_.max_tokens = llm_->max_tokens();
        current_.top_k = llm_->top_k();
        current_.top_p = llm_->top_p();
    }
}

bool LocalLlamaEngine::available() const {
    return llm_ != nullptr;
}

bool LocalLlamaEngine::apply_profile(const LLMGenerationProfile& p) {
    if (!llm_) return false;
    if (p == current_) return true;   // 无变化就不动，避免不必要的 sampler 重建
    llm_->apply_sampling(p.temperature, p.top_k, p.top_p, p.max_tokens);
    current_ = p;
    LOG_DEBUG("LocalLlamaEngine profile applied: temp={} top_k={} top_p={} max={}",
              p.temperature, p.top_k, p.top_p, p.max_tokens);
    return true;
}

LLMGenerationProfile LocalLlamaEngine::profile() const {
    return current_;
}

void LocalLlamaEngine::generate_stream(
    const std::string& prompt,
    const std::function<void(const std::string&)>& on_token,
    std::shared_ptr<CancelToken> cancel) {
    if (!llm_) return;
    llm_->set_cancel_token(cancel);
    llm_->generate_stream(prompt, [on_token](const LLMResponse& chunk) {
        if (chunk.text.empty()) return;   // 首 token 计时用的空 chunk 不转发
        on_token(chunk.text);
    });
}

void LocalLlamaEngine::stop() {
    if (llm_) llm_->stop();
}

}  // namespace voice_agent
