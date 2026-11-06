// 信息卡上盖的本地层：天气（设备自己拉的）、电量
#include "standby_card.h"

#include <cstdio>
#include <ctime>

#include "board.h"
#include "fontpack_lvgl.h"
#include "standby_classic_weather_store.h"

extern "C" {
LV_FONT_DECLARE(font_misans_light_50_2)
}

namespace {
lv_obj_t* LocalBox(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}
lv_obj_t* LocalLabel(lv_obj_t* parent, const lv_font_t* f, int x, int y, const char* t) {
    lv_obj_t* l = lv_label_create(parent);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    return l;
}
}  // namespace

void StandbyCard_BuildLocal(lv_obj_t* root) {
    if (root == nullptr) return;
    const lv_font_t* f30 = fontpack_lv_font_get(30, 4);
    const lv_font_t* f25 = fontpack_lv_font_get(25, 2);
    const lv_font_t* f18 = fontpack_lv_font_get(18, 2);
    if (f25 == nullptr) f25 = f30;
    if (f18 == nullptr) f18 = f25;
    // 天气块（卡片逻辑 y 168..286；服务器那块被盖掉）
    const WeatherSnap wx = StandbyClassic_WeatherRamCopy();
    // 只在本地天气是完整的、且不比卡上那份旧时才盖（否则保留卡上服务器画的那块）
    const long card_at = StandbyCard_FetchedAt();
    const bool newer = card_at == 0 || wx.fetched_at + 600 >= card_at;
    if (wx.valid && wx.has_temp && wx.has_range && newer && f30) {
        lv_obj_t* box = LocalBox(root, 0, 168, 480, 118);
        char t[64];
        std::snprintf(t, sizeof(t), "%d", wx.temp);
        lv_obj_t* tl = LocalLabel(box, &font_misans_light_50_2, 24, 14, t);
        lv_obj_update_layout(tl);
        const int tx = 24 + lv_obj_get_width(tl);
        LocalLabel(box, f30, tx + 2, 16, "°");
        const int x = tx + 36 > 150 ? tx + 36 : 150;
        std::snprintf(t, sizeof(t), "%s%s%s", wx.city, wx.city[0] ? " · " : "", wx.text);
        lv_obj_t* cl = LocalLabel(box, f30, x, 8, t);
        lv_obj_set_width(cl, 480 - 24 - x);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_DOT);
        if (wx.has_range) {
            std::snprintf(t, sizeof(t), "今天 %d~%d°C", wx.tmin, wx.tmax);
            LocalLabel(box, f25, x, 50, t);
        }
        if (wx.tomorrow[0]) {
            std::snprintf(t, sizeof(t), "明天 %s", wx.tomorrow);
            LocalLabel(box, f25, x, 82, t);
        }
        // 数据是几点的：超过 3 小时或来自开机前的旧记录就标出来
        const long now = static_cast<long>(time(nullptr));
        if (wx.fetched_at == 0) {
            LocalLabel(box, f18, 24, 88, "旧数据");
        } else if (now - wx.fetched_at > 3 * 3600) {
            struct tm tmv {};
            const time_t ft = wx.fetched_at;
            localtime_r(&ft, &tmv);
            std::snprintf(t, sizeof(t), "%d日%02d:%02d", tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
            LocalLabel(box, f18, 24, 88, t);
        }
    }
    // 电量（左下角）
    int level = 0;
    bool charging = false, discharging = false;
    if (f25 && Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        lv_obj_t* box = LocalBox(root, 16, 754, 236, 40);
        char t[32];
        std::snprintf(t, sizeof(t), "电量 %d%%%s", level, charging ? " 充电中" : "");
        LocalLabel(box, f25, 8, 6, t);
    }
}

