// src/util/gpu.hpp
#pragma once

namespace voice_agent {

// onnxruntime.dll 是否为 DirectML 版（是否导出 OrtSessionOptionsAppendExecutionProvider_DML）。
// DirectML 版运行时可让 VAD/ASR/TTS 在任意 GPU（含 Intel Arc 核显）上加速。
bool ort_dml_available();

// 尝试为 Ort::SessionOptions 挂载 DirectML EP（options 为 Ort::SessionOptions*）。
// 成功返回 true；DLL 不支持或 EP 初始化失败返回 false（上层应回退 CPU）。
bool ort_try_append_dml(void* options, int device_id = 0);

// 主 D3D12 适配器是否为 Intel（核显/Arc）。Intel + DirectML 对部分算子
// （如 Kokoro 的 grouped ConvTranspose）存在已知崩溃，上层可据此默认回退 CPU。
bool ort_dml_intel_adapter();

}  // namespace voice_agent
