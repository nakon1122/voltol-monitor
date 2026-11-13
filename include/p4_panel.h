#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int p4_panel_init(void);                                            // 0 = ok
int p4_panel_draw(int x0, int y0, int x1, int y1, const void *rgb565);  // portrait 480x800, end exclusive
int p4_touch_read(int *x, int *y);                                   // portrait coordinates, 1 = touched
#ifdef __cplusplus
}
#endif
