// src/gui/model_catalog.cpp
#include "gui/model_catalog.hpp"

#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <set>

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
            {},
            false,
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
            {},
            false,
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
            {},
            false,
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
            {},
            false,
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
            {},
            false,
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
            {},
            false,
            2004415232,  // ≈1.87 GB
            QStringLiteral("1.87 GB"),
        },
        {
            ModelCategory::Vad,
            QStringLiteral("ten-vad"),
            QStringLiteral("TEN VAD (ONNX)"),
            QStringLiteral("324KB 端到端流式 VAD，官方称精度优于 Silero"),
            QUrl(QStringLiteral(
                "https://huggingface.co/TEN-framework/ten-vad/resolve/main/src/onnx_model/ten-vad.onnx")),
            QStringLiteral("vad/ten-vad.onnx"),
            {},
            false,
            331776,  // ≈324 KB
            QStringLiteral("324 KB"),
        },
        {
            ModelCategory::Asr,
            QStringLiteral("sense-voice-small"),
            QStringLiteral("SenseVoice-small (int8)"),
            QStringLiteral("中文 CER 7.99% 的轻量识别，sherpa-onnx 官方导出"),
            QUrl(QStringLiteral(
                "https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/main/model.int8.onnx")),
            QStringLiteral("asr/sense-voice/model.int8.onnx"),
            {
                {QUrl(QStringLiteral(
                     "https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/main/tokens.txt")),
                 QStringLiteral("asr/sense-voice/tokens.txt")},
            },
            false,
            250609664,  // ≈239 MB
            QStringLiteral("239 MB"),
        },
        {
            ModelCategory::Asr,
            QStringLiteral("moonshine-base-zh"),
            QStringLiteral("Moonshine Base zh (int8)"),
            QStringLiteral("最小中文识别包，下载后自动解压"),
            QUrl(QStringLiteral(
                "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-base-zh-quantized-2026-02-27.tar.bz2")),
            QStringLiteral("asr/moonshine-zh/sherpa-onnx-moonshine-base-zh-quantized-2026-02-27.tar.bz2"),
            {},          // 附加文件：无（压缩包内自带 decoder/encoder/tokens）
            true,        // 下载后 tar 解压
            99905420,   // ≈95 MB
            QStringLiteral("95 MB"),
        },
        {
            ModelCategory::Llm,
            QStringLiteral("minicpm5-1b"),
            QStringLiteral("MiniCPM5 1B Instruct (Q4_K_M)"),
            QStringLiteral("清华 OpenBMB 轻量中文对话模型"),
            QUrl(QStringLiteral(
                "https://huggingface.co/openbmb/MiniCPM5-1B-GGUF/resolve/main/MiniCPM5-1B-Q4_K_M.gguf")),
            QStringLiteral("llm/MiniCPM5-1B-Q4_K_M.gguf"),
            {},
            false,
            721420288,  // ≈688 MB
            QStringLiteral("688 MB"),
        },
        {
            ModelCategory::Tts,
            QStringLiteral("piper-zh-huayan"),
            QStringLiteral("Piper zh_CN-huayan (medium)"),
            QStringLiteral("极轻量 VITS 中文语音，sherpa-onnx 官方打包（含 espeak-ng-data），CPU 实时合成"),
            QUrl(QStringLiteral(
                "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-zh_CN-huayan-medium.tar.bz2")),
            QStringLiteral("tts/piper-zh_CN-huayan-medium/vits-piper-zh_CN-huayan-medium.tar.bz2"),
            {},          // 压缩包内含 onnx + tokens + espeak-ng-data
            true,        // 下载后 tar 解压
            67255926,   // ≈64 MB
            QStringLiteral("64 MB"),
        },
    };
    return kModels;
}

namespace {
// 是否算作可选的模型文件（排除 tokens.txt / .json 等伴随文件与 .part 临时文件）
bool isModelFile(const QString& fileName) {
    const QString lower = fileName.toLower();
    return lower.endsWith(QStringLiteral(".gguf")) ||
           lower.endsWith(QStringLiteral(".onnx")) ||
           lower.endsWith(QStringLiteral(".bin")) ||
           lower.endsWith(QStringLiteral(".ggml"));
}

// 目录递归占用总字节数
qint64 dirTotalBytes(const QString& dir) {
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files,
                    QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) total += QFileInfo(it.next()).size();
    return total;
}
}  // namespace

std::vector<LocalModel> scanLocalModels() {
    std::vector<LocalModel> out;
    const QString root = QStringLiteral("models");
    QDir baseDir(root);
    if (!baseDir.exists()) return out;

    std::set<QString> seenDirs;  // 目录型条目去重
    QDirIterator it(root, QDir::Files,
                    QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = baseDir.relativeFilePath(path);
        const QFileInfo fi(path);

        if (!isModelFile(fi.fileName())) continue;

        // 按目标子目录分类（下载清单统一使用 vad/ asr/ tts/ llm/ 前缀），
        // 兼顾旧路径下的扩展名推断
        const QString relLower = rel.toLower();
        ModelCategory cat;
        if (relLower.startsWith(QStringLiteral("llm/"))) {
            cat = ModelCategory::Llm;
        } else if (relLower.startsWith(QStringLiteral("asr/"))) {
            cat = ModelCategory::Asr;
        } else if (relLower.startsWith(QStringLiteral("tts/"))) {
            cat = ModelCategory::Tts;
        } else if (relLower.startsWith(QStringLiteral("vad/"))) {
            cat = ModelCategory::Vad;
        } else if (fi.fileName().toLower().endsWith(QStringLiteral(".gguf"))) {
            cat = ModelCategory::Llm;
        } else {
            cat = ModelCategory::Vad;  // 兜底：其余模型文件视为 ONNX 类
        }

        if (cat == ModelCategory::Asr || cat == ModelCategory::Tts) {
            // 目录型模型：以“模型目录”为单位列出（ASR/TTS 引擎按目录加载）。
            // Whisper 的 onnx/ 子目录需上提一级，其余模型文件直接位于模型目录内。
            QDir dir = fi.absoluteDir();
            if (dir.dirName().compare(QStringLiteral("onnx"),
                                      Qt::CaseInsensitive) == 0)
                dir = QDir(dir.absolutePath());
            const QString dirRel = baseDir.relativeFilePath(dir.absolutePath());
            if (seenDirs.insert(dirRel).second) {
                out.push_back({cat, dirRel, dir.dirName(),
                               dirTotalBytes(dir.absolutePath())});
            }
        } else {
            out.push_back({cat, rel, fi.fileName(), fi.size()});
        }
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