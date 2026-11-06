#include "settings_theme_tab.h"

#include "assets/lang_config.h"
#include "home_screen/home_screen.h"
#include "settings_common.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"

#include <cstdio>

struct SettingsThemeOption {
    int style; // 卡片样式枚举
    const char* (*title)(); // 选项标题
    const char* (*current_text)(); // 当前选中文案
};

static const char* ThemeTitleWhite() { return Lang::Strings::SETTINGS_THEME_WHITE; }
static const char* ThemeTitleGray() { return Lang::Strings::SETTINGS_THEME_GRAY; }
static const char* ThemeTitleBorder() { return Lang::Strings::SETTINGS_THEME_BORDER; }
static const char* ThemeTitleSlash() { return Lang::Strings::SETTINGS_THEME_SLASH; }
static const char* ThemeCurWhite() { return Lang::Strings::SETTINGS_THEME_CURRENT_WHITE; }
static const char* ThemeCurGray() { return Lang::Strings::SETTINGS_THEME_CURRENT_GRAY; }
static const char* ThemeCurBorder() { return Lang::Strings::SETTINGS_THEME_CURRENT_BORDER; }
static const char* ThemeCurSlash() { return Lang::Strings::SETTINGS_THEME_CURRENT_SLASH; }

static constexpr SettingsThemeOption kThemeOptions[] = {
    {HomeScreen::kCardStyleWhite, ThemeTitleWhite, ThemeCurWhite},
    {HomeScreen::kCardStyleGray, ThemeTitleGray, ThemeCurGray},
    {HomeScreen::kCardStyleBorder, ThemeTitleBorder, ThemeCurBorder},
    {HomeScreen::kCardStyleSlash, ThemeTitleSlash, ThemeCurSlash},
};
static constexpr int kThemeOptionCount = sizeof(kThemeOptions) / sizeof(kThemeOptions[0]);

// 墨水屏全刷频率：每 N 次整屏级变化全刷一次（小变化按面积折算）（全刷会黑白闪，清残影）；0=不自动
static constexpr int kFullEveryOptions[] = {5, 15, 30, 60, 0};
static constexpr int kFullEveryCount = sizeof(kFullEveryOptions) / sizeof(kFullEveryOptions[0]);

struct SettingsThemeUi {
    lv_obj_t* btns[kThemeOptionCount] = {}; // 选项按钮
    lv_obj_t* current_lbl = nullptr; // 当前主题文案
    lv_obj_t* full_btns[kFullEveryCount] = {}; // 全刷频率按钮
};

static SettingsThemeUi s_ui;

static const char* ThemeCurrentText(int style) {
    for (int i = 0; i < kThemeOptionCount; ++i) {
        if (kThemeOptions[i].style == style) {
            return kThemeOptions[i].current_text();
        }
    }
    return kThemeOptions[0].current_text();
}

static void RefreshThemeOptions() {
    const int style = HomeScreen::LoadCardStyle();
    for (int i = 0; i < kThemeOptionCount; ++i) {
        if (s_ui.btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0),
                                    kThemeOptions[i].style == style);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, ThemeCurrentText(style));
    }
}

static void OnThemeOptionClicked(lv_event_t* e) {
    const int style = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (HomeScreen::LoadCardStyle() == style) {
        return;
    }
    HomeScreen::SaveCardStyle(style);
    RefreshThemeOptions();
}

static void RefreshFullEvery() {
    const int cur = LVAdapterDisplay::GetFullRefreshEvery();
    for (int i = 0; i < kFullEveryCount; ++i) {
        if (s_ui.full_btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.full_btns[i], lv_obj_get_child(s_ui.full_btns[i], 0),
                                    kFullEveryOptions[i] == cur);
        }
    }
}

static void OnFullEveryClicked(lv_event_t* e) {
    const int n = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    LVAdapterDisplay::SetFullRefreshEvery(n);
    RefreshFullEvery();
}

static void OnFullNowClicked(lv_event_t*) {
    if (auto* d = LVAdapterDisplay::Instance()) {
        d->RequestFullRefreshNow();
    }
}

static lv_obj_t* SmallButton(lv_obj_t* parent, const char* text, lv_coord_t w, lv_event_cb_t cb,
                             intptr_t ud) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, 52);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(btn);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(ud));
    return btn;
}

static void BuildFullRefreshSection(lv_obj_t* page) {
    lv_obj_t* split = lv_obj_create(page);
    lv_obj_remove_style_all(split);
    lv_obj_set_size(split, lv_pct(100), kSettingsSplitLineW);
    lv_obj_set_style_bg_color(split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, "全刷频率（每N次换页）");
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);

    lv_obj_t* grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < kFullEveryCount; ++i) {
        char buf[24];
        if (kFullEveryOptions[i] == 0) {
            std::snprintf(buf, sizeof(buf), "仅手动");
        } else {
            std::snprintf(buf, sizeof(buf), "%d次", kFullEveryOptions[i]);
        }
        s_ui.full_btns[i] = SmallButton(grid, buf, 88, OnFullEveryClicked, kFullEveryOptions[i]);
    }
    // 第 6 格：立即全刷一次（不参与选中态）
    SmallButton(grid, "刷一次", 88, OnFullNowClicked, 0);
    RefreshFullEvery();
}

void SettingsThemeTab_Reset() {
    s_ui = {};
}

void SettingsThemeTab_Build(lv_obj_t* page) {
    s_ui = {};

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_THEME_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_THEME_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kThemeOptionCount; ++i) {
        s_ui.btns[i] =
            SettingsCreateSelectableOption(page, kThemeOptions[i].title(), OnThemeOptionClicked,
                                           static_cast<intptr_t>(kThemeOptions[i].style));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshThemeOptions();
    BuildFullRefreshSection(page);
}
