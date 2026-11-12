// json.hpp —— 极简 JSON 解析/访问，只为实现本客户端需要的部分，零外部依赖。
// 为什么不用 nlohmann：devkitPro 包里不一定有，自带一份省掉一个 submodule。
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tbx {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    size_t size() const {
        if (type == Type::Array) return array.size();
        if (type == Type::Object) return object.size();
        return 0;
    }

    bool has(const std::string& key) const {
        if (type != Type::Object) return false;
        for (const auto& kv : object)
            if (kv.first == key) return true;
        return false;
    }

    const Json& operator[](const std::string& key) const {
        if (type == Type::Object)
            for (const auto& kv : object)
                if (kv.first == key) return kv.second;
        return null();
    }

    const Json& operator[](size_t idx) const {
        if (type == Type::Array && idx < array.size()) return array[idx];
        return null();
    }

    // TVBox 的接口经常把数字写成字符串，两种都收
    std::string asString(const std::string& def = "") const {
        switch (type) {
            case Type::String: return string;
            case Type::Number: {
                char buf[40];
                if (std::fabs(number) < 1e15 && number == (double)(long long)number)
                    std::snprintf(buf, sizeof(buf), "%lld", (long long)number);
                else
                    std::snprintf(buf, sizeof(buf), "%g", number);
                return std::string(buf);
            }
            case Type::Bool: return boolean ? "true" : "false";
            default: return def;
        }
    }

    long asLong(long def = 0) const {
        if (type == Type::Number) return (long)number;
        if (type == Type::String && !string.empty()) {
            char* end = nullptr;
            long v = std::strtol(string.c_str(), &end, 10);
            return (end && *end == '\0') ? v : def;
        }
        if (type == Type::Bool) return boolean ? 1 : 0;
        return def;
    }

    static const Json& null() {
        static const Json kNull;
        return kNull;
    }

    static Json parse(const std::string& text);
};

namespace detail {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Json parseDocument() {
        skipBom();
        skipWs();
        Json v = parseValue();
        skipWs();
        if (i_ != s_.size()) fail("尾部有多余字符");
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void fail(const char* msg) {
        throw std::runtime_error(std::string("JSON: ") + msg + " @" + std::to_string(i_));
    }

    void skipBom() {
        if (s_.size() >= 3 && (unsigned char)s_[0] == 0xEF &&
            (unsigned char)s_[1] == 0xBB && (unsigned char)s_[2] == 0xBF)
            i_ = 3;
        // 有些源在 JSON 前面塞一行注释
        while (i_ < s_.size()) {
            size_t p = i_;
            while (p < s_.size() && (s_[p] == ' ' || s_[p] == '\t' || s_[p] == '\r' || s_[p] == '\n')) ++p;
            if (p + 1 < s_.size() && s_[p] == '/' && s_[p + 1] == '/') {
                while (p < s_.size() && s_[p] != '\n') ++p;
                i_ = p;
            } else {
                break;
            }
        }
    }

    void skipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i_;
            else break;
        }
    }

    bool eof() const { return i_ >= s_.size(); }

    Json parseValue() {
        if (eof()) fail("意外结束");
        char c = s_[i_];
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': { Json j; j.type = Json::Type::String; j.string = parseString(); return j; }
            case 't': expect("true"); { Json j; j.type = Json::Type::Bool; j.boolean = true; return j; }
            case 'f': expect("false"); { Json j; j.type = Json::Type::Bool; j.boolean = false; return j; }
            case 'n': expect("null"); return Json();
            default: return parseNumber();
        }
    }

    void expect(const char* lit) {
        size_t n = std::string(lit).size();
        if (s_.compare(i_, n, lit) != 0) fail("字面量不匹配");
        i_ += n;
    }

    Json parseObject() {
        Json j;
        j.type = Json::Type::Object;
        ++i_;  // '{'
        skipWs();
        if (!eof() && s_[i_] == '}') { ++i_; return j; }
        while (true) {
            skipWs();
            if (eof() || s_[i_] != '"') fail("对象键必须是字符串");
            std::string key = parseString();
            skipWs();
            if (eof() || s_[i_] != ':') fail("缺少冒号");
            ++i_;
            skipWs();
            j.object.emplace_back(std::move(key), parseValue());
            skipWs();
            if (eof()) fail("对象未闭合");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; break; }
            fail("对象里出现意外字符");
        }
        return j;
    }

    Json parseArray() {
        Json j;
        j.type = Json::Type::Array;
        ++i_;  // '['
        skipWs();
        if (!eof() && s_[i_] == ']') { ++i_; return j; }
        while (true) {
            skipWs();
            j.array.push_back(parseValue());
            skipWs();
            if (eof()) fail("数组未闭合");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; break; }
            fail("数组里出现意外字符");
        }
        return j;
    }

    Json parseNumber() {
        size_t start = i_;
        if (!eof() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (!eof() && (std::isdigit((unsigned char)s_[i_]) || s_[i_] == '.' ||
                          s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        if (i_ == start) fail("不是合法数字");
        Json j;
        j.type = Json::Type::Number;
        j.number = std::atof(s_.substr(start, i_ - start).c_str());
        return j;
    }

    std::string parseString() {
        ++i_;  // '"'
        std::string out;
        while (true) {
            if (eof()) fail("字符串未闭合");
            char c = s_[i_++];
            if (c == '"') break;
            if (c != '\\') { out.push_back(c); continue; }
            if (eof()) fail("转义未闭合");
            char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {          // 代理对
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            i_ += 2;
                            unsigned lo = parseHex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("未知转义");
            }
        }
        return out;
    }

    unsigned parseHex4() {
        if (i_ + 4 > s_.size()) fail("\\u 长度不够");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else fail("\\u 里有非法字符");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
};

}  // namespace detail

inline Json Json::parse(const std::string& text) { return detail::Parser(text).parseDocument(); }

}  // namespace tbx
