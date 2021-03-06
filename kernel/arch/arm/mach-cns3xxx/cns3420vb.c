

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/serial_8250.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/physmap.h>
#include <linux/mtd/partitions.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/time.h>
#include <mach/hardware.h>
#include <mach/cns3xxx.h>
#include <mach/irqs.h>
#include "core.h"

static struct mtd_partition cns3420_nor_partitions[] = {
	{
		.name		= "uboot",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "kernel",
		.size		= 0x004C0000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "filesystem",
		.size		= 0x7000000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "filesystem2",
		.size		= 0x0AE0000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "ubootenv",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	},
};

static struct physmap_flash_data cns3420_nor_pdata = {
	.width = 2,
	.parts = cns3420_nor_partitions,
	.nr_parts = ARRAY_SIZE(cns3420_nor_partitions),
};

static struct resource cns3420_nor_res = {
	.start = CNS3XXX_FLASH_BASE,
	.end = CNS3XXX_FLASH_BASE + SZ_128M - 1,
	.flags = IORESOURCE_MEM | IORESOURCE_MEM_32BIT,
};

static struct platform_device cns3420_nor_pdev = {
	.name = "physmap-flash",
	.id = 0,
	.resource = &cns3420_nor_res,
	.num_resources = 1,
	.dev = {
		.platform_data = &cns3420_nor_pdata,
	},
};

static void __init cns3420_early_serial_setup(void)
{
#ifdef CONFIG_SERIAL_8250_CONSOLE
	static struct uart_port cns3420_serial_port = {
		.membase        = (void __iomem *)CNS3XXX_UART0_BASE_VIRT,
		.mapbase        = CNS3XXX_UART0_BASE,
		.irq            = IRQ_CNS3XXX_UART0,
		.iotype         = UPIO_MEM,
		.flags          = UPF_BOOT_AUTOCONF | UPF_FIXED_TYPE,
		.regshift       = 2,
		.uartclk        = 24000000,
		.line           = 0,
		.type           = PORT_16550A,
		.fifosize       = 16,
	};

	early_serial_setup(&cns3420_serial_port);
#endif
}

static struct platform_device *cns3420_pdevs[] __initdata = {
	&cns3420_nor_pdev,
};

static void __init cns3420_init(void)
{
	platform_add_devices(cns3420_pdevs, ARRAY_SIZE(cns3420_pdevs));

	pm_power_off = cns3xxx_power_off;
}

static struct map_desc cns3420_io_desc[] __initdata = {
	{
		.virtual	= CNS3XXX_UART0_BASE_VIRT,
		.pfn		= __phys_to_pfn(CNS3XXX_UART0_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
};

static void __init cns3420_map_io(void)
{
	cns3xxx_map_io();
	iotable_init(cns3420_io_desc, ARRAY_SIZE(cns3420_io_desc));

	cns3420_early_serial_setup();
}

MACHINE_START(CNS3420VB, "Cavium Networks CNS3420 Validation Board")
	.phys_io	= CNS3XXX_UART0_BASE,
	.io_pg_offst	= (CNS3XXX_UART0_BASE_VIRT >> 18) & 0xfffc,
	.boot_params	= 0x00000100,
	.map_io		= cns3420_map_io,
	.init_irq	= cns3xxx_init_irq,
	.timer		= &cns3xxx_timer,
	.init_machine	= cns3420_init,
MACHINE_END
