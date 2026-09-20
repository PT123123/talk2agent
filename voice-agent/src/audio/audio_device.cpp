// src/audio/audio_device.cpp
#include "audio_device.hpp"
#include "util/log.hpp"
#include <cstring>

// miniaudio 需要在包含 miniaudio.h 之前定义实现宏
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace voice_agent {

// ========== AudioDeviceManager 实现 ==========

struct AudioDeviceManager::Impl {
    ma_context context;
    bool initialized = false;
};

AudioDeviceManager::AudioDeviceManager() : impl_(std::make_unique<Impl>()) {}

AudioDeviceManager::~AudioDeviceManager() {
    if (impl_->initialized) {
        ma_context_uninit(&impl_->context);
    }
}

bool AudioDeviceManager::initialize() {
    if (impl_->initialized) {
        return true;
    }

    ma_context_config ctx_config = ma_context_config_init();
    ma_result result = ma_context_init(nullptr, 0, &ctx_config, &impl_->context);
    
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize audio context: {}", static_cast<int>(result));
        return false;
    }

    impl_->initialized = true;
    LOG_INFO("Audio context initialized successfully");
    return true;
}

std::vector<DeviceInfo> AudioDeviceManager::list_input_devices() const {
    std::vector<DeviceInfo> devices;
    if (!impl_->initialized) return devices;

    // v0.11.25 API: 分别获取 playback 和 capture 设备
    ma_device_info* playback_infos = nullptr;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    
    ma_result result = ma_context_get_devices(&impl_->context, 
        &playback_infos, &playback_count, 
        &capture_infos, &capture_count);
    
    if (result != MA_SUCCESS) {
        LOG_WARN("Failed to get input devices");
        return devices;
    }

    for (ma_uint32 i = 0; i < capture_count; i++) {
        DeviceInfo info;
        info.name = capture_infos[i].name;
        info.id = static_cast<int>(i);
        info.is_default = (i == 0);
        devices.push_back(info);
    }

    return devices;
}

std::vector<DeviceInfo> AudioDeviceManager::list_output_devices() const {
    std::vector<DeviceInfo> devices;
    if (!impl_->initialized) return devices;

    // v0.11.25 API: 分别获取 playback 和 capture 设备
    ma_device_info* playback_infos = nullptr;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    
    ma_result result = ma_context_get_devices(&impl_->context, 
        &playback_infos, &playback_count, 
        &capture_infos, &capture_count);
    
    if (result != MA_SUCCESS) {
        LOG_WARN("Failed to get output devices");
        return devices;
    }

    for (ma_uint32 i = 0; i < playback_count; i++) {
        DeviceInfo info;
        info.name = playback_infos[i].name;
        info.id = static_cast<int>(i);
        info.is_default = (i == 0);
        devices.push_back(info);
    }

    return devices;
}

DeviceInfo AudioDeviceManager::get_default_input_device() const {
    DeviceInfo info;
    auto devices = list_input_devices();
    if (!devices.empty()) {
        for (const auto& d : devices) {
            if (d.is_default) return d;
        }
        return devices[0];
    }
    return info;
}

DeviceInfo AudioDeviceManager::get_default_output_device() const {
    DeviceInfo info;
    auto devices = list_output_devices();
    if (!devices.empty()) {
        for (const auto& d : devices) {
            if (d.is_default) return d;
        }
        return devices[0];
    }
    return info;
}

bool AudioDeviceManager::is_config_supported(const AudioConfig& config, 
                                              const std::string& device_name, 
                                              bool is_input) const {
    (void)device_name;
    (void)is_input;
    // 简化实现：假设大多数设备支持标准配置
    return config.sample_rate == 48000 || config.sample_rate == 16000 || config.sample_rate == 44100;
}

// ========== AudioDevice 实现 ==========

struct AudioDevice::Impl {
    ma_device device;
    DataCallback input_callback;
    PlaybackCallback playback_callback;
    std::atomic<bool> is_input_mode{false};
    std::atomic<bool> is_output_mode{false};
    std::atomic<bool> is_duplex_mode{false};
    std::atomic<bool> running{false};
    
    Impl() : device{} {}
    
    // 内部数据处理方法
    void handle_input_data(const int16_t* input, ma_uint32 frame_count) {
        if (input_callback && input) {
            input_callback(input, frame_count);
        }
    }
    
    void handle_output_data(int16_t* output, ma_uint32 frame_count, ma_uint32 channels) {
        if (playback_callback && output) {
            playback_callback(output, frame_count);
        } else if (output) {
            std::memset(output, 0, frame_count * channels * sizeof(int16_t));
        }
    }
};

// 回调函数 - 封装 Impl 访问
static void input_callback_wrapper(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    auto* impl = static_cast<AudioDevice::Impl*>(device->pUserData);
    
    // 播放端静音（采集设备不播放）
    if (output) {
        std::memset(output, 0, frame_count * device->capture.channels * sizeof(ma_int16));
    }
    
    // 调用内部处理
    impl->handle_input_data(static_cast<const int16_t*>(input), frame_count);
}

static void output_callback_wrapper(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    auto* impl = static_cast<AudioDevice::Impl*>(device->pUserData);
    
    (void)input;
    
    // 调用内部处理
    impl->handle_output_data(static_cast<int16_t*>(output), frame_count, device->playback.channels);
}

static void duplex_callback_wrapper(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    auto* impl = static_cast<AudioDevice::Impl*>(device->pUserData);
    
    // 先处理输入数据
    impl->handle_input_data(static_cast<const int16_t*>(input), frame_count);
    
    // 再处理输出数据
    impl->handle_output_data(static_cast<int16_t*>(output), frame_count, device->playback.channels);
}

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
    stop();
}

bool AudioDevice::init_as_input(const AudioConfig& config, const std::string& device_name) {
    (void)device_name;  // 暂未实现设备选择
    config_ = config;
    
    ma_device_config dev_config = ma_device_config_init(ma_device_type_capture);
    dev_config.sampleRate = static_cast<ma_uint32>(config.sample_rate);
    dev_config.periodSizeInFrames = static_cast<ma_uint32>(config.frames_per_buffer);
    dev_config.dataCallback = input_callback_wrapper;
    dev_config.pUserData = impl_.get();
    
    // 设置采样格式和声道
    dev_config.capture.format = ma_format_s16;
    dev_config.capture.channels = static_cast<ma_uint32>(config.channels);
    
    ma_result result = ma_device_init(nullptr, &dev_config, &impl_->device);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize input device: {}", static_cast<int>(result));
        return false;
    }
    
    impl_->is_input_mode = true;
    LOG_INFO("Input device initialized: {} Hz, {} ch, {} frames/buffer",
             config.sample_rate, config.channels, config.frames_per_buffer);
    return true;
}

bool AudioDevice::init_as_output(const AudioConfig& config, const std::string& device_name) {
    (void)device_name;  // 暂未实现设备选择
    config_ = config;
    
    ma_device_config dev_config = ma_device_config_init(ma_device_type_playback);
    dev_config.sampleRate = static_cast<ma_uint32>(config.sample_rate);
    dev_config.periodSizeInFrames = static_cast<ma_uint32>(config.frames_per_buffer);
    dev_config.dataCallback = output_callback_wrapper;
    dev_config.pUserData = impl_.get();
    
    // 设置采样格式和声道
    dev_config.playback.format = ma_format_s16;
    dev_config.playback.channels = static_cast<ma_uint32>(config.channels);
    
    ma_result result = ma_device_init(nullptr, &dev_config, &impl_->device);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize output device: {}", static_cast<int>(result));
        return false;
    }
    
    impl_->is_output_mode = true;
    LOG_INFO("Output device initialized: {} Hz, {} ch, {} frames/buffer",
             config.sample_rate, config.channels, config.frames_per_buffer);
    return true;
}

bool AudioDevice::init_as_full_duplex(const AudioConfig& config, 
                                       const std::string& input_device, 
                                       const std::string& output_device) {
    (void)input_device;  // 暂未实现设备选择
    (void)output_device;
    config_ = config;
    
    ma_device_config dev_config = ma_device_config_init(ma_device_type_duplex);
    dev_config.sampleRate = static_cast<ma_uint32>(config.sample_rate);
    dev_config.periodSizeInFrames = static_cast<ma_uint32>(config.frames_per_buffer);
    dev_config.dataCallback = duplex_callback_wrapper;
    dev_config.pUserData = impl_.get();
    
    // 设置采集端格式和声道
    dev_config.capture.format = ma_format_s16;
    dev_config.capture.channels = static_cast<ma_uint32>(config.channels);
    
    // 设置播放端格式和声道
    dev_config.playback.format = ma_format_s16;
    dev_config.playback.channels = static_cast<ma_uint32>(config.channels);
    
    ma_result result = ma_device_init(nullptr, &dev_config, &impl_->device);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize duplex device: {}", static_cast<int>(result));
        return false;
    }
    
    impl_->is_duplex_mode = true;
    LOG_INFO("Full duplex device initialized: {} Hz, {} ch, {} frames/buffer",
             config.sample_rate, config.channels, config.frames_per_buffer);
    return true;
}

bool AudioDevice::start() {
    ma_result result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to start device: {}", static_cast<int>(result));
        return false;
    }
    impl_->running = true;
    running_ = true;
    LOG_INFO("Audio device started");
    return true;
}

bool AudioDevice::stop() {
    if (!running_) return true;
    
    ma_device_stop(&impl_->device);
    // 不调用 ma_device_uninit()，保持设备初始化状态以便重新 start
    impl_->running = false;
    running_ = false;
    
    LOG_INFO("Audio device stopped (device retained for restart)");
    return true;
}

void AudioDevice::set_input_callback(DataCallback cb) {
    impl_->input_callback = std::move(cb);
}

void AudioDevice::set_playback_callback(PlaybackCallback cb) {
    impl_->playback_callback = std::move(cb);
}

size_t AudioDevice::play(const int16_t* data, size_t frames) {
    // 对于输出设备，播放通过回调方式实现
    // 此方法保留用于兼容性
    (void)data;
    if (!impl_->is_output_mode || !running_) {
        return 0;
    }
    LOG_WARN("Direct play() called - use set_playback_callback instead for proper duplex operation");
    return frames;
}

}  // namespace voice_agent
