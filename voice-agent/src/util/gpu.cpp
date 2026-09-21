// src/util/gpu.cpp
#include "util/gpu.hpp"
#include "util/log.hpp"
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif
#ifdef USE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace voice_agent {
namespace {

#ifdef _WIN32
using OrtAppendDmlFn = OrtStatus* (ORT_API_CALL*)(OrtSessionOptions*, int);

// DirectML EP 注册函数只在 DirectML 版 onnxruntime.dll 中导出；
// 用 GetProcAddress 动态解析，CPU 版 DLL 或未加载时返回空，上层自然回退 CPU。
OrtAppendDmlFn dml_fn() {
    static OrtAppendDmlFn fn = []() -> OrtAppendDmlFn {
        HMODULE h = GetModuleHandleW(L"onnxruntime.dll");
        if (!h) return nullptr;
        return reinterpret_cast<OrtAppendDmlFn>(
            GetProcAddress(h, "OrtSessionOptionsAppendExecutionProvider_DML"));
    }();
    return fn;
}
#endif

}  // namespace

bool ort_dml_available() {
#ifdef _WIN32
    return dml_fn() != nullptr;
#else
    return false;
#endif
}

bool ort_try_append_dml(void* options, int device_id) {
#ifdef _WIN32
    auto fn = dml_fn();
    if (!fn || !options) return false;
    auto* so = static_cast<Ort::SessionOptions*>(options);
    auto st = fn(static_cast<OrtSessionOptions*>(*so), device_id);
    if (st) {
        const char* msg = Ort::GetApi().GetErrorMessage(st);
        LOG_WARN("DirectML EP 挂载失败: {}", msg ? msg : "unknown error");
        Ort::GetApi().ReleaseStatus(st);
        return false;
    }
    return true;
#else
    (void)options;
    (void)device_id;
    return false;
#endif
}

bool ort_dml_intel_adapter() {
    // 环境变量强制：VA_FORCE_TTS_DML=1 时忽略 Intel 回退，用于隔离验证
    // 当前 onnxruntime+DirectML 在 Intel 上是否真的崩溃。
    if (const char* v = std::getenv("VA_FORCE_TTS_DML"); v && v[0] == '1')
        return false;
#ifdef _WIN32
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))))
        return false;
    IDXGIAdapter1* adapter = nullptr;
    bool intel = false;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x8086)
            intel = true;
        adapter->Release();
        if (intel) break;
    }
    factory->Release();
    return intel;
#else
    return false;
#endif
}

}  // namespace voice_agent
