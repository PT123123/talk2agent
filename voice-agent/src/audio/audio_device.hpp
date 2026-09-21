// src/audio/audio_device.hpp
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

namespace voice_agent {

// 音频配置
struct AudioConfig {
    int sample_rate = 48000;          // 采样率
    int channels = 1;                 // 声道数（1=单声道）
    int frames_per_buffer = 480;      // 每缓冲帧数（10ms @ 48kHz）
    int bits_per_sample = 16;         // 位深度
    std::string input_device = "";    // 输入设备名（空=默认）
    std::string output_device = "";   // 输出设备名（空=默认）
};

// 设备信息
struct DeviceInfo {
    std::string name;
    bool is_default = false;
    int id = -1;
    // miniaudio 设备 ID 原始字节（供 ma_device_init 的 pDeviceID 使用；空 = 默认设备）
    std::vector<unsigned char> id_bytes;
};

class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    // 初始化音频上下文
    bool initialize();

    // 列出输入设备
    std::vector<DeviceInfo> list_input_devices() const;

    // 列出输出设备
    std::vector<DeviceInfo> list_output_devices() const;

    // 获取默认输入设备
    DeviceInfo get_default_input_device() const;

    // 获取默认输出设备
    DeviceInfo get_default_output_device() const;

    // 检查设备是否支持指定配置
    bool is_config_supported(const AudioConfig& config, const std::string& device_name, bool is_input) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 音频设备（采集或播放）
class AudioDevice {
public:
    using DataCallback = std::function<void(const int16_t* data, size_t frames)>;
    using PlaybackCallback = std::function<size_t(int16_t* output, size_t frames)>;

    AudioDevice();
    ~AudioDevice();

    // 初始化为输入设备（采集）
    bool init_as_input(const AudioConfig& config, const std::string& device_name = "");

    // 初始化为输出设备（播放）
    bool init_as_output(const AudioConfig& config, const std::string& device_name = "");

    // 初始化为全双工设备（同时采集和播放）
    bool init_as_full_duplex(const AudioConfig& config, const std::string& input_device = "", const std::string& output_device = "");

    // 启动设备
    bool start();

    // 停止设备
    bool stop();

    // 设置数据回调（输入设备用）
    void set_input_callback(DataCallback cb);

    // 设置播放回调（输出设备用）
    void set_playback_callback(PlaybackCallback cb);

    // 获取配置
    const AudioConfig& config() const { return config_; }

    // 是否正在运行
    bool is_running() const { return running_.load(); }

    // 播放数据（通过回调实现，这里保留兼容性方法）
    size_t play(const int16_t* data, size_t frames);

private:
    // Impl 定义在 cpp 文件中（public 允许回调函数访问）
public:
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    AudioConfig config_;
    std::atomic<bool> running_{false};
};

}  // namespace voice_agent
