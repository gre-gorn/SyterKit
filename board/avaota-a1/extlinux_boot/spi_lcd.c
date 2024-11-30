/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types.h>

#include <config.h>
#include <log.h>
#include <timer.h>

#include <common.h>
#include <jmp.h>
#include <mmu.h>
#include <smalloc.h>
#include <sstdlib.h>
#include <string.h>

#include <cli.h>
#include <cli_shell.h>
#include <cli_termesc.h>

#include <reg-ncat.h>
#include <sys-clk.h>
#include <sys-dram.h>
#include <sys-i2c.h>
#include <sys-rtc.h>
#include <sys-sdcard.h>
#include <sys-sid.h>
#include <sys-spi.h>

#include <pmu/axp.h>

#include <fdt_wrapper.h>
#include <ff.h>
#include <sys-sdhci.h>
#include <uart.h>

#define SPI_LCD_COLOR_WHITE 0xFFFF
#define SPI_LCD_COLOR_BLACK 0x0000
#define SPI_LCD_COLOR_BLUE 0x001F
#define SPI_LCD_COLOR_BRED 0XF81F
#define SPI_LCD_COLOR_GRED 0XFFE0
#define SPI_LCD_COLOR_GBLUE 0X07FF
#define SPI_LCD_COLOR_RED 0xF800
#define SPI_LCD_COLOR_MAGENTA 0xF81F
#define SPI_LCD_COLOR_GREEN 0x07E0
#define SPI_LCD_COLOR_CYAN 0x7FFF
#define SPI_LCD_COLOR_YELLOW 0xFFE0

extern sunxi_dma_t sunxi_dma;

static sunxi_spi_t sunxi_spi0_lcd = {
        .base = SUNXI_R_SPI_BASE,
        .id = 0,
        .clk_rate = 75 * 1000 * 1000,
        .gpio = {
                .gpio_cs = {GPIO_PIN(GPIO_PORTL, 10), GPIO_PERIPH_MUX6},
                .gpio_sck = {GPIO_PIN(GPIO_PORTL, 11), GPIO_PERIPH_MUX6},
                .gpio_mosi = {GPIO_PIN(GPIO_PORTL, 12), GPIO_PERIPH_MUX6},
        },
        .spi_clk = {
                .spi_clock_cfg_base = SUNXI_R_PRCM_BASE + SUNXI_S_SPI_CLK_REG,
                .spi_clock_factor_n_offset = SPI_CLK_SEL_FACTOR_N_OFF,
                .spi_clock_source = SPI_CLK_SEL_PERIPH_300M,
        },
        .parent_clk_reg = {
                .rst_reg_base = SUNXI_R_PRCM_BASE + SUNXI_S_SPI_BGR_REG,
                .rst_reg_offset = SPI_DEFAULT_CLK_RST_OFFSET(0),
                .gate_reg_base = SUNXI_R_PRCM_BASE + SUNXI_S_SPI_BGR_REG,
                .gate_reg_offset = SPI_DEFAULT_CLK_GATE_OFFSET(0),
                .parent_clk = 300000000,
        },
        .dma_handle = &sunxi_dma,
};

static gpio_mux_t lcd_dc_pins = {
        .pin = GPIO_PIN(GPIO_PORTL, 13),
        .mux = GPIO_OUTPUT,
};

static gpio_mux_t lcd_res_pins = {
        .pin = GPIO_PIN(GPIO_PORTL, 9),
        .mux = GPIO_OUTPUT,
};

static gpio_mux_t lcd_blk_pins = {
        .pin = GPIO_PIN(GPIO_PORTL, 8),
        .mux = GPIO_OUTPUT,
};

static void LCD_Set_DC(uint8_t val) {
    sunxi_gpio_set_value(lcd_dc_pins.pin, val);
}

static void LCD_Set_RES(uint8_t val) {
    sunxi_gpio_set_value(lcd_res_pins.pin, val);
}

static void LCD_Write_Bus(uint8_t dat) {
    uint8_t tx[1]; /* Transmit buffer */
    tx[0] = dat;
    /* Perform SPI transfer */
    if (sunxi_spi_transfer(&sunxi_spi0_lcd, SPI_IO_SINGLE, tx, 1, 0, 0) < 0)
        printk_error("SPI: SPI Xfer error!\n");
}

static void LCD_Write_Data_Bus(void *dat, uint32_t len) {
    if (sunxi_spi_transfer(&sunxi_spi0_lcd, SPI_IO_SINGLE, dat, len, 0, 0) < 0)
        printk_error("SPI: SPI Xfer error!\n");
}

static void LCD_WR_DATA(uint16_t dat) {
    LCD_Write_Bus(dat >> 8);
    LCD_Write_Bus(dat);
}

static void LCD_WR_DATA8(uint8_t dat) {
    LCD_Write_Bus(dat);
}

static void LCD_WR_REG(uint8_t dat) {
    LCD_Set_DC(0);
    LCD_Write_Bus(dat);
    LCD_Set_DC(1);
}

static void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    LCD_WR_REG(0x2a);
    LCD_WR_DATA(x1 + 40);
    LCD_WR_DATA(x2 + 40);
    LCD_WR_REG(0x2b);
    LCD_WR_DATA(y1 + 52);
    LCD_WR_DATA(y2 + 52);
    LCD_WR_REG(0x2c);
}

static void LCD_Open_BLK() {
    sunxi_gpio_set_value(lcd_blk_pins.pin, 1);
}

#define LCD_W 240
#define LCD_H 135

static void LCD_Fill_All(uint16_t color) {
    uint16_t i, j;
    LCD_Address_Set(0, 0, LCD_W - 1, LCD_H - 1);
    uint16_t *video_mem = smalloc(LCD_W * LCD_H);

    for (uint32_t i = 0; i < LCD_W * LCD_H; i++) {
        video_mem[i] = color;
    }

    LCD_Write_Data_Bus(video_mem, LCD_W * LCD_H * (sizeof(uint16_t) / sizeof(uint8_t)));

    sfree(video_mem);
}

static void LCD_Init(void) {
    sunxi_gpio_init(lcd_dc_pins.pin, lcd_dc_pins.mux);
    sunxi_gpio_init(lcd_res_pins.pin, lcd_res_pins.mux);
    sunxi_gpio_init(lcd_blk_pins.pin, lcd_blk_pins.mux);

    if (sunxi_spi_init(&sunxi_spi0_lcd) != 0) {
        printk_error("SPI: init failed\n");
    }

    LCD_Set_RES(0);
    mdelay(100);
    LCD_Set_RES(1);
    mdelay(100);

    LCD_WR_REG(0x11);
    mdelay(120);
    LCD_WR_REG(0x36);
    LCD_WR_DATA8(0xA0);

    LCD_WR_REG(0x3A);
    LCD_WR_DATA8(0x05);

    LCD_WR_REG(0xB2);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x00);
    LCD_WR_DATA8(0x33);
    LCD_WR_DATA8(0x33);

    LCD_WR_REG(0xB7);
    LCD_WR_DATA8(0x35);

    LCD_WR_REG(0xBB);
    LCD_WR_DATA8(0x19);

    LCD_WR_REG(0xC0);
    LCD_WR_DATA8(0x2C);

    LCD_WR_REG(0xC2);
    LCD_WR_DATA8(0x01);

    LCD_WR_REG(0xC3);
    LCD_WR_DATA8(0x12);

    LCD_WR_REG(0xC4);
    LCD_WR_DATA8(0x20);

    LCD_WR_REG(0xC6);
    LCD_WR_DATA8(0x0F);

    LCD_WR_REG(0xD0);
    LCD_WR_DATA8(0xA4);
    LCD_WR_DATA8(0xA1);

    LCD_WR_REG(0xE0);
    LCD_WR_DATA8(0xD0);
    LCD_WR_DATA8(0x04);
    LCD_WR_DATA8(0x0D);
    LCD_WR_DATA8(0x11);
    LCD_WR_DATA8(0x13);
    LCD_WR_DATA8(0x2B);
    LCD_WR_DATA8(0x3F);
    LCD_WR_DATA8(0x54);
    LCD_WR_DATA8(0x4C);
    LCD_WR_DATA8(0x18);
    LCD_WR_DATA8(0x0D);
    LCD_WR_DATA8(0x0B);
    LCD_WR_DATA8(0x1F);
    LCD_WR_DATA8(0x23);

    LCD_WR_REG(0xE1);
    LCD_WR_DATA8(0xD0);
    LCD_WR_DATA8(0x04);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x11);
    LCD_WR_DATA8(0x13);
    LCD_WR_DATA8(0x2C);
    LCD_WR_DATA8(0x3F);
    LCD_WR_DATA8(0x44);
    LCD_WR_DATA8(0x51);
    LCD_WR_DATA8(0x2F);
    LCD_WR_DATA8(0x1F);
    LCD_WR_DATA8(0x1F);
    LCD_WR_DATA8(0x20);
    LCD_WR_DATA8(0x23);

    LCD_WR_REG(0x21);

    LCD_WR_REG(0x29);

    LCD_Fill_All(0x0000);
}

#define SPLASH_START_X 52
#define SPLASH_START_Y 43
#define SPLASH_W 135
#define SPLASH_H 48

static void LCD_Show_Splash(uint8_t *splash_dest) {
    uint16_t i, j, k = 0;
    LCD_Address_Set(SPLASH_START_X, SPLASH_START_Y, SPLASH_START_X + SPLASH_W - 1, SPLASH_START_Y + SPLASH_H - 1);

    uint16_t *video_mem = smalloc(SPLASH_W * SPLASH_H);

    for (i = 0; i < SPLASH_W; i++) {
        for (j = 0; j < SPLASH_H; j++) {
            video_mem[k] = (splash_dest[k * 2] << 8) | splash_dest[k * 2 + 1];
            k++;
        }
    }

    LCD_Write_Data_Bus(video_mem, SPLASH_W * SPLASH_H * (sizeof(uint16_t) / sizeof(uint8_t)));

    sfree(video_mem);
}

static const unsigned char ascii_1206[][12] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*" ",0*/
        {0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}, /*"!",1*/
        {0x14, 0x14, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*""",2*/
        {0x00, 0x00, 0x0A, 0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x0A, 0x00, 0x00}, /*"#",3*/
        {0x00, 0x04, 0x0E, 0x15, 0x05, 0x06, 0x0C, 0x14, 0x15, 0x0E, 0x04, 0x00}, /*"$",4*/
        {0x00, 0x00, 0x12, 0x15, 0x0D, 0x15, 0x2E, 0x2C, 0x2A, 0x12, 0x00, 0x00}, /*"%",5*/
        {0x00, 0x00, 0x04, 0x0A, 0x0A, 0x36, 0x15, 0x15, 0x29, 0x16, 0x00, 0x00}, /*"&",6*/
        {0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*"'",7*/
        {0x10, 0x08, 0x08, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x10, 0x00}, /*"(",8*/
        {0x02, 0x04, 0x04, 0x08, 0x08, 0x08, 0x08, 0x08, 0x04, 0x04, 0x02, 0x00}, /*")",9*/
        {0x00, 0x00, 0x00, 0x04, 0x15, 0x0E, 0x0E, 0x15, 0x04, 0x00, 0x00, 0x00}, /*"*",10*/
        {0x00, 0x00, 0x00, 0x08, 0x08, 0x3E, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00}, /*"+",11*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x01, 0x00}, /*",",12*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*"-",13*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00}, /*".",14*/
        {0x00, 0x20, 0x10, 0x10, 0x08, 0x08, 0x04, 0x04, 0x02, 0x02, 0x01, 0x00}, /*"/",15*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00}, /*"0",16*/
        {0x00, 0x00, 0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00, 0x00}, /*"1",17*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x08, 0x04, 0x02, 0x01, 0x1F, 0x00, 0x00}, /*"2",18*/
        {0x00, 0x00, 0x0E, 0x11, 0x10, 0x0C, 0x10, 0x10, 0x11, 0x0E, 0x00, 0x00}, /*"3",19*/
        {0x00, 0x00, 0x08, 0x0C, 0x0C, 0x0A, 0x09, 0x1F, 0x08, 0x1C, 0x00, 0x00}, /*"4",20*/
        {0x00, 0x00, 0x1F, 0x01, 0x01, 0x0F, 0x11, 0x10, 0x11, 0x0E, 0x00, 0x00}, /*"5",21*/
        {0x00, 0x00, 0x0C, 0x12, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0E, 0x00, 0x00}, /*"6",22*/
        {0x00, 0x00, 0x1E, 0x10, 0x08, 0x08, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00}, /*"7",23*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00}, /*"8",24*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x19, 0x16, 0x10, 0x09, 0x06, 0x00, 0x00}, /*"9",25*/
        {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00}, /*":",26*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00}, /*";",27*/
        {0x00, 0x00, 0x10, 0x08, 0x04, 0x02, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}, /*"<",28*/
        {0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*"=",29*/
        {0x00, 0x00, 0x02, 0x04, 0x08, 0x10, 0x10, 0x08, 0x04, 0x02, 0x00, 0x00}, /*">",30*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x08, 0x04, 0x04, 0x00, 0x04, 0x00, 0x00}, /*"?",31*/
        {0x00, 0x00, 0x1C, 0x22, 0x29, 0x2D, 0x2D, 0x1D, 0x22, 0x1C, 0x00, 0x00}, /*"@",32*/
        {0x00, 0x00, 0x04, 0x04, 0x0C, 0x0A, 0x0A, 0x1E, 0x12, 0x33, 0x00, 0x00}, /*"A",33*/
        {0x00, 0x00, 0x0F, 0x12, 0x12, 0x0E, 0x12, 0x12, 0x12, 0x0F, 0x00, 0x00}, /*"B",34*/
        {0x00, 0x00, 0x1E, 0x11, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E, 0x00, 0x00}, /*"C",35*/
        {0x00, 0x00, 0x0F, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x0F, 0x00, 0x00}, /*"D",36*/
        {0x00, 0x00, 0x1F, 0x12, 0x0A, 0x0E, 0x0A, 0x02, 0x12, 0x1F, 0x00, 0x00}, /*"E",37*/
        {0x00, 0x00, 0x1F, 0x12, 0x0A, 0x0E, 0x0A, 0x02, 0x02, 0x07, 0x00, 0x00}, /*"F",38*/
        {0x00, 0x00, 0x1C, 0x12, 0x01, 0x01, 0x39, 0x11, 0x12, 0x0C, 0x00, 0x00}, /*"G",39*/
        {0x00, 0x00, 0x33, 0x12, 0x12, 0x1E, 0x12, 0x12, 0x12, 0x33, 0x00, 0x00}, /*"H",40*/
        {0x00, 0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F, 0x00, 0x00}, /*"I",41*/
        {0x00, 0x00, 0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x07}, /*"J",42*/
        {0x00, 0x00, 0x37, 0x12, 0x0A, 0x06, 0x0A, 0x12, 0x12, 0x37, 0x00, 0x00}, /*"K",43*/
        {0x00, 0x00, 0x07, 0x02, 0x02, 0x02, 0x02, 0x02, 0x22, 0x3F, 0x00, 0x00}, /*"L",44*/
        {0x00, 0x00, 0x3B, 0x1B, 0x1B, 0x1B, 0x15, 0x15, 0x15, 0x35, 0x00, 0x00}, /*"M",45*/
        {0x00, 0x00, 0x3B, 0x12, 0x16, 0x16, 0x1A, 0x1A, 0x12, 0x17, 0x00, 0x00}, /*"N",46*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00}, /*"O",47*/
        {0x00, 0x00, 0x0F, 0x12, 0x12, 0x0E, 0x02, 0x02, 0x02, 0x07, 0x00, 0x00}, /*"P",48*/
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x17, 0x19, 0x0E, 0x18, 0x00}, /*"Q",49*/
        {0x00, 0x00, 0x0F, 0x12, 0x12, 0x0E, 0x0A, 0x12, 0x12, 0x37, 0x00, 0x00}, /*"R",50*/
        {0x00, 0x00, 0x1E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x11, 0x0F, 0x00, 0x00}, /*"S",51*/
        {0x00, 0x00, 0x1F, 0x15, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00, 0x00}, /*"T",52*/
        {0x00, 0x00, 0x33, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x0C, 0x00, 0x00}, /*"U",53*/
        {0x00, 0x00, 0x33, 0x12, 0x12, 0x0A, 0x0A, 0x0C, 0x04, 0x04, 0x00, 0x00}, /*"V",54*/
        {0x00, 0x00, 0x15, 0x15, 0x15, 0x15, 0x0E, 0x0A, 0x0A, 0x0A, 0x00, 0x00}, /*"W",55*/
        {0x00, 0x00, 0x1B, 0x0A, 0x0A, 0x04, 0x04, 0x0A, 0x0A, 0x1B, 0x00, 0x00}, /*"X",56*/
        {0x00, 0x00, 0x1B, 0x0A, 0x0A, 0x0A, 0x04, 0x04, 0x04, 0x0E, 0x00, 0x00}, /*"Y",57*/
        {0x00, 0x00, 0x1F, 0x09, 0x08, 0x04, 0x04, 0x02, 0x12, 0x1F, 0x00, 0x00}, /*"Z",58*/
        {0x1C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1C, 0x00}, /*"[",59*/
        {0x00, 0x02, 0x02, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x10, 0x10, 0x00}, /*"\",60*/
        {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x00}, /*"]",61*/
        {0x04, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*"^",62*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F}, /*"_",63*/
        {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*"`",64*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x12, 0x1C, 0x12, 0x3C, 0x00, 0x00}, /*"a",65*/
        {0x00, 0x03, 0x02, 0x02, 0x02, 0x0E, 0x12, 0x12, 0x12, 0x0E, 0x00, 0x00}, /*"b",66*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x12, 0x02, 0x12, 0x0C, 0x00, 0x00}, /*"c",67*/
        {0x00, 0x18, 0x10, 0x10, 0x10, 0x1C, 0x12, 0x12, 0x12, 0x3C, 0x00, 0x00}, /*"d",68*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x12, 0x1E, 0x02, 0x1C, 0x00, 0x00}, /*"e",69*/
        {0x00, 0x18, 0x24, 0x04, 0x04, 0x1E, 0x04, 0x04, 0x04, 0x1E, 0x00, 0x00}, /*"f",70*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x12, 0x0C, 0x02, 0x1C, 0x22, 0x1C}, /*"g",71*/
        {0x00, 0x03, 0x02, 0x02, 0x02, 0x0E, 0x12, 0x12, 0x12, 0x37, 0x00, 0x00}, /*"h",72*/
        {0x00, 0x04, 0x04, 0x00, 0x00, 0x06, 0x04, 0x04, 0x04, 0x0E, 0x00, 0x00}, /*"i",73*/
        {0x00, 0x08, 0x08, 0x00, 0x00, 0x0C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x07}, /*"j",74*/
        {0x00, 0x03, 0x02, 0x02, 0x02, 0x1A, 0x0A, 0x06, 0x0A, 0x13, 0x00, 0x00}, /*"k",75*/
        {0x00, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F, 0x00, 0x00}, /*"l",76*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x15, 0x15, 0x15, 0x15, 0x00, 0x00}, /*"m",77*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x12, 0x12, 0x12, 0x37, 0x00, 0x00}, /*"n",78*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x12, 0x12, 0x12, 0x0C, 0x00, 0x00}, /*"o",79*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x12, 0x12, 0x12, 0x0E, 0x02, 0x07}, /*"p",80*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x12, 0x12, 0x12, 0x1C, 0x10, 0x38}, /*"q",81*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x06, 0x02, 0x02, 0x07, 0x00, 0x00}, /*"r",82*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x02, 0x0C, 0x10, 0x1E, 0x00, 0x00}, /*"s",83*/
        {0x00, 0x00, 0x00, 0x04, 0x04, 0x1E, 0x04, 0x04, 0x04, 0x1C, 0x00, 0x00}, /*"t",84*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x12, 0x12, 0x12, 0x3C, 0x00, 0x00}, /*"u",85*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x0A, 0x0A, 0x04, 0x04, 0x00, 0x00}, /*"v",86*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x15, 0x0E, 0x0A, 0x0A, 0x00, 0x00}, /*"w",87*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x0A, 0x04, 0x0A, 0x1B, 0x00, 0x00}, /*"x",88*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x12, 0x12, 0x0C, 0x08, 0x04, 0x03}, /*"y",89*/
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x08, 0x04, 0x04, 0x1E, 0x00, 0x00}, /*"z",90*/
        {0x18, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08, 0x08, 0x08, 0x08, 0x18, 0x00}, /*"{",91*/
        {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08}, /*"|",92*/
        {0x06, 0x04, 0x04, 0x04, 0x04, 0x08, 0x04, 0x04, 0x04, 0x04, 0x06, 0x00}, /*"}",93*/
        {0x16, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*"~",94*/
};

static void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey) {
    uint8_t temp, sizex, t, m = 0;
    uint16_t i, TypefaceNum;// Number of bytes for one character
    uint16_t x0 = x;
    sizex = sizey / 2;
    TypefaceNum = (sizex / 8 + ((sizex % 8) ? 1 : 0)) * sizey;
    num = num - ' ';                                    // Get the offset value
    LCD_Address_Set(x, y, x + sizex - 1, y + sizey - 1);// Set the cursor position
    for (i = 0; i < TypefaceNum; i++) {
        temp = ascii_1206[num][i];// Call 6x12 font
        for (t = 0; t < 8; t++) {
            if (temp & (0x01 << t))
                LCD_WR_DATA(fc);
            else
                LCD_WR_DATA(bc);
            m++;
            if (m % sizex == 0) {
                m = 0;
                break;
            }
        }
    }
}


static void LCD_ShowString(uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey) {
    printk_debug("LCD: Show String: \"%s\"\n", p);
    while (*p != '\0') {
        LCD_ShowChar(x, y, *p, fc, bc, sizey);
        x += sizey / 2;
        p++;
    }
}