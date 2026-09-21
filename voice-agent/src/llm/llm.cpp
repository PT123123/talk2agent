// src/llm/llm.cpp
#include "llm.hpp"
#include "util/log.hpp"

#ifdef USE_LLAMACPP
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#endif

#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>

namespace {
inline double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Mock 回复：只提取 prompt 中最后一条 [user] 输入做简短回显，
// 绝不把系统提示/工具清单/历史整段吐回对话区（那是"回复里全是工具定义"的根源）。
std::string mock_reply(const std::string& prompt) {
    std::string last_user;
    size_t pos = 0;
    while (true) {
        size_t u = prompt.find("[user] ", pos);
        if (u == std::string::npos) break;
        u += 7;
        size_t nl = prompt.find('\n', u);
        if (nl == std::string::npos) {
            last_user = prompt.substr(u);
            break;
        }
        last_user = prompt.substr(u, nl - u);
        pos = nl + 1;
    }
    if (last_user.empty()) last_user = prompt;
    return "（Mock 模式）您说：「" + last_user +
           "」。当前没有加载真实 LLM 模型，请在“设置”页下载 GGUF 模型，"
           "或确认 models/llm 下已有模型文件。";
}
}  // namespace

namespace voice_agent {

// ========== LLM 实现（pimpl） ==========
// 优先加载真实 GGUF（llama.cpp）；未提供模型或加载失败时回退到 Mock，
// 保证无模型环境下框架仍可运行、测试稳定。
struct LLM::Impl {
    LLMConfig cfg;
    bool real_{false};

#ifdef USE_LLAMACPP
    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    struct llama_sampler* smpl_ = nullptr;
    std::string chat_tmpl_;

    ~Impl() {
        if (smpl_) llama_sampler_free(smpl_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
    }

    bool load(const LLMConfig& config) {
        cfg = config;

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = cfg.n_gpu_layers;
        model_ = llama_model_load_from_file(config.model_path.c_str(), mparams);
        if (!model_) {
            LOG_WARN("llama.cpp: failed to load model '{}' - falling back to Mock",
                     config.model_path);
            return true;  // 回退 Mock
        }

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = config.n_ctx;
        cparams.n_threads = config.n_threads;
        cparams.n_threads_batch = config.n_threads;
        cparams.embeddings = false;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            LOG_ERROR("llama.cpp: failed to create context - falling back to Mock");
            llama_model_free(model_);
            model_ = nullptr;
            return true;
        }

        auto sparams = llama_sampler_chain_default_params();
        smpl_ = llama_sampler_chain_init(sparams);
        llama_sampler_chain_add(smpl_, llama_sampler_init_top_k(50));
        llama_sampler_chain_add(smpl_, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(smpl_, llama_sampler_init_temp(cfg.temperature));
        llama_sampler_chain_add(
            smpl_, llama_sampler_init_dist(
                       static_cast<uint32_t>(
                           std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count())));

        const char* t = llama_model_chat_template(model_, nullptr);
        if (t) chat_tmpl_ = t;

        real_ = true;
        LOG_INFO("llama.cpp: loaded '{}' (ctx={}, threads={})",
                 config.model_path, config.n_ctx, config.n_threads);
        return true;
    }

    std::vector<llama_token> tokenize_(const std::string& text,
                                       bool add_special,
                                       bool parse_special) const {
        const llama_vocab* vocab = llama_model_get_vocab(model_);
        std::vector<llama_token> toks(text.size() + 8);
        int n = llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                               toks.data(), static_cast<int>(toks.size()),
                               add_special, parse_special);
        toks.resize(n);
        return toks;
    }

    // 把传入的整段 prompt 作为一条 user 消息套用模型的 chat template，
    // 生成带 assistant 起始符的输入（Qwen3 等需以此遵循系统工具指令）。
    std::string format_prompt_(const std::string& prompt) const {
        if (chat_tmpl_.empty()) return prompt;

        std::vector<llama_chat_message> msgs(1);
        msgs[0].role = "user";
        msgs[0].content = prompt.c_str();

        int capacity = static_cast<int>(prompt.size() * 2) + 256;
        std::string buf;
        for (;;) {
            buf.resize(capacity);
            int n = llama_chat_apply_template(chat_tmpl_.c_str(), msgs.data(),
                                              msgs.size(), true, buf.data(), capacity);
            if (n >= 0) {
                buf.resize(n);
                return buf;
            }
            capacity = -n;
        }
    }

    std::string generate(const std::string& prompt, LLMCallback* callback,
                         std::atomic<bool>* generating) {
        if (!real_ || !model_ || !ctx_ || !smpl_) {
            return generate_mock_(prompt, callback, generating);
        }

        const llama_vocab* vocab = llama_model_get_vocab(model_);
        const llama_token eos = llama_vocab_eos(vocab);

        std::vector<llama_token> toks = tokenize_(format_prompt_(prompt), true, true);
        const int n_ctx = static_cast<int>(llama_n_ctx(ctx_));
        const int budget = static_cast<int>(toks.size()) + cfg.max_tokens;
        if (budget > n_ctx) {
            LOG_WARN("llama.cpp: prompt({}) + max_tokens({}) exceeds ctx({})",
                     toks.size(), cfg.max_tokens, n_ctx);
        }

        if (callback) {
            LLMResponse start{};
            start.text.clear();  // 首个空 chunk 触发 first-token 计时
            callback->operator()(start);
        }

        std::string full;
        int generated = 0;
        for (int i = 0; i < cfg.max_tokens; ++i) {
            if (generating && !generating->load()) break;

            llama_batch batch = llama_batch_get_one(toks.data(),
                                                    static_cast<int32_t>(toks.size()));
            if (llama_decode(ctx_, batch) != 0) break;

            const llama_token id = llama_sampler_sample(smpl_, ctx_, -1);
            if (id < 0 || id == eos) break;

            std::string piece;
            piece.resize(64);
            int nlen = llama_token_to_piece(vocab, id, piece.data(),
                                            static_cast<int32_t>(piece.size()),
                                            0, false);
            if (nlen < 0) {
                piece.resize(static_cast<size_t>(-nlen));
                nlen = llama_token_to_piece(vocab, id, piece.data(),
                                            static_cast<int32_t>(piece.size()),
                                            0, false);
            }
            if (nlen < 0) break;
            piece.resize(static_cast<size_t>(nlen));

            full += piece;
            ++generated;
            if (callback) {
                LLMResponse c;
                c.text = std::move(piece);
                c.tokens_generated = generated;
                callback->operator()(c);
            }

            toks = {id};
        }

        if (callback) {
            LLMResponse end{};
            end.eos = true;
            end.tokens_generated = generated;
            callback->operator()(end);
        }
        return full;
    }

    std::string generate_mock_(const std::string& prompt, LLMCallback* callback,
                               std::atomic<bool>* generating) {
        std::string response = mock_reply(prompt);
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
#else
    bool load(const LLMConfig& config) {
        cfg = config;
        LOG_INFO("MockLLM: initialized ({})", config.model_path);
        return true;
    }

    std::string generate(const std::string& prompt, LLMCallback* callback,
                         std::atomic<bool>* generating) {
        std::string response = mock_reply(prompt);
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
#endif
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

bool LLM::real_backend() const {
    return impl_ != nullptr && impl_->real_;
}

std::string LLM::provider_label() const {
#ifdef USE_LLAMACPP
    if (impl_ && impl_->real_ && impl_->model_) {
        // 请求了 GPU 下放（n_gpu_layers>0）时，枚举 ggml 已注册的设备；
        // 存在 GPU/iGPU（含集成显卡，如 Intel Arc）即判为 Vulkan 后端。
        if (impl_->cfg.n_gpu_layers > 0) {
            ggml_backend_dev_props props{};
            const size_t devs = ggml_backend_dev_count();
            for (size_t i = 0; i < devs; ++i) {
                ggml_backend_dev_get_props(ggml_backend_dev_get(i), &props);
                if (props.type == GGML_BACKEND_DEVICE_TYPE_GPU ||
                    props.type == GGML_BACKEND_DEVICE_TYPE_IGPU)
                    return "Vulkan GPU";
            }
        }
        return "CPU";
    }
#else
    (void)impl_;
#endif
    return "Mock";
}

std::string LLM::generate(const std::string& prompt) {
    if (!impl_) return "";

    generating_ = true;
    const auto t0 = std::chrono::steady_clock::now();
    std::string result = impl_->generate(prompt, nullptr, &generating_);
    generating_ = false;
    last_generate_ms_.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count());
    return result;
}

void LLM::generate_stream(const std::string& prompt, LLMCallback callback) {
    if (!impl_) return;

    const double t0 = now_ms();
    double first_ms = 0.0;
    bool first_seen = false;
    LLMCallback wrapped = [&](const LLMResponse& c) {
        if (!first_seen && !c.text.empty()) {
            first_seen = true;
            first_ms = now_ms() - t0;
        }
        if (callback) callback(c);
    };

    generating_ = true;
    impl_->generate(prompt, &wrapped, &generating_);
    generating_ = false;

    last_generate_ms_.store(now_ms() - t0);
    if (first_seen) last_first_token_ms_.store(first_ms);
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