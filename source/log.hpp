// log.hpp —— 极简文件日志：真机没有终端，出问题时只能靠这个落盘。
// 日志写到 sdmc:/switch/tvbox-nx/log.txt（与 .nro 同目录）。
#pragma once

#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace tbx {

inline void logLine(const std::string& msg) {
    mkdir("sdmc:/switch/tvbox-nx", 0777);
    FILE* f = std::fopen("sdmc:/switch/tvbox-nx/log.txt", "a");
    if (!f) return;
    std::fprintf(f, "%s\n", msg.c_str());
    std::fclose(f);
}

}  // namespace tbx
