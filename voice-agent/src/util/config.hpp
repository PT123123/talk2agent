// src/util/config.hpp
#pragma once
#include "../core/types.hpp"
#include "log.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ========== 轻量 YAML 解析 ==========
// 配置文件是 agent.yaml（YAML），而 nlohmann::json 只能读 JSON。
// 这里提供一个针对本项目 YAML 子集的极简解析器，仅支持：
//   - 注释（# 开头）
//   - 顶层 key: value 与两级分组（key: 后换行，子项缩进两个空格）
//   - 标量：数字 / 布尔 / 字符串（含 URL 等含冒号的值，取第一个 ': ' 前为键）
namespace yaml_light {

// 去掉行首/行尾空白
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// 统计行首都缩进空格数
inline size_t indent_of(const std::string& s) {
    size_t n = 0;
    while (n < s.size() && s[n] == ' ') ++n;
    return n;
}

// 把去除注释后的行去掉行尾注释
inline std::string strip_comment(const std::string& s) {
    std::string t = s;
    size_t pos = t.find('#');
    if (pos != std::string::npos) t = t.substr(0, pos);
    return t;
}

// 把 YAML 标量转成 nlohmann::json 值
inline json to_value(const std::string& raw) {
    std::string v = trim(raw);
    if (v.empty()) return json(nullptr);
    // 去掉可选引号
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') ||
         (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
    }
    if (v == "true") return json(true);
    if (v == "false") return json(false);
    if (v == "null" || v == "~") return json(nullptr);
    // 尝试数字（整数/浮点），失败按字符串
    try {
        size_t sz = 0;
        if (v.find_first_not_of("+-0123456789.") == std::string::npos &&
            !v.empty()) {
            long double d = std::stold(v, &sz);
            if (sz == v.size()) {
                // 整数优先
                if (v.find('.') == std::string::npos) {
                    try { return json(std::stoll(v)); } catch (...) {}
                }
                return json(double(d));
            }
        }
    } catch (...) {}
    return json(v);
}

// 递归解析：lines 中从 idx 开始、缩进 >= indent 的行，构建一个对象
inline json parse_block(const std::vector<std::string>& lines, size_t& idx,
                        size_t indent) {
    json obj = json::object();
    while (idx < lines.size()) {
        const std::string& line = lines[idx];
        size_t ind = indent_of(line);
        if (line.empty()) { ++idx; continue; }
        if (ind < indent) break;                      // 上溯到父级
        std::string body = trim(strip_comment(line));
        if (body.empty()) { ++idx; continue; }

        // 键值分割：找 "key: " 或行尾 "key:"（分组）
        size_t colon = std::string::npos;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == ':') {
                size_t next = i + 1;
                if (next >= body.size() || body[next] == ' ') { colon = i; break; }
            }
        }
        if (colon == std::string::npos) { ++idx; continue; }   // 非 key:value 行，跳过
        std::string key = trim(body.substr(0, colon));
        std::string rest = trim(body.substr(colon + 1));

        if (rest.empty()) {
            // 分组：子项缩进更大。解析子块。
            size_t child_indent = std::string::npos;
            for (size_t k = idx + 1; k < lines.size(); ++k) {
                size_t ci = indent_of(lines[k]);
                if (ci > ind) { child_indent = ci; break; }
            }
            size_t old = idx;
            ++idx;   // 跳过分组头
            if (child_indent != std::string::npos) {
                obj[key] = parse_block(lines, idx, child_indent);
            } else {
                obj[key] = json(nullptr);
            }
            (void)old;
        } else {
            obj[key] = to_value(rest);
            ++idx;
        }
    }
    return obj;
}

inline json parse(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    size_t idx = 0;
    return parse_block(lines, idx, 0);
}
}  // namespace yaml_light

// ========== 配置管理 ==========

inline AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open config: " + path);
    }

    std::stringstream ss;
    ss << f.rdbuf();
    json j = yaml_light::parse(ss.str());

    // 音频配置
    if (j.contains("audio")) {
        auto& a = j["audio"];
        cfg.audio_device = a.value("device", 0);
        cfg.audio_sample_rate = a.value("sample_rate", 48000);
        cfg.audio_channels = a.value("channels", 1);
        cfg.audio_buffer_ms = a.value("buffer_ms", 10);
    }

    // VAD 参数
    if (j.contains("vad")) {
        auto& v = j["vad"];
        cfg.vad_threshold = v.value("threshold", 0.5f);
        cfg.vad_min_speech_ms = v.value("min_speech_ms", 220);
        cfg.vad_min_silence_ms = v.value("min_silence_ms", 300);
    }

    // 打断参数
    if (j.contains("barge_in")) {
        auto& b = j["barge_in"];
        cfg.barge_in_min_ms = b.value("min_ms", 160);
        cfg.backchannel_max_ms = b.value("backchannel_max_ms", 600);
        cfg.coherence_max = b.value("coherence_max", 0.6f);
        cfg.fade_out_ms = b.value("fade_out_ms", 40);
    }

    // EOU 参数
    if (j.contains("eou")) {
        auto& e = j["eou"];
        cfg.eou_fast_ms = e.value("fast_ms", 350);
        cfg.eou_force_ms = e.value("force_ms", 900);
        cfg.max_utterance_ms = e.value("max_utterance_ms", 20000);
    }

    // 模型路径
    cfg.vad_model = j.value("vad_model", "models/vad/silero_vad.onnx");
    cfg.asr_model = j.value("asr_model", "models/asr/whisper-tiny");
    cfg.tts_model = j.value("tts_model", "models/tts/kokoro-multi-lang-v1_0");
    cfg.llm_model = j.value("llm_model", "models/qwen3-4b-q4_k_m.gguf");

    // 搜索
    if (j.contains("search")) {
        auto& s = j["search"];
        cfg.searxng_url = s.value("searxng_url", "http://localhost:8080");
        cfg.tavily_key = s.value("tavily_key", "");
        cfg.brave_key = s.value("brave_key", "");
    }

    // 日志
    cfg.log_level = j.value("log_level", "info");
    cfg.log_file = j.value("log_file", "");

    return cfg;
}

// 打印配置（用于调试）
inline void print_config(const AppConfig& cfg) {
    LOG_INFO("=== Configuration ===");
    LOG_INFO("Audio: device={}, rate={}, channels={}, buffer={}ms",
             cfg.audio_device, cfg.audio_sample_rate, cfg.audio_channels, cfg.audio_buffer_ms);
    LOG_INFO("VAD: threshold={}, min_speech={}ms, min_silence={}ms",
             cfg.vad_threshold, cfg.vad_min_speech_ms, cfg.vad_min_silence_ms);
    LOG_INFO("Barge-in: min={}ms, backchannel_max={}ms, fade_out={}ms",
             cfg.barge_in_min_ms, cfg.backchannel_max_ms, cfg.fade_out_ms);
    LOG_INFO("EOU: fast={}ms, force={}ms, max_utterance={}ms",
             cfg.eou_fast_ms, cfg.eou_force_ms, cfg.max_utterance_ms);
    LOG_INFO("Models: VAD={}, ASR={}, TTS={}, LLM={}",
             cfg.vad_model, cfg.asr_model, cfg.tts_model, cfg.llm_model);
    LOG_INFO("Search: searxng={}", cfg.searxng_url);
}
