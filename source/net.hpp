// net.hpp —— libcurl 的最小封装（同步 GET）+ URL 编码。
#pragma once

#include <string>

namespace tbx {

struct HttpResult {
    long status = 0;
    std::string body;
    std::string error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// 同步 GET。Switch 上网络栈由 libnx socket 提供，curl 用 devkitPro 的 switch-curl。
HttpResult httpGet(const std::string& url, int timeoutSec = 12);

// 查询参数编码（UTF-8 原文直接百分号转义）
std::string urlEncode(const std::string& text);

// 读整个文件；失败返回 false
bool readWholeFile(const std::string& path, std::string& out);

}  // namespace tbx
