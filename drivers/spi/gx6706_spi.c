// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip Cygnus SPI NOR controller (GX6706), driver-model.
 *
 * The controller is a 32-bit DesignWare SSI at 0xA0E00000. GxLoader does not
 * use full-duplex transfers: the opcode and address go out with CTRLR0 in
 * TX-only mode, then a receive uses RX-only mode and one DR write to start
 * the clocks. An external chip-select latch (0xA030A904 on the 1.02a core,
 * 0xA0E00404 otherwise) stays asserted across the SSIENR toggle so the NOR
 * remains selected. RX_SAMPLE_DLY is 4; leaving it at reset shifts the JEDEC
 * ID by one bit.
 */

#include <dm.h>
#include <errno.h>
#include <fuse.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define GX6706_SPI_TIMEOUT	1000000
#define GX6706_SPI_MAX_FIFO	256

#define DW_CTRLR0		0x00
#define DW_CTRLR1		0x04
#define DW_SSIENR		0x08
#define DW_SER			0x10
#define DW_BAUDR		0x14
#define DW_RXFTLR		0x1c
#define DW_RXFLR			0x24
#define DW_SR			0x28
#define DW_IMR			0x2c
#define DW_RISR			0x34
#define DW_ICR			0x48
#define DW_VERSION		0x5c
#define DW_DR			0x60
#define DW_RX_SAMPLE_DLY	0xf0
#define DW_SPI_CTRLR0		0xf4

#define DW_SR_BUSY		BIT(0)
#define DW_SR_TFNF		BIT(1)
#define DW_SR_TFE		BIT(2)
#define DW_SR_RFNE		BIT(3)
#define DW_RISR_RXOI		BIT(3)

#define DW_CTRLR0_READ		0x00000807
#define DW_CTRLR0_WRITE		0x00000407
#define DW_BAUDR_VENDOR		6
#define DW_RX_SAMPLE_VENDOR	4
#define DW_SPI_CTRLR0_VENDOR	0x202
#define DW_VERSION_102A		0x3130322a

#define GX6706_SPI_GATE		0xa030a900
#define GX6706_SPI_CS_LEGACY	0xa030a904
#define GX6706_SPI_CS_NEW	0xa0e00404
#define GX6706_SPI_ROUTE	0xa030a1f0
#define GX6706_SPI_WRAP		0xa0701000
#define GX6706_WRAP_CTRL	0x000
#define GX6706_WRAP_PAD		0x280
#define GX6706_WRAP_PAD_HI	0x284

struct gx6706_spi_priv {
	void __iomem *regs;
	u32 cs_reg;
	u32 rx_fifo_len;
};

static int dw_wait(struct gx6706_spi_priv *priv, u32 mask, bool set)
{
	u32 i;

	for (i = 0; i < GX6706_SPI_TIMEOUT; i++) {
		u32 value = readl(priv->regs + DW_SR);

		if (!!(value & mask) == set)
			return 0;
	}

	return -ETIMEDOUT;
}

static void dw_disable(struct gx6706_spi_priv *priv)
{
	writel(0, priv->regs + DW_SSIENR);
}

static int dw_detect_rx_fifo(struct gx6706_spi_priv *priv)
{
	u32 depth;

	/* RXFTLR accepts 0 through FIFO depth - 1 while SSI is disabled. */
	for (depth = 1; depth <= GX6706_SPI_MAX_FIFO; depth++) {
		writel(depth, priv->regs + DW_RXFTLR);
		if (readl(priv->regs + DW_RXFTLR) != depth)
			break;
	}
	writel(0, priv->regs + DW_RXFTLR);
	if (depth == 1 || depth > GX6706_SPI_MAX_FIFO)
		return -EINVAL;

	priv->rx_fifo_len = depth;
	return 0;
}

static int dw_rx_error(struct gx6706_spi_priv *priv, int ret, u32 got, u32 len)
{
	u32 status = readl(priv->regs + DW_SR);
	u32 level = readl(priv->regs + DW_RXFLR);
	u32 raw = readl(priv->regs + DW_RISR);

	/* Disabling SSI clears the FIFO and can clear the overflow evidence. */
	printf("gx6706-spi: RX %s after %u/%u bytes (sr=%08x rxflr=%u risr=%08x)\n",
	       ret == -EIO ? "overflow" : "timeout", got, len, status, level, raw);
	dw_disable(priv);
	return ret;
}

static void dw_flush_rx(struct gx6706_spi_priv *priv)
{
	while (readl(priv->regs + DW_SR) & DW_SR_RFNE)
		readl(priv->regs + DW_DR);
}

static void dw_cs(struct gx6706_spi_priv *priv, bool assert)
{
	writel(assert ? 2 : 3, priv->cs_reg);
}

static void dw_prepare(struct gx6706_spi_priv *priv, u32 ctrlr0, u32 rx_len)
{
	dw_disable(priv);
	writel(DW_RX_SAMPLE_VENDOR, priv->regs + DW_RX_SAMPLE_DLY);
	writel(ctrlr0, priv->regs + DW_CTRLR0);
	writel(rx_len, priv->regs + DW_CTRLR1);
	dw_flush_rx(priv);
	readl(priv->regs + DW_ICR);
	writel(0, priv->regs + DW_RXFTLR);
	writel(DW_SPI_CTRLR0_VENDOR, priv->regs + DW_SPI_CTRLR0);
	writel(1, priv->regs + DW_SER);
	writel(1, priv->regs + DW_SSIENR);
}

static int dw_tx(struct gx6706_spi_priv *priv, const u8 *buf, u32 len)
{
	u32 sent = 0;

	if (!len)
		return 0;
	if (dw_wait(priv, DW_SR_BUSY, false))
		return -ETIMEDOUT;

	dw_prepare(priv, DW_CTRLR0_WRITE, 0);
	while (sent < len) {
		if (dw_wait(priv, DW_SR_TFNF, true)) {
			dw_disable(priv);
			return -ETIMEDOUT;
		}
		writel(buf[sent++], priv->regs + DW_DR);
	}
	if (dw_wait(priv, DW_SR_TFE, true) ||
	    dw_wait(priv, DW_SR_BUSY, false)) {
		dw_disable(priv);
		return -ETIMEDOUT;
	}
	dw_disable(priv);
	return 0;
}

static int dw_rx(struct gx6706_spi_priv *priv, u8 *buf, u32 len)
{
	while (len) {
		u32 chunk = min(len, priv->rx_fifo_len);
		u32 got = 0;
		u32 idle = 0;

		if (dw_wait(priv, DW_SR_BUSY, false))
			return -ETIMEDOUT;
		/*
		 * RX-only clocks cannot pause when the FIFO fills. Match the
		 * vendor byte path: one burst must fit entirely in the RX FIFO.
		 * The external CS latch remains asserted between bursts.
		 */
		dw_prepare(priv, DW_CTRLR0_READ, chunk - 1);
		/* One DR write starts the programmed RX-only count. */
		writel(0, priv->regs + DW_DR);

		while (got < chunk) {
			u32 available = readl(priv->regs + DW_RXFLR);

			if (available) {
				available = min(available, chunk - got);
				while (available--)
					buf[got++] = (u8)readl(priv->regs + DW_DR);
				idle = 0;
			} else {
				if (readl(priv->regs + DW_RISR) & DW_RISR_RXOI)
					return dw_rx_error(priv, -EIO, got, chunk);
				if (++idle == GX6706_SPI_TIMEOUT)
					return dw_rx_error(priv, -ETIMEDOUT, got, chunk);
			}
		}
		if (readl(priv->regs + DW_RISR) & DW_RISR_RXOI)
			return dw_rx_error(priv, -EIO, got, chunk);
		if (dw_wait(priv, DW_SR_BUSY, false))
			return dw_rx_error(priv, -ETIMEDOUT, got, chunk);
		dw_disable(priv);
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int gx6706_spi_exec_op(struct spi_slave *slave,
			      const struct spi_mem_op *op)
{
	struct udevice *bus = dev_get_parent(slave->dev);
	struct gx6706_spi_priv *priv = dev_get_priv(bus);
	u8 hdr[16];
	u32 hdr_len = 0;
	u32 i;
	int ret = 0;

	hdr[hdr_len++] = op->cmd.opcode;
	for (i = 0; i < op->addr.nbytes; i++)
		hdr[hdr_len++] = op->addr.val >> (8 * (op->addr.nbytes - 1 - i));
	for (i = 0; i < op->dummy.nbytes; i++)
		hdr[hdr_len++] = 0;

	dw_cs(priv, true);
	ret = dw_tx(priv, hdr, hdr_len);
	if (!ret && op->data.nbytes) {
		if (op->data.dir == SPI_MEM_DATA_IN)
			ret = dw_rx(priv, op->data.buf.in, op->data.nbytes);
		else if (op->data.dir == SPI_MEM_DATA_OUT)
			ret = dw_tx(priv, op->data.buf.out, op->data.nbytes);
	}
	dw_cs(priv, false);

	return ret;
}

static bool gx6706_spi_supports_op(struct spi_slave *slave,
				   const struct spi_mem_op *op)
{
	if (op->cmd.buswidth != 1 || op->addr.buswidth > 1 ||
	    op->dummy.buswidth > 1 || op->data.buswidth > 1)
		return false;
	if (op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr)
		return false;
	if (op->addr.nbytes > 4 || op->dummy.nbytes > 8)
		return false;
	if (1 + op->addr.nbytes + op->dummy.nbytes > 16)
		return false;

	return true;
}

static const struct spi_controller_mem_ops gx6706_spi_mem_ops = {
	.supports_op	= gx6706_spi_supports_op,
	.exec_op	= gx6706_spi_exec_op,
};

static int gx6706_spi_claim_bus(struct udevice *dev)
{
	return 0;
}

static int gx6706_spi_release_bus(struct udevice *dev)
{
	struct gx6706_spi_priv *priv = dev_get_priv(dev_get_parent(dev));

	dw_cs(priv, false);
	return 0;
}

static int gx6706_spi_set_speed(struct udevice *bus, uint hz)
{
	return 0;
}

static int gx6706_spi_set_mode(struct udevice *bus, uint mode)
{
	return 0;
}

static int gx6706_spi_xfer(struct udevice *dev, unsigned int bitlen,
			   const void *dout, void *din, unsigned long flags)
{
	struct gx6706_spi_priv *priv = dev_get_priv(dev_get_parent(dev));
	unsigned int bytes = bitlen / 8;
	int ret = 0;

	if (bitlen % 8)
		return -EINVAL;
	if (dout && din)
		return -EOPNOTSUPP;

	if (flags & SPI_XFER_BEGIN)
		dw_cs(priv, true);
	if (dout)
		ret = dw_tx(priv, dout, bytes);
	else if (din)
		ret = dw_rx(priv, din, bytes);
	if (ret || (flags & SPI_XFER_END))
		dw_cs(priv, false);

	return ret;
}

static u8 gx6706_efuse_byte(u32 addr, u8 fallback)
{
	u32 value;

	if (fuse_read(0, addr, &value) || !value)
		return fallback;

	return (u8)value;
}

static void gx6706_spi_pad_calibrate(void)
{
	u8 variant = gx6706_efuse_byte(0x138, 0);
	u8 fallback_hi, fallback, pad_hi, pad;
	u32 value;

	if ((variant & 0x0f) == 3) {
		fallback_hi = 0xda;
		fallback = 0xcb;
	} else {
		fallback_hi = 0x24;
		fallback = 0x7b;
	}
	pad_hi = gx6706_efuse_byte(0x126, fallback_hi);
	pad = gx6706_efuse_byte(0x12a, fallback);

	value = readl(GX6706_SPI_WRAP + GX6706_WRAP_PAD_HI);
	writel((value & ~0xff) | pad_hi, GX6706_SPI_WRAP + GX6706_WRAP_PAD_HI);
	value = readl(GX6706_SPI_WRAP + GX6706_WRAP_PAD);
	writel((value & 0xffffff0f) | (pad & 0xf0),
	       GX6706_SPI_WRAP + GX6706_WRAP_PAD);
}

/*
 * IO-domain routing GxLoader performs before the SPI route. Both clock
 * selectors are 6 on the generic 672 MHz H5/S5 configuration.
 */
static void gx6706_spi_io_init(void)
{
	u32 value;

	value = readl(0xa4809000) & 0xc1ffffff;
	writel(value | (6 << 25), 0xa4809000);
	value = readl(0xa4808000) & 0xc1ffffff;
	writel(value | (6 << 25), 0xa4808000);

	value = readl(0xa4808070) & 0xff00ffff;
	writel(value | 0x00590000, 0xa4808070);
	writel(1, 0xa4800138);
	writel(0, 0xa4800140);
	writel(0, 0xa4800144);
	writel(0x00010000, 0xa4800148);
	writel(0, 0xa480014c);
	writel(readl(0xa4804164) & ~0xff, 0xa4804164);
	writel(readl(0xa4804168) & ~0xff, 0xa4804168);
	writel(0x000a0320, 0xa480900c);
	writel(0x00163863, 0xa48081b0);
	writel(0x00163863, 0xa48081b8);
	writel(0x10000001, 0xa4809110);
	writel(0x10000001, 0xa4809130);
	writel(0x10000001, 0xa4809150);
}

static int gx6706_spi_probe(struct udevice *bus)
{
	struct gx6706_spi_priv *priv = dev_get_priv(bus);
	u32 route, wrapper, version;
	int ret;

	priv->regs = dev_read_addr_ptr(bus);
	if (!priv->regs)
		return -EINVAL;

	writel(0, GX6706_SPI_WRAP + GX6706_WRAP_CTRL);
	writel(0xc0, GX6706_SPI_WRAP + GX6706_WRAP_CTRL);
	gx6706_spi_io_init();

	route = readl(GX6706_SPI_ROUTE);
	route &= ~BIT(0);
	route |= BIT(1);
	route &= ~0x00000f70;
	writel(route, GX6706_SPI_ROUTE);
	writeb(5, 0xa030a794);
	gx6706_spi_pad_calibrate();

	wrapper = readl(GX6706_SPI_WRAP + GX6706_WRAP_PAD);
	wrapper |= BIT(0);
	writel(wrapper, GX6706_SPI_WRAP + GX6706_WRAP_PAD);
	wrapper = readl(GX6706_SPI_WRAP + GX6706_WRAP_PAD);
	wrapper |= BIT(1);
	writel(wrapper, GX6706_SPI_WRAP + GX6706_WRAP_PAD);

	writel(1, GX6706_SPI_GATE);
	version = readl(priv->regs + DW_VERSION);
	priv->cs_reg = version == DW_VERSION_102A ? GX6706_SPI_CS_LEGACY :
						    GX6706_SPI_CS_NEW;
	writel(3, priv->cs_reg);

	dw_disable(priv);
	writel(0, priv->regs + DW_IMR);
	ret = dw_detect_rx_fifo(priv);
	if (ret) {
		printf("gx6706-spi: cannot detect RX FIFO depth\n");
		return ret;
	}
	writel(DW_CTRLR0_READ, priv->regs + DW_CTRLR0);
	writel(0, priv->regs + DW_CTRLR1);
	writel(DW_BAUDR_VENDOR, priv->regs + DW_BAUDR);
	writel(DW_RX_SAMPLE_VENDOR, priv->regs + DW_RX_SAMPLE_DLY);
	writel(DW_SPI_CTRLR0_VENDOR, priv->regs + DW_SPI_CTRLR0);
	readl(priv->regs + DW_ICR);

	return 0;
}

static const struct dm_spi_ops gx6706_spi_ops = {
	.claim_bus	= gx6706_spi_claim_bus,
	.release_bus	= gx6706_spi_release_bus,
	.xfer		= gx6706_spi_xfer,
	.set_speed	= gx6706_spi_set_speed,
	.set_mode	= gx6706_spi_set_mode,
	.mem_ops	= &gx6706_spi_mem_ops,
};

static const struct udevice_id gx6706_spi_ids[] = {
	{ .compatible = "nationalchip,gx6706-spi" },
	{}
};

U_BOOT_DRIVER(gx6706_spi) = {
	.name		= "gx6706_spi",
	.id		= UCLASS_SPI,
	.of_match	= gx6706_spi_ids,
	.ops		= &gx6706_spi_ops,
	.priv_auto	= sizeof(struct gx6706_spi_priv),
	.probe		= gx6706_spi_probe,
};
