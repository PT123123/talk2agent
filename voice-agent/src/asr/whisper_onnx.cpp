// src/asr/whisper_onnx.cpp
#include "asr/whisper_onnx.hpp"
#include "asr/whisper_frontend.hpp"
#include "util/log.hpp"

#ifdef USE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace voice_agent {

using json = nlohmann::json;

// =====================================================
// BPE tokenizer（Whisper 风格 byte-level + merges）
// =====================================================
namespace {

std::string byte_to_bpe(int b) {
    if (b >= 33 && b <= 126) return std::string(1, (char)b);
    char buf[8];
    snprintf(buf, sizeof(buf), "<0x%02X>", b & 0xFF);
    return buf;
}

class BpeTokenizer {
public:
    std::unordered_map<std::string, int> token_to_id;
    std::unordered_map<int, std::string> id_to_token;
    std::map<std::pair<std::string, std::string>, int> ranks;

    bool load(const std::string& vocab_path, const std::string& merges_path) {
        std::ifstream vf(vocab_path);
        if (!vf) return false;
        json j; vf >> j;
        for (auto& [tok, id] : j.items()) {
            token_to_id[tok] = id.get<int>();
            id_to_token[id.get<int>()] = tok;
        }

        std::ifstream mf(merges_path);
        if (!mf) return false;
        std::string line; int r = 0;
        while (std::getline(mf, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto sp = line.find(' ');
            if (sp == std::string::npos) continue;
            ranks[{line.substr(0, sp), line.substr(sp + 1)}] = r++;
        }
        return !token_to_id.empty();
    }

    // text → token ids（byte-level BPE greedy）
    std::vector<int> encode(const std::string& text) {
        std::vector<std::string> words;
        for (unsigned char c : text) words.push_back(byte_to_bpe((int)c));
        while (true) {
            int best_rank = INT_MAX, best_pos = -1;
            std::pair<std::string, std::string> best;
            for (size_t i = 0; i + 1 < words.size(); ++i) {
                auto it = ranks.find({words[i], words[i + 1]});
                if (it != ranks.end() && it->second < best_rank) {
                    best_rank = it->second; best_pos = (int)i; best = it->first;
                }
            }
            if (best_pos < 0) break;
            words[best_pos] = best.first + best.second;
            words.erase(words.begin() + best_pos + 1);
        }
        std::vector<int> ids;
        for (auto& w : words) {
            auto it = token_to_id.find(w);
            if (it != token_to_id.end()) ids.push_back(it->second);
        }
        return ids;
    }

    std::string decode(const std::vector<int>& ids, const std::vector<int>& skip_from) {
        std::string out;
        for (size_t i = 0; i < ids.size(); ++i) {
            auto it = id_to_token.find(ids[i]);
            if (it == id_to_token.end()) continue;
            std::string s = it->second;
            if (s.length() >= 4 && s[0] == '<') {
                if (s.find("0x") == 1) {  // <0xXX> → 原始字节
                    int b = std::stoi(s.substr(3, 2), nullptr, 16);
                    out.push_back((char)b);
                }
                continue;  // 跳过 <|...|> 控制 token
            }
            // 还原 GPT-2 byte 空格 'Ġ'(U+0120 → UTF-8 0xC4 0xA0) 为普通空格，其余字节直通
            for (size_t j = 0; j < s.size(); ++j) {
                unsigned char c = (unsigned char)s[j];
                if (c == 0xC4 && j + 1 < s.size() && (unsigned char)s[j + 1] == 0xA0) {
                    out.push_back(' '); ++j;
                } else {
                    out.push_back((char)c);
                }
            }
        }
        return out;
    }
};

// 是否把字符 token 直接拼接（英文需空格恢复，这里由调用方再处理）
}  // namespace

struct WhisperOnnx::Impl {
#ifdef USE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> encoder, decoder;
    Ort::MemoryInfo mem{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
#endif
    WhisperFrontend frontend;
    BpeTokenizer tok;
    int n_ctx = 448;
    int n_mels = 80;
    int d_model = 384;
    int enc_len = 3000;  // Whisper 原始 mel 帧数（30s @ 10ms），conv stride2 下采样到 1500
    int eot = 50257;
    int sot = 50258;
    int lang_en = 50259;
    int task_transcribe = 50359;
    int no_timestamps = 50363;
    bool ready = false;
};

// =====================================================
// ORT 运行辅助
// =====================================================
#ifdef USE_ONNXRUNTIME

static std::vector<float> run_onnx(Ort::Session& s, Ort::MemoryInfo& mem,
                                   const std::vector<std::string>& in_names,
                                   std::vector<Ort::Value> inputs) {
    std::vector<const char*> in_chars;
    for (auto& n : in_names) in_chars.push_back(n.c_str());
    Ort::AllocatedStringPtr op = s.GetOutputNameAllocated(0, Ort::AllocatorWithDefaultOptions());
    std::string oname(op.get());
    const char* out_chars[1] = {oname.c_str()};
    auto ret = s.Run(Ort::RunOptions{nullptr}, in_chars.data(),
                     (Ort::Value*)inputs.data(), inputs.size(), out_chars, 1);
    auto info = ret[0].GetTensorTypeAndShapeInfo();
    auto sh = info.GetShape();
    int64_t total = 1; for (auto d : sh) total *= d;
    std::vector<float> data(total);
    memcpy(data.data(), ret[0].GetTensorData<float>(), total * sizeof(float));
    return data;
}

static std::vector<float> run_encoder(Ort::Session& enc, Ort::MemoryInfo& mem,
                                      const float* mel, int rows, int cols) {
    std::array<int64_t, 3> shape{1, rows, cols};
    auto in = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(mel),
                                              (size_t)rows * cols, shape.data(), shape.size());
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(in));
    return run_onnx(enc, mem, {"input_features"}, std::move(inputs));
}

#endif

// =====================================================
// 公共接口
// =====================================================
WhisperOnnx::WhisperOnnx() = default;
WhisperOnnx::~WhisperOnnx() = default;

bool WhisperOnnx::load(const std::string& model_dir) {
#ifdef USE_ONNXRUNTIME
    try {
        impl_ = std::make_unique<Impl>();
        impl_->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "whisper");

        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(4);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const std::string enc = model_dir + "/onnx/encoder_model.onnx";
        const std::string dec = model_dir + "/onnx/decoder_model.onnx";
        if (!std::filesystem::exists(enc) || !std::filesystem::exists(dec)) {
            error_ = "models not found in " + model_dir;
            return false;
        }
        // Windows 上的 ORT Session 需要宽字符路径
        const std::wstring w_enc(enc.begin(), enc.end());
        const std::wstring w_dec(dec.begin(), dec.end());
        impl_->encoder = std::make_unique<Ort::Session>(*impl_->env, w_enc.c_str(), so);
        impl_->decoder = std::make_unique<Ort::Session>(*impl_->env, w_dec.c_str(), so);

        if (!impl_->tok.load(model_dir + "/vocab.json", model_dir + "/merges.txt")) {
            error_ = "tokenizer load failed";
            return false;
        }
        impl_->frontend.init();
        loaded_ = true;
        LOG_INFO("WhisperOnnx loaded: {}", model_dir);
        return true;
    } catch (const std::exception& e) {
        error_ = std::string("onnx init: ") + e.what();
        LOG_ERROR("WhisperOnnx load error: {}", error_);
        return false;
    }
#else
    (void)model_dir;
    error_ = "built without ONNX Runtime";
    return false;
#endif
}

std::string WhisperOnnx::transcribe(const int16_t* pcm, size_t frames) {
    if (!loaded_ || !impl_) return {};
#ifdef USE_ONNXRUNTIME
    try {
        const int rows = impl_->n_mels;   // 80
        const int cols = impl_->enc_len;  // 3000
        auto mel = impl_->frontend.compute(pcm, frames);
        const int n_frames = (int)(mel.size() / rows);
        if (n_frames <= 0) return {};

        // pad 到 80 x 3000（填充区用静音 log-mel 值，与 OpenAI 官方一致）
        // → trans 排序为 (mel_bin, time)，喂给 encoder
        std::vector<float> melp((size_t)rows * cols, -1.5f);
        for (int f = 0; f < n_frames && f < cols; ++f)
            for (int m = 0; m < rows; ++m)
                melp[(size_t)m * cols + f] = mel[(size_t)f * rows + m];

        auto enc_out = run_encoder(*impl_->encoder, impl_->mem, melp.data(), rows, cols);
        const int d_model = impl_->d_model;
        // Whisper 编码器对时间维做 2x 下采样（3000 → 1500），
        // 帧数从实际输出张量推断，不能假设等于输入长度。
        if (d_model <= 0 || enc_out.size() % (size_t)d_model != 0) return {};
        const int enc_frames = (int)(enc_out.size() / d_model);
        if (enc_frames <= 0) return {};

        // greedy 解码：无 KV-cache，每次整段前向
        std::vector<int64_t> toks{impl_->sot, impl_->lang_en, impl_->task_transcribe,
                                  impl_->no_timestamps};
        std::array<int64_t, 3> enc_shape{1, enc_frames, d_model};

        for (int step = 0; step < impl_->n_ctx; ++step) {
            std::array<int64_t, 2> ids_shape{1, (int64_t)toks.size()};
            auto ids_t = Ort::Value::CreateTensor<int64_t>(
                impl_->mem, toks.data(), toks.size(), ids_shape.data(), ids_shape.size());
            auto enc_t = Ort::Value::CreateTensor<float>(
                impl_->mem, enc_out.data(), (size_t)enc_frames * d_model,
                enc_shape.data(), enc_shape.size());
            Ort::Value in_v[2] = {std::move(ids_t), std::move(enc_t)};
            const char* in_n[2] = {"input_ids", "encoder_hidden_states"};

            Ort::AllocatedStringPtr op = impl_->decoder->GetOutputNameAllocated(
                0, Ort::AllocatorWithDefaultOptions());
            std::string oname(op.get());
            const char* out_n[1] = {oname.c_str()};
            auto ret = impl_->decoder->Run(Ort::RunOptions{nullptr}, in_n, in_v, 2, out_n, 1);

            auto info = ret[0].GetTensorTypeAndShapeInfo();
            auto sh = info.GetShape();
            if (sh.size() != 3) break;
            const int len = (int)sh[1], vtok = (int)sh[2];
            const float* lg = ret[0].GetTensorData<float>();
            const float* last = lg + (size_t)(len - 1) * vtok;
            int best = 0; float bv = -1e30f;
            for (int v = 0; v < vtok; ++v)
                if (last[v] > bv) { bv = last[v]; best = v; }
            if (best == impl_->eot) break;
            toks.push_back(best);
        }

        // 解码为文本（跳过 4 个前导控制 token：sot/lang/task/no_timestamps）
        std::vector<int> ids(toks.begin() + 4, toks.end());
        return impl_->tok.decode(ids, {});
    } catch (const std::exception& e) {
        LOG_ERROR("WhisperOnnx transcribe: {}", e.what());
        return {};
    }
#else
    (void)pcm; (void)frames;
    return {};
#endif
}

}  // namespace voice_agent