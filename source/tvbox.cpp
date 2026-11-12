// tvbox.cpp
#include "tvbox.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>

#include "net.hpp"

namespace tbx {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitBy(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    if (sep.empty()) {
        out.push_back(s);
        return out;
    }
    size_t pos = 0;
    while (true) {
        size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
    return out;
}

Site parseSite(const Json& j) {
    Site s;
    s.key = j["key"].asString();
    s.name = j["name"].asString(s.key);
    s.api = trim(j["api"].asString());
    s.type = (int)j["type"].asLong(1);
    s.searchable = j["searchable"].asLong(1) != 0;
    return s;
}

Vod parseVod(const Json& j) {
    Vod v;
    v.id = j["vod_id"].asLong();
    v.name = j["vod_name"].asString();
    v.remarks = j["vod_remarks"].asString();
    v.pic = j["vod_pic"].asString();
    v.content = j["vod_content"].asString();
    v.year = j["vod_year"].asString();
    v.area = j["vod_area"].asString();
    v.routes = splitPlayUrls(j["vod_play_from"].asString(), j["vod_play_url"].asString());
    return v;
}

}  // namespace

bool isDirectStream(const std::string& url) {
    if (!startsWith(url, "http://") && !startsWith(url, "https://")) return false;
    std::string path = url;
    size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path = path.substr(0, cut);
    path = lower(path);

    static const char* exts[] = {".m3u8", ".mp4", ".mkv", ".flv", ".ts",
                                 ".mpd",  ".m4v", ".mov", ".webm"};
    for (const char* e : exts) {
        size_t n = std::string(e).size();
        if (path.size() >= n && path.compare(path.size() - n, n, e) == 0) return true;
    }
    return false;
}

std::vector<Route> splitPlayUrls(const std::string& from, const std::string& urls) {
    std::vector<Route> routes;
    if (trim(urls).empty()) return routes;

    std::vector<std::string> groups = splitBy(urls, "$$$");
    std::vector<std::string> names = splitBy(from, "$$$");

    for (size_t g = 0; g < groups.size(); ++g) {
        Route route;
        std::string raw = groups[g];
        if (!trim(raw).empty()) {
            route.from = (g < names.size() && !trim(names[g]).empty())
                             ? trim(names[g])
                             : "线路" + std::to_string(g + 1);
            for (const std::string& piece : splitBy(raw, "#")) {
                std::string item = trim(piece);
                if (item.empty()) continue;
                Episode ep;
                size_t dollar = item.find('$');
                if (dollar == std::string::npos) {
                    ep.name = "第" + std::to_string(route.episodes.size() + 1) + "集";
                    ep.url = item;
                } else {
                    ep.name = trim(item.substr(0, dollar));
                    ep.url = trim(item.substr(dollar + 1));
                }
                if (ep.url.empty()) continue;
                ep.direct = isDirectStream(ep.url);
                route.episodes.push_back(std::move(ep));
            }
        }
        if (!route.episodes.empty()) routes.push_back(std::move(route));
    }
    return routes;
}

bool isCmsSite(const Site& site, std::string& why) {
    if (site.type == 3) {
        why = "type=3（csp/蜘蛛），需 ART 或 JS 引擎";
        return false;
    }
    if (site.type == 0) {
        why = "type=0（XML 站源），未实现";
        return false;
    }
    if (site.api.empty()) {
        why = "api 为空";
        return false;
    }
    if (!startsWith(site.api, "http://") && !startsWith(site.api, "https://")) {
        why = "api 不是 http(s)，多半是本地 csp 脚本";
        return false;
    }
    std::string tail = lower(site.api);
    if (tail.size() > 4 &&
        (tail.compare(tail.size() - 4, 4, ".jar") == 0 ||
         tail.compare(tail.size() - 3, 3, ".js") == 0 ||
         tail.compare(tail.size() - 3, 3, ".py") == 0)) {
        why = "spider 脚本源";
        return false;
    }
    why = "CMS JSON";
    return true;
}

bool Config::fromText(const std::string& text, Config& out, std::string& err) {
    out = Config();
    std::string body = trim(text);
    if (body.empty()) {
        err = "配置内容为空";
        return false;
    }

    // 直接给一条 api 地址
    if (startsWith(body, "http://") || startsWith(body, "https://")) {
        Site s;
        s.key = "manual";
        s.name = "手填地址";
        s.type = 1;
        s.api = body;
        out.sites.push_back(s);
        return true;
    }

    Json root;
    try {
        root = Json::parse(body);
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败：") + e.what();
        return false;
    }

    const Json* sitesNode = nullptr;
    if (root.isObject() && root.has("sites")) {
        sitesNode = &root["sites"];
        out.parseEntries = (int)root["parses"].size();
    } else if (root.isArray()) {
        sitesNode = &root;
    } else if (root.isObject() && root.has("api")) {
        out.sites.push_back(parseSite(root));
        return !out.sites.empty();
    } else {
        err = "不认识的配置结构：需要含 sites 的配置，或单个 api 地址";
        return false;
    }

    if (!sitesNode->isArray()) {
        err = "sites 不是数组";
        return false;
    }

    for (size_t i = 0; i < sitesNode->size(); ++i) {
        const Json& item = (*sitesNode)[i];
        if (!item.isObject()) continue;
        Site s = parseSite(item);
        std::string why;
        if (!isCmsSite(s, why)) {
            ++out.skippedSpiders;
            continue;
        }
        if (s.name.empty()) s.name = "站源 " + std::to_string(out.sites.size() + 1);
        out.sites.push_back(std::move(s));
    }

    if (out.sites.empty()) {
        err = "配置里没有可用的 CMS 站源（蜘蛛源在 Switch 上跑不了）";
        return false;
    }
    return true;
}

bool Config::loadFromFile(const std::string& path, const std::string& origin,
                          Config& out, std::string& err) {
    std::string text;
    if (!readWholeFile(path, text)) {
        err = "读不到文件：" + path;
        return false;
    }
    if (!fromText(text, out, err)) return false;
    out.origin = origin;
    return true;
}

bool CmsClient::call(const std::string& query, Json& out, std::string& err) {
    if (api_.empty()) {
        err = "api 为空";
        return false;
    }
    std::string url = api_;
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += query;

    HttpResult r = httpGet(url);
    if (!r.ok()) {
        err = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }
    try {
        out = Json::parse(r.body);
    } catch (const std::exception& e) {
        err = std::string("返回不是合法 JSON：") + e.what();
        return false;
    }
    return true;
}

bool CmsClient::fetchCategories(std::vector<Category>& out, std::string& err) {
    out.clear();
    Json doc;
    if (!call("ac=list", doc, err)) return false;

    const Json& cls = doc["class"];
    if (!cls.isArray()) {
        // 有些源不带 class，只给 list，就退化成"全部"
        Category all;
        all.id = 0;
        all.name = "全部";
        out.push_back(all);
        return true;
    }
    for (size_t i = 0; i < cls.size(); ++i) {
        Category c;
        c.id = cls[i]["type_id"].asLong(0);
        c.name = cls[i]["type_name"].asString("分类" + std::to_string(i + 1));
        out.push_back(std::move(c));
    }
    return true;
}

bool CmsClient::fetchList(long categoryId, int page, std::vector<Vod>& out, long& pageCount,
                          std::string& err) {
    out.clear();
    pageCount = 1;

    std::string query = "ac=detail&t=" + std::to_string(categoryId) +
                        "&pg=" + std::to_string(page < 1 ? 1 : page);
    Json doc;
    if (!call(query, doc, err)) return false;

    const Json& list = doc["list"];
    if (!list.isArray() || list.size() == 0) {
        // 少数源只认 ac=videolist
        Json fallback;
        err.clear();
        if (call("ac=videolist&t=" + std::to_string(categoryId) + "&pg=" +
                     std::to_string(page < 1 ? 1 : page), fallback, err)) {
            doc = fallback;
        } else {
            return false;
        }
    }

    const Json& rows = doc["list"];
    for (size_t i = 0; i < rows.size(); ++i) out.push_back(parseVod(rows[i]));

    long pc = doc["pagecount"].asLong(1);
    pageCount = pc > 0 ? pc : 1;
    return true;
}

bool CmsClient::fetchDetail(long vodId, Vod& out, std::string& err) {
    Json doc;
    if (!call("ac=detail&ids=" + std::to_string(vodId), doc, err)) return false;

    const Json& rows = doc["list"];
    if (!rows.isArray() || rows.size() == 0) {
        err = "接口没有返回该影片详情";
        return false;
    }
    out = parseVod(rows[0]);
    return true;
}

bool CmsClient::fetchSearch(const std::string& keyword, std::vector<Vod>& out, std::string& err) {
    out.clear();
    Json doc;
    if (!call("ac=detail&wd=" + urlEncode(keyword), doc, err)) return false;

    const Json& rows = doc["list"];
    for (size_t i = 0; i < rows.size(); ++i) out.push_back(parseVod(rows[i]));
    return true;
}

}  // namespace tbx
