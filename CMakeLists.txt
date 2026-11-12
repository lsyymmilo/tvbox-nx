cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# 工程结构对齐 dragonflylee/switchfin（同样基于 borealis 的 Switch 应用）
#   libs/borealis 由 CI 在构建时 clone（见 .github/workflows/build-nro.yml）
# ---------------------------------------------------------------------------

set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

if(NOT EXISTS ${BOREALIS_LIBRARY}/CMakeLists.txt)
    message(FATAL_ERROR "libs/borealis 不存在，先 clone：git clone --depth 1 https://github.com/XITRIX/borealis.git libs/borealis")
endif()

# 平台选项（必须在 project() 之前 include）
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(tvbox-nx CXX C)

set(VERSION_MAJOR "1")
set(VERSION_MINOR "0")
set(VERSION_ALTER "0")
set(PROJECT_TITLEID "0100000000000000")
set(PROJECT_AUTHOR "tvbox-nx")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/romfs/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Release" FORCE)
endif()

set(MAIN_SRC
    source/main.cpp
    source/net.cpp
    source/tvbox.cpp
    source/player.cpp
    source/ui.cpp
)

# Switch 平台必需：libnx 服务初始化（socket / romfs / 字体 / applet 生命周期）
if(PLATFORM_SWITCH)
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif()

# borealis 本体（把 build 目录放出去，避免和工程根冲突）
add_subdirectory(${BOREALIS_LIBRARY} ${CMAKE_BINARY_DIR}/borealis)

# 依赖：mpv（播放）与 libcurl（网络），交叉编译走 portlibs 的 pkg-config
find_package(PkgConfig REQUIRED)

if(PLATFORM_SWITCH)
    pkg_search_module(MPV REQUIRED mpv)
    pkg_search_module(LIBCURL REQUIRED libcurl)
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS})
    list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARIES} ${LIBCURL_LIBRARIES})
else()
    find_package(CURL REQUIRED)
    pkg_search_module(MPV REQUIRED mpv)
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIRS})
    list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARIES} CURL::libcurl)
endif()

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

# 目标
program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20)
target_include_directories(${PROJECT_NAME} PRIVATE source ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections ${APP_PLATFORM_OPTION})
target_link_libraries(${PROJECT_NAME} PRIVATE borealis ${APP_PLATFORM_LIB})

# 产出 .nro
if(PLATFORM_SWITCH)
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
endif()
