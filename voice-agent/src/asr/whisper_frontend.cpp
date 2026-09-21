// src/asr/whisper_frontend.cpp
#include "asr/whisper_frontend.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace voice_agent {

// =====================================================
// OpenAI Whisper 前端（与官方 log_mel_spectrogram 对齐）
//  - N_FFT=400 / HOP=160 / N_MELS=80 / fmax=8000
//  - 周期 Hann 窗
//  - Slaney 归一化 mel 滤波器组（复刻 librosa.filters.mel）
// 处理链：PCM → |STFT|² → mel@power → log10(clamp≥1e-10)
//        → clamp(max-8) → (x+4)/4
// =====================================================

namespace {

constexpr double kPi = 3.14159265358979323846;

void fft_radix2(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<double> u = a[i + j];
                std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}

// 对 400 点帧做一次 DFT，只算低 n_bins=n_fft/2+1 个 bin（奈奎斯特以下）
// 400 = 2^4 * 25 非 2 的幂，用直接 DFT（每帧 n_fft*n_bins 次复乘，足够快）
void dft_lowbin(const double* x, std::complex<double>* bin, int n_bins) {
    const int N = WhisperFrontend::kNfft;
    for (int k = 0; k < n_bins; ++k) {
        double re = 0.0, im = 0.0;
        const double w = -2.0 * kPi * k / N;
        // 用递推 sin/cos 减少重算
        double cs = std::cos(w), sn = std::sin(w);
        double c = 1.0, s = 0.0;
        for (int n = 0; n < N; ++n) {
            re += x[n] * c;
            im += x[n] * s;
            // 旋转 (c,s) *= exp(j w)
            double nc = c * cs - s * sn;
            s = c * sn + s * cs;
            c = nc;
        }
        bin[k] = std::complex<double>(re, im);
    }
}

double hz_to_mel_slaney(double f) {
    const double f_min = 0.0, f_sp = 200.0 / 3;
    double mel = (f - f_min) / f_sp;
    const double min_log_hz = 1000.0;
    const double logstep = std::log(6.4) / 27.0;
    if (f >= min_log_hz) {
        mel = (min_log_hz - f_min) / f_sp + std::log(f / min_log_hz) / logstep;
    }
    return mel;
}

double mel_to_hz_slaney(double m) {
    const double f_min = 0.0, f_sp = 200.0 / 3;
    const double min_log_hz = 1000.0;
    const double logstep = std::log(6.4) / 27.0;
    double f = f_min + f_sp * m;
    const double min_log_mel = (min_log_hz - f_min) / f_sp;
    if (m >= min_log_mel) {
        f = min_log_hz * std::exp(logstep * (m - min_log_mel));
    }
    return f;
}

}  // namespace

void WhisperFrontend::init() const {
    if (initialized_) return;
    // Slaney 归一化 mel 滤波器组（librosa filters.mel, norm='slaney'）
    const int n_freqs = kNfft / 2 + 1;               // 201
    constexpr int n_mels = kNmels;                   // 80
    const double fmax = 8000.0;
    double mel_low = hz_to_mel_slaney(0.0);
    double mel_high = hz_to_mel_slaney(fmax);
    std::vector<double> mel(n_mels + 2);
    for (int i = 0; i < n_mels + 2; ++i)
        mel[i] = mel_low + (mel_high - mel_low) * i / (n_mels + 1.0);
    std::vector<double> mel_hz(n_mels + 2);
    for (int i = 0; i < n_mels + 2; ++i) mel_hz[i] = mel_to_hz_slaney(mel[i]);

    filterbank_.assign((size_t)n_mels * n_freqs, 0.0f);
    // enorm = 2.0 / (mel_hz[2:] - mel_hz[:-2])
    for (int m = 0; m < n_mels; ++m) {
        const double hz_c   = mel_hz[m + 1];
        const double hz_lo  = mel_hz[m];
        const double hz_hi  = mel_hz[m + 2];
        const double left_m = mel_hz[m];
        const double width_m = hz_hi - hz_lo;
        const double enorm  = 2.0 / (hz_hi - hz_lo);
        float* row = filterbank_.data() + (size_t)m * n_freqs;
        // 找 bin 边界：idx = floor((n_fft+1)*hz / sr)
        auto bin_of = [this](double hz) {
            return (int)std::floor((kNfft + 1.0) * hz / kSampleRate);
        };
        int li = std::max(0, bin_of(mel_hz[m]));
        int ci = std::max(0, bin_of(mel_hz[m + 1]));
        int ri = std::max(0, bin_of(mel_hz[m + 2]));
        // 频率轴 bins（Hz）
        std::vector<double> fb(n_freqs);
        for (int k = 0; k < n_freqs; ++k) fb[k] = kSampleRate * 0.5 * k / (n_freqs - 1.0);
        for (int k = li; k < ri; ++k) {
            double v = 0.0;
            if (k < ci && ci > li) {
                v = (fb[k] - mel_hz[m]) / (hz_c - left_m);
            } else if (k >= ci && ri > ci) {
                v = (hz_hi - fb[k]) / (hz_hi - hz_c);
            }
            row[k] = (float)(v * enorm);
        }
    }
    initialized_ = true;
}

void WhisperFrontend::ensure_initialized() const {
    if (!initialized_) init();
}

std::vector<float> WhisperFrontend::compute(const int16_t* pcm,
                                            size_t frames) const {
    ensure_initialized();
    std::vector<float> pcmf(frames);
    for (size_t i = 0; i < frames; ++i) pcmf[i] = pcm[i] / 32768.0f;

    const size_t n_frames = 1 + (frames - kNfft) / kHop;
    const int n_freqs = kNfft / 2 + 1;   // 201
    std::vector<float> mel(n_frames * kNmels, 0.0f);

    std::vector<double> win(kNfft);
    for (int i = 0; i < kNfft; ++i) win[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (double)kNfft));

    std::vector<double> frame(kNfft);
    std::vector<std::complex<double>> bins(n_freqs);
    std::vector<double> power(n_freqs);

    for (size_t f = 0; f < n_frames; ++f) {
        const size_t start = f * (size_t)kHop;
        for (int i = 0; i < kNfft; ++i) frame[i] = pcmf[start + i] * win[i];
        dft_lowbin(frame.data(), bins.data(), n_freqs);
        for (int k = 0; k < n_freqs; ++k)
            power[k] = bins[k].real() * bins[k].real() + bins[k].imag() * bins[k].imag();
        // filter 输出（未 log），再 log 归一化整体做一次
        float* mrow = mel.data() + f * kNmels;
        const float* fb = filterbank_.data();
        for (int m = 0; m < kNmels; ++m) {
            double sum = 0.0;
            const float* fm = fb + (size_t)m * n_freqs;
            for (int k = 0; k < n_freqs; ++k) sum += (double)fm[k] * power[k];
            mrow[m] = (float)sum;  // 稍后统一 log+clamp
        }
    }

    // 全局 log10(clamp≥1e-10) + 动态范围 clamp(max-8) + (x+4)/4
    for (auto& v : mel) v = std::log10(std::max(v, 1e-10f));
    float mmax = *std::max_element(mel.begin(), mel.end());
    for (auto& v : mel) v = std::max(v, mmax - 8.0f) * 0.25f + 1.0f;
    return mel;
}

}  // namespace voice_agent