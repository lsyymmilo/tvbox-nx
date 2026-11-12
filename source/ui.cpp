// ui.cpp —— borealis 界面：源选择 / 分类 / 列表 / 详情 / 播放。
//
// 设计取舍：
//   * 数据请求是同步的（点击后阻塞 1~2 秒），换来没有线程/回调的复杂度。
//     TODO：接网络慢的源时可以换成工作线程 + brls::sync 回 UI 线程。
//   * 播放用 libmpv render API 画到 FBO 0，由 MpvView::draw 每帧驱动。
//     若你的 borealis 版本 View::draw 形参不同，编译会在这一处报错；
//     mpv 渲染这段可直接对照 switchfin 的 mpv View 实现补齐。
#include "ui.hpp"

#include <GLFW/glfw3.h>

#include <string>
#include <vector>

#include "net.hpp"

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
        lastError = "未找到 config.json，当前使用内置示例源";
        siteIndex = 0;
        return true;
    }
    lastError = err;
    return false;
}

namespace {

// ============================================================ 播放视图

class MpvView : public brls::View {
public:
    MpvView(std::string url, std::string title)
        : url_(std::move(url)), title_(std::move(title)) {
        // A 暂停 / LB 后退 30s / RB 前进 30s / Y 切换倍速
        registerAction("暂停", brls::ControllerButton::BUTTON_A, [](brls::View*) {
            state().player.togglePause();
            return true;
        });
        registerAction("快退", brls::ControllerButton::BUTTON_LB, [](brls::View*) {
            state().player.seekRelative(-30.0);
            return true;
        });
        registerAction("快进", brls::ControllerButton::BUTTON_RB, [](brls::View*) {
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

        mpv_render_context_update(ctx_);

        mpv_opengl_fbo fbo;
        fbo.fbo = 0;
        fbo.w = (int)width;
        fbo.h = (int)height;
        fbo.internal_format = 0;

        int flipY = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(ctx_, params);
    }

    bool isTranslucent() override { return false; }

private:
    // mpv 要求的是 (void* ctx, const char* name) 两参形式
    static void* getProcAddress(void* ctx, const char* name) {
        (void)ctx;
        return (void*)glfwGetProcAddress(name);
    }

    void createContext() {
        mpv_opengl_init_params gl;
        gl.get_proc_address = &MpvView::getProcAddress;
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
        state().player.attachRender(ctx_);
        // TODO(必补)：mpv_render_context_set_update_callback 里要触发一次界面重绘。
        // 未接该回调时：首帧与 seek 后能出画面，但正常播放过程中画面不会持续刷新。
    }

    std::string url_;
    std::string title_;
    mpv_render_context* ctx_ = nullptr;
    bool opened_ = false;
    double speed_ = 1.0;
};

// ============================================================ 详情页

class DetailFrame : public brls::List {
public:
    explicit DetailFrame(const Vod& vod) {
        brls::Label* title = new brls::Label();
        title->setText(vod.name);
        addView(title);

        std::string meta = vod.year;
        if (!vod.area.empty()) meta += (meta.empty() ? "" : "  ") + vod.area;
        if (!vod.remarks.empty()) meta += (meta.empty() ? "" : "  ") + vod.remarks;
        if (!meta.empty()) {
            brls::Label* m = new brls::Label();
            m->setText(meta);
            addView(m);
        }

        if (!vod.content.empty()) {
            brls::Label* c = new brls::Label();
            c->setText(vod.content);
            addView(c);
        }

        if (vod.routes.empty()) {
            addView(new brls::Header("没有解析到播放串"));
            return;
        }

        for (size_t i = 0; i < vod.routes.size(); ++i) {
            const Route& route = vod.routes[i];
            addView(new brls::Header(route.from));
            for (size_t k = 0; k < route.episodes.size(); ++k) {
                const Episode& ep = route.episodes[k];
                brls::ListItem* item = new brls::ListItem(ep.name);
                if (ep.direct) {
                    item->setDetail("直链");
                    std::string url = ep.url;
                    std::string label = vod.name + " " + ep.name;
                    item->registerClickAction([url, label](brls::View*) {
                        openPlayer(url, label);
                        return true;
                    });
                } else {
                    item->setDetail("需解析，不支持");
                    item->registerClickAction([](brls::View*) {
                        brls::Application::notify("该集是网页地址，Switch 上没有解析器");
                        return true;
                    });
                }
                addView(item);
            }
        }
    }
};

// ============================================================ 影片列表页

class VodListFrame : public brls::List {
public:
    VodListFrame(const std::string& title, const std::vector<Vod>& vods, const std::string& api) {
        addView(new brls::Header(title));

        if (vods.empty()) {
            brls::Label* empty = new brls::Label();
            empty->setText("这个分类没有数据");
            addView(empty);
            return;
        }

        for (size_t i = 0; i < vods.size(); ++i) {
            const Vod& v = vods[i];
            std::string text = v.name;
            if (!v.remarks.empty()) text += "  [" + v.remarks + "]";

            long id = v.id;
            std::string itemApi = api;
            brls::ListItem* item = new brls::ListItem(text);
            item->registerClickAction([itemApi, id](brls::View*) {
                CmsClient client(itemApi);
                Vod full;
                std::string err;
                if (!client.fetchDetail(id, full, err)) {
                    brls::Application::notify("详情加载失败: " + err);
                    return true;
                }
                brls::Application::pushView(new DetailFrame(full));
                return true;
            });
            addView(item);
        }
    }
};

// ============================================================ 分类页

class CategoryFrame : public brls::List {
public:
    explicit CategoryFrame(const std::string& api) {
        CmsClient client(api);
        std::vector<Category> cats;
        std::string err;

        if (!client.fetchCategories(cats, err)) {
            brls::Label* fail = new brls::Label();
            fail->setText("接口失败：" + err);
            addView(fail);
            return;
        }

        addView(new brls::Header("分类"));
        for (size_t i = 0; i < cats.size(); ++i) {
            long id = cats[i].id;
            std::string name = cats[i].name;
            std::string itemApi = api;

            brls::ListItem* item = new brls::ListItem(name);
            item->registerClickAction([itemApi, id, name](brls::View*) {
                openCategory(id, name);
                return true;
            });
            addView(item);
        }
    }
};

// ============================================================ 搜索页

class SearchFrame : public brls::List {
public:
    explicit SearchFrame(const std::string& api) {
        addView(new brls::Header("搜索"));

        std::string itemApi = api;
        brls::ListItem* item = new brls::ListItem("输入关键词");
        item->setDetail("按 A 打开键盘");
        item->registerClickAction([itemApi](brls::View*) {
            brls::Application::getImeManager()->openForText(
                [itemApi](std::string text) {
                    if (text.empty()) return;
                    CmsClient client(itemApi);
                    std::vector<Vod> vods;
                    std::string err;
                    if (!client.fetchSearch(text, vods, err)) {
                        brls::Application::notify("搜索失败: " + err);
                        return;
                    }
                    brls::Application::pushView(
                        new VodListFrame("搜索：" + text, vods, itemApi));
                },
                "搜索", "输入片名后确认", 32, "");
            return true;
        });
        addView(item);
    }
};

// ============================================================ 源管理页

class SourceFrame : public brls::List {
public:
    SourceFrame() { refresh(); }

    void refresh() {
        AppState& st = state();
        addView(new brls::Header("配置"));

        std::string summary = st.hasSite()
                                  ? ("当前源：" + st.site().name + "   共 " +
                                     std::to_string(st.config.sites.size()) + " 个 CMS 源")
                                  : "没有可用源";
        brls::Label* info = new brls::Label();
        info->setText(summary);
        addView(info);

        if (!st.lastError.empty()) {
            brls::Label* warn = new brls::Label();
            warn->setText(st.lastError);
            addView(warn);
        }

        if (st.config.skippedSpiders > 0) {
            brls::Label* skip = new brls::Label();
            skip->setText("已跳过 " + std::to_string(st.config.skippedSpiders) +
                          " 个蜘蛛源（type=3，需要 ART/JS，Switch 跑不了）");
            addView(skip);
        }

        addView(new brls::Header("站源列表"));
        for (size_t i = 0; i < st.config.sites.size(); ++i) {
            const Site& s = st.config.sites[i];
            std::string text = s.name;
            if ((int)i == st.siteIndex) text = "[当前] " + text;

            brls::ListItem* item = new brls::ListItem(text);
            item->setDetail(s.api);
            size_t idx = i;
            item->registerClickAction([idx](brls::View*) {
                state().siteIndex = (int)idx;
                brls::Application::notify("已切换到 " + state().site().name);
                return true;
            });
            addView(item);
        }

        addView(new brls::Header("操作"));
        brls::ListItem* reload = new brls::ListItem("重新读取 config.json");
        reload->setDetail(state().configPath);
        reload->registerClickAction([](brls::View*) {
            if (state().loadConfig())
                brls::Application::notify("配置已重新加载");
            else
                brls::Application::notify("加载失败: " + state().lastError);
            return true;
        });
        addView(reload);

        brls::ListItem* manual = new brls::ListItem("临时填一个 API 地址");
        manual->setDetail("形如 https://host/api.php/provide/vod");
        manual->registerClickAction([](brls::View*) {
            brls::Application::getImeManager()->openForText(
                [](std::string text) {
                    if (text.empty()) return;
                    Config cfg;
                    std::string err;
                    if (!Config::fromText(text, cfg, err)) {
                        brls::Application::notify("地址不可用: " + err);
                        return;
                    }
                    state().config = cfg;
                    state().siteIndex = 0;
                    state().lastError.clear();
                    brls::Application::notify("已临时切换到该地址");
                },
                "API 地址", "粘贴 CMS 接口地址", 200, "https://");
            return true;
        });
        addView(manual);
    }
};

}  // namespace

// ============================================================ 对外接口

void openCategory(long categoryId, const std::string& title) {
    if (!state().hasSite()) {
        brls::Application::notify("先选择一个站源");
        return;
    }
    CmsClient client(state().site().api);
    std::vector<Vod> vods;
    long pages = 1;
    std::string err;
    if (!client.fetchList(categoryId, 1, vods, pages, err)) {
        brls::Application::notify("列表加载失败: " + err);
        return;
    }
    brls::Application::pushView(new VodListFrame(title, vods, state().site().api));
}

void openDetail(const Vod& vod) { brls::Application::pushView(new DetailFrame(vod)); }

void openPlayer(const std::string& url, const std::string& title) {
    std::string err;
    if (!state().player.init(err)) {
        brls::Application::notify("播放器初始化失败: " + err);
        return;
    }
    brls::Application::pushView(new MpvView(url, title));
}

brls::View* createRoot() {
    AppState& st = state();
    st.loadConfig();

    brls::TabFrame* root = new brls::TabFrame();
    root->addTab("源", new SourceFrame());

    if (st.hasSite()) {
        root->addTab("分类", new CategoryFrame(st.site().api));
        root->addTab("搜索", new SearchFrame(st.site().api));
    } else {
        brls::List* empty = new brls::List();
        brls::Label* tip = new brls::Label();
        tip->setText("没有可用站源：把 config.json 放到 sdmc:/switch/tvbox-nx/ 下");
        empty->addView(tip);
        root->addTab("分类", empty);
    }
    return root;
}

}  // namespace tbx
