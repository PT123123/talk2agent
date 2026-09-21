// src/asr/sherpa_asr.cpp
#include "sherpa_asr.hpp"
#include "util/log.hpp"
#include <cctype>
#include <cstring>
#include <filesystem>
#include <vector>

#ifdef USE_SHERPAONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

#include "util/gpu.hpp"

namespace voice_agent {

namespace {
inline bool file_exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}
}  // namespace

struct SherpaAsr::Impl {
#ifdef USE_SHERPAONNX
    const SherpaOnnxOfflineRecognizer* recognizer_{nullptr};
#endif
};

SherpaAsr::SherpaAsr() = default;
SherpaAsr::~SherpaAsr() {
#ifdef USE_SHERPAONNX
    if (impl_ && impl_->recognizer_) {
        SherpaOnnxDestroyOfflineRecognizer(impl_->recognizer_);
    }
#endif
}

bool SherpaAsr::load(const std::string& model_dir, int num_threads,
                     const std::string& provider) {
#ifdef USE_SHERPAONNX
    const std::string lower = [&]() {
        std::string s = model_dir;
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    const bool is_sense = lower.find("sense") != std::string::npos;
    const bool is_moonshine_classic =
        file_exists(model_dir + "/preprocessor.onnx") &&
        file_exists(model_dir + "/encoder.onnx");
    const bool is_moonshine_v2 =
        file_exists(model_dir + "/encoder_model.ort") &&
        file_exists(model_dir + "/decoder_model_merged.ort");
    const bool is_moonshine = is_moonshine_classic || is_moonshine_v2;

    // provider: auto → 运行时支持 DirectML 就用 GPU，否则 CPU
    std::string prov = provider;
    if (prov == "auto") prov = ort_dml_available() ? "dml" : "cpu";
    if (prov == "dml" && !ort_dml_available()) {
        LOG_WARN("SherpaAsr: provider=dml 但运行时无 DirectML 支持，回退 cpu");
        prov = "cpu";
    }
    const bool use_gpu = (prov == "dml");

    // Moonshine 为 int8/.ort 量化模型，DirectML 反量化过慢且 .ort 兼容性差，固定 CPU
    if (is_moonshine && prov == "dml") {
        LOG_WARN("SherpaAsr: Moonshine 量化模型走 CPU（DirectML 不适用）");
        prov = "cpu";
    }

    // 模型选择：GPU 优先 fp32（int8 在 DirectML 上需逐层反量化，实测慢约 20 倍）；
    // CPU 优先 int8（体积小、速度快）
    const std::string model =
        is_sense ? (file_exists(model_dir + "/model.onnx") ? model_dir + "/model.onnx"
                                                           : model_dir + "/model.int8.onnx")
                 : (use_gpu ? (file_exists(model_dir + "/model.onnx") ? model_dir + "/model.onnx"
                                                                      : model_dir + "/model.int8.onnx")
                            : (file_exists(model_dir + "/model.int8.onnx") ? model_dir + "/model.int8.onnx"
                                                                           : model_dir + "/model.onnx"));
    const bool int8_model = model.find("int8") != std::string::npos;
    if (use_gpu && int8_model) {
        LOG_WARN("SherpaAsr: 仅找到 int8 模型，DirectML 下推理过慢，回退 cpu");
        prov = "cpu";
    }
    const std::string tokens = model_dir + "/tokens.txt";
    if (is_moonshine) {
        if ((is_moonshine_classic && !file_exists(model_dir + "/cached_decoder.onnx")) ||
            !file_exists(tokens)) {
            error_ = "missing moonshine files/tokens in " + model_dir;
            LOG_ERROR("SherpaAsr: {}", error_);
            return false;
        }
    } else if (!file_exists(model) || !file_exists(tokens)) {
        error_ = "missing model/tokens in " + model_dir;
        LOG_ERROR("SherpaAsr: {}", error_);
        return false;
    }

    // fp32 模型在 DirectML 下走完整图优化（算子融合）；int8 会死锁/过慢，已在上方回退 CPU。
    // provider 配置串格式 "directml:配置文件"，配置缺失时退回默认（BASIC，安全）。
    if (prov == "dml") {
        static const char* kOrtAllConf = "configs/dml_ort_all.conf";
        prov = file_exists(kOrtAllConf) ? "directml:" + std::string(kOrtAllConf)
                                        : "directml";
    }

    SherpaOnnxOfflineRecognizerConfig c;
    memset(&c, 0, sizeof(c));
    c.feat_config.sample_rate = 16000;
    c.feat_config.feature_dim = 80;
    c.model_config.num_threads = num_threads > 0 ? num_threads : 4;
    c.model_config.provider = prov.c_str();
    c.model_config.debug = 0;
    c.model_config.tokens = tokens.c_str();
    c.decoding_method = "greedy_search";

    if (is_moonshine) {
        if (is_moonshine_v2) {
            // v2 版：encoder_model.ort + decoder_model_merged.ort
            const std::string enc = model_dir + "/encoder_model.ort";
            const std::string dec = model_dir + "/decoder_model_merged.ort";
            c.model_config.moonshine.encoder = enc.c_str();
            c.model_config.moonshine.merged_decoder = dec.c_str();
        } else {
            // 经典版：preprocessor + encoder + cached/uncached decoder
            const std::string pre = model_dir + "/preprocessor.onnx";
            const std::string enc = model_dir + "/encoder.onnx";
            const std::string unc = model_dir + "/uncached_decoder.onnx";
            const std::string cac = model_dir + "/cached_decoder.onnx";
            c.model_config.moonshine.preprocessor = pre.c_str();
            c.model_config.moonshine.encoder = enc.c_str();
            c.model_config.moonshine.uncached_decoder = unc.c_str();
            c.model_config.moonshine.cached_decoder = cac.c_str();
        }
        name_ = "moonshine-zh";
    } else if (is_sense) {
        c.model_config.sense_voice.model = model.c_str();
        c.model_config.sense_voice.language = "zh";
        c.model_config.sense_voice.use_itn = 1;
        name_ = "sense-voice";
    } else {
        c.model_config.paraformer.model = model.c_str();
        name_ = "paraformer-zh";
    }

    impl_ = std::make_unique<Impl>();
    provider_ = (prov.rfind("directml", 0) == 0) ? "directml" : "cpu";
    impl_->recognizer_ = SherpaOnnxCreateOfflineRecognizer(&c);
    if (!impl_->recognizer_) {
        error_ = "SherpaOnnxCreateOfflineRecognizer failed";
        LOG_ERROR("SherpaAsr: {}", error_);
        return false;
    }
    loaded_ = true;
    LOG_INFO("SherpaAsr: {} backend loaded (model={}, provider={})", name_, model, prov);
    return true;
#else
    error_ = "built without USE_SHERPAONNX";
    LOG_WARN("SherpaAsr: {}", error_);
    return false;
#endif
}

std::string SherpaAsr::model_name() const { return name_; }

std::string SherpaAsr::provider_label() const {
    return provider_ == "directml" ? "DirectML GPU" : "CPU";
}

std::string SherpaAsr::transcribe(const int16_t* pcm, size_t frames) {
#ifdef USE_SHERPAONNX
    if (!loaded_ || !impl_ || !impl_->recognizer_ || !pcm || frames == 0) return {};

    std::vector<float> f(frames);
    for (size_t i = 0; i < frames; ++i) f[i] = pcm[i] / 32768.0f;

    const SherpaOnnxOfflineStream* stream =
        SherpaOnnxCreateOfflineStream(impl_->recognizer_);
    if (!stream) return {};
    SherpaOnnxAcceptWaveformOffline(stream, 16000, f.data(),
                                    static_cast<int32_t>(f.size()));
    SherpaOnnxDecodeOfflineStream(impl_->recognizer_, stream);
    const SherpaOnnxOfflineRecognizerResult* r =
        SherpaOnnxGetOfflineStreamResult(stream);
    std::string text = r && r->text ? r->text : "";
    SherpaOnnxDestroyOfflineRecognizerResult(r);
    SherpaOnnxDestroyOfflineStream(stream);

    // SenseVoice 输出可能带 <|zh|><|NEUTRAL|> 等事件标签，剥掉
    if (name_ == "sense-voice" && !text.empty()) {
        std::string cleaned;
        cleaned.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '<') {
                size_t end = text.find('>', i);
                if (end != std::string::npos) {
                    i = end + 1;
                    continue;
                }
            }
            cleaned.push_back(text[i]);
            ++i;
        }
        text = cleaned;
    }
    return text;
#else
    return {};
#endif
}

}  // namespace voice_agent
