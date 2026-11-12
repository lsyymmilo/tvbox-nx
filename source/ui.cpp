// ui.cpp —— borealis 界面：源选择 / 分类 / 列表 / 详情 / 播放。
//
// 对齐当前 borealis（moonlight_wiliwili 分支）的真实 API：
//   * 没有 brls::List / brls::ListItem / Application::pushView
//   * 页面 = ScrollingFrame + Header + Button，压栈用 Application::pushActivity(new Activity(view))
//   * Header 没有带参构造，必须 setTitle()
//
// 设计取舍：数据请求同步阻塞（点一下等 1~2 秒），换掉线程/回调的复杂度。
#include "ui.hpp"

#include <GLFW/glfw3.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "net.hpp"
#include "log.hpp"

namespace tbx {

AppState& state() {
    static AppState s;
    return s;
}

bool AppState::loadConfig() {
    std::string err;
    if (Config::loadFromFile(configPath, "sdmc", config, err)) {
        lastError.clear();
        siteIndex = 0;
        return true;
    }
    std::string romfsErr;
    if (Config::loadFromFile("romfs:/config.sample.json", "romfs", config, romfsErr)) {
        lastError = "未找到 config.json，当前用内置示例源（把配置放到 sdmc:/switch/tvbox-nx/config.json）";
        siteIndex = 0;
        return true;
    }
    lastError = err;
    return false;
}

namespace {

// ---------------------------------------------------------------- 页面构件

// 一页 = 可滚动的壳 + 里面真正堆控件的 Box。
// 这一版 borealis 有两个坑必须绕开：
//   1) ScrollingFrame 自己 setPadding 直接抛异常（NO_PADDING），padding 得设在内容视图上；
//   2) ScrollingFrame::addView 等价于 setContentView，是"替换"而不是"追加"，
//      往它上面连续 addView 只会剩最后一个控件。
struct Page {
    brls::ScrollingFrame* frame = nullptr;
    brls::Box* content          = nullptr;
};

Page newPage(const std::string& title) {
    logLine("newPage: " + title + " begin");
    Page page;
    page.frame   = new brls::ScrollingFrame();
    page.content = new brls::Box(brls::Axis::COLUMN);
    page.content->setPadding(30, 30, 30, 20);
    page.frame->setContentView(page.content);

    brls::Header* header = new brls::Header();
    header->setTitle(title);
    page.content->addView(header);
    logLine("newPage: " + title + " header ok");
    return page;
}

void addInfo(Page& page, const std::string& text) {
    brls::Label* label = new brls::Label();
    label->setText(text);
    label->setMargins(0, 0, 12, 0);
    page.content->addView(label);
}

void addSection(Page& page, const std::string& text) {
    brls::Header* header = new brls::Header();
    header->setTitle(text);
    page.content->addView(header);
}

void addRow(Page& page, const std::string& text, std::function<void()> onClick) {
    brls::Button* row = new brls::Button();
    row->setText(text);
    row->setMargins(0, 0, 14, 0);
    row->registerClickAction([onClick](brls::View*) {
        onClick();
        return true;
    });
    page.content->addView(row);
}

void pushPage(brls::View* view) {
    brls::Application::pushActivity(new brls::Activity(view));
}

// ---------------------------------------------------------------- 播放视图

class MpvView : public brls::View {
public:
    MpvView(std::string url, std::string title) : url_(std::move(url)), title_(std::move(title)) {
        this->setFocusable(true);

        registerAction("暂停/继续", brls::ControllerButton::BUTTON_A, [](brls::View*) {
            state().player.togglePause();
            return true;
        });
        registerAction("后退 30s", brls::ControllerButton::BUTTON_LB, [](brls::View*) {
            state().player.seekRelative(-30.0);
            return true;
        });
        registerAction("前进 30s", brls::ControllerButton::BUTTON_RB, [](brls::View*) {
            state().player.seekRelative(30.0);
            return true;
        });
        registerAction("倍速", brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
            speed_ = (speed_ < 1.4) ? 1.5 : 1.0;
            state().player.setSpeed(speed_);
            brls::Application::notify(speed_ > 1.0 ? "1.5x" : "1.0x");
            return true;
        });
    }

    ~MpvView() override {
        if (ctx_) {
            mpv_render_context_set_update_callback(ctx_, nullptr, nullptr);
            mpv_render_context_free(ctx_);
            ctx_ = nullptr;
        }
    }

    void onLayout() override {
        if (!ctx_) createContext();
        if (!opened_ && ctx_) {
            opened_ = true;
            std::string err;
            if (state().player.open(url_, err))
                brls::Application::notify("播放: " + title_);
            else
                brls::Application::notify("播放失败: " + err);
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* frameCtx) override {
        if (!ctx_) return;

        mpv_opengl_fbo fbo;
        fbo.fbo             = 0;
        fbo.w               = (int)width;
        fbo.h               = (int)height;
        fbo.internal_format = 0;

        int flipY = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(ctx_, params);
    }

    static void onUpdate(void* self) {
        MpvView* view = reinterpret_cast<MpvView*>(self);
        brls::sync([view]() { view->invalidate(); });
    }

private:
    static void* getProcAddress(void* ctx, const char* name) {
        (void)ctx;
        return (void*)glfwGetProcAddress(name);
    }

    void createContext() {
        mpv_opengl_init_params gl;
        gl.get_proc_address     = &MpvView::getProcAddress;
        gl.get_proc_address_ctx = nullptr;

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&ctx_, state().player.handle(), params) < 0) {
            ctx_ = nullptr;
            brls::Application::notify("mpv 渲染上下文创建失败");
            return;
        }
        mpv_render_context_set_update_callback(ctx_, &MpvView::onUpdate, this);
        state().player.attachRender(ctx_);
    }

    std::string url_;
    std::string title_;
    mpv_render_context* ctx_ = nullptr;
    bool opened_              = false;
    double speed_             = 1.0;
};

// ---------------------------------------------------------------- 详情页

brls::View* buildDetail(const Vod& vod) {
    Page page = newPage(vod.name);

    std::string meta = vod.year;
    if (!vod.area.empty()) meta += (meta.empty() ? "" : "  ") + vod.area;
    if (!vod.remarks.empty()) meta += (meta.empty() ? "" : "  ") + vod.remarks;
    if (!meta.empty()) addInfo(page, meta);
    if (!vod.content.empty()) addInfo(page, vod.content);

    if (vod.routes.empty()) {
        addInfo(page, "没有解析到播放串");
        return page.frame;
    }

    for (size_t i = 0; i < vod.routes.size(); ++i) {
        const Route& route = vod.routes[i];
        addSection(page, route.from);

        for (size_t k = 0; k < route.episodes.size(); ++k) {
            const Episode& ep = route.episodes[k];
            std::string label = ep.name + (ep.direct ? "   [直链]" : "   [需解析，不支持]");

            if (ep.direct) {
                std::string url   = ep.url;
                std::string title = vod.name + " " + ep.name;
                addRow(page, label, [url, title]() { openPlayer(url, title); });
            } else {
                addRow(page, label, []() {
                    brls::Application::notify("这一集是网页地址，Switch 上没有解析器，不能直连播放");
                });
            }
        }
    }
    return page.frame;
}

// ---------------------------------------------------------------- 影片列表页

brls::View* buildVodList(const std::string& title, const std::vector<Vod>& vods, const std::string& api) {
    Page page = newPage(title);

    if (vods.empty()) {
        addInfo(page, "这个分类没有数据");
        return page.frame;
    }

    for (size_t i = 0; i < vods.size(); ++i) {
        const Vod& v = vods[i];
        std::string text = v.name;
        if (!v.remarks.empty()) text += "   [" + v.remarks + "]";

        long id           = v.id;
        std::string itemApi = api;
        addRow(page, text, [itemApi, id]() {
            CmsClient client(itemApi);
            Vod full;
            std::string err;
            if (!client.fetchDetail(id, full, err)) {
                brls::Application::notify("详情加载失败: " + err);
                return;
            }
            pushPage(buildDetail(full));
        });
    }
    return page.frame;
}

// ---------------------------------------------------------------- 搜索页

brls::View* buildSearch(const std::string& api) {
    Page page = newPage("搜索");
    std::string itemApi = api;

    addInfo(page, "按 A 打开系统键盘输入片名");
    addRow(page, "输入关键词", [itemApi]() {
        brls::Application::getImeManager()->openForText(
            [itemApi](std::string text) {
                if (text.empty()) return;
                brls::Application::notify("搜索中…");
                CmsClient client(itemApi);
                std::vector<Vod> vods;
                std::string err;
                if (!client.fetchSearch(text, vods, err)) {
                    brls::Application::notify("搜索失败: " + err);
                    return;
                }
                pushPage(buildVodList("搜索：" + text, vods, itemApi));
            },
            "搜索", "输入片名后确认", 32, "");
    });
    return page.frame;
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

void openCategory(long categoryId, const std::string& title) {
    if (!state().hasSite()) {
        brls::Application::notify("先选择一个站源");
        return;
    }
    brls::Application::notify("加载中…");

    CmsClient client(state().site().api);
    std::vector<Vod> vods;
    long pages = 1;
    std::string err;
    if (!client.fetchList(categoryId, 1, vods, pages, err)) {
        brls::Application::notify("列表加载失败: " + err);
        return;
    }
    pushPage(buildVodList(title, vods, state().site().api));
}

void openDetail(const Vod& vod) { pushPage(buildDetail(vod)); }

void openPlayer(const std::string& url, const std::string& title) {
    std::string err;
    if (!state().player.init(err)) {
        brls::Application::notify("播放器初始化失败: " + err);
        return;
    }
    pushPage(new MpvView(url, title));
}

brls::View* createRoot() {
    logLine("createRoot: begin");
    AppState& st = state();
    st.loadConfig();
    logLine("createRoot: config loaded, sites=" + std::to_string(st.config.sites.size()) +
            " skipped=" + std::to_string(st.config.skippedSpiders));

    Page page = newPage("TVBox-NX");
    logLine("createRoot: page created");

    std::string summary = st.hasSite()
                              ? ("当前源：" + st.site().name + "    共 " +
                                 std::to_string(st.config.sites.size()) + " 个 CMS 源")
                              : "没有可用源";
    addInfo(page, summary);
    if (!st.lastError.empty()) addInfo(page, st.lastError);
    if (st.config.skippedSpiders > 0) {
        addInfo(page, "已跳过 " + std::to_string(st.config.skippedSpiders) +
                          " 个蜘蛛源（type=3，需要 ART/JS，Switch 跑不了）");
    }

    addSection(page, "站源列表");
    if (st.config.sites.empty()) {
        addInfo(page, "把 config.json 放到 sdmc:/switch/tvbox-nx/ 下再重启程序");
    }
    for (size_t i = 0; i < st.config.sites.size(); ++i) {
        const Site& s = st.config.sites[i];
        std::string text = s.name;
        if ((int)i == st.siteIndex) text = "[当前] " + text;
        text += "   " + s.api;

        size_t idx = i;
        addRow(page, text, [idx]() {
            state().siteIndex = (int)idx;
            brls::Application::notify("已切换到 " + state().site().name);
        });
    }

    addSection(page, "浏览");
    if (st.hasSite()) {
        std::string api = st.site().api;
        addRow(page, "分类列表", [api]() {
            CmsClient client(api);
            std::vector<Category> cats;
            std::string err;
            if (!client.fetchCategories(cats, err)) {
                brls::Application::notify("分类接口失败: " + err);
                return;
            }
            Page catsPage = newPage("分类");
            if (cats.empty()) {
                addInfo(catsPage, "这个源没有返回分类");
            }
            for (size_t k = 0; k < cats.size(); ++k) {
                long id       = cats[k].id;
                std::string n = cats[k].name;
                addRow(catsPage, n, [id, n]() { openCategory(id, n); });
            }
            pushPage(catsPage.frame);
        });
        addRow(page, "搜索", [api]() { pushPage(buildSearch(api)); });
    } else {
        addInfo(page, "没有可用站源，无法浏览");
    }

    addSection(page, "操作");
    addRow(page, "重新读取 config.json   " + st.configPath, []() {
        if (state().loadConfig())
            brls::Application::notify("配置已重新加载");
        else
            brls::Application::notify("加载失败: " + state().lastError);
    });
    addRow(page, "临时填一个 CMS 接口地址", []() {
        brls::Application::getImeManager()->openForText(
            [](std::string text) {
                if (text.empty()) return;
                Config cfg;
                std::string err;
                if (!Config::fromText(text, cfg, err)) {
                    brls::Application::notify("地址不可用: " + err);
                    return;
                }
                state().config    = cfg;
                state().siteIndex = 0;
                state().lastError.clear();
                brls::Application::notify("已临时切换到该地址");
            },
            "API 地址", "形如 https://host/api.php/provide/vod", 200, "https://");
    });

    logLine("createRoot: done");
    return page.frame;
}

}  // namespace tbx
