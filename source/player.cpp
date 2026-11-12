// player.cpp
#include "player.hpp"

#include <cstdio>

namespace tbx {

Player::~Player() { shutdown(); }

bool Player::init(std::string& err) {
    if (mpv_) return true;

    mpv_ = mpv_create();
    if (!mpv_) {
        err = "mpv_create 失败（switch-libmpv 是否链接正确？）";
        return false;
    }

    // 关键：渲染交给 libmpv render API，由我们的 GL View 画到 FBO 0
    mpv_set_option_string(mpv_, "vo", "libmpv");
    // Switch 上 mpv 没有可用硬解，交给 CPU；软解能力见 README 的性能说明
    mpv_set_option_string(mpv_, "hwdec", "no");
    mpv_set_option_string(mpv_, "idle", "yes");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "osc", "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv_, "cache", "yes");
    mpv_set_option_string(mpv_, "cache-secs", "20");
    mpv_set_option_string(mpv_, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(mpv_, "demuxer-max-back-bytes", "16MiB");
    mpv_set_option_string(mpv_, "vd-lavc-threads", "3");
    mpv_set_option_string(mpv_, "video-sync", "audio");
    mpv_set_option_string(mpv_, "audio-pitch-correction", "yes");
    // 屏幕是 1280x720，让 mpv 按需缩放
    mpv_set_option_string(mpv_, "scale", "bilinear");
    mpv_set_option_string(mpv_, "dither-depth", "no");

    if (mpv_initialize(mpv_) < 0) {
        err = "mpv_initialize 失败";
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }
    return true;
}

bool Player::open(const std::string& url, std::string& err) {
    if (!mpv_) {
        err = "播放器未初始化";
        return false;
    }
    if (url.empty()) {
        err = "空地址";
        return false;
    }
    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    if (mpv_command(mpv_, cmd) < 0) {
        err = "loadfile 失败";
        return false;
    }
    loaded_ = true;
    return true;
}

void Player::togglePause() {
    if (!mpv_) return;
    int paused = 0;
    mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
    paused = paused ? 0 : 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
}

void Player::seekRelative(double seconds) {
    if (!mpv_) return;
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "seek %.1f relative", seconds);
    mpv_command_string(mpv_, cmd);
}

void Player::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void Player::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(mpv_, cmd);
    loaded_ = false;
}

void Player::shutdown() {
    if (render_) {
        mpv_render_context_free(render_);
        render_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    loaded_ = false;
}

bool Player::consumeFrameFlag() {
    if (!framePending_) return false;
    framePending_ = false;
    return true;
}

}  // namespace tbx
