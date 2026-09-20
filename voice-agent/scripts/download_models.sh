#!/bin/bash
# scripts/download_models.sh
# 模型下载脚本（Windows 上用 Git Bash 或 WSL 运行）
# 
# Intel Arc 显存建议：使用 Qwen3-1.5B Q4_K_M（~1GB）
# 显存充足时：使用 Qwen3-4B Q4_K_M（~2.5GB）

set -e

MODELS_DIR="models"
mkdir -p "$MODELS_DIR"

echo "============================================"
echo "Voice Agent - 模型下载脚本"
echo "============================================"

# ========== 辅助函数 ==========
download_file() {
    local url=$1
    local output=$2
    local name=$3

    if [ -f "$output" ]; then
        echo "[SKIP] $name already exists"
        return 0
    fi

    echo "[DOWNLOAD] $name..."
    curl -L -o "$output" "$url" --progress-bar
    echo "[DONE] $name"
}

# ========== VAD 模型 ==========
echo ""
echo "=== VAD Models ==="

# TEN VAD
download_file \
    "https://huggingface.co/rhasspy/ten-vad/resolve/main/ten_vad_v2.onnx" \
    "$MODELS_DIR/ten-vad.onnx" \
    "TEN VAD"

# Silero VAD v5
download_file \
    "https://github.com/snakers4/silero-vad/releases/download/v5.0.0/silero_vad.onnx" \
    "$MODELS_DIR/silero_vad.onnx" \
    "Silero VAD v5"

# ========== Smart Turn ==========
echo ""
echo "=== Turn Detection ==="

download_file \
    "https://github.com/pipecat-ai/pipecat/releases/download/smart_turn_v3.2/smart_turn.onnx" \
    "$MODELS_DIR/smart_turn.onnx" \
    "Smart Turn v3.2"

# ========== ASR 模型 ==========
echo ""
echo "=== ASR Models ==="

# SenseVoice (中文/英文/日文/韩文/粤语)
download_file \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2" \
    "$MODELS_DIR/sensevoice.tar.bz2" \
    "SenseVoice (tar.bz2)"

# 解压
echo "[EXTRACT] SenseVoice..."
if command -v tar &> /dev/null; then
    tar -xjf "$MODELS_DIR/sensevoice.tar.bz2" -C "$MODELS_DIR"
    mv "$MODELS_DIR/sherpa-onnx-sense-voice"* "$MODELS_DIR/sensevoice"
    rm "$MODELS_DIR/sensevoice.tar.bz2"
    echo "[DONE] SenseVoice extracted"
else
    echo "[WARN] tar not found, please extract manually"
fi

# Paraformer 流式（备选）
# download_file \
#     "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/streaming-paraformer-bilingual-zh-en.tar.bz2" \
#     "$MODELS_DIR/paraformer.tar.bz2" \
#     "Paraformer Streaming"

# ========== TTS 模型 ==========
echo ""
echo "=== TTS Models ==="

# Kokoro TTS (默认，82M)
download_file \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-v1.0-onnx.tar.bz2" \
    "$MODELS_DIR/kokoro.tar.bz2" \
    "Kokoro TTS (tar.bz2)"

echo "[EXTRACT] Kokoro..."
if command -v tar &> /dev/null; then
    tar -xjf "$MODELS_DIR/kokoro.tar.bz2" -C "$MODELS_DIR"
    mv "$MODELS_DIR/kokoro-v1.0-onnx" "$MODELS_DIR/kokoro"
    rm "$MODELS_DIR/kokoro.tar.bz2"
    echo "[DONE] Kokoro extracted"
else
    echo "[WARN] tar not found, please extract manually"
fi

# ========== LLM 模型 ==========
echo ""
echo "=== LLM Models ==="

# 模型选择提示
echo "LLM 模型选择："
echo "  1) Qwen3-1.5B Q4_K_M (~1GB) - Intel Arc 2GB 推荐"
echo "  2) Qwen3-4B Q4_K_M (~2.5GB) - 需要更大显存"
echo "  3) 跳过（稍后手动下载）"
read -p "请选择 [1-3]: " choice

case $choice in
    1)
        echo "[INFO] Downloading Qwen3-1.5B..."
        # HuggingFace Qwen3-1.5B GGUF
        download_file \
            "https://huggingface.co/Qwen/Qwen3-1.5B-GGUF/resolve/main/qwen3-1.5b-q4_k_m.gguf" \
            "$MODELS_DIR/qwen3-1.5b-q4_k_m.gguf" \
            "Qwen3-1.5B Q4_K_M"
        ;;
    2)
        echo "[INFO] Downloading Qwen3-4B..."
        download_file \
            "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/qwen3-4b-q4_k_m.gguf" \
            "$MODELS_DIR/qwen3-4b-q4_k_m.gguf" \
            "Qwen3-4B Q4_K_M"
        ;;
    *)
        echo "[SKIP] LLM download skipped"
        ;;
esac

# ========== Embedding/Rerank（可选）==========
echo ""
echo "=== Embedding Models (Optional) ==="
read -p "下载 Qwen3 Embedding 模型? [y/N]: " yn
if [[ "$yn" =~ ^[Yy]$ ]]; then
    download_file \
        "https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF/resolve/main/qwen3-embedding-0.6b-q4_k_m.gguf" \
        "$MODELS_DIR/qwen3-embedding-0.6b-q4_k_m.gguf" \
        "Qwen3 Embedding 0.6B"
fi

# ========== 完成 ==========
echo ""
echo "============================================"
echo "下载完成！"
echo "============================================"
echo ""
echo "模型目录：$MODELS_DIR"
ls -la "$MODELS_DIR"
echo ""
echo "下一步："
echo "  1. 安装依赖：vcpkg install 或手动下载第三方库"
echo "  2. 构建：cmake -B build && cmake --build build"
echo "  3. 运行：./build/voice-agent"
