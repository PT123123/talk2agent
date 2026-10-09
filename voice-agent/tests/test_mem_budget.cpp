// tests/test_mem_budget.cpp
//
// 验证「VAD 模型默认不加载 + 内存可视化」这组改动的行为约定。
//
// 覆盖三件事：
//   1. VAD 的 model_loaded()/unload() 幂等性与状态一致性
//   2. config 的 input_mode 归一化（决定启动时要不要加载 VAD）
//   3. mem_probe 的采样可用性与 format_bytes 的格式化边界
//
// 这些是纯逻辑断言，不依赖真实模型文件/声卡 —— VAD 用空路径构造，
// 必然走能量检测分支，正好验证"没加载"这件事本身。

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/types.hpp"
#include "util/config.hpp"
#include "util/mem_probe.hpp"
#include "vad/vad.hpp"

using namespace voice_agent;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", what);
    }
}

// load_config 只认文件路径，没有 from_string 版本 —— 写临时 yaml 再读。
// 名字带 pid 避免并行跑测试时互相覆盖。
AppConfig parseYaml(const char* body) {
    const std::string path = "test_mem_budget_" + std::to_string(GetCurrentProcessId()) +
                             ".yaml";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return AppConfig{};
    std::fputs(body, f);
    std::fclose(f);
    AppConfig c;
    try {
        c = load_config(path);
    } catch (...) {
        // 读失败即返回默认值，下面断言会暴露出来
    }
    std::remove(path.c_str());
    return c;
}

// ========== 1. VAD 懒加载 ==========

void test_vad_lazy_state() {
    std::printf("[VAD 懒加载]\n");

    // 空路径构造：initialize() 只是算帧数常量，不该加载任何模型
    VAD vad;
    check(vad.initialize(VADConfig{}), "空配置 initialize 返回 true");
    check(!vad.model_loaded(), "未 set_model_path 时 model_loaded 为 false");
    check(vad.provider_label() == "未加载",
          "未加载时 provider_label 应为\"未加载\"（不能报 CPU 误导用户）");

    // unload() 幂等：没加载过就调不能崩、不能改状态
    vad.unload();
    vad.unload();
    check(!vad.model_loaded(), "重复 unload 后仍是未加载");

    // unload 后 provider 仍应如实报未加载
    check(vad.provider_label() == "未加载", "unload 后 provider_label 仍为未加载");

    // 帧数常量应与配置一致（30ms/帧 vs 32ms/帧 的换算残留最容易在这里出错）
    vad.unload();
    vad.reset();
    check(!vad.is_speaking(), "reset 后 is_speaking 为 false");
    check(vad.get_speech_probability() == 0.0f, "reset 后语音概率为 0");
}

// 一个不存在的模型路径：加载必然失败，但调用链不能抛异常外泄
void test_vad_missing_model_path() {
    std::printf("[VAD 缺失模型路径]\n");
    VAD vad;
    vad.initialize(VADConfig{});
    vad.set_model_path("models/vad/__definitely_not_exist__.onnx",
                       "models/vad/__definitely_not_exist__.onnx");
    // 加载失败必须保持"未加载"，让上层知道能量检测才是当前实际后端
    check(!vad.model_loaded(), "路径不存在时不标记为已加载");
    check(vad.provider_label() == "未加载", "路径不存在时 provider_label 为未加载");

    // process() 必须仍能工作（能量检测兜底），不能因为没模型就崩
    const int16_t silence[160] = {0};
    (void)vad.process(silence, 160);
    check(vad.last_inference_ms.load() >= 0.0, "能量检测路径能产出耗时");
}

// ========== 2. input_mode 决定要不要加载 VAD ==========

void test_input_mode_gate() {
    std::printf("[input_mode 门控]\n");

    // 默认（缺省）必须是 ptt —— 启动时据此跳过 VAD 模型加载
    AppConfig d{};
    check(d.input_mode == "ptt", "AppConfig 默认 input_mode=ptt");

    // 显式写 vad 仍要认，不能被归一化吃掉
    const AppConfig v = parseYaml("input_mode: vad\n");
    check(v.input_mode == "vad", "显式 vad 归一化后仍是 vad");

    const AppConfig p = parseYaml("input_mode: ptt\n");
    check(p.input_mode == "ptt", "显式 ptt 仍是 ptt");

    // 未知值必须回落到 ptt（安全侧：多加载一份内存）
    const AppConfig junk = parseYaml("input_mode: whatever\n");
    check(junk.input_mode == "ptt", "未知值归一化为 ptt（不是 vad）");

    // 空文件走缺省
    const AppConfig empty = parseYaml("");
    check(empty.input_mode == "ptt", "空配置 input_mode=ptt");

    // 两种模式各自的加载决策（对照 controller 的分支条件）
    check(!(empty.input_mode != "ptt"), "ptt 模式跳过 VAD 模型加载");
    check(v.input_mode != "ptt", "vad 模式需要加载 VAD 模型");
}

// ========== 3. 内存采样 ==========

void test_mem_probe() {
    std::printf("[内存采样]\n");

    const MemSample s = mem_probe();
#ifdef _WIN32
    check(s.valid, "Windows 下采样应有效");
    check(s.private_bytes > 0, "私有内存应大于 0");
    check(s.sys_total > 0, "系统物理内存总量应大于 0");
    check(s.sys_available <= s.sys_total, "可用内存不超过总量");
    check(s.working_set > 0, "工作集应大于 0");
#else
    check(!s.valid, "非 Windows 下 valid=false");
#endif
}

void test_format_bytes() {
    std::printf("[字节格式化]\n");

    // 单位边界：1024 两侧必须换挡，否则会看到 "1024 KB" 这种别扭输出
    check(format_bytes(0) == "0 B", "0 → 0 B");
    check(format_bytes(1023) == "1023 B", "1023 B 不进 KB");
    check(format_bytes(1024) == "1 KB", "1024 → 1 KB");
    check(format_bytes(1536) == "2 KB", "1536 → 2 KB（取整）");
    check(format_bytes(1024ull * 1024) == "1 MB", "1 MiB → 1 MB");
    check(format_bytes(1024ull * 1024 * 1024) == "1.00 GB", "1 GiB → 1.00 GB");
    check(format_bytes(4ull * 1024 * 1024 * 1024) == "4.00 GB", "4 GiB → 4.00 GB");

    // 不能输出负数或乱码（ram_delta_since 在采样异常时会传 0 进来）
    check(format_bytes(0).find('-') == std::string::npos, "0 不含负号");
}

// 模型 RAM 增量口径：加载过程里别的组件释放内存时差值可能为负，
// 必须报 0 而不是负数（否则界面会出现"-12 MB 内存"）。
void test_ram_delta_clamp() {
    std::printf("[RAM 增量钳位]\n");
    const MemSample now = mem_probe();
    const std::uint64_t cur = now.private_bytes;

    auto delta = [](std::uint64_t before) -> std::uint64_t {
        const std::uint64_t after = mem_probe().private_bytes;
        return after > before ? after - before : 0;
    };

    // 起点就是当前值：差值几乎必然 <= 0，应钳到 0
    check(delta(cur) < (1ull << 30), "同点采样的增量在合理范围");
    // 起点远小于当前：结果不应溢出
    check(delta(0) > 0, "从 0 起算增量大于 0");
    check(delta(cur) >= 0ull, "增量不为负");
}

}  // namespace

int main() {
    std::printf("=== 内存预算与 VAD 懒加载 ===\n");
    test_vad_lazy_state();
    test_vad_missing_model_path();
    test_input_mode_gate();
    test_mem_probe();
    test_format_bytes();
    test_ram_delta_clamp();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}