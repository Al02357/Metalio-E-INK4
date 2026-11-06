/**
 * @file epd_i1_fast_blit.h
 * @brief LVGL I1→I1 图片不透明拷贝的快速路径（接 LV_DRAW_SW_I1_BLEND_NORMAL_TO_I1 钩子）
 *
 * LVGL 默认逐像素 get_bit/set_bit，整屏 480×800 的阅读页图要约 190ms；按字节拷贝约 5ms。
 * 只在 lv_draw_sw_blend_to_i1.c 里生效（经 epd_i1_glyph_thin.h 引入，且该文件已包含 blend_private.h）。
 */
#ifndef EPD_I1_FAST_BLIT_H
#define EPD_I1_FAST_BLIT_H

#ifdef LV_DRAW_SW_BLEND_PRIVATE_H
#include <string.h>

/* 源从第 0 位开始、目标从 relative_area.x1 % 8 位开始，逐行拷 dest_w 位（高位在前，与 LVGL set_bit 一致） */
static inline lv_result_t epd_i1_blit_normal(lv_draw_sw_blend_image_dsc_t * dsc)
{
    const int32_t w = dsc->dest_w;
    const int32_t h = dsc->dest_h;
    const int32_t ofs = dsc->relative_area.x1 % 8;
    uint8_t * d = (uint8_t *)dsc->dest_buf;
    const uint8_t * s = (const uint8_t *)dsc->src_buf;
    const int32_t full = w >> 3;
    const int32_t rem = w & 7;
    for(int32_t y = 0; y < h; y++) {
        if(ofs == 0) {
            memcpy(d, s, (size_t)full);
            if(rem) {
                const uint8_t m = (uint8_t)(0xFF << (8 - rem));
                d[full] = (uint8_t)((d[full] & ~m) | (s[full] & m));
            }
        }
        else {
            const int32_t nbytes = (w + 7) >> 3;
            for(int32_t i = 0; i < nbytes; i++) {
                const int32_t bits = (w - i * 8) >= 8 ? 8 : (w - i * 8);
                const uint8_t m = (uint8_t)(0xFF << (8 - bits));
                const uint8_t v = (uint8_t)(s[i] & m);
                uint8_t * p = d + i;  /* 目标第 ofs + 8i 位所在字节 */
                p[0] = (uint8_t)((p[0] & ~(m >> ofs)) | (v >> ofs));
                if(ofs + bits > 8) {
                    const uint8_t m2 = (uint8_t)(m << (8 - ofs));
                    p[1] = (uint8_t)((p[1] & ~m2) | (uint8_t)(v << (8 - ofs)));
                }
            }
        }
        d += dsc->dest_stride;
        s += dsc->src_stride;
    }
    return LV_RESULT_OK;
}

#define LV_DRAW_SW_I1_BLEND_NORMAL_TO_I1(dsc) epd_i1_blit_normal(dsc)
#endif /* LV_DRAW_SW_BLEND_PRIVATE_H */

#endif /* EPD_I1_FAST_BLIT_H */
