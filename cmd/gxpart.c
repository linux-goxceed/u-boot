// SPDX-License-Identifier: GPL-2.0+
/*
 * gxpart - parse the NationalChip GxLoader partition table from SPI flash.
 *
 * Table format (reverse-engineered from the on-flash TABLE partition and the
 * vendor bootloader's partition.c):
 *
 *   offset 0x00: magic     AA BC DE FA
 *   offset 0x04: count      u8  (number of partition records)
 *   offset 0x05: record[0]  24 bytes:
 *                  +0x00 name  9 bytes, NUL-padded
 *                  +0x09 size  3 bytes, BIG-endian, in bytes
 *                  +0x0c ...   vendor fields (version/flags/crc), unused here
 *
 * Partitions are laid out consecutively from flash offset 0, so the start of
 * each partition is the running sum of the preceding partition sizes.
 *
 * The command prints the table and installs a matching "mtdparts" environment
 * variable so the standard "mtd" / "sf" commands can address partitions by
 * name.
 */

#include <command.h>
#include <env.h>
#include <spi_flash.h>
#include <vsprintf.h>
#include <linux/string.h>

#define GXPART_TABLE_OFFSET	0x10000
#define GXPART_TABLE_LEN	512
#define GXPART_REC_SIZE		24
#define GXPART_NAME_LEN		9
#define GXPART_MAX		16

static const u8 gxpart_magic[4] = { 0xaa, 0xbc, 0xde, 0xfa };

static int do_gxpart(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	unsigned int bus = CONFIG_SF_DEFAULT_BUS;
	unsigned int cs = CONFIG_SF_DEFAULT_CS;
	u32 table_off = GXPART_TABLE_OFFSET;
	struct spi_flash *flash;
	u8 buf[GXPART_TABLE_LEN];
	char parts[512];
	unsigned int count, i, pos;
	u32 offset = 0;
	int ret, len;

	if (argc > 1)
		table_off = hextoul(argv[1], NULL);

	flash = spi_flash_probe(bus, cs, CONFIG_SF_DEFAULT_SPEED,
				CONFIG_SF_DEFAULT_MODE);
	if (!flash) {
		printf("gxpart: SPI flash probe failed\n");
		return CMD_RET_FAILURE;
	}

	ret = spi_flash_read(flash, table_off, sizeof(buf), buf);
	if (ret) {
		printf("gxpart: read @0x%x failed (%d)\n", table_off, ret);
		spi_flash_free(flash);
		return CMD_RET_FAILURE;
	}
	spi_flash_free(flash);

	if (memcmp(buf, gxpart_magic, sizeof(gxpart_magic))) {
		printf("gxpart: bad magic %02x %02x %02x %02x @0x%x\n",
		       buf[0], buf[1], buf[2], buf[3], table_off);
		return CMD_RET_FAILURE;
	}

	count = buf[4];
	if (!count || count > GXPART_MAX) {
		printf("gxpart: implausible partition count %u\n", count);
		return CMD_RET_FAILURE;
	}

	printf("GxLoader partition table (%u entries):\n", count);
	printf("  %-9s %-10s %-10s\n", "name", "offset", "size");

	pos = 5;
	len = snprintf(parts, sizeof(parts), "mtdparts=spi-nor:");
	for (i = 0; i < count; i++) {
		char name[GXPART_NAME_LEN + 1];
		u32 size;

		if (pos + GXPART_REC_SIZE > sizeof(buf))
			break;

		memcpy(name, &buf[pos], GXPART_NAME_LEN);
		name[GXPART_NAME_LEN] = '\0';

		size = (buf[pos + 9] << 16) | (buf[pos + 10] << 8) |
			buf[pos + 11];

		printf("  %-9s 0x%08x 0x%08x\n", name, offset, size);

		if (len < (int)sizeof(parts))
			len += snprintf(parts + len, sizeof(parts) - len,
					"%s0x%x@0x%x(%s)", i ? "," : "",
					size, offset, name);

		offset += size;
		pos += GXPART_REC_SIZE;
	}

	if (len > 0 && len < (int)sizeof(parts)) {
		env_set("mtdparts", parts);
		printf("gxpart: set mtdparts (total 0x%x bytes)\n", offset);
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(gxpart, 2, 1, do_gxpart,
	   "parse GxLoader SPI-flash partition table",
	   "[table_offset]\n"
	   "    - read/parse the GxLoader partition table (default @0x10000)\n"
	   "      and install a matching 'mtdparts' environment variable");
