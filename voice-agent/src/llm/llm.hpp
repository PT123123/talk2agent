// src/llm/llm.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "core/types.hpp"
#include "core/cancel_token.hpp"

namespace voice_agent {

// LLM 生成配置
struct LLMConfig {
    std::string model_path;          // 模型路径 (.gguf)
    int n_ctx = 4096;                // 上下文窗口
    int n_threads = 4;               // CPU 线程数
    int n_gpu_layers = 0;            // GPU 层数 (0=仅CPU)
    float temperature = 0.7f;        // 温度
    int max_tokens = 512;            // 最大生成长度
    float repeat_penalty = 1.1f;     // 重复惩罚
    std::string prompt_template;      // 提示词模板
};

// LLM 响应
struct LLMResponse {
    std::string text;                // 生成文本
    bool eos = false;                // 是否结束
    int tokens_generated = 0;        // 生成的 token 数
};

// LLM 回调
using LLMCallback = std::function<void(const LLMResponse& chunk)>;

class LLM {
public:
    LLM();
    ~LLM();

    // 初始化 LLM
    bool initialize(const LLMConfig& config);

    // 生成文本（阻塞）
    std::string generate(const std::string& prompt);

    // 流式生成文本
    void generate_stream(const std::string& prompt, LLMCallback callback);

    // 中断生成
    void stop();

    // 设置取消令牌
    void set_cancel_token(std::shared_ptr<CancelToken> token);

    // 是否正在生成
    bool is_generating() const { return generating_; }

    // 获取配置
    const LLMConfig& config() const { return config_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LLMConfig config_;
    std::atomic<bool> generating_{false};
    std::shared_ptr<CancelToken> cancel_token_;
};

// 创建 LLM 实例
std::unique_ptr<LLM> create_llama_llm();

}  // namespace voice_agent
