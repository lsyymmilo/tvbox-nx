// main.cpp
#include <borealis.hpp>
#include <switch.h>

#include "player.hpp"
#include "ui.hpp"

int main(int argc, char* argv[]) {
    // curl / 站源接口走 libnx 的 BSD socket
    socketInitializeDefault();

    if (!brls::Application::init()) {
        brls::Logger::error("borealis 初始化失败");
        socketExit();
        return 1;
    }

    brls::Application::createWindow("TVBox-NX");
    brls::Application::setGlobalQuit(true);

    brls::Application::pushView(tbx::createRoot());

    while (brls::Application::mainLoop()) {
        // 主循环
    }

    tbx::state().player.shutdown();
    socketExit();
    return 0;
}
