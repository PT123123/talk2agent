// src/memory/text_features.cpp
#include "text_features.hpp"
#include <cctype>

namespace voice_agent {

namespace {

// 返回给定 UTF-8 首字节对应的序列长度
int utf8_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    return 4;
}

}  // namespace

std::vector<std::string> tokenize_units(const std::string& text) {
    std::vector<std::string> out;
    std::string word;  // ascii word buffer

    auto flush = [&]() {
        if (!word.empty()) {
            out.push_back(std::move(word));
            word.clear();
        }
    };

    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (std::isalnum(unsigned(c))) {
                word.push_back(static_cast<char>(std::tolower(unsigned(c))));
            } else {
                flush();
            }
            ++i;
        } else {
            // 多字节：每个 CJK/其他 unicode 字符作为一个 token，保持原文
            int len = utf8_len(c);
            if (len > static_cast<int>(n - i)) len = static_cast<int>(n - i);
            flush();
            out.emplace_back(text, i, static_cast<size_t>(len));
            i += static_cast<size_t>(len);
        }
    }
    flush();
    return out;
}

}  // namespace voice_agent