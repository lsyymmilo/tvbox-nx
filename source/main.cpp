// main.cpp
#include <borealis.hpp>

#include "player.hpp"
#include "ui.hpp"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // socket / romfs / applet 生命周期由 borealis 的 switch_wrapper.c 初始化，
    // 它已经打进本程序（见 CMakeLists.txt 的 MAIN_SRC），这里不再重复 socketInitialize。

    if (!brls::Application::init()) {
        brls::Logger::error("borealis 初始化失败");
        return 1;
    }

    brls::Application::createWindow("TVBox-NX");
    brls::Application::setGlobalQuit(true);

    brls::Application::pushActivity(new brls::Activity(tbx::createRoot()));

    while (brls::Application::mainLoop()) {
    }

    tbx::state().player.shutdown();
    return 0;
}
