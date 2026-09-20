# download_audio_libs.ps1 - 下载音频相关第三方库
# M1 阶段需要：miniaudio + speexdsp

$ErrorActionPreference = 'Stop'
$libs_dir = Split-Path -Parent $MyInvocation.MyCommand.Path | Join-Path -ChildPath "third_party"

Write-Host "下载音频第三方库到: $libs_dir"

# 1. miniaudio - 轻量级音频库（单头文件 + C 实现）
$miniaudio_dir = Join-Path $libs_dir "miniaudio"
if (-not (Test-Path $miniaudio_dir)) {
    New-Item -ItemType Directory -Path $miniaudio_dir -Force | Out-Null
}

# 下载 miniaudio.h
$miniaudio_url = "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
$miniaudio_path = Join-Path $miniaudio_dir "miniaudio.h"
try {
    Invoke-WebRequest -Uri $miniaudio_url -OutFile $miniaudio_path
    Write-Host "[OK] miniaudio.h 下载成功"
} catch {
    Write-Warning "下载 miniaudio.h 失败: $_"
}

# 2. speexdsp - 回声消除（AEC）算法库
$speexdsp_dir = Join-Path $libs_dir "speexdsp"
if (-not (Test-Path $speexdsp_dir)) {
    New-Item -ItemType Directory -Path $speexdsp_dir -Force | Out-Null
}

# 下载 speexdsp 源码
$speex_url = "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/fftwrap.c"
$speex_path = Join-Path $speexdsp_dir "fftwrap.c"
try {
    Invoke-WebRequest -Uri $speex_url -OutFile $speex_path
    Write-Host "[OK] fftwrap.c 下载成功"
} catch {
    Write-Warning "下载 fftwrap.c 失败: $_"
}

# 下载其他 speexdsp 源文件
$speex_files = @(
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/buffer.c",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/filterbank.c",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/kiss_fft.c",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/mdf.c",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/fftwrap.h",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/filterbank.h",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/kiss_fft.h",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/mdf.h",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/preprocess.c",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/preprocess.h",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/resample.c",
    "https://raw.githubusercontent.com/xiph/speexdsp/SpeexDSP-1.2.0/libspeexdsp/resample.h"
)

foreach ($file_url in $speex_files) {
    $file_name = Split-Path -Leaf $file_url
    $file_path = Join-Path $speexdsp_dir $file_name
    try {
        Invoke-WebRequest -Uri $file_url -OutFile $file_path
        Write-Host "[OK] $file_name 下载成功"
    } catch {
        Write-Warning "下载 $file_name 失败: $_"
    }
}

Write-Host ""
Write-Host "下载完成！"
Write-Host "注意：某些文件可能需要从 GitHub 仓库克隆完整源码"
