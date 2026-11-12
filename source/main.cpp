// main.cpp
#include <borealis.hpp>

#include <exception>

#include "log.hpp"
#include "player.hpp"
#include "ui.hpp"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // socket / romfs / 字体 / applet 生命周期由 borealis 的 switch_wrapper.c 初始化，
    // 它已经打进本程序（见 CMakeLists.txt 的 MAIN_SRC），这里不再重复 socketInitialize。
    tbx::logLine("main: enter");

    if (!brls::Application::init()) {
        brls::Logger::error("borealis 初始化失败");
        tbx::logLine("main: Application::init FAILED");
        return 1;
    }
    tbx::logLine("main: Application::init ok");

    brls::Application::createWindow("TVBox-NX");
    brls::Application::setGlobalQuit(true);
    tbx::logLine("main: window ok");

    try {
        brls::Application::pushActivity(new brls::Activity(tbx::createRoot()));
        tbx::logLine("main: root pushed");
    } catch (const std::exception& e) {
        tbx::logLine(std::string("main: createRoot threw: ") + e.what());
        return 1;
    } catch (...) {
        tbx::logLine("main: createRoot threw unknown");
        return 1;
    }

    while (brls::Application::mainLoop()) {
    }

    tbx::logLine("main: loop exited");
    tbx::state().player.shutdown();
    return 0;
}
