// src/audio/audio.hpp
// 音频模块统一导出头文件

#pragma once

#include "audio_device.hpp"
#include "audio_pipeline.hpp"
#include "echo_cancellation.hpp"

// 导出主要类型
namespace voice_agent {
    using AudioConfig = ::voice_agent::AudioConfig;
    using DeviceInfo = ::voice_agent::DeviceInfo;
    using AudioDeviceManager = ::voice_agent::AudioDeviceManager;
    using AudioDevice = ::voice_agent::AudioDevice;
    using AudioPipeline = ::voice_agent::AudioPipeline;
    using FullDuplexPipeline = ::voice_agent::FullDuplexPipeline;
    using EchoCanceller = ::voice_agent::EchoCanceller;
    using AECConfig = ::voice_agent::AECConfig;
    using AECStats = ::voice_agent::AECStats;
}
