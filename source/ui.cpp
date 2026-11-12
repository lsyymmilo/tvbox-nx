// ui.cpp —— 仿 TVBox 的 Switch 界面：左侧分类栏 + 右侧海报墙 + 详情页选集。
//
// 这一版 borealis（moonlight_wiliwili 分支）的两个硬约束已在这里绕开：
//   * ScrollingFrame 自己 setPadding 会抛异常 → padding 设在内容 Box 上；
//   * ScrollingFrame::addView 等于 setContentView（替换而非追加）→ 统一用内容 Box。
#include "ui.hpp"

#include <GLFW/glfw3.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "log.hpp"
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
        lastError = "未找到 config.json，当前用内置示例源（把配置放到 sdmc:/switch/tvbox-nx/config.json）";
        siteIndex = 0;
        return true;
    }
    lastError = err;
    return false;
}

namespace {

// ------------------------------------------------------------------ 配色 / 尺寸

constexpr float kSidebarW = 250.f;
constexpr float kCardW    = 196.f;
constexpr float kPosterH  = 288.f;
constexpr int kCols       = 5;
constexpr int kMaxPosters = 25;  // 首屏最多下载这么多张海报，避免卡住界面

NVGcolor colText() { return nvgRGB(238, 240, 244); }
NVGcolor colDim() { return nvgRGB(148, 154, 166); }
NVGcolor colPanel() { return nvgRGB(23, 25, 29); }
NVGcolor colAccent() { return nvgRGB(229, 76, 76); }

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color, bool singleLine = true) {
    brls::Label* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    label->setSingleLine(singleLine);
    return label;
}

// 海报：同步下载后直接塞进纹理。失败就不显示图，排版不受影响。
void loadPoster(brls::Image* image, const std::string& url) {
    if (url.empty()) return;
    HttpResult r = httpGet(url, 10);
    if (!r.ok() || r.body.size() < 64) return;
    image->setImageFromMem(reinterpret_cast<const unsigned char*>(r.body.data()),
                           (int)r.body.size());
}

// ------------------------------------------------------------------ 播放视图

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
    bool opened_             = false;
    double speed_            = 1.0;
};

// ------------------------------------------------------------------ 详情页

brls::View* buildDetailView(const Vod& vod, const std::string& api);

// 点卡片 → 先按 id 取详情（列表接口不带播放串），再建详情页
void openDetailById(const std::string& api, long id, const std::string& name) {
    brls::Application::notify("加载中…");
    CmsClient client(api);
    Vod full;
    std::string err;
    if (!client.fetchDetail(id, full, err) || full.routes.empty()) {
        brls::Application::notify("详情加载失败: " + (err.empty() ? "没有播放数据" : err));
        return;
    }
    brls::Application::pushActivity(new brls::Activity(buildDetailView(full, api)));
}

brls::Button* makeEpButton(const std::string& text, float width, std::function<void()> onClick) {
    brls::Button* b = new brls::Button();
    b->setText(text);
    b->setFontSize(16);
    b->setWidth(width);
    b->setHeight(50);
    b->setMargins(0, 10, 10, 0);
    b->registerClickAction([onClick](brls::View*) {
        onClick();
        return true;
    });
    return b;
}

brls::View* buildDetailView(const Vod& vod, const std::string& api) {
    (void)api;
    logLine("detail: " + vod.name + " routes=" + std::to_string(vod.routes.size()));

    brls::ScrollingFrame* page = new brls::ScrollingFrame();
    brls::Box* content         = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(26, 30, 30, 26);
    page->setContentView(content);

    // ---- 顶部：海报 + 标题/元信息/简介
    brls::Box* head = new brls::Box(brls::Axis::ROW);
    head->setWidthPercentage(100);
    head->setMargins(0, 0, 18, 0);

    brls::Image* poster = new brls::Image();
    poster->setWidth(230);
    poster->setHeight(340);
    poster->setCornerRadius(10);
    poster->setScalingType(brls::ImageScalingType::FILL);
    poster->setImageAlign(brls::ImageAlignment::CENTER);
    loadPoster(poster, vod.pic);
    head->addView(poster);

    brls::Box* info = new brls::Box(brls::Axis::COLUMN);
    info->setGrow(1);
    info->setMargins(0, 0, 0, 22);
    info->setPadding(0, 0, 0, 0);

    brls::Label* title = makeLabel(vod.name, 30, colText());
    title->setMargins(0, 0, 10, 0);
    info->addView(title);

    std::string meta = vod.year;
    if (!vod.area.empty()) meta += (meta.empty() ? "" : "  ·  ") + vod.area;
    if (!vod.remarks.empty()) meta += (meta.empty() ? "" : "  ·  ") + vod.remarks;
    if (!meta.empty()) {
        brls::Label* m = makeLabel(meta, 17, colDim());
        m->setMargins(0, 0, 10, 0);
        info->addView(m);
    }

    if (!vod.content.empty()) {
        brls::Label* desc = makeLabel(vod.content, 16, colDim(), false);
        desc->setIsWrapping(true);
        desc->setMargins(0, 0, 8, 0);
        info->addView(desc);
    }
    head->addView(info);
    content->addView(head);

    // ---- 选集：每条线路一段标题 + 每行 5 个按钮
    for (size_t i = 0; i < vod.routes.size(); ++i) {
        const Route& route = vod.routes[i];

        brls::Label* section = makeLabel(route.from.empty() ? "线路" : route.from, 21, colText());
        section->setMargins(10, 0, 10, 0);
        content->addView(section);

        brls::Box* row = nullptr;
        for (size_t k = 0; k < route.episodes.size(); ++k) {
            if (k % kCols == 0) {
                row = new brls::Box(brls::Axis::ROW);
                content->addView(row);
            }
            const Episode& ep = route.episodes[k];
            std::string text = ep.name;
            if (!ep.direct) text += " (需解析)";

            if (ep.direct) {
                std::string url   = ep.url;
                std::string t     = vod.name + " " + ep.name;
                row->addView(makeEpButton(text, 190, [url, t]() { openPlayer(url, t); }));
            } else {
                row->addView(makeEpButton(text, 190, []() {
                    brls::Application::notify("这一集是网页地址，Switch 上没有解析器，不能直连播放");
                }));
            }
        }
    }
    return page;
}

// ------------------------------------------------------------------ 海报墙

brls::Box* makeCard(const Vod& v, const std::string& api, bool withPoster) {
    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setWidth(kCardW);
    card->setHeight(kPosterH + 64);
    card->setCornerRadius(12);
    card->setHighlightPadding(4);
    card->setHighlightCornerRadius(14);
    card->setMargins(0, 14, 16, 0);
    card->setFocusable(true);

    long id           = v.id;
    std::string name  = v.name;
    std::string apiS  = api;
    card->registerClickAction([apiS, id, name](brls::View*) {
        openDetailById(apiS, id, name);
        return true;
    });

    brls::Image* poster = new brls::Image();
    poster->setWidth(kCardW);
    poster->setHeight(kPosterH);
    poster->setCornerRadius(12);
    poster->setScalingType(brls::ImageScalingType::FILL);
    poster->setImageAlign(brls::ImageAlignment::CENTER);
    if (withPoster) loadPoster(poster, v.pic);
    card->addView(poster);

    brls::Label* nameLabel = makeLabel(v.name, 18, colText());
    nameLabel->setMargins(8, 0, 0, 0);
    card->addView(nameLabel);

    if (!v.remarks.empty()) {
        brls::Label* rem = makeLabel(v.remarks, 14, colDim());
        rem->setMargins(3, 0, 0, 0);
        card->addView(rem);
    }
    return card;
}

void fillGrid(brls::ScrollingFrame* frame, const std::vector<Vod>& vods, const std::string& api) {
    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(10, 4, 24, 10);

    if (vods.empty()) {
        brls::Label* empty = makeLabel("这个分类没有数据", 20, colDim());
        empty->setMargins(20, 20, 0, 0);
        content->addView(empty);
        frame->setContentView(content);
        return;
    }

    brls::Box* row = nullptr;
    for (size_t i = 0; i < vods.size(); ++i) {
        if (i % kCols == 0) {
            row = new brls::Box(brls::Axis::ROW);
            row->setMargins(i == 0 ? 0 : 6, 0, 0, 0);
            content->addView(row);
        }
        bool withPoster = ((int)i < kMaxPosters);
        row->addView(makeCard(vods[i], api, withPoster));
    }
    frame->setContentView(content);
}

// ------------------------------------------------------------------ 首页（侧栏 + 海报墙）

struct Home {
    brls::ScrollingFrame* sidebar = nullptr;
    brls::ScrollingFrame* grid    = nullptr;
    brls::Label* title            = nullptr;
    brls::Label* hint             = nullptr;

    std::string api() const { return state().hasSite() ? state().site().api : std::string(); }

    void loadCategory(long id, const std::string& name) {
        if (title) title->setText(name);
        if (hint) hint->setText("加载中…");
        std::string useApi = api();
        if (useApi.empty()) {
            if (hint) hint->setText("没有可用站源");
            return;
        }
        CmsClient client(useApi);
        std::vector<Vod> vods;
        long pages = 1;
        std::string err;
        if (!client.fetchList(id, 1, vods, pages, err)) {
            if (hint) hint->setText("列表加载失败: " + err);
            return;
        }
        if (hint) hint->setText(std::to_string(vods.size()) + " 部   ·   共 " + std::to_string(pages) + " 页");
        fillGrid(grid, vods, useApi);
    }

    void rebuildSidebar() {
        brls::Box* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(18, 14, 20, 16);

        brls::Label* logo = makeLabel("TVBox-NX", 30, colAccent());
        logo->setMargins(0, 0, 6, 0);
        content->addView(logo);

        std::string src = state().hasSite() ? state().site().name : "无可用源";
        brls::Label* srcLabel = makeLabel("源: " + src, 14, colDim());
        srcLabel->setMargins(0, 0, 14, 0);
        content->addView(srcLabel);

        // 搜索
        brls::Button* search = new brls::Button();
        search->setText("搜索");
        search->setFontSize(18);
        search->setWidth(200);
        search->setHeight(48);
        search->setMargins(0, 0, 10, 0);
        search->registerClickAction([](brls::View*) {
            if (!state().hasSite()) {
                brls::Application::notify("先配置一个可用站源");
                return true;
            }
            brls::Application::getImeManager()->openForText(
                [](std::string text) {
                    if (text.empty()) return;
                    std::string useApi = state().site().api;
                    brls::Application::notify("搜索中…");
                    CmsClient client(useApi);
                    std::vector<Vod> vods;
                    std::string err;
                    if (!client.fetchSearch(text, vods, err)) {
                        brls::Application::notify("搜索失败: " + err);
                        return;
                    }
                    brls::Box* box = new brls::Box(brls::Axis::COLUMN);
                    box->setPadding(10, 4, 24, 10);
                    brls::Label* t = makeLabel("搜索：" + text, 22, colText());
                    t->setMargins(10, 0, 10, 0);
                    box->addView(t);

                    brls::Box* row = nullptr;
                    for (size_t i = 0; i < vods.size(); ++i) {
                        if (i % kCols == 0) {
                            row = new brls::Box(brls::Axis::ROW);
                            row->setMargins(i == 0 ? 0 : 6, 0, 0, 0);
                            box->addView(row);
                        }
                        row->addView(makeCard(vods[i], useApi, (int)i < kMaxPosters));
                    }
                    if (vods.empty()) {
                        brls::Label* e = makeLabel("没有搜到结果", 20, colDim());
                        e->setMargins(10, 0, 0, 0);
                        box->addView(e);
                    }

                    brls::ScrollingFrame* f = new brls::ScrollingFrame();
                    f->setContentView(box);
                    brls::Application::pushActivity(new brls::Activity(f));
                },
                "搜索", "输入片名后确认", 32, "");
            return true;
        });
        content->addView(search);

        // 换源
        brls::Button* next = new brls::Button();
        next->setText("切换源");
        next->setFontSize(18);
        next->setWidth(200);
        next->setHeight(48);
        next->setMargins(0, 0, 14, 0);
        next->registerClickAction([](brls::View*) {
            AppState& st = state();
            if (st.config.sites.size() < 2) {
                brls::Application::notify("配置里只有一个源");
                return true;
            }
            st.siteIndex = (st.siteIndex + 1) % (int)st.config.sites.size();
            brls::Application::notify("已切换到 " + st.site().name + "，重新打开本程序后生效");
            return true;
        });
        content->addView(next);

        brls::Label* catTitle = makeLabel("分类", 18, colText());
        catTitle->setMargins(0, 0, 8, 0);
        content->addView(catTitle);

        // 分类按钮：点了就在右侧换海报墙，不跳页
        const std::vector<Category>& cats = state().categories;
        for (size_t i = 0; i < cats.size(); ++i) {
            long id       = cats[i].id;
            std::string n = cats[i].name;
            brls::Button* b = new brls::Button();
            b->setText(n);
            b->setFontSize(17);
            b->setWidth(200);
            b->setHeight(46);
            b->setMargins(0, 0, 6, 0);
            Home* self = this;
            b->registerClickAction([self, id, n](brls::View*) {
                self->loadCategory(id, n);
                return true;
            });
            content->addView(b);
        }

        if (cats.empty()) {
            brls::Label* e = makeLabel("分类接口没返回数据", 15, colDim());
            e->setMargins(0, 0, 8, 0);
            content->addView(e);
        }

        if (!state().lastError.empty()) {
            brls::Label* e = makeLabel(state().lastError, 14, colDim(), false);
            e->setIsWrapping(true);
            e->setMargins(8, 0, 0, 0);
            content->addView(e);
        }

        sidebar->setContentView(content);
    }

    void reload() {
        AppState& st = state();
        st.loadConfig();
        st.categories.clear();
        if (st.hasSite()) {
            CmsClient client(st.site().api);
            std::string err;
            client.fetchCategories(st.categories, err);
        }
        rebuildSidebar();
        if (!st.categories.empty()) {
            loadCategory(st.categories[0].id, st.categories[0].name);
        } else {
            if (title) title->setText("TVBox-NX");
            if (hint) hint->setText("没有可用分类");
            brls::Box* content = new brls::Box(brls::Axis::COLUMN);
            content->setPadding(20, 10, 0, 10);
            brls::Label* e = makeLabel(st.hasSite() ? "这个源没有返回分类" : "没有可用站源，把 config.json 放到 sdmc:/switch/tvbox-nx/",
                                       18, colDim(), false);
            e->setIsWrapping(true);
            content->addView(e);
            grid->setContentView(content);
        }
    }
};

}  // namespace

// ------------------------------------------------------------------ 对外接口

brls::View* createRoot() {
    logLine("createRoot: begin");
    AppState& st = state();
    st.loadConfig();
    logLine("createRoot: config loaded, sites=" + std::to_string(st.config.sites.size()) +
            " skipped=" + std::to_string(st.config.skippedSpiders));

    // 故意不释放：Home 被分类按钮的 lambda 长期引用，跟着进程一起活到退出
    Home* home = new Home();

    // 整体：左栏（可滚动） + 右侧（标题条 + 海报墙）
    brls::Box* root = new brls::Box(brls::Axis::ROW);
    root->setBackgroundColor(colPanel());

    brls::ScrollingFrame* sidebar = new brls::ScrollingFrame();
    sidebar->setWidth(kSidebarW);
    sidebar->setHeightPercentage(100);
    root->addView(sidebar);
    home->sidebar = sidebar;
    logLine("createRoot: sidebar ok");

    brls::Box* main = new brls::Box(brls::Axis::COLUMN);
    main->setGrow(1);
    main->setPadding(10, 6, 0, 10);

    brls::Box* titleBar = new brls::Box(brls::Axis::ROW);
    titleBar->setMargins(10, 0, 12, 0);

    brls::Label* title = makeLabel("TVBox-NX", 26, colText());
    title->setMargins(0, 0, 18, 0);
    titleBar->addView(title);
    home->title = title;

    brls::Label* hint = makeLabel("", 15, colDim());
    titleBar->addView(hint);
    home->hint = hint;

    main->addView(titleBar);

    brls::ScrollingFrame* grid = new brls::ScrollingFrame();
    grid->setGrow(1);
    grid->setWidthPercentage(100);
    main->addView(grid);
    home->grid = grid;

    root->addView(main);

    // 拉分类 + 默认加载第一个分类
    if (st.hasSite()) {
        CmsClient client(st.site().api);
        std::string err;
        client.fetchCategories(st.categories, err);
        logLine("createRoot: categories=" + std::to_string(st.categories.size()));
    }
    home->rebuildSidebar();
    if (!st.categories.empty()) {
        home->loadCategory(st.categories[0].id, st.categories[0].name);
    }
    logLine("createRoot: done");

    return root;
}

void openCategory(long categoryId, const std::string& title) {
    (void)categoryId;
    (void)title;
}

void openDetail(const Vod& vod) {
    brls::Application::pushActivity(new brls::Activity(buildDetailView(vod, "")));
}

void openPlayer(const std::string& url, const std::string& title) {
    std::string err;
    if (!state().player.init(err)) {
        brls::Application::notify("播放器初始化失败: " + err);
        return;
    }
    brls::Application::pushActivity(new brls::Activity(new MpvView(url, title)));
}

}  // namespace tbx
