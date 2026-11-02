/*
 * Microsoft UC120 USB Type-C / USB PD PHY (Lattice iCE5LP2K) on the
 * Lumia 950 XL (cityman).
 *
 * The FPGA does the Type-C attach detection (DRP), the BMC PHY and GoodCRC.
 * This driver does the rest of what the Windows ice5lp_2k.sys and tyc.dll
 * did, as reconstructed from their binaries:
 *  - sink attach: charging current from the partner Rp (1.5 A / 3 A);
 *  - source attach: VBUS (USB host), VCONN, USB PD 2.0 source contract
 *    (5 V) and DisplayPort alternate mode, the HD3SS460-style mux and the
 *    ANX7816 bridge HPD.
 *
 * Register access: one SPI frame per access, command byte reg << 3 | write;
 * a read returns the value in the third byte of a three byte frame.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/slimport.h>
#include <linux/spi/spi.h>
#include <linux/wait.h>

#define UC120_TX_CMD		0	/* write: length starts TX, 0x20 hard reset */
#define UC120_STATUS		1	/* rx length [4:0], status [7:5] */
#define UC120_INT		2	/* events, write 0xff to clear */
#define UC120_INT_MASK		3
#define UC120_CTRL		4
#define  CTRL_IRQ_EN		BIT(0)
#define  CTRL_UNATTACHED	BIT(1)
#define  CTRL_PD_EN		BIT(7)
#define UC120_ROLE		5
#define  ROLE_SOURCE		BIT(0)
#define  ROLE_DFP		BIT(2)
#define  ROLE_VCONN		BIT(5)
#define  ROLE_VBUS_IN		BIT(6)
#define  ROLE_RESET		BIT(7)
#define UC120_CONN		7	/* Rp [5:4], orientation bit 6 */
#define UC120_TX_FIFO		16
#define UC120_RX_FIFO		17

#define TC_DETACH		1
#define TC_DFP_UFP		2
#define TC_DFP_CABLE_UFP	3
#define TC_DETACH2		4
#define TC_UFP_DFP		5

/* PD 2.0 */
#define PD_CTRL_ACCEPT		3
#define PD_CTRL_REJECT		4
#define PD_CTRL_PS_RDY		6
#define PD_CTRL_GET_SRC_CAP	7
#define PD_CTRL_GET_SINK_CAP	8
#define PD_CTRL_SOFT_RESET	13
#define PD_DATA_SRC_CAP		1
#define PD_DATA_REQUEST		2
#define PD_DATA_SINK_CAP	4
#define PD_DATA_VDM		15
#define PD_SRC_PDO		0x3601905a	/* fixed 5 V 900 mA, DRP, USB comm, DRD */
#define PD_SNK_PDO		0x2601912c	/* fixed 5 V 3 A */

#define VDM_HDR(cmd, pos, svid)	((u32)(svid) << 16 | BIT(15) | (pos) << 8 | (cmd))
#define SVID_PD			0xff00
#define SVID_DP			0xff01
#define VDM_DISC_IDENT		1
#define VDM_DISC_SVID		2
#define VDM_DISC_MODES		3
#define VDM_ENTER		4
#define VDM_ATTENTION		6
#define VDM_DP_STATUS		16
#define VDM_DP_CONFIG		17

#define MSG_Q			8

static unsigned int poll_ms = 2000;
module_param(poll_ms, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(poll_ms, "fallback register poll period while idle, 0 = IRQ only");

static bool dp_alt_mode = true;
module_param(dp_alt_mode, bool, S_IRUGO | S_IWUSR);

struct pd_msg {
	int len;
	u8 b[32];
};

enum uc120_state {
	ST_DETACHED,
	ST_SINK,		/* we are UFP, charging */
	ST_SOURCE,		/* we are DFP, no PD partner (yet) */
	ST_PD_READY,		/* PD contract */
	ST_DP,			/* DP mode configured */
};

static const char * const state_names[] = {
	"detached", "sink", "source", "pd-ready", "displayport",
};

struct uc120 {
	struct spi_device *spi;
	struct device *dev;
	struct clk *xtal;
	struct regulator *vconn;
	struct power_supply *usb_psy;
	int irq;
	int gpio_pol, gpio_amsel, gpio_en;
	u8 cal[10];

	struct task_struct *thread;
	wait_queue_head_t wq;
	bool irq_pending;
	struct mutex lock;		/* SPI and status */
	u8 *txbuf, *rxbuf;

	u8 ctrl, role;
	enum uc120_state state;
	int orient;			/* 1 = CC1, 2 = CC2 */
	int rp;				/* 1 default, 2 1.5 A, 3 3 A */
	int tc_event;			/* pending Type-C event, 0 none */
	int tx_status;
	int tx_id;
	struct pd_msg q[MSG_Q];
	int qh, qt;
	bool vconn_on, vbus_on, hpd, cable;
	int pin;
	int hard_resets;
	u32 partner_id;
	u32 dp_mode_vdo;
	int default_cc_ma;
};

/* ---- SPI ---- */

static int uc120_xfer(struct uc120 *uc, int len)
{
	struct spi_transfer t = {
		.tx_buf = uc->txbuf,
		.rx_buf = uc->rxbuf,
		.len = len,
	};
	struct spi_message m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(uc->spi, &m);
}

static int uc120_read(struct uc120 *uc, u8 reg)
{
	int rc;

	memset(uc->txbuf, 0, 3);
	uc->txbuf[0] = reg << 3;
	rc = uc120_xfer(uc, 3);
	return rc ? rc : uc->rxbuf[2];
}

static int uc120_write(struct uc120 *uc, u8 reg, u8 val)
{
	uc->txbuf[0] = reg << 3 | 1;
	uc->txbuf[1] = val;
	return uc120_xfer(uc, 2);
}

static int uc120_read_fifo(struct uc120 *uc, u8 *buf, int len)
{
	int rc;

	memset(uc->txbuf, 0, len + 2);
	uc->txbuf[0] = UC120_RX_FIFO << 3;
	rc = uc120_xfer(uc, len + 2);
	if (!rc)
		memcpy(buf, uc->rxbuf + 2, len);
	return rc;
}

static int uc120_write_fifo(struct uc120 *uc, const u8 *buf, int len)
{
	uc->txbuf[0] = UC120_TX_FIFO << 3 | 1;
	memcpy(uc->txbuf + 1, buf, len);
	return uc120_xfer(uc, len + 1);
}

static void uc120_set_ctrl(struct uc120 *uc, u8 set, u8 clr)
{
	uc->ctrl = (uc->ctrl & ~clr) | set;
	uc120_write(uc, UC120_CTRL, uc->ctrl);
}

static void uc120_set_role(struct uc120 *uc, u8 set, u8 clr)
{
	int v = uc120_read(uc, UC120_ROLE);

	uc->role = ((v < 0 ? uc->role : v) & ~clr) | set;
	uc120_write(uc, UC120_ROLE, uc->role);
}

/* ---- hardware around the PHY ---- */

static struct power_supply *uc120_usb_psy(struct uc120 *uc)
{
	if (!uc->usb_psy)
		uc->usb_psy = power_supply_get_by_name("usb");
	return uc->usb_psy;
}

static void uc120_set_input_current(struct uc120 *uc, int ma)
{
	struct power_supply *psy = uc120_usb_psy(uc);
	union power_supply_propval v = { .intval = ma };

	if (!psy || !psy->set_property)
		return;
	if (uc->default_cc_ma < 0) {
		union power_supply_propval d;

		if (!psy->get_property(psy,
				POWER_SUPPLY_PROP_INPUT_CURRENT_MAX, &d))
			uc->default_cc_ma = d.intval;
	}
	psy->set_property(psy, POWER_SUPPLY_PROP_INPUT_CURRENT_MAX, &v);
	dev_info(uc->dev, "input current limit %d mA\n", ma);
}

static void uc120_set_vbus(struct uc120 *uc, bool on)
{
	struct power_supply *psy = uc120_usb_psy(uc);
	union power_supply_propval v = { .intval = on };

	if (uc->vbus_on == on || !psy || !psy->set_property)
		return;
	/* USB host mode; dwc3 turns the OTG boost (VBUS) on with it */
	psy->set_property(psy, POWER_SUPPLY_PROP_USB_OTG, &v);
	uc->vbus_on = on;
}

static void uc120_set_vconn(struct uc120 *uc, bool on)
{
	if (uc->vconn_on == on)
		return;
	if (on) {
		if (uc->vconn && regulator_enable(uc->vconn))
			dev_err(uc->dev, "VCONN supply enable failed\n");
		uc120_set_role(uc, ROLE_VCONN | ROLE_SOURCE, 0);
	} else {
		uc120_set_role(uc, 0, ROLE_VCONN);
		if (uc->vconn)
			regulator_disable(uc->vconn);
	}
	uc->vconn_on = on;
}

enum { MUX_OFF, MUX_USB, MUX_DP };

static void uc120_mux(struct uc120 *uc, int mode)
{
	if (!gpio_is_valid(uc->gpio_en))
		return;
	/* the switch: EN, POL = orientation, AMSEL high = USB only, low = DP */
	gpio_set_value_cansleep(uc->gpio_pol, mode ? uc->orient == 2 : 0);
	gpio_set_value_cansleep(uc->gpio_amsel, mode == MUX_USB);
	gpio_set_value_cansleep(uc->gpio_en, mode != MUX_OFF);
	dev_dbg(uc->dev, "mux %d orient %d\n", mode, uc->orient);
}

static void uc120_hpd(struct uc120 *uc, bool on)
{
	if (uc->hpd == on)
		return;
	uc->hpd = on;
	if (on)
		uc120_mux(uc, MUX_DP);
#ifdef CONFIG_SLIMPORT_ANX7816
	slimport_typec_hpd(on);
#endif
	dev_info(uc->dev, "DisplayPort HPD %s\n", on ? "high" : "low");
}

/* ---- PHY events and PD messages ---- */

static void uc120_poll(struct uc120 *uc)
{
	int s = uc120_read(uc, UC120_INT), s1, code, len, r7;

	if (s <= 0)
		return;
	if (s & 0x03) {
		s1 = uc120_read(uc, UC120_STATUS);
		code = s1 >> 5;
		len = s1 & 0x1f;
		if ((s & 2) && (code == 0 || code == 7) && len >= 2 &&
		    len <= 30) {
			struct pd_msg *m = &uc->q[uc->qt % MSG_Q];

			if (uc->qt - uc->qh < MSG_Q &&
			    !uc120_read_fifo(uc, m->b, len)) {
				m->len = len;
				uc->qt++;
			}
			if (code == 7)
				uc->tx_status = 6;
		}
		if ((s & 2) && code == 5) {
			dev_info(uc->dev, "hard reset received\n");
			uc->tc_event = 99;
		}
		if ((s & 2) && code == 6)
			uc->tx_status = 6;
		if ((s & 1) && uc->tx_status < 0)
			uc->tx_status = code;
	}
	if (s & 0xfc) {
		int ev = (s >> 2) & 0xf;

		r7 = uc120_read(uc, UC120_CONN);
		if (r7 >= 0) {
			uc->rp = (r7 >> 4) & 3;
			if (ev)
				uc->orient = (r7 & 0x40) ? 2 : 1;
		}
		if (ev)
			uc->tc_event = ev;
		else if ((s & 0xc0) && !uc->tc_event)
			uc->tc_event = 100;	/* Rp change only */
		dev_dbg(uc->dev, "int %02x conn %02x\n", s, r7);
	}
	uc120_write(uc, UC120_INT, 0xff);
}

static bool uc120_detached(struct uc120 *uc)
{
	return uc->tc_event == TC_DETACH || uc->tc_event == TC_DETACH2 ||
	       uc->tc_event == 99 || kthread_should_stop();
}

static int uc120_send(struct uc120 *uc, u16 hdr, const u32 *dos, int nd)
{
	u8 b[30];
	int len = 2 + 4 * nd, i, try;
	unsigned long end;

	hdr |= 0x0040 | 0x0020 | 0x0100 | (uc->tx_id << 9) | (nd << 12);
	b[0] = hdr;
	b[1] = hdr >> 8;
	for (i = 0; i < nd; i++) {
		b[2 + 4 * i] = dos[i];
		b[3 + 4 * i] = dos[i] >> 8;
		b[4 + 4 * i] = dos[i] >> 16;
		b[5 + 4 * i] = dos[i] >> 24;
	}
	for (try = 0; try < 3; try++) {
		uc->tx_status = -1;
		uc120_write_fifo(uc, b, len);
		uc120_write(uc, UC120_TX_CMD, len);
		end = jiffies + msecs_to_jiffies(25);
		while (uc->tx_status < 0 && time_before(jiffies, end)) {
			usleep_range(200, 400);
			uc120_poll(uc);
		}
		if (!uc->tx_status)
			break;
	}
	dev_dbg(uc->dev, "tx %04x %08x status %d\n", hdr, nd ? dos[0] : 0,
		uc->tx_status);
	if (uc->tx_status)
		return -EIO;
	uc->tx_id = (uc->tx_id + 1) & 7;
	return 0;
}

static int uc120_recv(struct uc120 *uc, struct pd_msg *m, int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);

	for (;;) {
		if (uc->qh != uc->qt) {
			*m = uc->q[uc->qh % MSG_Q];
			uc->qh++;
			dev_dbg(uc->dev, "rx %02x%02x len %d\n", m->b[1],
				m->b[0], m->len);
			return 1;
		}
		if (uc120_detached(uc))
			return -ENODEV;
		if (time_after(jiffies, end))
			return 0;
		usleep_range(300, 600);
		uc120_poll(uc);
	}
}

static inline u16 msg_hdr(const struct pd_msg *m)
{
	return m->b[0] | m->b[1] << 8;
}

static inline int msg_ndo(const struct pd_msg *m)
{
	int nd = (msg_hdr(m) >> 12) & 7;

	return min(nd, (m->len - 2) / 4);
}

static inline u32 msg_do(const struct pd_msg *m, int i)
{
	const u8 *d = m->b + 2 + 4 * i;

	return d[0] | d[1] << 8 | d[2] << 16 | (u32)d[3] << 24;
}

static bool msg_is(const struct pd_msg *m, int type, bool data)
{
	return (msg_hdr(m) & 0xf) == type && (msg_ndo(m) > 0) == data;
}

/* Structured VDM with ACK; BUSY is retried, NAK/no answer fail */
static int uc120_vdm(struct uc120 *uc, u32 vh, const u32 *vdo, int n,
		     struct pd_msg *ack)
{
	u32 d[7];
	int i, rc, busy;

	for (busy = 0; busy < 10; busy++) {
		d[0] = vh;
		for (i = 0; i < n; i++)
			d[1 + i] = vdo[i];
		if (uc120_send(uc, PD_DATA_VDM, d, 1 + n))
			return -EIO;
		for (;;) {
			rc = uc120_recv(uc, ack, 200);
			if (rc <= 0) {
				dev_info(uc->dev, "no answer to VDM %d\n",
					 vh & 0x1f);
				return rc ? rc : -ETIMEDOUT;
			}
			if (msg_is(ack, PD_DATA_VDM, true) &&
			    (msg_do(ack, 0) & 0x1f) == (vh & 0x1f))
				break;
			dev_dbg(uc->dev, "skipped message %04x\n",
				msg_hdr(ack));
		}
		switch ((msg_do(ack, 0) >> 6) & 3) {
		case 1:
			return 0;
		case 2:
			dev_info(uc->dev, "VDM %d NAK\n", vh & 0x1f);
			return -EOPNOTSUPP;
		}
		msleep(100);
	}
	return -EBUSY;
}

static int uc120_dp_configure(struct uc120 *uc)
{
	struct pd_msg m;
	u32 cfg = (u32)uc->pin << 8 | 0x06;	/* UFP_U as UFP_D, DP 1.3 */
	int rc = uc120_vdm(uc, VDM_HDR(VDM_DP_CONFIG, 1, SVID_DP), &cfg, 1, &m);

	if (!rc)
		uc->state = ST_DP;
	return rc;
}

static int uc120_dp_alt_mode(struct uc120 *uc)
{
	struct pd_msg m;
	u32 st = 1, mv;
	int i, nd, b0, b12, rc;
	bool found = false;

	rc = uc120_vdm(uc, VDM_HDR(VDM_DISC_IDENT, 0, SVID_PD), NULL, 0, &m);
	if (rc)
		return rc;
	uc->partner_id = msg_ndo(&m) > 1 ? msg_do(&m, 1) : 0;
	if (!(uc->partner_id & BIT(26)))
		return -EOPNOTSUPP;

	rc = uc120_vdm(uc, VDM_HDR(VDM_DISC_SVID, 0, SVID_PD), NULL, 0, &m);
	if (rc)
		return rc;
	nd = msg_ndo(&m);
	for (i = 1; i < nd; i++)
		if ((msg_do(&m, i) >> 16) == SVID_DP ||
		    (msg_do(&m, i) & 0xffff) == SVID_DP)
			found = true;
	if (!found)
		return -EOPNOTSUPP;

	rc = uc120_vdm(uc, VDM_HDR(VDM_DISC_MODES, 0, SVID_DP), NULL, 0, &m);
	if (rc)
		return rc;
	if (msg_ndo(&m) < 2)
		return -EPROTO;
	mv = uc->dp_mode_vdo = msg_do(&m, 1);
	b0 = mv & 0xff;
	b12 = ((mv >> 8) | (mv >> 16)) & 0xff;
	if ((b0 & 3) == 2 || !(b0 & 1))
		return -EOPNOTSUPP;	/* not UFP_D capable */
	/* as Windows: pin assignment E, then D, F, C */
	if (b12 & 0x10)
		uc->pin = 0x10;
	else if (b12 & 0x08)
		uc->pin = 0x08;
	else if ((mv >> 8) & 0x20)
		uc->pin = 0x20;
	else if (b12 & 0x04)
		uc->pin = 0x04;
	else
		return -EOPNOTSUPP;

	rc = uc120_vdm(uc, VDM_HDR(VDM_ENTER, 1, SVID_DP), NULL, 0, &m);
	if (rc)
		return rc;
	rc = uc120_vdm(uc, VDM_HDR(VDM_DP_STATUS, 1, SVID_DP), &st, 1, &m);
	if (rc)
		return rc;
	if (msg_ndo(&m) > 1 && (msg_do(&m, 1) & (BIT(5) | BIT(6))))
		return -EOPNOTSUPP;	/* USB or exit requested */
	/* configure even with HPD low, the sink sends Attention later */
	rc = uc120_dp_configure(uc);
	if (rc)
		return rc;
	if (msg_ndo(&m) > 1 && (msg_do(&m, 1) & BIT(7)))
		uc120_hpd(uc, true);
	dev_info(uc->dev, "DisplayPort mode, pin %c, partner %08x mode %08x\n",
		 uc->pin == 0x10 ? 'E' : uc->pin == 0x08 ? 'D' :
		 uc->pin == 0x20 ? 'F' : 'C', uc->partner_id, mv);
	return 0;
}

/* source contract: Source_Capabilities until GoodCRC, then Request */
static int uc120_source_contract(struct uc120 *uc)
{
	u32 pdo = PD_SRC_PDO;
	struct pd_msg m;
	int i, rc;

	for (i = 0; i < 50; i++) {
		uc->tx_id = 0;
		if (!uc120_send(uc, PD_DATA_SRC_CAP, &pdo, 1))
			break;
		if (uc120_detached(uc))
			return -ENODEV;
		msleep(150);
		uc120_poll(uc);
	}
	if (i == 50)
		return -ENODEV;	/* not a PD sink */
	for (;;) {
		rc = uc120_recv(uc, &m, 500);
		if (rc <= 0)
			return rc ? rc : -ETIMEDOUT;
		if (msg_is(&m, PD_DATA_REQUEST, true))
			break;
	}
	if (((msg_do(&m, 0) >> 28) & 7) != 1) {
		uc120_send(uc, PD_CTRL_REJECT, NULL, 0);
		return -EPROTO;
	}
	uc120_send(uc, PD_CTRL_ACCEPT, NULL, 0);
	msleep(30);
	uc120_send(uc, PD_CTRL_PS_RDY, NULL, 0);
	uc->state = ST_PD_READY;
	dev_info(uc->dev, "PD contract 5 V, request %08x\n", msg_do(&m, 0));
	return 0;
}

static void uc120_detach(struct uc120 *uc)
{
	uc120_hpd(uc, false);
	uc120_mux(uc, MUX_OFF);
	uc120_set_ctrl(uc, CTRL_UNATTACHED, CTRL_PD_EN);
	uc120_set_vbus(uc, false);
	uc120_set_vconn(uc, false);
	uc120_set_role(uc, 0, ROLE_SOURCE | ROLE_DFP | ROLE_VBUS_IN);
	if (uc->state == ST_SINK && uc->default_cc_ma >= 0)
		uc120_set_input_current(uc, uc->default_cc_ma);
	uc->state = ST_DETACHED;
	uc->qh = uc->qt = 0;
	uc->pin = 0;
	uc->partner_id = 0;
	uc->hard_resets = 0;
	dev_info(uc->dev, "detached\n");
}

/* PD contract and DisplayPort mode; hard reset restarts it up to 3 times */
static void uc120_negotiate(struct uc120 *uc)
{
	int rc;

	for (;;) {
		uc->qh = uc->qt = 0;
		uc->state = ST_SOURCE;
		rc = uc120_source_contract(uc);
		if (rc != -ENODEV || uc->tc_event != 99 ||
		    uc->hard_resets++ >= 3)
			break;
		/* hard reset from the sink: VBUS off 660 ms, on, again */
		uc->tc_event = 0;
		uc120_set_vbus(uc, false);
		msleep(660);
		uc120_set_vbus(uc, true);
		msleep(200);
	}
	if (rc) {
		if (!uc120_detached(uc))
			dev_info(uc->dev, "no PD contract (%d), USB only\n", rc);
		return;
	}
	if (dp_alt_mode && uc->cable) {
		rc = uc120_dp_alt_mode(uc);
		if (rc && rc != -ENODEV)
			dev_info(uc->dev, "no DisplayPort mode (%d)\n", rc);
	}
}

static void uc120_attach_source(struct uc120 *uc, bool cable)
{
	dev_info(uc->dev, "sink attached on CC%d%s\n", uc->orient,
		 cable ? ", VCONN requested" : "");
	uc->state = ST_SOURCE;
	uc->cable = cable;
	/* tyc InitializeDfpRole: mux USB, VCONN, 100 ms, VBUS, 100 ms */
	uc120_set_ctrl(uc, 0, CTRL_UNATTACHED);
	uc120_mux(uc, MUX_USB);
	if (cable)
		uc120_set_vconn(uc, true);
	msleep(100);
	uc120_set_vbus(uc, true);
	msleep(100);
	uc120_set_role(uc, ROLE_SOURCE | ROLE_DFP, ROLE_VBUS_IN);
	uc120_set_ctrl(uc, CTRL_PD_EN, 0);
	msleep(200);	/* VBUS ramp */
	uc120_negotiate(uc);
}

static void uc120_attach_sink(struct uc120 *uc)
{
	static const int ma[] = { 0, 0, 1500, 3000 };

	uc->state = ST_SINK;
	uc120_set_ctrl(uc, 0, CTRL_UNATTACHED);
	uc120_mux(uc, MUX_USB);
	dev_info(uc->dev, "source attached on CC%d, Rp %d\n", uc->orient,
		 uc->rp);
	uc120_set_input_current(uc, ma[uc->rp & 3]);
}

/* messages while a contract is up */
static void uc120_handle_msg(struct uc120 *uc, struct pd_msg *m)
{
	u32 pdo;

	if (msg_is(m, PD_CTRL_SOFT_RESET, false)) {
		uc->tx_id = 0;
		uc120_send(uc, PD_CTRL_ACCEPT, NULL, 0);
	} else if (msg_is(m, PD_CTRL_GET_SRC_CAP, false)) {
		pdo = PD_SRC_PDO;
		uc120_send(uc, PD_DATA_SRC_CAP, &pdo, 1);
	} else if (msg_is(m, PD_CTRL_GET_SINK_CAP, false)) {
		pdo = PD_SNK_PDO;
		uc120_send(uc, PD_DATA_SINK_CAP, &pdo, 1);
	} else if (msg_is(m, PD_DATA_REQUEST, true)) {
		uc120_send(uc, PD_CTRL_ACCEPT, NULL, 0);
		msleep(30);
		uc120_send(uc, PD_CTRL_PS_RDY, NULL, 0);
	} else if (msg_is(m, PD_DATA_VDM, true) &&
		   (msg_do(m, 0) & 0x1f) == VDM_ATTENTION &&
		   (msg_do(m, 0) >> 16) == SVID_DP && msg_ndo(m) > 1) {
		u32 st = msg_do(m, 1);
		bool hpd = st & BIT(7);

		dev_info(uc->dev, "DP attention %08x\n", st);
		if (st & (BIT(5) | BIT(6)))
			uc120_hpd(uc, false);
		else if (hpd && !uc->hpd) {
			struct pd_msg ack;
			u32 cfg = (u32)uc->pin << 8 | 0x06;

			if (!uc120_vdm(uc, VDM_HDR(VDM_DP_CONFIG, 1, SVID_DP),
				       &cfg, 1, &ack))
				uc120_hpd(uc, true);
		} else if (!hpd)
			uc120_hpd(uc, false);
	} else {
		dev_dbg(uc->dev, "ignored message %04x\n", msg_hdr(m));
	}
}

static void uc120_handle(struct uc120 *uc)
{
	struct pd_msg m;
	int ev = uc->tc_event;

	uc->tc_event = 0;
	switch (ev) {
	case TC_DETACH:
	case TC_DETACH2:
		if (uc->state != ST_DETACHED)
			uc120_detach(uc);
		break;
	case TC_DFP_UFP:
	case TC_DFP_CABLE_UFP:
		if (uc->state == ST_DETACHED)
			uc120_attach_source(uc, ev == TC_DFP_CABLE_UFP);
		break;
	case TC_UFP_DFP:
		if (uc->state == ST_DETACHED)
			uc120_attach_sink(uc);
		break;
	case 99:	/* hard reset while a contract was up */
		if (uc->state >= ST_PD_READY) {
			uc120_hpd(uc, false);
			uc120_mux(uc, MUX_USB);
			uc->hard_resets = 0;
			uc120_set_vbus(uc, false);
			msleep(660);
			uc120_set_vbus(uc, true);
			msleep(200);
			uc120_negotiate(uc);
		}
		break;
	case 100:	/* Rp change */
		if (uc->state == ST_SINK)
			uc120_set_input_current(uc,
				uc->rp == 3 ? 3000 : uc->rp == 2 ? 1500 : 0);
		break;
	case 0:
		break;
	default:
		dev_info(uc->dev, "unhandled Type-C state %d\n", ev);
	}
	while (uc->state >= ST_PD_READY && uc->qh != uc->qt &&
	       uc120_recv(uc, &m, 0) > 0)
		uc120_handle_msg(uc, &m);
	if (uc->state < ST_PD_READY)
		uc->qh = uc->qt;
}

static int uc120_hw_init(struct uc120 *uc)
{
	static const u8 calreg[10] = { 18, 19, 20, 21, 26, 22, 23, 24, 25, 27 };
	int i, v;

	uc120_write(uc, UC120_CTRL, 0x06);
	uc120_write(uc, UC120_ROLE, ROLE_RESET | 0x08);
	uc120_write(uc, 13, 0x02);
	for (i = 0; i < 10; i++)
		uc120_write(uc, calreg[i], uc->cal[i]);
	uc120_write(uc, UC120_INT, 0xff);
	uc120_write(uc, UC120_INT_MASK, 0xff);
	uc->ctrl = 0x06 | CTRL_IRQ_EN;
	uc120_write(uc, UC120_CTRL, uc->ctrl);
	uc->role = 0x08;
	uc120_write(uc, UC120_ROLE, uc->role);
	v = uc120_read(uc, UC120_ROLE);
	if (v < 0 || (v & 0x18) != 0x08) {
		dev_err(uc->dev, "self test failed, role %02x\n", v);
		return -ENODEV;
	}
	return 0;
}

static int uc120_thread(void *data)
{
	struct uc120 *uc = data;

	set_freezable();
	while (!kthread_should_stop()) {
		long t = poll_ms ? msecs_to_jiffies(poll_ms) :
				   MAX_SCHEDULE_TIMEOUT;

		wait_event_freezable_timeout(uc->wq, uc->irq_pending ||
					     kthread_should_stop(), t);
		uc->irq_pending = false;
		mutex_lock(&uc->lock);
		uc120_poll(uc);
		while (uc->tc_event || uc->qh != uc->qt) {
			uc120_handle(uc);
			uc120_poll(uc);
			if (kthread_should_stop())
				break;
		}
		mutex_unlock(&uc->lock);
	}
	return 0;
}

static irqreturn_t uc120_irq(int irq, void *data)
{
	struct uc120 *uc = data;

	uc->irq_pending = true;
	wake_up(&uc->wq);
	return IRQ_HANDLED;
}

static ssize_t uc120_status_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct uc120 *uc = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE,
		"state=%s orient=CC%d rp=%d vbus=%d vconn=%d hpd=%d pin=%02x partner=%08x mode=%08x\n",
		state_names[uc->state], uc->orient, uc->rp, uc->vbus_on,
		uc->vconn_on, uc->hpd, uc->pin, uc->partner_id,
		uc->dp_mode_vdo);
}
static DEVICE_ATTR(status, S_IRUGO, uc120_status_show, NULL);

static int uc120_gpio(struct uc120 *uc, const char *name, int val)
{
	int gpio = of_get_named_gpio(uc->dev->of_node, name, 0);

	if (!gpio_is_valid(gpio))
		return gpio;
	if (devm_gpio_request_one(uc->dev, gpio, val ? GPIOF_OUT_INIT_HIGH :
				  GPIOF_OUT_INIT_LOW, name))
		return -EBUSY;
	return gpio;
}

static int uc120_probe(struct spi_device *spi)
{
	static const u8 nominal[10] = { 0x0d, 0x7d, 0x32, 0x5f, 0x9e,
					0x0d, 0x7d, 0x32, 0x5f, 0x9e };
	struct device_node *np = spi->dev.of_node;
	struct uc120 *uc;
	int rc, gpio;

	uc = devm_kzalloc(&spi->dev, sizeof(*uc), GFP_KERNEL);
	if (!uc)
		return -ENOMEM;
	uc->txbuf = devm_kzalloc(&spi->dev, 40, GFP_KERNEL | GFP_DMA);
	uc->rxbuf = devm_kzalloc(&spi->dev, 40, GFP_KERNEL | GFP_DMA);
	if (!uc->txbuf || !uc->rxbuf)
		return -ENOMEM;
	uc->spi = spi;
	uc->dev = &spi->dev;
	uc->default_cc_ma = -1;
	uc->orient = 1;
	mutex_init(&uc->lock);
	init_waitqueue_head(&uc->wq);
	spi_set_drvdata(spi, uc);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	rc = spi_setup(spi);
	if (rc)
		return rc;

	if (of_property_read_u8_array(np, "microsoft,calibration", uc->cal,
				      sizeof(uc->cal)))
		memcpy(uc->cal, nominal, sizeof(uc->cal));

	uc->xtal = devm_clk_get(uc->dev, "xtal");
	if (IS_ERR(uc->xtal)) {
		if (PTR_ERR(uc->xtal) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		uc->xtal = NULL;
	} else {
		rc = clk_prepare_enable(uc->xtal);
		if (rc)
			return rc;
	}

	uc->vconn = devm_regulator_get(uc->dev, "vconn");
	if (IS_ERR(uc->vconn)) {
		if (PTR_ERR(uc->vconn) == -EPROBE_DEFER) {
			rc = -EPROBE_DEFER;
			goto err_clk;
		}
		uc->vconn = NULL;
	}

	uc->gpio_pol = uc120_gpio(uc, "microsoft,mux-pol-gpio", 0);
	uc->gpio_amsel = uc120_gpio(uc, "microsoft,mux-amsel-gpio", 0);
	uc->gpio_en = uc120_gpio(uc, "microsoft,mux-en-gpio", 0);
	if (!gpio_is_valid(uc->gpio_pol) || !gpio_is_valid(uc->gpio_amsel))
		uc->gpio_en = -EINVAL;

	mutex_lock(&uc->lock);
	rc = uc120_hw_init(uc);
	mutex_unlock(&uc->lock);
	if (rc)
		goto err_clk;

	gpio = of_get_named_gpio(np, "microsoft,irq-gpio", 0);
	if (gpio_is_valid(gpio) && !devm_gpio_request_one(uc->dev, gpio,
						GPIOF_IN, "uc120-irq")) {
		uc->irq = gpio_to_irq(gpio);
		rc = devm_request_irq(uc->dev, uc->irq, uc120_irq,
				      IRQF_TRIGGER_FALLING, "uc120", uc);
		if (rc) {
			dev_warn(uc->dev, "no IRQ (%d), polling\n", rc);
			uc->irq = 0;
		} else {
			enable_irq_wake(uc->irq);
		}
	}

	device_create_file(uc->dev, &dev_attr_status);

	uc->thread = kthread_run(uc120_thread, uc, "uc120");
	if (IS_ERR(uc->thread)) {
		rc = PTR_ERR(uc->thread);
		goto err_file;
	}
	dev_info(uc->dev, "UC120 Type-C PHY ready, irq %d\n", uc->irq);
	return 0;

err_file:
	device_remove_file(uc->dev, &dev_attr_status);
err_clk:
	if (uc->xtal)
		clk_disable_unprepare(uc->xtal);
	return rc;
}

static int uc120_remove(struct spi_device *spi)
{
	struct uc120 *uc = spi_get_drvdata(spi);

	kthread_stop(uc->thread);
	mutex_lock(&uc->lock);
	uc->tc_event = 0;
	if (uc->state != ST_DETACHED)
		uc120_detach(uc);
	uc120_set_ctrl(uc, 0, CTRL_IRQ_EN);
	mutex_unlock(&uc->lock);
	device_remove_file(uc->dev, &dev_attr_status);
	if (uc->xtal)
		clk_disable_unprepare(uc->xtal);
	return 0;
}

static const struct of_device_id uc120_of_match[] = {
	{ .compatible = "microsoft,uc120" },
	{ }
};
MODULE_DEVICE_TABLE(of, uc120_of_match);

static struct spi_driver uc120_driver = {
	.driver = {
		.name = "uc120",
		.owner = THIS_MODULE,
		.of_match_table = uc120_of_match,
	},
	.probe = uc120_probe,
	.remove = uc120_remove,
};
module_spi_driver(uc120_driver);

MODULE_DESCRIPTION("Microsoft UC120 USB Type-C PD PHY");
MODULE_LICENSE("GPL v2");
