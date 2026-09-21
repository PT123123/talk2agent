// src/gui/model_catalog.cpp
#include "gui/model_catalog.hpp"

#include <QDir>
#include <QFileInfo>
#include <QDirIterator>

namespace voice_agent {
namespace gui {

QString modelCategoryName(ModelCategory c) {
    switch (c) {
        case ModelCategory::Vad: return QStringLiteral("VAD");
        case ModelCategory::Asr: return QStringLiteral("ASR");
        case ModelCategory::Tts: return QStringLiteral("TTS");
        case ModelCategory::Llm: return QStringLiteral("LLM");
    }
    return QStringLiteral("?");
}

QString modelCategoryConfigKey(ModelCategory c) {
    switch (c) {
        case ModelCategory::Vad: return QStringLiteral("vad_model");
        case ModelCategory::Asr: return QStringLiteral("asr_model");
        case ModelCategory::Tts: return QStringLiteral("tts_model");
        case ModelCategory::Llm: return QStringLiteral("llm_model");
    }
    return QStringLiteral("?");
}

const std::vector<ModelEntry>& downloadableModels() {
    static const std::vector<ModelEntry> kModels = {
        {
            ModelCategory::Vad,
            QStringLiteral("silero-vad"),
            QStringLiteral("Silero VAD v5"),
            QStringLiteral("轻量端到端语音活动检测（ONNX）"),
            QUrl(QStringLiteral(
                "https://models.silero.ai/models/en/en_v5.onnx")),
            QStringLiteral("vad/silero_vad.onnx"),
            117591080,
            QStringLiteral("112 MB"),
        },
        {
            ModelCategory::Asr,
            QStringLiteral("whisper-tiny-en"),
            QStringLiteral("Whisper tiny.en (ggml)"),
            QStringLiteral("极小的英文语音识别（whisper.cpp 量化）"),
            QUrl(QStringLiteral(
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin")),
            QStringLiteral("asr/ggml-tiny.en.bin"),
            81474360,  // ≈77.7 MB
            QStringLiteral("78 MB"),
        },
        {
            ModelCategory::Tts,
            QStringLiteral("kokoro-82m"),
            QStringLiteral("Kokoro 82M (ONNX)"),
            QStringLiteral("高质量轻量神经语音合成（q8f16）"),
            QUrl(QStringLiteral(
                "https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/main/onnx/model_q8f16.onnx")),
            QStringLiteral("tts/kokoro.onnx"),
            90177536,  // ≈86 MB
            QStringLiteral("86 MB"),
        },
        {
            ModelCategory::Llm,
            QStringLiteral("qwen25-05b"),
            QStringLiteral("Qwen2.5 0.5B Instruct (Q4_K_M)"),
            QStringLiteral("极小开源 Instruct 模型，1GB 内可跑"),
            QUrl(QStringLiteral(
                "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf")),
            QStringLiteral("llm/qwen2.5-0.5b-instruct-q4_k_m.gguf"),
            514850816,  // ≈491 MB
            QStringLiteral("491 MB"),
        },
        {
            ModelCategory::Llm,
            QStringLiteral("qwen25-15b"),
            QStringLiteral("Qwen2.5 1.5B Instruct (Q4_K_M)"),
            QStringLiteral("对口型更强的小模型，2GB 内可跑"),
            QUrl(QStringLiteral(
                "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf")),
            QStringLiteral("llm/qwen2.5-1.5b-instruct-q4_k_m.gguf"),
            1031917056,  // ≈984 MB
            QStringLiteral("984 MB"),
        },
        {
            ModelCategory::Llm,
            QStringLiteral("qwen25-3b"),
            QStringLiteral("Qwen2.5 3B Instruct (Q4_K_M)"),
            QStringLiteral("中等规模，理解力显著提升"),
            QUrl(QStringLiteral(
                "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf")),
            QStringLiteral("llm/qwen2.5-3b-instruct-q4_k_m.gguf"),
            2004415232,  // ≈1.87 GB
            QStringLiteral("1.87 GB"),
        },
    };
    return kModels;
}

std::vector<LocalModel> scanLocalModels() {
    std::vector<LocalModel> out;
    const QString root = QStringLiteral("models");
    QDir baseDir(root);
    if (!baseDir.exists()) return out;

    QDirIterator it(root, QDir::Files,
                    QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = baseDir.relativeFilePath(path);
        const QFileInfo fi(path);

        ModelCategory cat;
        const QString lower = fi.fileName().toLower();
        const QString parent = fi.absoluteDir().dirName().toLower();
        if (lower.endsWith(QStringLiteral(".gguf"))) {
            cat = ModelCategory::Llm;
        } else if (parent.contains(QStringLiteral("whisper")) ||
                   lower.contains(QStringLiteral("whisper"))) {
            cat = ModelCategory::Asr;
        } else if (parent.contains(QStringLiteral("kokoro")) ||
                   lower.contains(QStringLiteral("kokoro"))) {
            cat = ModelCategory::Tts;
        } else if (lower.endsWith(QStringLiteral(".onnx")) ||
                   lower.contains(QStringLiteral("silero")) ||
                   lower.contains(QStringLiteral("vad"))) {
            cat = ModelCategory::Vad;
        } else {
            continue;  // 未知文件，忽略
        }
        out.push_back({cat, rel, fi.fileName(), fi.size()});
    }
    return out;
}

QStringList categoryEntries(ModelCategory c) {
    QStringList out;
    // 内置可下载项（即使未下载也显示，便于“选择后下载”）
    for (const auto& e : downloadableModels())
        if (e.category == c) out << e.name;
    // 本地已存在模型（去重）
    for (const auto& l : scanLocalModels())
        if (l.category == c && !out.contains(l.name)) out << l.name;
    return out;
}

}  // namespace voice_agent::gui
}  // namespace voice_agent