// player.hpp —— libmpv 播放器封装（与 borealis 解耦，渲染在 ui.cpp 里接）。
#pragma once

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <string>

namespace tbx {

class Player {
public:
    ~Player();

    bool init(std::string& err);
    bool open(const std::string& url, std::string& err);

    void togglePause();
    void seekRelative(double seconds);
    void setSpeed(double speed);
    void stop();
    void shutdown();

    bool isLoaded() const { return loaded_; }
    mpv_handle* handle() const { return mpv_; }

    // 渲染上下文由持有 GL 上下文的 View 创建后交进来
    void attachRender(mpv_render_context* ctx) { render_ = ctx; }
    mpv_render_context* renderContext() const { return render_; }

    // 有新帧时返回 true（由 mpv 的 update 回调置位）
    bool consumeFrameFlag();
    void setFrameFlag() { framePending_ = true; }

private:
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ = nullptr;
    volatile bool framePending_ = false;
    bool loaded_ = false;
};

}  // namespace tbx
