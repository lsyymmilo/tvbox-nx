// host_test.cpp —— 在 PC 上验证 source/tvbox.cpp + json.hpp + net.cpp 的真实行为。
// 与 tools/probe.py 跑的是同一套协议，但走的是 C++ 代码路径。
//
// 用法：host_test <api-url> <out-file>
// 退出码：0 成功且有直链；1 某步失败；2 参数错；3 接口通但无直链
#include <cstdio>
#include <string>
#include <vector>

#include "tvbox.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: host_test <api-url> <out-file>\n");
        return 2;
    }

    std::string api = argv[1];
    std::string outPath = argv[2];
    std::FILE* fp = std::fopen(outPath.c_str(), "wb");
    if (!fp) {
        std::printf("cannot open %s\n", outPath.c_str());
        return 2;
    }
    auto emit = [fp](const std::string& line) { std::fprintf(fp, "%s\n", line.c_str()); };

    tbx::CmsClient client(api);
    std::string err;

    std::vector<tbx::Category> cats;
    if (!client.fetchCategories(cats, err)) {
        emit("FAIL categories: " + err);
        std::fclose(fp);
        return 1;
    }
    emit("categories=" + std::to_string(cats.size()));
    for (size_t i = 0; i < cats.size(); ++i)
        emit("  [" + std::to_string(cats[i].id) + "] " + cats[i].name);

    std::vector<tbx::Vod> rows;
    long pages = 0;
    long firstCat = cats.empty() ? 0 : cats[0].id;
    if (!client.fetchList(firstCat, 1, rows, pages, err)) {
        emit("FAIL list: " + err);
        std::fclose(fp);
        return 1;
    }
    emit("rows=" + std::to_string(rows.size()) + " pages=" + std::to_string(pages));

    tbx::Vod detail;
    long id = rows.empty() ? 0 : rows[0].id;
    if (!client.fetchDetail(id, detail, err)) {
        emit("FAIL detail: " + err);
        std::fclose(fp);
        return 1;
    }
    emit("detail: name=" + detail.name + " routes=" + std::to_string(detail.routes.size()));

    int total = 0, direct = 0;
    for (size_t r = 0; r < detail.routes.size(); ++r) {
        const tbx::Route& route = detail.routes[r];
        emit("  route[" + std::string(route.from) + "]");
        for (size_t e = 0; e < route.episodes.size(); ++e) {
            const tbx::Episode& ep = route.episodes[e];
            ++total;
            if (ep.direct) ++direct;
            emit("    " + ep.name + "  " + (ep.direct ? "DIRECT" : "NEED-PARSE") + "  " + ep.url);
        }
    }
    emit("episodes=" + std::to_string(total) + " direct=" + std::to_string(direct));

    std::vector<tbx::Vod> hits;
    if (client.fetchSearch("测试", hits, err))
        emit("search hits=" + std::to_string(hits.size()));
    else
        emit("search failed: " + err);

    // 顺手验一下配置解析：TVBox 配置 / 单条地址 / 蜘蛛源跳过
    tbx::Config cfg;
    std::string cfgErr;
    std::string cfgJson =
        "{\"sites\":[{\"key\":\"a\",\"name\":\"A 站\",\"type\":1,\"api\":\"" + api +
        "\",\"searchable\":1},{\"key\":\"s\",\"name\":\"蜘蛛\",\"type\":3,\"api\":\"csp_X\"}],"
        "\"parses\":[{\"name\":\"p\",\"type\":1,\"url\":\"https://x\"}]}";
    if (tbx::Config::fromText(cfgJson, cfg, cfgErr)) {
        emit("config: sites=" + std::to_string(cfg.sites.size()) + " skipped=" +
             std::to_string(cfg.skippedSpiders) + " parses=" + std::to_string(cfg.parseEntries));
    } else {
        emit("config FAIL: " + cfgErr);
    }

    std::fclose(fp);
    return direct > 0 ? 0 : 3;
}
