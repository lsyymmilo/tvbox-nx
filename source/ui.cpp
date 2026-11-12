// ui.cpp —— 仿 TVBox 的 Switch 界面：左侧分类栏 + 右侧海报墙 + 详情页选集。
//
// 这一版 borealis（moonlight_wiliwili 分支）的两个硬约束已在这里绕开：
//   * ScrollingFrame 自己 setPadding 会抛异常 → padding 设在内容 Box 上；
//   * ScrollingFrame::addView 等于 setContentView（替换而非追加）→ 统一用内容 Box。
#include "ui.hpp"

#include <GLFW/glfw3.h>
#include <dirent.h>

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

// 只负责搭内容，往哪个 frame 里塞由调用方决定（塞之前要先清焦点，见 Home::swapGridContent）
brls::Box* buildGridContent(const std::vector<Vod>& vods, const std::string& api) {
    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(10, 4, 24, 10);

    if (vods.empty()) {
        brls::Label* empty = makeLabel("这个分类没有数据", 20, colDim());
        empty->setMargins(20, 20, 0, 0);
        content->addView(empty);
        return content;
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
    return content;
}

// ------------------------------------------------------------------ 首页（侧栏 + 海报墙）

struct Home;
brls::View* buildSourcePage(Home* home);   // 换源页，定义在后面

struct Home {
    brls::ScrollingFrame* sidebar = nullptr;
    brls::ScrollingFrame* grid    = nullptr;
    brls::Label* title            = nullptr;
    brls::Label* hint             = nullptr;
    brls::Label* srcLabel         = nullptr;
    brls::Label* errLabel         = nullptr;
    brls::Button* sourceBtn       = nullptr;   // 稳定焦点锚点：永不随内容重建被销毁
    brls::Box* catList            = nullptr;   // 分类按钮容器，只换它的子控件

    std::string api() const { return state().hasSite() ? state().site().api : std::string(); }

    // 任何 clearViews / setContentView 之前，先把焦点挪到不会销毁的按钮上。
    // 否则 Application::currentFocus 与祖先的 lastFocusedView 会指向已被 delete 的控件，
    // 下一次 giveFocus 就会在已释放对象上执行 onFocusLost（真机 2168-0001 Instruction Abort）。
    void parkFocus() {
        if (sourceBtn) brls::Application::giveFocus(sourceBtn);
    }

    // 换海报墙内容：先清掉 frame 上可能悬垂的 lastFocusedView，再换 contentView
    void swapGridContent(brls::Box* content) {
        if (!grid) return;
        grid->setLastFocusedView(nullptr);
        grid->setContentView(content);
    }

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
        parkFocus();                                   // 旧海报卡片要整批删掉，焦点先撤出
        brls::Box* content = buildGridContent(vods, useApi);
        swapGridContent(content);
        brls::Application::giveFocus(content);         // 落到第一张卡片上
    }

    // 侧栏骨架：只在启动时搭一次。搜索/换源按钮留在这层，作为稳定的焦点落点
    void buildSidebar() {
        brls::Box* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(18, 14, 20, 16);

        brls::Label* logo = makeLabel("TVBox-NX", 30, colAccent());
        logo->setMargins(0, 0, 6, 0);
        content->addView(logo);

        srcLabel = makeLabel("源: …", 14, colDim());
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

        // 换源：点开下级菜单，可以选本地配置文件，也可以从网络加载
        sourceBtn = new brls::Button();
        sourceBtn->setText("切换源");
        sourceBtn->setFontSize(18);
        sourceBtn->setWidth(200);
        sourceBtn->setHeight(48);
        sourceBtn->setMargins(0, 0, 14, 0);
        sourceBtn->registerClickAction([this](brls::View*) {
            brls::Application::pushActivity(new brls::Activity(buildSourcePage(this)));
            return true;
        });
        content->addView(sourceBtn);

        brls::Label* catTitle = makeLabel("分类", 18, colText());
        catTitle->setMargins(0, 0, 8, 0);
        content->addView(catTitle);

        catList = new brls::Box(brls::Axis::COLUMN);
        catList->setWidthPercentage(100);
        catList->setPadding(0, 0, 0, 0);
        content->addView(catList);

        errLabel = makeLabel("", 14, colDim(), false);
        errLabel->setIsWrapping(true);
        errLabel->setMargins(8, 0, 0, 0);
        content->addView(errLabel);

        sidebar->setContentView(content);
    }

    // 只换分类按钮和两行文字，骨架与焦点锚点不动
    void refillCategories() {
        if (!catList) return;
        parkFocus();                    // 即将删掉的按钮可能带着焦点，先撤出
        catList->clearViews();          // 顺带清掉 lastFocusedView，避免指向已删除控件

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
            catList->addView(b);
        }

        if (cats.empty()) {
            brls::Label* e = makeLabel("分类接口没返回数据", 15, colDim());
            e->setMargins(0, 0, 8, 0);
            catList->addView(e);
        }

        if (srcLabel)
            srcLabel->setText("源: " + (state().hasSite() ? state().site().name : std::string("无可用源")));
        if (errLabel) errLabel->setText(state().lastError);
    }

    // 用当前 state().config 重新拉分类、重建侧栏与海报墙（不重新读文件）
    void refreshSources() {
        AppState& st = state();
        parkFocus();                    // 重建前先把焦点撤到稳定按钮上
        st.categories.clear();
        if (st.hasSite()) {
            CmsClient client(st.site().api);
            std::string err;
            client.fetchCategories(st.categories, err);
        }
        refillCategories();
        if (!st.categories.empty()) {
            loadCategory(st.categories[0].id, st.categories[0].name);
        } else {
            if (title) title->setText("TVBox-NX");
            if (hint) hint->setText("没有可用分类");
            brls::Box* content = new brls::Box(brls::Axis::COLUMN);
            content->setPadding(20, 10, 0, 10);
            brls::Label* e = makeLabel(st.hasSite() ? "这个源没有返回分类"
                                                   : "没有可用站源，点左侧「切换源」填一个地址",
                                       18, colDim(), false);
            e->setIsWrapping(true);
            content->addView(e);
            swapGridContent(content);
        }
    }
};

// ------------------------------------------------------------------ 换源页

// sdmc:/switch/tvbox-nx 下扫到的 .json（配置文件）
std::vector<std::string> listLocalConfigs() {
    std::vector<std::string> files;
    DIR* dir = opendir("sdmc:/switch/tvbox-nx");
    if (!dir) return files;
    while (struct dirent* ent = readdir(dir)) {
        std::string name = ent->d_name;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
            files.push_back(name);
    }
    closedir(dir);
    return files;
}

// 让一份配置文本生效：解析 → 存成 config.json（下次开机沿用）→ 刷新界面 → 退回主界面
void applyConfigText(Home* home, const std::string& text, const std::string& from) {
    Config cfg;
    std::string err;
    if (!Config::fromText(text, cfg, err) || cfg.sites.empty()) {
        brls::Application::notify("配置不可用: " + (err.empty() ? std::string("没有可用站源") : err));
        return;
    }

    AppState& st = state();
    st.config    = cfg;
    st.siteIndex = 0;
    st.lastError.clear();
    ensureDir("sdmc:/switch/tvbox-nx");
    writeWholeFile(st.configPath, text);   // 落地，下次开机直接读
    logLine("source: applied, from=" + from + " sites=" + std::to_string(cfg.sites.size()) +
            " skippedSpiders=" + std::to_string(cfg.skippedSpiders));
    brls::Application::notify("已应用（" + from + "）：" + std::to_string(cfg.sites.size()) + " 个源");
    // 顺序很关键：先把焦点从"即将被重建掉的控件"上撤走，再重建，最后才退页。
    // 反过来的话 popActivity 会从 focusStack 里取出一个已 delete 的按钮去拿焦点 → 崩。
    home->parkFocus();
    home->refreshSources();
    brls::Application::popActivity();
}

void applyConfigFile(Home* home, const std::string& path) {
    std::string text;
    if (!readWholeFile(path, text)) {
        brls::Application::notify("读不到配置文件: " + path);
        return;
    }
    applyConfigText(home, text, path);
}

brls::Button* menuButton(const std::string& text, float width = 620) {
    brls::Button* b = new brls::Button();
    b->setText(text);
    b->setFontSize(19);
    b->setWidth(width);
    b->setHeight(56);
    b->setMargins(0, 0, 12, 0);
    return b;
}

brls::View* buildSourcePage(Home* home) {
    brls::ScrollingFrame* page = new brls::ScrollingFrame();
    brls::Box* content         = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(26, 40, 30, 30);
    page->setContentView(content);

    brls::Label* title = makeLabel("切换源", 28, colText());
    title->setMargins(0, 0, 6, 0);
    content->addView(title);

    brls::Label* sub = makeLabel(
        "可填单个 CMS 接口地址，也可给整份 TVBox 配置（json）。应用后会存成 "
        "sdmc:/switch/tvbox-nx/config.json，下次开机直接沿用。", 15, colDim(), false);
    sub->setIsWrapping(true);
    sub->setMargins(0, 0, 18, 0);
    content->addView(sub);

    // ---- 从网络加载
    brls::Button* fromNet = menuButton("从网络加载配置（输入 URL）");
    fromNet->registerClickAction([home](brls::View*) {
        brls::Application::getImeManager()->openForText(
            [home](std::string url) {
                if (url.empty()) return;
                brls::Application::notify("下载配置中…");
                HttpResult r = httpGet(url, 15);
                if (!r.ok()) {
                    brls::Application::notify(
                        "下载失败: " + (r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error));
                    return;
                }
                if (r.body.size() < 64) {
                    brls::Application::notify("下载内容太短，不像配置");
                    return;
                }
                applyConfigText(home, r.body, "网络");
            },
            "配置地址", "TVBox 配置 json 的地址，或单个 .../api.php/provide/vod", 300, "https://");
        return true;
    });
    content->addView(fromNet);

    // ---- 直接输接口地址
    brls::Button* fromApi = menuButton("直接输入 CMS 接口地址");
    fromApi->registerClickAction([home](brls::View*) {
        brls::Application::getImeManager()->openForText(
            [home](std::string url) {
                if (url.empty()) return;
                applyConfigText(home, url, "接口地址");
            },
            "接口地址", "形如 https://host/api.php/provide/vod", 300, "https://");
        return true;
    });
    content->addView(fromApi);

    // ---- 本地配置文件：扫目录列出来
    brls::Label* localTitle = makeLabel("本地配置文件", 20, colText());
    localTitle->setMargins(8, 0, 8, 0);
    content->addView(localTitle);

    std::vector<std::string> files = listLocalConfigs();
    if (files.empty()) {
        brls::Label* e = makeLabel("sdmc:/switch/tvbox-nx/ 下没有 .json 文件", 15, colDim());
        e->setMargins(0, 0, 10, 0);
        content->addView(e);
    }
    for (size_t i = 0; i < files.size(); ++i) {
        std::string path = "sdmc:/switch/tvbox-nx/" + files[i];
        brls::Button* b  = menuButton(files[i], 500);
        b->registerClickAction([home, path](brls::View*) {
            applyConfigFile(home, path);
            return true;
        });
        content->addView(b);
    }

    brls::Button* sample = menuButton("内置示例源（romfs:/config.sample.json）", 500);
    sample->registerClickAction([home](brls::View*) {
        applyConfigFile(home, "romfs:/config.sample.json");
        return true;
    });
    content->addView(sample);

    // ---- 手动输路径
    brls::Button* fromPath = menuButton("手动输入本地文件路径");
    fromPath->registerClickAction([home](brls::View*) {
        brls::Application::getImeManager()->openForText(
            [home](std::string path) {
                if (path.empty()) return;
                applyConfigFile(home, path);
            },
            "文件路径", "例如 sdmc:/switch/tvbox-nx/config.json", 300, "sdmc:/");
        return true;
    });
    content->addView(fromPath);

    brls::Label* backHint = makeLabel("按 B 返回", 14, colDim());
    backHint->setMargins(12, 0, 0, 0);
    content->addView(backHint);

    return page;
}

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
    home->buildSidebar();
    home->refillCategories();
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
