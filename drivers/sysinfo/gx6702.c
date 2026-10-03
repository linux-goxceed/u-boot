// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip GX6702/GX6706 silicon identification
 *
 * The stock GxLoader presents both fields in reverse byte order. Short
 * chip names are zero-padded at the beginning of the raw 12-byte field.
 */

#include <dm.h>
#include <stdio.h>
#include <string.h>
#include <sysinfo.h>
#include <asm/io.h>

#define GX6702_CHIP_NAME_ADDR	0xa030a190
#define GX6702_CHIP_NAME_SIZE	12
#define GX6702_PUBLIC_ID_ADDR	0xa030a560
#define GX6702_PUBLIC_ID_SIZE	8

struct gx6702_sysinfo_priv {
	char serial[GX6702_PUBLIC_ID_SIZE * 2 + 1];
	char revision[GX6702_CHIP_NAME_SIZE + 1];
};

static int gx6702_read_chip_name(char *name)
{
	u8 raw[GX6702_CHIP_NAME_SIZE];
	int first = 0;
	int i;

	for (i = 0; i < GX6702_CHIP_NAME_SIZE; i++)
		raw[i] = readb(GX6702_CHIP_NAME_ADDR + i);

	while (first < GX6702_CHIP_NAME_SIZE && !raw[first])
		first++;
	if (first == GX6702_CHIP_NAME_SIZE)
		return -ENODEV;

	for (i = first; i < GX6702_CHIP_NAME_SIZE; i++) {
		if (raw[i] < 0x20 || raw[i] > 0x7e)
			return -EINVAL;
	}

	for (i = GX6702_CHIP_NAME_SIZE - 1; i >= first; i--)
		*name++ = raw[i];
	*name = '\0';

	return 0;
}

static int gx6702_read_public_id(char *serial)
{
	static const char hex[] = "0123456789abcdef";
	u8 raw[GX6702_PUBLIC_ID_SIZE];
	bool all_zero = true;
	bool all_ff = true;
	int i;

	for (i = 0; i < GX6702_PUBLIC_ID_SIZE; i++) {
		raw[i] = readb(GX6702_PUBLIC_ID_ADDR + i);
		all_zero &= raw[i] == 0;
		all_ff &= raw[i] == 0xff;
	}
	if (all_zero || all_ff)
		return -ENODEV;

	for (i = GX6702_PUBLIC_ID_SIZE - 1; i >= 0; i--) {
		*serial++ = hex[raw[i] >> 4];
		*serial++ = hex[raw[i] & 0xf];
	}
	*serial = '\0';

	return 0;
}

static int gx6702_sysinfo_detect(struct udevice *dev)
{
	struct gx6702_sysinfo_priv *priv = dev_get_priv(dev);
	int ret;

	ret = gx6702_read_chip_name(priv->revision);
	if (ret)
		return ret;

	return gx6702_read_public_id(priv->serial);
}

static int gx6702_sysinfo_get_str(struct udevice *dev, int id, size_t size,
				  char *val)
{
	struct gx6702_sysinfo_priv *priv = dev_get_priv(dev);
	const char *str;

	switch (id) {
	case SYSID_SM_SYSTEM_SERIAL:
		str = priv->serial;
		break;
	case SYSID_SM_BASEBOARD_VERSION:
		str = priv->revision;
		break;
	default:
		return -EINVAL;
	}

	if (!size)
		return -ENOSPC;
	strlcpy(val, str, size);

	return 0;
}

static const char *gx_board_family(const char *name)
{
	if (!strncmp(name, "6701", 4) || !strncmp(name, "6702", 4) ||
	    !strncmp(name, "6703", 4))
		return "Gemini";
	if (!strncmp(name, "6705", 4) || !strncmp(name, "6706", 4))
		return "Cygnus";
	return NULL;
}

void gx_sysinfo_print_board(void)
{
	struct udevice *dev;
	char name[GX6702_CHIP_NAME_SIZE + 1];
	const char *family;

	name[0] = '\0';
	if (!sysinfo_get(&dev)) {
		sysinfo_detect(dev);
		if (sysinfo_get_str(dev, SYSID_SM_BASEBOARD_VERSION,
				    sizeof(name), name))
			name[0] = '\0';
	}

	if (!name[0]) {
		printf("Board: NationalChip (chip name unavailable)\n");
		return;
	}

	family = gx_board_family(name);
	if (!family) {
		printf("Board: NationalChip GX%.4s (%.6s)\n", name, name);
		return;
	}

	printf("Board: NationalChip GX%.4s (%s %.6s)\n", name, family, name);
}

static const struct sysinfo_ops gx6702_sysinfo_ops = {
	.detect = gx6702_sysinfo_detect,
	.get_str = gx6702_sysinfo_get_str,
};

static const struct udevice_id gx6702_sysinfo_ids[] = {
	{ .compatible = "nationalchip,gx6702-sysinfo" },
	{ .compatible = "nationalchip,gx6706-sysinfo" },
	{ }
};

U_BOOT_DRIVER(gx6702_sysinfo) = {
	.name = "gx6702_sysinfo",
	.id = UCLASS_SYSINFO,
	.of_match = gx6702_sysinfo_ids,
	.ops = &gx6702_sysinfo_ops,
	.priv_auto = sizeof(struct gx6702_sysinfo_priv),
};
