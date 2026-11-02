// The HDMI modes. A pixel clock is 240 MHz divided by an integer and the lane rate follows it (platform_esp32_hdmi.cpp, the clocks). 1280x720 is WLED-MM's timing at 60 MHz, measured on the Olimex ESP32-P4-PC; the vendor's own runs at 80 MHz from a source the P4 cannot divide to it. The other modes are the vendor's macros, kept until each is replaced by a measured timing: 1024x768, 1280x800 and 1920x1080 ask for 56 and 70 MHz, which the DPI rounds to 60 and 80.
// C rather than C++ because the macros nest designators (`.flags.disable_lp`), which C++ does not accept.
#include "sdkconfig.h"

#if defined(CONFIG_MM_HDMI) && defined(CONFIG_IDF_TARGET_ESP32P4)
#include <stddef.h>
#include "esp_lcd_lt8912b.h"

// The order platform_esp32_hdmi.cpp's kModes lists. 1920x1080 at 60 Hz is left out, as Espressif's BSP leaves it out: WLED-MM measures the DPI's DMA from PSRAM underrunning from 80 MHz up, and it needs 120.
const esp_lcd_dpi_panel_config_t mm_hdmi_dpi[] = {
    LT8912B_800x600_PANEL_60HZ_DPI_CONFIG_WITH_FBS(1),
    LT8912B_1024x768_PANEL_60HZ_DPI_CONFIG_WITH_FBS(1),
    {.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_PLL_F240M, .dpi_clock_freq_mhz = 60, .in_color_format = LCD_COLOR_FMT_RGB888, .num_fbs = 1,
     .video_timing = {.h_size = 1280, .v_size = 720, .hsync_back_porch = 28, .hsync_pulse_width = 32, .hsync_front_porch = 10,
                      .vsync_back_porch = 13, .vsync_pulse_width = 5, .vsync_front_porch = 3},
     .flags.disable_lp = true},
    LT8912B_1280x800_PANEL_60HZ_DPI_CONFIG_WITH_FBS(1),
    LT8912B_1920x1080_PANEL_30HZ_DPI_CONFIG_WITH_FBS(1),
};
const esp_lcd_panel_lt8912b_video_timing_t mm_hdmi_timing[] = {
    ESP_LCD_LT8912B_VIDEO_TIMING_800x600_60Hz(),
    ESP_LCD_LT8912B_VIDEO_TIMING_1024x768_60Hz(),
    {.hfp = 10, .hs = 32, .hbp = 28, .hact = 1280, .htotal = 1350, .vfp = 3, .vs = 5, .vbp = 13, .vact = 720, .vtotal = 741,
     .h_polarity = 1, .v_polarity = 0, .vic = 4, .aspect_ratio = LT8912B_ASPECT_RATION_16_9, .pclk_mhz = 60},
    ESP_LCD_LT8912B_VIDEO_TIMING_1280x800_60Hz(),
    ESP_LCD_LT8912B_VIDEO_TIMING_1920x1080_30Hz(),
};
const size_t mm_hdmi_mode_count = sizeof(mm_hdmi_timing) / sizeof(mm_hdmi_timing[0]);
_Static_assert(sizeof(mm_hdmi_dpi) / sizeof(mm_hdmi_dpi[0]) == sizeof(mm_hdmi_timing) / sizeof(mm_hdmi_timing[0]), "one DPI config per timing");
#endif
