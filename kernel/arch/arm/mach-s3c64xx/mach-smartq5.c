

#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/gpio_keys.h>
#include <linux/i2c-gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/platform_device.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include <mach/map.h>
#include <mach/regs-fb.h>
#include <mach/regs-gpio.h>
#include <mach/s3c6410.h>

#include <plat/cpu.h>
#include <plat/devs.h>
#include <plat/fb.h>
#include <plat/gpio-cfg.h>

#include "mach-smartq.h"

static void __init smartq5_lcd_setup_gpio(void)
{
	gpio_request(S3C64XX_GPM(0), "LCD SCEN pin");
	gpio_request(S3C64XX_GPM(1), "LCD SCL pin");
	gpio_request(S3C64XX_GPM(2), "LCD SDA pin");
	gpio_request(S3C64XX_GPM(3), "LCD power");

	/* turn power off */
	gpio_direction_output(S3C64XX_GPM(0), 1);
	gpio_direction_input(S3C64XX_GPM(1));
	gpio_direction_input(S3C64XX_GPM(2));
	gpio_direction_output(S3C64XX_GPM(3), 0);
}

static struct i2c_gpio_platform_data smartq5_lcd_control = {
	.sda_pin		= S3C64XX_GPM(2),
	.scl_pin		= S3C64XX_GPM(1),
};

static struct platform_device smartq5_lcd_control_device = {
	.name			= "i2c-gpio",
	.id			= 1,
	.dev.platform_data	= &smartq5_lcd_control,
};

static struct gpio_led smartq5_leds[] __initdata = {
	{
		.name			= "smartq5:green",
		.active_low		= 1,
		.gpio			= S3C64XX_GPN(8),
	},
	{
		.name			= "smartq5:red",
		.active_low		= 1,
		.gpio			= S3C64XX_GPN(9),
	},
};

static struct gpio_led_platform_data smartq5_led_data = {
	.num_leds = ARRAY_SIZE(smartq5_leds),
	.leds = smartq5_leds,
};

static struct platform_device smartq5_leds_device = {
	.name			= "leds-gpio",
	.id			= -1,
	.dev.platform_data	= &smartq5_led_data,
};

/* Labels according to the SmartQ manual */
static struct gpio_keys_button smartq5_buttons[] = {
	{
		.gpio			= S3C64XX_GPL(14),
		.code			= KEY_POWER,
		.desc			= "Power",
		.active_low		= 1,
		.debounce_interval	= 5,
		.type                   = EV_KEY,
	},
	{
		.gpio			= S3C64XX_GPN(2),
		.code			= KEY_KPMINUS,
		.desc			= "Minus",
		.active_low		= 1,
		.debounce_interval	= 5,
		.type                   = EV_KEY,
	},
	{
		.gpio			= S3C64XX_GPN(12),
		.code			= KEY_KPPLUS,
		.desc			= "Plus",
		.active_low		= 1,
		.debounce_interval	= 5,
		.type                   = EV_KEY,
	},
	{
		.gpio			= S3C64XX_GPN(15),
		.code			= KEY_ENTER,
		.desc			= "Move",
		.active_low		= 1,
		.debounce_interval	= 5,
		.type                   = EV_KEY,
	},
};

static struct gpio_keys_platform_data smartq5_buttons_data  = {
	.buttons	= smartq5_buttons,
	.nbuttons	= ARRAY_SIZE(smartq5_buttons),
};

static struct platform_device smartq5_buttons_device  = {
	.name		= "gpio-keys",
	.id		= 0,
	.num_resources	= 0,
	.dev		= {
		.platform_data	= &smartq5_buttons_data,
	}
};

static struct s3c_fb_pd_win smartq5_fb_win0 = {
	.win_mode	= {
		.pixclock	= 1000000000000ULL /
				((40+1+216+800)*(10+1+35+480)*80),
		.left_margin	= 40,
		.right_margin	= 216,
		.upper_margin	= 10,
		.lower_margin	= 35,
		.hsync_len	= 1,
		.vsync_len	= 1,
		.xres		= 800,
		.yres		= 480,
	},
	.max_bpp	= 32,
	.default_bpp	= 16,
};

static struct s3c_fb_platdata smartq5_lcd_pdata __initdata = {
	.setup_gpio	= s3c64xx_fb_gpio_setup_24bpp,
	.win[0]		= &smartq5_fb_win0,
	.vidcon0	= VIDCON0_VIDOUT_RGB | VIDCON0_PNRMODE_RGB,
	.vidcon1	= VIDCON1_INV_HSYNC | VIDCON1_INV_VSYNC |
			  VIDCON1_INV_VDEN,
};

static struct platform_device *smartq5_devices[] __initdata = {
	&smartq5_leds_device,
	&smartq5_buttons_device,
	&smartq5_lcd_control_device,
};

static void __init smartq5_machine_init(void)
{
	s3c_fb_set_platdata(&smartq5_lcd_pdata);

	smartq_machine_init();
	smartq5_lcd_setup_gpio();

	platform_add_devices(smartq5_devices, ARRAY_SIZE(smartq5_devices));
}

MACHINE_START(SMARTQ5, "SmartQ 5")
	/* Maintainer: Maurus Cuelenaere <mcuelenaere AT gmail DOT com> */
	.phys_io	= S3C_PA_UART & 0xfff00000,
	.io_pg_offst	= (((u32)S3C_VA_UART) >> 18) & 0xfffc,
	.boot_params	= S3C64XX_PA_SDRAM + 0x100,
	.init_irq	= s3c6410_init_irq,
	.map_io		= smartq_map_io,
	.init_machine	= smartq5_machine_init,
	.timer		= &s3c24xx_timer,
MACHINE_END
