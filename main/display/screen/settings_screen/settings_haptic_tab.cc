#include "settings_haptic_tab.h"

#include <cstdio>
#include <string>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "settings_common.h"

struct SettingsHapticOption {
    bool enabled; // 是否开启震动
    const char* (*title)(); // 选项标题
    const char* (*current_text)(); // 当前选中文案
};

static const char* HapticTitleOn() { return Lang::Strings::SETTINGS_HAPTIC_ON; }
static const char* HapticTitleOff() { return Lang::Strings::SETTINGS_HAPTIC_OFF; }
static const char* HapticCurOn() { return Lang::Strings::SETTINGS_HAPTIC_CURRENT_ON; }
static const char* HapticCurOff() { return Lang::Strings::SETTINGS_HAPTIC_CURRENT_OFF; }

static constexpr SettingsHapticOption kOptions[] = {
    {true, HapticTitleOn, HapticCurOn},
    {false, HapticTitleOff, HapticCurOff},
};
static constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);
static constexpr int kIntensityOptionCount = HAPTIC_LEVELS;

struct SettingsHapticUi {
    lv_obj_t* btns[kOptionCount] = {}; // 开关选项按钮
    lv_obj_t* intensity_btns[kIntensityOptionCount] = {}; // 强度选项按钮
    lv_obj_t* current_lbl = nullptr; // 当前开关文案
    lv_obj_t* intensity_lbl = nullptr; // 当前强度文案
};

static SettingsHapticUi s_ui;

static const char* CurrentText(bool enabled) {
    for (int i = 0; i < kOptionCount; ++i) {
        if (kOptions[i].enabled == enabled) {
            return kOptions[i].current_text();
        }
    }
    return kOptions[0].current_text();
}

static std::string IntensityCurrentText(int intensity) {
    return "当前：第 " + std::to_string(intensity + 1) + " 档（共 " + std::to_string(HAPTIC_LEVELS) + " 档）";
}

static void RefreshOptions() {
    const bool enabled = HapticIsEnabled();
    for (int i = 0; i < kOptionCount; ++i) {
        if (s_ui.btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0),
                                    kOptions[i].enabled == enabled);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, CurrentText(enabled));
    }

    const int intensity = HapticGetIntensity();
    for (int i = 0; i < kIntensityOptionCount; ++i) {
        if (s_ui.intensity_btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.intensity_btns[i],
                                    lv_obj_get_child(s_ui.intensity_btns[i], 0), i == intensity);
        }
    }
    if (s_ui.intensity_lbl != nullptr) {
        lv_label_set_text(s_ui.intensity_lbl, IntensityCurrentText(intensity).c_str());
    }
}

static void OnOptionClicked(lv_event_t* e) {
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    if (HapticIsEnabled() == enable) {
        return;
    }
    HapticSetEnabled(enable);
    RefreshOptions();
    // 关→开时 AttachClick 仍读到旧值不会震，此处补一次确认反馈
    if (enable) {
        HapticPulseIfEnabled();
    }
}

static void OnIntensityClicked(lv_event_t* e) {
    const int intensity = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (HapticGetIntensity() == intensity) {
        return;
    }
    HapticSetIntensity(intensity);
    RefreshOptions();
    HapticPulseIfEnabled();  // 选完马上震一下，感受新档位
}

void SettingsHapticTab_Reset() {
    s_ui = {};
}

void SettingsHapticTab_Build(lv_obj_t* page) {
    s_ui = {};

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_HAPTIC_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_HAPTIC_HINT);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kOptionCount; ++i) {
        s_ui.btns[i] = SettingsCreateSelectableOption(
            page, kOptions[i].title(), OnOptionClicked,
            static_cast<intptr_t>(kOptions[i].enabled ? 1 : 0));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* split = lv_obj_create(page);
    lv_obj_remove_style_all(split);
    lv_obj_set_size(split, lv_pct(100), kSettingsSplitLineW);
    lv_obj_set_style_bg_color(split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* intensity_title = lv_label_create(page);
    lv_label_set_text(intensity_title, Lang::Strings::SETTINGS_HAPTIC_INTENSITY_TITLE);
    lv_obj_set_style_text_font(intensity_title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(intensity_title, lv_color_black(), 0);
    lv_obj_clear_flag(intensity_title, LV_OBJ_FLAG_CLICKABLE);

    // 7 档：一行小按钮 1..7（左弱右强）
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < kIntensityOptionCount; ++i) {
        char t[4];
        std::snprintf(t, sizeof(t), "%d", i + 1);
        lv_obj_t* b = SettingsCreateSelectableOption(row, t, OnIntensityClicked, static_cast<intptr_t>(i));
        lv_obj_set_size(b, 52, 56);
        lv_obj_t* l = lv_obj_get_child(b, 0);
        if (l) lv_obj_center(l);
        s_ui.intensity_btns[i] = b;
    }

    s_ui.intensity_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.intensity_lbl, "");
    lv_obj_set_style_text_font(s_ui.intensity_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.intensity_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.intensity_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshOptions();
}
