#include "display_service.h"
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include "config.h"
#include "avatar_display.h"
LcdDisplay* DisplayService::InitializeDisplay(const Config& c) {
    spi_bus_config_t b{}; b.mosi_io_num=GPIO_NUM_37; b.miso_io_num=GPIO_NUM_NC; b.sclk_io_num=GPIO_NUM_36; b.quadwp_io_num=GPIO_NUM_NC; b.quadhd_io_num=GPIO_NUM_NC; b.max_transfer_sz=DISPLAY_WIDTH*DISPLAY_HEIGHT*sizeof(uint16_t); ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&b,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io=nullptr; esp_lcd_panel_io_spi_config_t ic{}; ic.cs_gpio_num=GPIO_NUM_3; ic.dc_gpio_num=GPIO_NUM_35; ic.spi_mode=2; ic.pclk_hz=40000000; ic.trans_queue_depth=10; ic.lcd_cmd_bits=8; ic.lcd_param_bits=8; ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST,&ic,&io));
    esp_lcd_panel_handle_t p=nullptr; esp_lcd_panel_dev_config_t pc{}; pc.reset_gpio_num=GPIO_NUM_NC; pc.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR; pc.bits_per_pixel=16; ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io,&pc,&p)); esp_lcd_panel_reset(p); if(c.reset_panel)c.reset_panel(); esp_lcd_panel_init(p); esp_lcd_panel_invert_color(p,true); esp_lcd_panel_swap_xy(p,DISPLAY_SWAP_XY); esp_lcd_panel_mirror(p,DISPLAY_MIRROR_X,DISPLAY_MIRROR_Y); return new M5StackAvatarDisplay(io,p,DISPLAY_WIDTH,DISPLAY_HEIGHT,DISPLAY_OFFSET_X,DISPLAY_OFFSET_Y,DISPLAY_MIRROR_X,DISPLAY_MIRROR_Y,DISPLAY_SWAP_XY);
}
