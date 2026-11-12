// tvbox.hpp —— 源配置模型 + CMS(type=1) 接口客户端。
//
// 支持的边界（刻意为之，写死在这里）：只做 TVBox 配置里 type=1 的 CMS JSON 站源，
// 也就是 api 形如 https://host/api.php/provide/vod 的接口。
// type=3（csp/jar/js/py 蜘蛛）、type=0（XML 站源）一律跳过——前者要 ART 或 JS 引擎，
// 后者要另写一套 XML 解析，Switch 上没有意义。
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace tbx {

struct Site {
    std::string key;
    std::string name;
    std::string api;
    int type = 1;
    bool searchable = true;
};

struct Config {
    std::vector<Site> sites;
    int skippedSpiders = 0;      // 配置里被跳过的 type=3 数量
    int parseEntries = 0;        // 配置里 parses 的数量（本客户端不用）
    std::string origin;          // "sdmc" / "romfs" / "manual"

    // 解析文本：既可以是完整 TVBox 配置，也可以是单条 api 地址
    static bool fromText(const std::string& text, Config& out, std::string& err);
    static bool loadFromFile(const std::string& path, const std::string& origin,
                             Config& out, std::string& err);
};

// 判断某个站源能不能被本客户端使用；不能时 why 给出人话原因
bool isCmsSite(const Site& site, std::string& why);

struct Category {
    long id = 0;
    std::string name;
};

struct Episode {
    std::string name;
    std::string url;
    bool direct = false;   // true = 可直接交给 mpv 的直链
};

struct Route {
    std::string from;
    std::vector<Episode> episodes;
};

struct Vod {
    long id = 0;
    std::string name;
    std::string remarks;
    std::string pic;
    std::string content;
    std::string year;
    std::string area;
    std::vector<Route> routes;
};

// 直链判定：http(s) 且路径以 mpv 能直接吃的后缀结尾
bool isDirectStream(const std::string& url);

// 把 vod_play_from / vod_play_url 拆成线路+剧集
std::vector<Route> splitPlayUrls(const std::string& from, const std::string& urls);

class CmsClient {
public:
    explicit CmsClient(std::string api) : api_(std::move(api)) {}
    const std::string& api() const { return api_; }

    bool fetchCategories(std::vector<Category>& out, std::string& err);
    bool fetchList(long categoryId, int page, std::vector<Vod>& out, long& pageCount,
                   std::string& err);
    bool fetchDetail(long vodId, Vod& out, std::string& err);
    bool fetchSearch(const std::string& keyword, std::vector<Vod>& out, std::string& err);

private:
    bool call(const std::string& query, Json& out, std::string& err);
    std::string api_;
};

}  // namespace tbx
