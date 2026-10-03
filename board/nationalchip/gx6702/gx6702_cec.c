// SPDX-License-Identifier: GPL-2.0+
/*
 * DesignWare HDMI CEC engine on the GX6702 TX at 0xA4F00000.
 *
 * Register sequence matches the SDK 2.4.0 eCos driver (byte registers at
 * base + reg * 4).  Opcodes match the LXDVB501 product stack.  Logical
 * address candidates are the tuner addresses 3, 6, 7 and 10.
 *
 * cecmode 0/1/2 is the 32-bit word at shared offset 0x90.  The LPC engine,
 * not this HDMI block, posts Image View On and System Standby.  `gxcec on`
 * stores the mode; viewon and standby ask the open 8051 to post 0x04 or 0x36.
 * This file still drives the DesignWare block for raw tx, poll, and address
 * claim.  0x7D31 WKUPCTRL is only read.
 *
 * Give Device Power Status is answered with 0x02 (in transition to on).
 * Reporting "on" (0x00) before the HDMI audio device exists is the
 * LXDVB501 resume bug this port does not copy.
 */

#include <console.h>
#include <fdtdec.h>
#include <dw_hdmi.h>
#include <edid.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <vsprintf.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "gx6702_video.h"

#define CEC_CTRL		0x7d00
#define CEC_STAT		0x7d01
#define CEC_MASK		0x7d02
#define CEC_ADDR_L		0x7d05
#define CEC_ADDR_H		0x7d06
#define CEC_TX_CNT		0x7d07
#define CEC_RX_CNT		0x7d08
#define CEC_TX_DATA		0x7d10
#define CEC_RX_DATA		0x7d20
#define CEC_LOCK		0x7d30
#define CEC_WKUPCTRL		0x7d31

#define CEC_STAT_DONE		0x01
#define CEC_STAT_EOM		0x02
#define CEC_STAT_NACK		0x04
#define CEC_STAT_ARB_LOST	0x08
#define CEC_STAT_ERROR_INIT	0x10
#define CEC_STAT_ERROR_FOLL	0x20
#define CEC_STAT_WAKEUP		0x40

#define CEC_CTRL_START_NORMAL	0x03
#define CEC_MASK_ALL		0x7f
#define CEC_LA_UNREGISTERED	15
#define CEC_BROADCAST		0x0f
#define CEC_DEVICE_TUNER	3
#define CEC_POWER_TO_ON		0x02
#define CEC_CAT_TIMEOUT		9
#define CEC_TX_TIMEOUT_MS	1000

#define CEC_OP_IMAGE_VIEW_ON	0x04
#define CEC_OP_TEXT_VIEW_ON	0x0d
#define CEC_OP_STANDBY		0x36
#define CEC_OP_ACTIVE_SOURCE	0x82
#define CEC_OP_GIVE_PHYS_ADDR	0x83
#define CEC_OP_REPORT_PHYS_ADDR	0x84
#define CEC_OP_SET_STREAM_PATH	0x86
#define CEC_OP_VENDOR_ID	0x87
#define CEC_OP_GIVE_POWER	0x8f
#define CEC_OP_REPORT_POWER	0x90

struct cec_name {
	u8 code;
	const char *name;
};

static const struct cec_name cec_opcodes[] = {
	{ 0x04, "Image View On" },
	{ 0x0d, "Text View On" },
	{ 0x36, "System Standby" },
	{ 0x44, "User Control Pressed" },
	{ 0x45, "User Control Released" },
	{ 0x82, "Active Source" },
	{ 0x83, "Give Physical Address" },
	{ 0x84, "Report Physical Address" },
	{ 0x86, "Set Stream Path" },
	{ 0x87, "Device Vendor ID" },
	{ 0x8f, "Give Device Power Status" },
	{ 0x90, "Report Power Status" },
};

static const struct cec_name cec_keys[] = {
	{ 0x00, "Select" },
	{ 0x01, "Up" },
	{ 0x02, "Down" },
	{ 0x03, "Left" },
	{ 0x04, "Right" },
	{ 0x09, "Root Menu" },
	{ 0x0a, "Setup Menu" },
	{ 0x0b, "Contents Menu" },
	{ 0x0c, "Favorite Menu" },
	{ 0x0d, "Exit" },
	{ 0x20, "Digit 0" },
	{ 0x21, "Digit 1" },
	{ 0x22, "Digit 2" },
	{ 0x23, "Digit 3" },
	{ 0x24, "Digit 4" },
	{ 0x25, "Digit 5" },
	{ 0x26, "Digit 6" },
	{ 0x27, "Digit 7" },
	{ 0x28, "Digit 8" },
	{ 0x29, "Digit 9" },
	{ 0x30, "Channel Up" },
	{ 0x31, "Channel Down" },
	{ 0x40, "Power" },
	{ 0x41, "Volume Up" },
	{ 0x42, "Volume Down" },
	{ 0x43, "Mute" },
	{ 0x44, "Play" },
	{ 0x45, "Stop" },
	{ 0x46, "Pause" },
};

static const u8 cec_la_candidates[] = { 3, 6, 7, 10 };

static int cec_mode;
static bool cec_enabled;
static u8 cec_la = CEC_LA_UNREGISTERED;
static bool cec_pa_valid;
static u16 cec_pa;

static const char *cec_lookup(const struct cec_name *table, int n, u8 code)
{
	int i;

	for (i = 0; i < n; i++) {
		if (table[i].code == code)
			return table[i].name;
	}
	return NULL;
}

/* eCos ISR category: later bits overwrite, DONE is last so it wins. */
static unsigned int cec_category(u8 raw)
{
	unsigned int cat = 0;

	if (raw & CEC_STAT_WAKEUP)
		cat = 7;
	if (raw & CEC_STAT_ERROR_FOLL)
		cat = 6;
	if (raw & CEC_STAT_ERROR_INIT)
		cat = 5;
	if (raw & CEC_STAT_ARB_LOST)
		cat = 4;
	if (raw & CEC_STAT_NACK)
		cat = 3;
	if (raw & CEC_STAT_EOM)
		cat = 2;
	if (raw & CEC_STAT_DONE)
		cat = 1;
	return cat;
}

static const char *cec_category_name(unsigned int cat)
{
	switch (cat) {
	case 1:
		return "DONE";
	case 2:
		return "EOM";
	case 3:
		return "NACK";
	case 4:
		return "ARB_LOST";
	case 5:
		return "ERROR_INIT";
	case 6:
		return "ERROR_FOLL";
	case 7:
		return "WAKEUP";
	case CEC_CAT_TIMEOUT:
		return "TIMEOUT";
	default:
		return "NONE";
	}
}

static void cec_program_la(u8 la)
{
	u16 mask;

	if (la >= CEC_LA_UNREGISTERED)
		mask = 0x8000;
	else
		mask = 1u << la;
	gx6702_hdmi_writeb(CEC_ADDR_L, mask & 0xff);
	gx6702_hdmi_writeb(CEC_ADDR_H, (mask >> 8) & 0xff);
}

static void cec_ungate(void)
{
	u8 clk = gx6702_hdmi_readb(HDMI_MC_CLKDIS);

	clk &= (u8)~HDMI_MC_CLKDIS_CECCLK_DISABLE;
	gx6702_hdmi_writeb(HDMI_MC_CLKDIS, clk);
}

static void cec_gate(void)
{
	u8 clk = gx6702_hdmi_readb(HDMI_MC_CLKDIS);

	clk |= HDMI_MC_CLKDIS_CECCLK_DISABLE;
	gx6702_hdmi_writeb(HDMI_MC_CLKDIS, clk);
}

struct cec_tx_status {
	u8 stat;
	u8 ih;
};

static u8 cec_tx_bits(const struct cec_tx_status *st)
{
	return st->stat | st->ih;
}

/*
 * On the LXDVB104, a finished self-ping leaves CEC_STAT at 0 and latches
 * the NACK in HDMI_IH_CEC_STAT0 (0x04).  CTRL falls back to 0x02.  The
 * same bit positions are used; the interrupt status is the one that moves.
 */
static int cec_wait(struct cec_tx_status *st)
{
	ulong start = get_timer(0);
	u8 bits;

	do {
		st->stat = gx6702_hdmi_readb(CEC_STAT);
		st->ih = gx6702_hdmi_readb(HDMI_IH_CEC_STAT0);
		bits = cec_tx_bits(st);
		if (bits & (CEC_STAT_DONE | CEC_STAT_NACK | CEC_STAT_ARB_LOST |
			    CEC_STAT_ERROR_INIT | CEC_STAT_ERROR_FOLL))
			return cec_category(bits);
		udelay(200);
	} while (get_timer(start) < CEC_TX_TIMEOUT_MS);

	return CEC_CAT_TIMEOUT;
}

static int cec_xfer(const u8 *frame, int len, struct cec_tx_status *st)
{
	int i;

	if (!cec_enabled)
		return -EIO;
	if (!frame || len < 1 || len > 16 || !st)
		return -EINVAL;

	gx6702_hdmi_writeb(CEC_CTRL, 0);
	gx6702_hdmi_writeb(CEC_STAT, 0xff);
	gx6702_hdmi_writeb(HDMI_IH_CEC_STAT0, 0xff);
	for (i = 0; i < len; i++)
		gx6702_hdmi_writeb(CEC_TX_DATA + i, frame[i]);
	gx6702_hdmi_writeb(CEC_TX_CNT, len);
	gx6702_hdmi_writeb(CEC_CTRL, CEC_CTRL_START_NORMAL);
	return cec_wait(st);
}

static void cec_format_pa(u16 pa, char *buf, int len)
{
	snprintf(buf, len, "%x.%x.%x.%x",
		 (pa >> 12) & 0xf, (pa >> 8) & 0xf,
		 (pa >> 4) & 0xf, pa & 0xf);
}

static const char *cec_power_name(u8 st)
{
	switch (st) {
	case 0x00:
		return "On";
	case 0x01:
		return "Standby";
	case 0x02:
		return "In transition to On";
	case 0x03:
		return "In transition to Standby";
	default:
		return NULL;
	}
}

static void cec_describe(const u8 *frame, int len, char *buf, int buflen)
{
	const char *op_name;
	const char *extra;
	char pa[16];
	u8 op;

	if (len < 2) {
		snprintf(buf, buflen, "header only");
		return;
	}

	op = frame[1];
	op_name = cec_lookup(cec_opcodes, ARRAY_SIZE(cec_opcodes), op);
	if (!op_name) {
		snprintf(buf, buflen, "opcode 0x%02x", op);
		return;
	}

	extra = NULL;
	pa[0] = '\0';
	if ((op == 0x44 || op == 0x45) && len >= 3) {
		extra = cec_lookup(cec_keys, ARRAY_SIZE(cec_keys), frame[2]);
		if (extra)
			snprintf(buf, buflen, "%s: %s", op_name, extra);
		else
			snprintf(buf, buflen, "%s: key 0x%02x", op_name, frame[2]);
		return;
	}
	if ((op == CEC_OP_SET_STREAM_PATH || op == CEC_OP_REPORT_PHYS_ADDR ||
	     op == CEC_OP_ACTIVE_SOURCE) && len >= 4) {
		cec_format_pa(((u16)frame[2] << 8) | frame[3], pa, sizeof(pa));
		snprintf(buf, buflen, "%s %s", op_name, pa);
		return;
	}
	if (op == CEC_OP_REPORT_POWER && len >= 3) {
		extra = cec_power_name(frame[2]);
		if (extra)
			snprintf(buf, buflen, "%s: %s", op_name, extra);
		else
			snprintf(buf, buflen, "%s: 0x%02x", op_name, frame[2]);
		return;
	}
	snprintf(buf, buflen, "%s", op_name);
}

static void cec_print_frame(const char *tag, const u8 *frame, int len)
{
	char text[96];
	int i;

	cec_describe(frame, len, text, sizeof(text));
	printf("gxcec: %s", tag);
	for (i = 0; i < len; i++)
		printf(" %02x", frame[i]);
	printf("  %s\n", text);
}

static void cec_print_result(const struct cec_tx_status *st, unsigned int cat)
{
	u8 bits = cec_tx_bits(st);
	bool ack = (bits & CEC_STAT_DONE) && !(bits & CEC_STAT_NACK);
	const char *how = "nack";

	if (cat == CEC_CAT_TIMEOUT)
		how = "timeout";
	else if (ack)
		how = "ack";
	printf("gxcec: tx stat %02x ih %02x eCos %u %s %s\n",
	       st->stat, st->ih, cat, cec_category_name(cat), how);
}

int gx6702_cec_tx(const u8 *frame, int len)
{
	struct cec_tx_status st;
	u8 bits;
	int cat;

	if (!cec_enabled) {
		printf("gxcec: clock gated, run gxcec on\n");
		return -EIO;
	}
	cat = cec_xfer(frame, len, &st);
	if (cat < 0)
		return cat;
	cec_print_result(&st, cat);
	if (cat == CEC_CAT_TIMEOUT)
		return -ETIMEDOUT;
	bits = cec_tx_bits(&st);
	if ((bits & CEC_STAT_DONE) && !(bits & CEC_STAT_NACK))
		return 0;
	return -EIO;
}

static bool cec_pa_from_edid(const u8 *edid, int len, u16 *pa)
{
	const struct edid_cea861_info *cea;
	unsigned int end, i, db_len, db_type;

	if (len < 256 || edid[0x7e] == 0)
		return false;
	cea = (const struct edid_cea861_info *)(edid + 128);
	if (cea->extension_tag != EDID_CEA861_EXTENSION_TAG)
		return false;

	/*
	 * dtd_offset counts from the start of the CEA block.  data[] begins
	 * at byte 4, so the data-block region ends at dtd_offset - 4.
	 */
	if (!cea->dtd_offset)
		end = 123;
	else if (cea->dtd_offset < 4)
		return false;
	else
		end = cea->dtd_offset - 4;
	if (end > 123)
		end = 123;

	for (i = 0; i + 1 < end; ) {
		db_type = EDID_CEA861_DB_TYPE(*cea, i);
		db_len = EDID_CEA861_DB_LEN(*cea, i);
		i++;
		if (i + db_len > end)
			break;
		if (db_type == EDID_CEA861_DB_VENDOR && db_len >= 5 &&
		    cea->data[i] == 0x03 &&
		    cea->data[i + 1] == 0x0c &&
		    cea->data[i + 2] == 0x00) {
			*pa = ((u16)cea->data[i + 3] << 8) | cea->data[i + 4];
			return true;
		}
		i += db_len;
	}
	return false;
}

static void cec_read_pa(void)
{
	u8 edid[256];
	int len;
	char pa[16];

	cec_pa_valid = false;
	len = gx6702_hdmi_read_edid(edid, sizeof(edid));
	if (len < 0) {
		printf("gxcec: EDID unavailable (%d), physical address unknown\n",
		       len);
		return;
	}
	if (!cec_pa_from_edid(edid, len, &cec_pa)) {
		printf("gxcec: no HDMI VSDB physical address\n");
		return;
	}
	cec_pa_valid = true;
	cec_format_pa(cec_pa, pa, sizeof(pa));
	printf("gxcec: physical address %s\n", pa);
}

static int cec_claim(void)
{
	int i;
	struct cec_tx_status st;
	int cat;
	u8 ping;

	gx6702_hdmi_writeb(CEC_CTRL, 0);
	gx6702_hdmi_writeb(CEC_MASK, 0);
	gx6702_hdmi_writeb(CEC_LOCK, 0);
	gx6702_hdmi_writeb(CEC_STAT, 0xff);
	gx6702_hdmi_writeb(HDMI_IH_CEC_STAT0, 0xff);
	gx6702_hdmi_writeb(HDMI_IH_MUTE_CEC_STAT0, 0);

	for (i = 0; i < (int)ARRAY_SIZE(cec_la_candidates); i++) {
		u8 la = cec_la_candidates[i];

		cec_program_la(la);
		ping = (la << 4) | la;
		cat = cec_xfer(&ping, 1, &st);
		if (cat < 0)
			return cat;
		printf("gxcec: ping la %u stat %02x ih %02x eCos %u %s%s\n",
		       la, st.stat, st.ih, cat, cec_category_name(cat),
		       (cat == 1 || cat == 3) ? " line-alive" : "");
		/*
		 * Category 3 is NACK without DONE: the address is free.
		 * Category 1 is an ACK: someone already owns it.  Raw is
		 * printed so a TV that disagrees with that decoding can
		 * correct it.
		 */
		if (cat == 3) {
			cec_la = la;
			printf("gxcec: claimed logical address %u\n", la);
			return 0;
		}
	}

	cec_la = CEC_LA_UNREGISTERED;
	cec_program_la(CEC_LA_UNREGISTERED);
	printf("gxcec: no free logical address; listening as unregistered 15\n");
	return -ENODEV;
}

static void cec_announce(void)
{
	u8 frame[5];

	if (cec_la >= CEC_LA_UNREGISTERED)
		return;

	frame[0] = (cec_la << 4) | CEC_BROADCAST;
	frame[1] = CEC_OP_VENDOR_ID;
	frame[2] = 0;
	frame[3] = 0;
	frame[4] = 0;
	gx6702_cec_tx(frame, 5);

	if (!cec_pa_valid)
		return;
	frame[1] = CEC_OP_REPORT_PHYS_ADDR;
	frame[2] = cec_pa >> 8;
	frame[3] = cec_pa & 0xff;
	frame[4] = CEC_DEVICE_TUNER;
	gx6702_cec_tx(frame, 5);
}

static void cec_maybe_reply(const u8 *frame, int len)
{
	u8 init, dest, op;
	u8 reply[5];

	if (len < 2 || cec_la >= CEC_LA_UNREGISTERED)
		return;
	init = frame[0] >> 4;
	dest = frame[0] & 0x0f;
	if (init == cec_la || init >= CEC_LA_UNREGISTERED)
		return;
	if (dest != cec_la && dest != CEC_BROADCAST)
		return;

	op = frame[1];
	if (op == CEC_OP_GIVE_PHYS_ADDR && cec_pa_valid) {
		reply[0] = (cec_la << 4) | CEC_BROADCAST;
		reply[1] = CEC_OP_REPORT_PHYS_ADDR;
		reply[2] = cec_pa >> 8;
		reply[3] = cec_pa & 0xff;
		reply[4] = CEC_DEVICE_TUNER;
		printf("gxcec: reply Report Physical Address\n");
		gx6702_cec_tx(reply, 5);
		return;
	}
	if (op == CEC_OP_GIVE_POWER) {
		reply[0] = (cec_la << 4) | init;
		reply[1] = CEC_OP_REPORT_POWER;
		reply[2] = CEC_POWER_TO_ON;
		printf("gxcec: reply Report Power Status 0x02 (in transition to on)\n");
		gx6702_cec_tx(reply, 3);
	}
}

static void cec_drain_rx(void)
{
	u8 stat = gx6702_hdmi_readb(CEC_STAT);
	u8 ih = gx6702_hdmi_readb(HDMI_IH_CEC_STAT0);
	u8 bits = stat | ih;
	u8 cnt = gx6702_hdmi_readb(CEC_RX_CNT);
	u8 frame[16];
	int n, i;

	if (bits & CEC_STAT_WAKEUP)
		printf("gxcec: WAKEUP latched stat %02x ih %02x\n", stat, ih);

	if ((bits & CEC_STAT_EOM) || cnt) {
		n = cnt;
		if (n > 16)
			n = 16;
		if (!n)
			n = 1;
		for (i = 0; i < n; i++)
			frame[i] = gx6702_hdmi_readb(CEC_RX_DATA + i);
		gx6702_hdmi_writeb(CEC_LOCK, 0);
		cec_print_frame("rx", frame, n);
		cec_maybe_reply(frame, n);
	}

	if (stat)
		gx6702_hdmi_writeb(CEC_STAT, stat);
	if (ih)
		gx6702_hdmi_writeb(HDMI_IH_CEC_STAT0, ih);
}

int gx6702_cec_mode(void)
{
	return cec_mode;
}

u8 gx6702_cec_logical(void)
{
	return cec_la;
}

bool gx6702_cec_physical(u16 *pa)
{
	if (!pa || !cec_pa_valid)
		return false;
	*pa = cec_pa;
	return true;
}

int gx6702_cec_set_mode(int mode)
{
	int claimed;

	if (mode < 0 || mode > 2)
		return -EINVAL;

	if (mode == 0) {
		gx6702_hdmi_writeb(CEC_MASK, CEC_MASK_ALL);
		cec_gate();
		cec_enabled = false;
		cec_mode = 0;
		cec_la = CEC_LA_UNREGISTERED;
		printf("gxcec: mode 0, CEC clock gated\n");
		return 0;
	}

	cec_ungate();
	cec_read_pa();
	cec_ungate();
	cec_enabled = true;
	claimed = cec_claim();
	if (!claimed)
		cec_announce();
	cec_mode = mode;
	printf("gxcec: mode %d, HDMI CEC clock ungated (TV wake is the LPC engine)\n",
	       mode);
	return 0;
}

void gx6702_cec_dump(void)
{
	u8 clk = gx6702_hdmi_readb(HDMI_MC_CLKDIS);
	char pa[16];

	if (cec_pa_valid)
		cec_format_pa(cec_pa, pa, sizeof(pa));
	else
		snprintf(pa, sizeof(pa), "unknown");

	printf("gxcec: mode %d logical %u pa %s clock %s\n",
	       cec_mode, cec_la, pa,
	       (clk & HDMI_MC_CLKDIS_CECCLK_DISABLE) ? "gated" : "running");
	printf("gxcec: CLKDIS %02x CTRL %02x STAT %02x MASK %02x\n",
	       clk,
	       gx6702_hdmi_readb(CEC_CTRL),
	       gx6702_hdmi_readb(CEC_STAT),
	       gx6702_hdmi_readb(CEC_MASK));
	printf("gxcec: ADDR_L %02x ADDR_H %02x TX_CNT %u RX_CNT %u LOCK %02x\n",
	       gx6702_hdmi_readb(CEC_ADDR_L),
	       gx6702_hdmi_readb(CEC_ADDR_H),
	       gx6702_hdmi_readb(CEC_TX_CNT),
	       gx6702_hdmi_readb(CEC_RX_CNT),
	       gx6702_hdmi_readb(CEC_LOCK));
	printf("gxcec: IH_STAT %02x IH_MUTE %02x WKUPCTRL %02x (not written)\n",
	       gx6702_hdmi_readb(HDMI_IH_CEC_STAT0),
	       gx6702_hdmi_readb(HDMI_IH_MUTE_CEC_STAT0),
	       gx6702_hdmi_readb(CEC_WKUPCTRL));
}

int gx6702_cec_poll(unsigned int seconds)
{
	ulong start;

	if (!cec_enabled) {
		printf("gxcec: clock gated, run gxcec on\n");
		return -EIO;
	}
	if (!seconds) {
		cec_drain_rx();
		return 0;
	}

	printf("gxcec: poll %u s (ctrl-c to stop)\n", seconds);
	start = get_timer(0);
	do {
		cec_drain_rx();
		if (ctrlc()) {
			printf("gxcec: poll stopped\n");
			return -EINTR;
		}
		udelay(1000);
	} while (get_timer(start) < seconds * 1000UL);
	return 0;
}

static int cec_send_broadcast(u8 opcode)
{
	u8 frame[2];

	if (cec_la >= CEC_LA_UNREGISTERED)
		printf("gxcec: logical address 15, broadcast header is 0xff\n");
	frame[0] = (cec_la << 4) | CEC_BROADCAST;
	frame[1] = opcode;
	return gx6702_cec_tx(frame, 2);
}

int gx6702_cec_view_on(void)
{
	u8 frame[4];
	struct cec_tx_status st;
	const u8 directed[] = {
		CEC_OP_TEXT_VIEW_ON,
		CEC_OP_IMAGE_VIEW_ON,
	};
	int i, cat, ret = 0;

	if (!cec_enabled) {
		printf("gxcec: clock gated, run gxcec on\n");
		return -EIO;
	}

	frame[0] = (cec_la << 4) & 0xf0;
	for (i = 0; i < 2; i++) {
		frame[1] = directed[i];
		cec_print_frame("tx", frame, 2);
		cat = cec_xfer(frame, 2, &st);
		if (cat < 0)
			return cat;
		cec_print_result(&st, cat);
		if (cat == CEC_CAT_TIMEOUT)
			ret = -ETIMEDOUT;
	}

	if (!cec_pa_valid) {
		printf("gxcec: physical address unknown, Active Source skipped\n");
		return ret;
	}

	frame[0] = (cec_la << 4) | CEC_BROADCAST;
	frame[1] = CEC_OP_ACTIVE_SOURCE;
	frame[2] = cec_pa >> 8;
	frame[3] = cec_pa & 0xff;
	cec_print_frame("tx", frame, 4);
	cat = cec_xfer(frame, 4, &st);
	if (cat < 0)
		return cat;
	cec_print_result(&st, cat);
	if (cat == CEC_CAT_TIMEOUT)
		ret = -ETIMEDOUT;
	return ret;
}

int gx6702_cec_standby(void)
{
	return cec_send_broadcast(CEC_OP_STANDBY);
}
