


#define NFC_PG_SIZE_256		0
#define NFC_PG_SIZE_512		1
#define NFC_PG_SIZE_OFFSET	9

#define NFC_NWIDTH_8		0
#define NFC_NWIDTH_16		1
#define NFC_NWIDTH_OFFSET	8

#define NFC_RDDLY_OFFSET	4
#define NFC_WRDLY_OFFSET	0

#define NFC_STAT_NBUSY		1

struct bf5xx_nand_platform {
	/* NAND chip information */
	unsigned short		page_size;
	unsigned short		data_width;

	/* RD/WR strobe delay timing information, all times in SCLK cycles */
	unsigned short		rd_dly;
	unsigned short		wr_dly;

	/* NAND MTD partition information */
	int                     nr_partitions;
	struct mtd_partition    *partitions;
};
