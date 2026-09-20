// src/llm/llm.cpp
#include "llm.hpp"
#include "util/log.hpp"
#include <thread>
#include <chrono>

namespace voice_agent {

// ========== LLM 实现（pimpl） ==========
struct LLM::Impl {
    bool load(const LLMConfig& config) {
        LOG_INFO("MockLLM: initialized ({})", config.model_path);
        return true;
    }

    std::string generate(const std::string& prompt, LLMCallback* callback, std::atomic<bool>* generating) {
        std::string response = "这是模拟 LLM 的回复。您的输入是: " + prompt;

        if (callback) {
            for (size_t i = 0; i < response.size(); i += 5) {
                if (!generating->load()) break;

                size_t end = std::min(i + 5, response.size());
                LLMResponse chunk;
                chunk.text = response.substr(i, end - i);
                chunk.eos = (end >= response.size());
                chunk.tokens_generated = static_cast<int>(end);
                callback->operator()(chunk);

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        return response;
    }

    void stop() {}
};

// ========== LLM 主类实现 ==========

LLM::LLM() = default;
LLM::~LLM() = default;

bool LLM::initialize(const LLMConfig& config) {
    config_ = config;
    impl_ = std::make_unique<Impl>();

    if (!impl_->load(config)) {
        LOG_ERROR("LLM: failed to load model");
        return false;
    }

    LOG_INFO("LLM: initialized context={}, threads={}, gpu_layers={}",
             config.n_ctx, config.n_threads, config.n_gpu_layers);
    return true;
}

std::string LLM::generate(const std::string& prompt) {
    if (!impl_) return "";

    generating_ = true;
    std::string result = impl_->generate(prompt, nullptr, &generating_);
    generating_ = false;
    return result;
}

void LLM::generate_stream(const std::string& prompt, LLMCallback callback) {
    if (!impl_) return;

    generating_ = true;
    impl_->generate(prompt, &callback, &generating_);
    generating_ = false;
}

void LLM::stop() {
    generating_ = false;
    if (impl_) {
        impl_->stop();
    }
}

void LLM::set_cancel_token(std::shared_ptr<CancelToken> token) {
    cancel_token_ = std::move(token);
}

// ========== 工厂函数 ==========

std::unique_ptr<LLM> create_llama_llm() {
    return std::make_unique<LLM>();
}

}  // namespace voice_agent
