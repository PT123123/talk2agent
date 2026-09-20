// src/memory/text_features.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 聚合一阶特征：中性分词 + 词频 ==========
// 拉丁语按单词切分（小写），CJK 按单字切分。—— 召回/打分单位。
std::vector<std::string> tokenize_units(const std::string& text);

inline std::string lowercase_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// CJK 单字组成的连续 token 也是切分粒度的子集：
// 判定某个字节是否为 UTF-8 多字节（用于调用方提示）。
inline bool is_utf8_lead(unsigned char c) { return (c & 0xC0) == 0xC0; }

}  // namespace voice_agent