cmake_minimum_required(VERSION 3.13)
project(tvbox-nx CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -O2")

# devkitPro / libnx 的 Switch 工具链
if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "没找到 DEVKITPRO 环境变量，先在 devkitPro 的 msys2 shell 里跑一遍")
endif()
include("$ENV{DEVKITPRO}/cmake/Switch.cmake")

# borealis 以子模块方式放在 libs/borealis
#   git submodule add https://github.com/XITRIX/borealis libs/borealis
add_subdirectory(libs/borealis)

set(SOURCES
    source/main.cpp
    source/net.cpp
    source/tvbox.cpp
    source/player.cpp
    source/ui.cpp
)

add_executable(tvbox-nx ${SOURCES})
target_include_directories(tvbox-nx PRIVATE source)

# borealis 需要 glfw；音频/解码由 switch-libmpv 提供；curl 走 switch-curl
target_link_libraries(tvbox-nx PRIVATE
    borealis
    mpv
    curl
    glfw
    EGL
    glapi
    nx
)

# romfs 里放默认配置和图标
nx_generate_nacp(tvbox.nacp
    APP_TITLE "TVBox-NX"
    APP_AUTHOR "tvbox-nx"
    APP_VERSION "1.0.0"
)

nx_create_nro(tvbox-nx
    NACP tvbox.nacp
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/romfs/icon.jpg"
    ROMFS "${CMAKE_CURRENT_SOURCE_DIR}/romfs"
    AUTHOR "tvbox-nx"
    VERSION "1.0.0"
)
