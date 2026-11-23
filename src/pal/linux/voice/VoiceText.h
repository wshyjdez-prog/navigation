#pragma once
// 语音文本小工具：识别结果 JSON 字段提取、中文分词空格去除、UTF-8 码点切分。
#include <cstdint>
#include <string>
#include <vector>

namespace p1::voice {

inline std::vector<std::uint32_t> utf8Codepoints(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (std::size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

inline void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

inline bool isCjk(std::uint32_t cp) { return cp >= 0x2E80 && cp <= 0x9FFF; }

// 去掉中文字符之间的空格（Vosk 以词为单位输出“前方 三百 米”），首尾空白一并去除。
inline std::string joinChinese(const std::string& s) {
    const auto cps = utf8Codepoints(s);
    std::string out;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        if (cps[i] == ' ') {
            const bool prevCjk = i > 0 && isCjk(cps[i - 1]);
            const bool nextCjk = i + 1 < cps.size() && isCjk(cps[i + 1]);
            if (prevCjk || nextCjk || out.empty() || i + 1 == cps.size()) continue;
        }
        appendUtf8(out, cps[i]);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

// 从 {"key" : "value"} 形式的简单 JSON 中取字符串字段（Vosk 结果格式固定，不含转义引号以外的结构）。
inline std::string jsonStringField(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    auto p = json.find(pattern);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pattern.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    std::string out;
    for (std::size_t i = p + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { out.push_back(json[++i]); continue; }
        if (json[i] == '"') break;
        out.push_back(json[i]);
    }
    return out;
}

}  // namespace p1::voice
