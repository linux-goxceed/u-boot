// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip Gemini SPI flash controller (GX6702 / GX6605), driver-model.
 *
 * Reverse-engineered from the GX6702 BootROM. Flash access goes through the
 * "gxflash" engine at 0x00302000 (uncached alias 0xA0302000 after the vendor
 * stage-1 MMU), which the BootROM uses for all NOR reads (routine @ 0x1af4):
 *
 *   - CTRL  (+0x00): bit 12 = GO strobe, bit 17 = CS hold
 *   - STAT  (+0x04): bit 0  = ready/done (poll until set)
 *   - CMD   (+0x08): 32-bit word shifted out on MOSI, MSB byte first
 *   - DATA  (+0x0C): 32-bit word shifted in on MISO, first byte in bits [7:0]
 *   - AUX   (+0x10): cleared during engine init only
 *
 * Each GO strobe performs a full-duplex 32-bit shift (BootROM @ 0x1af4): write
 * the outgoing word to CMD, set CTRL bit 12, poll STAT bit 0, clear bit 12, then
 * read DATA for the received word. The word is transmitted big-endian (the flash
 * opcode goes in the top byte, e.g. CMD = (0x03 << 24) | addr for a READ) but
 * received little-endian: the BootROM's flash->RAM copy stores DATA with a
 * native word store and increments the address by 4, so the first flash byte
 * lands in DATA bits [7:0]. CTRL stays at the 0x29C043 init value throughout
 * (no per-byte direction toggling). CS is held across a whole spi-mem operation
 * via CTRL bit 17 (BootROM @ 0x1a34 / 0x1a20).
 *
 * Clock gates and the pin/engine route register are programmed once at probe
 * (BootROM @ 0x1edc): clk 62/16 and route 0xA010221C <- (1 << 22).
 */

#include <dm.h>
#include <errno.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

/* Fixed SoC registers (uncached alias, absolute). */
#define GX6702_CLK_A		0xA4D00204
#define GX6702_CLK_B		0xA4D00300
#define GX6702_CLK_A_VAL	62
#define GX6702_CLK_B_VAL	16

/*
 * Pin/route register (BootROM @ 0x1eec writes 1<<22). The BootROM runs with
 * the MMU off and uses the raw physical address 0x0010221C; under U-Boot the
 * vendor stage-1 MMU is on, so peripherals must be reached through the uncached
 * 0xA0000000 alias (same as the 0xA0302000 engine and 0xA4D003xx clocks).
 */
#define GX6702_SPI_ROUTE_REG	0xA010221C
#define GX6702_SPI_ROUTE_BASE	(1 << 22)

/* gxflash engine register offsets (base from device tree). */
#define GX_FLASH_CTRL		0x00
#define GX_FLASH_STAT		0x04
#define GX_FLASH_CMD		0x08
#define GX_FLASH_DATA		0x0C
#define GX_FLASH_AUX		0x10

#define GX_CTRL_GO		BIT(12)
#define GX_CTRL_CSHOLD		BIT(17)
#define GX_STAT_RDY		BIT(0)

/*
 * CTRL init value the BootROM programs before every flash access (@ 0x1adc):
 * CTRL = 0x29C043, AUX (+0x10) = 0. The BootROM's working read path keeps CTRL
 * at this value across opcode/address/data phases, so we program it once at
 * probe rather than preserving whatever the vendor pre-loader left behind.
 */
#define GX_FLASH_CTRL_INIT	0x0029C043

#define GX_SPI_WAIT_MAX		1000000

struct gx6702_spi_priv {
	void __iomem *regs;
};

static int gx_flash_wait_ready(struct gx6702_spi_priv *priv)
{
	unsigned int i;

	for (i = 0; i < GX_SPI_WAIT_MAX; i++) {
		if (readl(priv->regs + GX_FLASH_STAT) & GX_STAT_RDY)
			return 0;
	}

	return -ETIMEDOUT;
}

/*
 * One 32-bit full-duplex shift on the gxflash engine (BootROM @ 0x1af4): load
 * the outgoing word into CMD, pulse the GO strobe (bit 12), poll STAT bit 0
 * until done, drop GO, then read the captured word from DATA. CTRL (mode bits
 * and CS-hold bit 17) is preserved across the read-modify-write.
 */
static int gx_flash_word(struct gx6702_spi_priv *priv, u32 out, u32 *in)
{
	void __iomem *regs = priv->regs;
	u32 ctrl;
	int ret;

	writel(out, regs + GX_FLASH_CMD);

	ctrl = readl(regs + GX_FLASH_CTRL) & ~GX_CTRL_GO;
	writel(ctrl | GX_CTRL_GO, regs + GX_FLASH_CTRL);
	ret = gx_flash_wait_ready(priv);
	writel(ctrl, regs + GX_FLASH_CTRL);	/* drop GO, keep CTRL/CS */
	if (ret)
		return ret;

	if (in)
		*in = readl(regs + GX_FLASH_DATA);

	return 0;
}

static void gx_flash_cs(struct gx6702_spi_priv *priv, bool assert)
{
	u32 ctrl = readl(priv->regs + GX_FLASH_CTRL);

	if (assert)
		ctrl |= GX_CTRL_CSHOLD;
	else
		ctrl &= ~GX_CTRL_CSHOLD;
	writel(ctrl, priv->regs + GX_FLASH_CTRL);
}

/*
 * spi-mem: stream cmd / addr / dummy / data as one CS-asserted transaction,
 * matching the BootROM gxflash read path (@ 0x1af4). The engine shifts a full
 * 32-bit word per GO, so the whole transfer is flattened into a byte stream
 * (opcode, then address, then dummy, then data) and clocked four bytes at a
 * time: outgoing bytes are packed MSB-first into CMD, and for read data the
 * captured DATA word is unpacked LSB-first (first byte in bits [7:0]).
 */
static int gx6702_spi_exec_op(struct spi_slave *slave,
			      const struct spi_mem_op *op)
{
	struct udevice *bus = dev_get_parent(slave->dev);
	struct gx6702_spi_priv *priv = dev_get_priv(bus);
	const u8 *dout = NULL;
	u8 *din = NULL;
	u8 hdr[8];
	unsigned int hdr_len = 0, data_len = 0, total, pos, i;
	bool data_in = false;
	int ret;

	hdr[hdr_len++] = op->cmd.opcode;
	for (i = 0; i < op->addr.nbytes; i++)
		hdr[hdr_len++] = op->addr.val >> (8 * (op->addr.nbytes - 1 - i));
	for (i = 0; i < op->dummy.nbytes; i++)
		hdr[hdr_len++] = 0xff;

	if (op->data.nbytes) {
		data_len = op->data.nbytes;
		if (op->data.dir == SPI_MEM_DATA_IN) {
			din = op->data.buf.in;
			data_in = true;
		} else {
			dout = op->data.buf.out;
		}
	}

	total = hdr_len + data_len;
	gx_flash_cs(priv, true);

	for (pos = 0; pos < total; pos += 4) {
		u32 cmd = 0, data = 0;
		bool need_read = false;
		unsigned int lane;

		for (lane = 0; lane < 4; lane++) {
			unsigned int p = pos + lane;
			u8 b;

			if (p >= total)
				break;
			if (p < hdr_len)
				b = hdr[p];
			else if (data_in) {
				b = 0;
				need_read = true;
			} else {
				b = dout[p - hdr_len];
			}
			cmd |= (u32)b << (24 - 8 * lane);
		}

		ret = gx_flash_word(priv, cmd, need_read ? &data : NULL);
		if (ret)
			goto out;

		if (need_read) {
			for (lane = 0; lane < 4; lane++) {
				unsigned int p = pos + lane;

				if (p >= total)
					break;
				if (p >= hdr_len)
					din[p - hdr_len] = (data >> (8 * lane)) & 0xff;
			}
		}
	}

out:
	gx_flash_cs(priv, false);
	return ret;
}

static bool gx6702_spi_supports_op(struct spi_slave *slave,
				   const struct spi_mem_op *op)
{
	/* Single-lane only; the gxflash engine has no dual/quad path. */
	if (op->cmd.buswidth != 1 || op->addr.buswidth > 1 ||
	    op->dummy.buswidth > 1 || op->data.buswidth > 1)
		return false;
	if (op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr)
		return false;

	return true;
}

static const struct spi_controller_mem_ops gx6702_spi_mem_ops = {
	.supports_op	= gx6702_spi_supports_op,
	.exec_op	= gx6702_spi_exec_op,
};

static int gx6702_spi_xfer(struct udevice *dev, unsigned int bitlen,
			   const void *dout, void *din, unsigned long flags)
{
	struct gx6702_spi_priv *priv = dev_get_priv(dev_get_parent(dev));
	const u8 *tx = dout;
	u8 *rx = din;
	unsigned int bytes = bitlen / 8;
	unsigned int pos;
	int ret = 0;

	if (bitlen % 8)
		return -EINVAL;

	if (flags & SPI_XFER_BEGIN)
		gx_flash_cs(priv, true);

	for (pos = 0; pos < bytes; pos += 4) {
		u32 cmd = 0, data = 0;
		unsigned int lane;

		for (lane = 0; lane < 4; lane++) {
			unsigned int p = pos + lane;
			u8 out;

			if (p >= bytes)
				break;
			out = tx ? tx[p] : 0xff;
			cmd |= (u32)out << (24 - 8 * lane);
		}

		ret = gx_flash_word(priv, cmd, rx ? &data : NULL);
		if (ret)
			break;

		if (rx) {
			for (lane = 0; lane < 4; lane++) {
				unsigned int p = pos + lane;

				if (p >= bytes)
					break;
				rx[p] = (data >> (8 * lane)) & 0xff;
			}
		}
	}

	if (flags & SPI_XFER_END)
		gx_flash_cs(priv, false);

	return ret;
}

static int gx6702_spi_claim_bus(struct udevice *dev)
{
	return 0;
}

static int gx6702_spi_release_bus(struct udevice *dev)
{
	struct gx6702_spi_priv *priv = dev_get_priv(dev_get_parent(dev));

	gx_flash_cs(priv, false);
	return 0;
}

static int gx6702_spi_set_speed(struct udevice *bus, uint hz)
{
	return 0;
}

static int gx6702_spi_set_mode(struct udevice *bus, uint mode)
{
	return 0;
}

static int gx6702_spi_probe(struct udevice *bus)
{
	struct gx6702_spi_priv *priv = dev_get_priv(bus);

	priv->regs = dev_read_addr_ptr(bus);
	if (!priv->regs)
		return -EINVAL;

	/* Clock gates (BootROM @ 0x1edc). */
	writel(GX6702_CLK_A_VAL, GX6702_CLK_A);
	writel(GX6702_CLK_B_VAL, GX6702_CLK_B);

	/* Pin/engine route (write-only; never read back). */
	writel(GX6702_SPI_ROUTE_BASE, GX6702_SPI_ROUTE_REG);

	/* gxflash engine init (BootROM @ 0x1adc): CTRL = 0x29C043, AUX = 0. */
	writel(GX_FLASH_CTRL_INIT, priv->regs + GX_FLASH_CTRL);
	writel(0, priv->regs + GX_FLASH_AUX);

	gx_flash_cs(priv, false);

	return 0;
}

static const struct dm_spi_ops gx6702_spi_ops = {
	.claim_bus	= gx6702_spi_claim_bus,
	.release_bus	= gx6702_spi_release_bus,
	.xfer		= gx6702_spi_xfer,
	.set_speed	= gx6702_spi_set_speed,
	.set_mode	= gx6702_spi_set_mode,
	.mem_ops	= &gx6702_spi_mem_ops,
};

static const struct udevice_id gx6702_spi_ids[] = {
	{ .compatible = "nationalchip,gx6702-spi" },
	{ .compatible = "nationalchip,gx6605-spi" },
	{}
};

U_BOOT_DRIVER(gx6702_spi) = {
	.name		= "gx6702_spi",
	.id		= UCLASS_SPI,
	.of_match	= gx6702_spi_ids,
	.ops		= &gx6702_spi_ops,
	.priv_auto	= sizeof(struct gx6702_spi_priv),
	.probe		= gx6702_spi_probe,
};
