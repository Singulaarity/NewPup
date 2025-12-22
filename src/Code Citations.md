# Code Citations

## License: MIT
https://github.com/lvgl/lvgl/tree/a7d03e09ef1c8ab0d435561583cfb90d00c7e9e1/examples/LVGL_Arduino.ino

```
disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow
```


## License: MIT
https://github.com/motomak/Test-LVGL833-TFT_eSPI/tree/9264ef524a4fdc185b7511f08056cba5f6126d99/touch.ino

```
area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1
```


## License: unknown
https://github.com/Protocentral/protocentral_healthypi_5_firmware/tree/6df3169463738b1f013b908e4ed76b080aca006d/rp2040/healthypi_rp2040_display_lvgl/healthypi_display.cpp

```
// Display flushing callback
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1)
```


## License: GPL_3_0
https://github.com/Jason6111/sd2/tree/b99ace6bd46a9eab3e46110bb7b4604ff1b45b29/src/main.ino

```
tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors(&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

/
```

