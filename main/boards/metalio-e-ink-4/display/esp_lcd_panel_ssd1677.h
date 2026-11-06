/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 刷新完成 ISR 回调类型 */
typedef bool (*esp_lcd_epaper_panel_cb_t)(const esp_lcd_panel_handle_t handle, const void *edata, void *user_data);

typedef struct {
    esp_lcd_epaper_panel_cb_t on_epaper_refresh_done; // 刷新结束回调
} epaper_panel_callbacks_t;

typedef struct {
    int busy_gpio_num;         // BUSY 脚
    bool non_copy_mode;        // true：直接用调用方缓冲，不做内部拷贝
    bool use_fast_full_update; // true：允许快速全刷模式
} esp_lcd_ssd1677_config_t;

typedef enum {
    SSD1677_EPAPER_BITMAP_CURRENT = 0, // 写 0x24 当前帧
    SSD1677_EPAPER_BITMAP_PREVIOUS,    // 写 0x26 上一帧
} esp_lcd_ssd1677_bitmap_color_t;

typedef enum {
    SSD1677_EPAPER_REFRESH_FULL = 0,  // 全刷（旧 0xC7 / 优化 0xC4）
    SSD1677_EPAPER_REFRESH_FULL_FAST, // 快速全刷
    SSD1677_EPAPER_REFRESH_PARTIAL,   // 局刷（旧 0xCF / 优化 0xCC）
    SSD1677_EPAPER_REFRESH_STANDBY,   // 进/出待机与关机（同全刷档）
    SSD1677_EPAPER_REFRESH_GRAY,      // 0x22=0xCC + 外置灰 LUT（同步等 BUSY）
    SSD1677_EPAPER_REFRESH_CUSTOM,    // 0x22=0xCC + 调用方 epaper_panel_load_custom_lut 下发的 LUT（同步等 BUSY）
} esp_lcd_ssd1677_refresh_mode_t;

/**
 * @brief 创建 SSD1677 墨水屏 panel
 * @param io panel IO 句柄
 * @param panel_dev_config 设备配置（vendor_config 指向 esp_lcd_ssd1677_config_t）
 * @param ret_panel 输出 panel 句柄
 * @return ESP_OK 成功
 */
esp_err_t esp_lcd_new_panel_ssd1677(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief 启动刷新；FULL 类轮询 BUSY，PARTIAL 挂 BUSY 下降沿 ISR
 * @note MCU LUT 仅在全↔局切换时重下
 */
esp_err_t epaper_panel_refresh_screen(esp_lcd_panel_t *panel);

/**
 * @brief 设置下次刷新模式
 */
esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_t *panel, esp_lcd_ssd1677_refresh_mode_t mode);

/**
 * @brief 设置 draw_bitmap 目标平面（0x24 / 0x26）
 */
esp_err_t epaper_panel_set_bitmap_color(esp_lcd_panel_t *panel, esp_lcd_ssd1677_bitmap_color_t color);

/**
 * @brief 注册刷新完成回调
 */
esp_err_t epaper_panel_register_event_callbacks(esp_lcd_panel_t *panel, epaper_panel_callbacks_t *cbs, void *user_ctx);

/**
 * @brief 每次局刷前：写边框 0x80（无 HW RST / soft reset）
 */
esp_err_t epaper_panel_prepare_for_partial(esp_lcd_panel_t *panel);

/** 下发调用方自己的 112 字节 LUT（格式同 epd_lut_4gray.h）；配合 SSD1677_EPAPER_REFRESH_CUSTOM 使用 */
esp_err_t epaper_panel_load_custom_lut(esp_lcd_panel_t *panel, const uint8_t *lut112);
/** 改局刷 LUT 的帧数/帧率（<=0 表示不改），下次局刷生效 */
void epaper_panel_set_partial_timing(esp_lcd_panel_t *panel, int tp0, int tp1, int fr);
/** 局刷后面板模拟电路是否还开着 */
bool epaper_panel_analog_on(esp_lcd_panel_t *panel);
/** 最近一次刷新发的 0x22 参数 */
uint8_t epaper_panel_last_ctrl2(esp_lcd_panel_t *panel);
/** 开着就掉电（0x22=0x83 + 0x20，等 BUSY）；调用方须保证没有刷新在进行 */
/** 刷完、模拟电源还开着时把 VCOM 拉回 0（不给面板留直流偏压）；下次刷新自动重载 LUT */
esp_err_t epaper_panel_park_vcom(esp_lcd_panel_t *panel);
esp_err_t epaper_panel_power_off_if_on(esp_lcd_panel_t *panel);

/** 最近一次刷新的 BUSY 时长（ms），耗时打点用 */
uint32_t epaper_panel_last_busy_ms(esp_lcd_panel_t *panel);

/**
 * @brief 进入 Deep Sleep（0x10）；唤醒需硬件 RST
 */
esp_err_t epaper_panel_deep_sleep(esp_lcd_panel_t *panel);

#ifdef __cplusplus
}
#endif
