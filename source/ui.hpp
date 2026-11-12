// ui.hpp —— borealis 界面层入口。
#pragma once

#include <borealis.hpp>

#include <vector>

#include "player.hpp"
#include "tvbox.hpp"

namespace tbx {

struct AppState {
    Config config;
    std::vector<Category> categories;   // 首页分类（按站源拉取一次）
    int siteIndex = 0;
    Player player;
    std::string configPath = "sdmc:/switch/tvbox-nx/config.json";
    std::string lastError;

    bool loadConfig();
    bool hasSite() const { return !config.sites.empty() && siteIndex >= 0 &&
                                  siteIndex < (int)config.sites.size(); }
    Site& site() { return config.sites[siteIndex]; }
};

AppState& state();

// 构建根界面（源 / 分类 / 搜索 三个 tab）
brls::View* createRoot();

void openCategory(long categoryId, const std::string& title);
void openDetail(const Vod& vod);
void openPlayer(const std::string& url, const std::string& title);

}  // namespace tbx
