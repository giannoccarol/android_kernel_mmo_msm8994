/*
 * Scanner CCI per il Lumia 950 XL (cityman).
 *
 * Apre il subdev msm_sensor_init e manda VIDIOC_MSM_SENSOR_INIT_CFG
 * con la sequenza di potenza del DTS cityman. L'ID SMIA non torna
 * nell'ioctl: msm_sensor_init_subdev_ioctl restituisce sempre 0, e il
 * valore sta nel log di msm_sensor_match_id. Questo binario lo legge
 * da /dev/kmsg.
 *
 * slave_addr e' a 8 bit: il kernel fa sid = slave_addr >> 1.
 * L'ID atteso e' 0, cosi' un ACK non blocca lo slot (is_probe_succeed).
 * Va eseguito sul kernel con il DTB cityman e la patch che stampa l'ID.
 * Non scrive nulla sul sensore: la lettura e' una WORD a 0x0000.
 */
#include <media/msm_camsensor_sdk.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int i32;

enum sensor_sub_module_t {
	SUB_MODULE_SENSOR,
	SUB_MODULE_CHROMATIX,
	SUB_MODULE_ACTUATOR,
	SUB_MODULE_EEPROM,
	SUB_MODULE_LED_FLASH,
	SUB_MODULE_STROBE_FLASH,
	SUB_MODULE_CSID,
	SUB_MODULE_CSID_3D,
	SUB_MODULE_CSIPHY,
	SUB_MODULE_CSIPHY_3D,
	SUB_MODULE_OIS,
	SUB_MODULE_MAX
};

struct msm_sensor_info_t {
	char sensor_name[32];
	u32 session_id;
	i32 subdev_id[SUB_MODULE_MAX];
	i32 subdev_intf[SUB_MODULE_MAX];
	u8 is_mount_angle_valid;
	u32 sensor_mount_angle;
	int modes_supported;
	enum camb_position_t position;
};

enum msm_sensor_init_cfg_type_t {
	CFG_SINIT_PROBE,
	CFG_SINIT_PROBE_DONE,
	CFG_SINIT_PROBE_WAIT_DONE
};

struct sensor_init_cfg_data {
	enum msm_sensor_init_cfg_type_t cfgtype;
	struct msm_sensor_info_t probed_info;
	char entity_name[32];
	union {
		void *setting;
	} cfg;
};

#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT 8
#define _IOC_SIZESHIFT 16
#define _IOC_DIRSHIFT 30
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOWR(type, nr, size) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))

#define BASE_VIDIOC_PRIVATE 192
#define VIDIOC_MSM_SENSOR_INIT_CFG \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 10, struct sensor_init_cfg_data)

typedef char assert_ps[(sizeof(struct msm_sensor_power_setting) == 104) ? 1 : -1];
typedef char assert_init[(sizeof(struct sensor_init_cfg_data) == 184) ? 1 : -1];

#define SYS_ioctl 29
#define SYS_openat 56
#define SYS_close 57
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_exit_group 94
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_RDWR 2
#define O_NONBLOCK 04000
#define SEEK_END 2

struct step {
	int type;
	int val;
	long cfg;
	int delay;
};

static const struct step rear_up[] = {
	{ SENSOR_VREG, CAM_VAF, 1, 1 },
	{ SENSOR_VREG, CAM_VANA, 1, 1 },
	{ SENSOR_VREG, CAM_VDIG, 1, 1 },
	{ SENSOR_VREG, CAM_VIO, 1, 1 },
	{ SENSOR_GPIO, SENSOR_GPIO_RESET, 1, 30 },
	{ SENSOR_GPIO, SENSOR_GPIO_STANDBY, 1, 30 },
	{ SENSOR_CLK, SENSOR_CAM_MCLK, 9600000, 5 },
};

static const struct step front_up[] = {
	{ SENSOR_VREG, CAM_VANA, 1, 1 },
	{ SENSOR_VREG, CAM_VIO, 1, 1 },
	{ SENSOR_GPIO, SENSOR_GPIO_RESET, 1, 30 },
	{ SENSOR_GPIO, SENSOR_GPIO_STANDBY, 1, 30 },
	{ SENSOR_CLK, SENSOR_CAM_MCLK, 9600000, 5 },
};

/* 8 bit, candidati prima. Toshiba ADRSEL: 0x20 e 0x6C. */
static const u16 candidates[] = {
	0x20, 0x6c, 0x34, 0x36, 0x6e, 0x10, 0x18, 0x1a,
	0x50, 0x5a, 0x42, 0x90, 0xa0, 0xb0
};

static long sc(long n, long a, long b, long c, long d)
{
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	register long x8 asm("x8") = n;

	asm volatile("svc #0"
		: "+r"(x0)
		: "r"(x1), "r"(x2), "r"(x3), "r"(x8)
		: "memory", "cc");
	return x0;
}

static int slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static void wr(const char *s)
{
	sc(SYS_write, 1, (long)s, slen(s), 0);
}

static void wrn(const char *s)
{
	wr(s);
	wr("\n");
}

static int eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int starts(const char *s, const char *p)
{
	while (*p) {
		if (*s++ != *p++)
			return 0;
	}
	return 1;
}

static void put_hex16(unsigned v)
{
	char b[8];
	int i;
	const char *h = "0123456789abcdef";

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 4; i++)
		b[2 + i] = h[(v >> ((3 - i) * 4)) & 0xf];
	b[6] = 0;
	wr(b);
}

static int parse_u32(const char *s, u32 *out)
{
	u32 v = 0;
	int hex = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		hex = 1;
		s += 2;
	}
	if (!*s)
		return -1;
	while (*s) {
		char c = *s++;
		u32 d;

		if (c >= '0' && c <= '9')
			d = (u32)(c - '0');
		else if (hex && c >= 'a' && c <= 'f')
			d = (u32)(c - 'a' + 10);
		else if (hex && c >= 'A' && c <= 'F')
			d = (u32)(c - 'A' + 10);
		else
			return -1;
		v = hex ? (v << 4) | d : v * 10u + d;
	}
	*out = v;
	return 0;
}

static void zero(void *p, int n)
{
	char *c = p;

	while (n--)
		*c++ = 0;
}

static int find_subdev(char *path, int path_max)
{
	int i;

	for (i = 0; i < 64; i++) {
		char sys[64];
		char name[64];
		int fd, n, k, d;

		/* /sys/class/video4linux/v4l-subdevN/name */
		const char *pre = "/sys/class/video4linux/v4l-subdev";
		k = 0;
		while (pre[k]) {
			sys[k] = pre[k];
			k++;
		}
		d = i;
		if (d >= 10) {
			sys[k++] = (char)('0' + d / 10);
			d %= 10;
		}
		sys[k++] = (char)('0' + d);
		sys[k++] = '/';
		sys[k++] = 'n';
		sys[k++] = 'a';
		sys[k++] = 'm';
		sys[k++] = 'e';
		sys[k] = 0;

		fd = (int)sc(SYS_openat, AT_FDCWD, (long)sys, O_RDONLY, 0);
		if (fd < 0)
			continue;
		n = (int)sc(SYS_read, fd, (long)name, sizeof(name) - 1, 0);
		sc(SYS_close, fd, 0, 0, 0);
		if (n <= 0)
			continue;
		name[n] = 0;
		while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r'))
			name[--n] = 0;
		if (!eq(name, "msm_sensor_init"))
			continue;

		pre = "/dev/v4l-subdev";
		k = 0;
		while (pre[k] && k < path_max - 4) {
			path[k] = pre[k];
			k++;
		}
		d = i;
		if (d >= 10)
			path[k++] = (char)('0' + d / 10);
		path[k++] = (char)('0' + (d % 10));
		path[k] = 0;
		return 0;
	}
	return -1;
}

static const char *find_tag(const char *buf, int n, const char *tag)
{
	int tlen = slen(tag);
	int i;

	for (i = 0; i + tlen <= n; i++) {
		int j;

		for (j = 0; j < tlen; j++) {
			if (buf[i + j] != tag[j])
				break;
		}
		if (j == tlen)
			return buf + i;
	}
	return 0;
}

static int hex_byte(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Legge l'intero dopo "0x" puntato da p. */
static int parse_hex_at(const char *p, u32 *out)
{
	u32 v = 0;
	int digits = 0;

	if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X'))
		return -1;
	p += 2;
	while (digits < 8) {
		int d = hex_byte(*p);

		if (d < 0)
			break;
		v = (v << 4) | (u32)d;
		p++;
		digits++;
	}
	if (!digits)
		return -1;
	*out = v;
	return 0;
}

enum probe_kind {
	PROBE_NACK,
	PROBE_ACK,
	PROBE_ACK_OPAQUE,
	PROBE_POWER,
	PROBE_LOCKED,
	PROBE_BADSLOT,
	PROBE_SILENT
};

struct probe_result {
	enum probe_kind kind;
	u32 id;
};

static void kmsg_drain(int kfd)
{
	char buf[512];

	if (kfd < 0)
		return;
	sc(SYS_lseek, kfd, 0, SEEK_END, 0);
	while (sc(SYS_read, kfd, (long)buf, sizeof(buf), 0) > 0)
		;
}

static void kmsg_collect(int kfd, char *buf, int cap, int *len)
{
	int n = 0;

	if (kfd < 0) {
		*len = 0;
		return;
	}
	while (n < cap - 1) {
		long r = sc(SYS_read, kfd, (long)(buf + n), cap - 1 - n, 0);

		if (r <= 0)
			break;
		n += (int)r;
	}
	buf[n] = 0;
	*len = n;
}

static void classify(const char *buf, int n, struct probe_result *r)
{
	const char *id;
	const char *p;

	zero(r, sizeof(*r));
	r->kind = PROBE_SILENT;
	if (!buf || n <= 0)
		return;

	id = find_tag(buf, n, "read id 0x");
	if (id && parse_hex_at(id + slen("read id "), &r->id) == 0) {
		r->kind = PROBE_ACK;
		return;
	}
	if (find_tag(buf, n, "read id failed")) {
		r->kind = PROBE_NACK;
		return;
	}
	if (find_tag(buf, n, "chip id doesnot match")) {
		r->kind = PROBE_ACK_OPAQUE;
		return;
	}
	if (find_tag(buf, n, "already probed") ||
	    find_tag(buf, n, "probe succeeded")) {
		r->kind = PROBE_LOCKED;
		return;
	}
	p = find_tag(buf, n, "invalid camera id");
	if (!p)
		p = find_tag(buf, n, "failed: s_ctrl");
	if (p) {
		r->kind = PROBE_BADSLOT;
		return;
	}
	if (find_tag(buf, n, "power up failed") ||
	    find_tag(buf, n, "set voltage failed") ||
	    find_tag(buf, n, "request gpio") ||
	    find_tag(buf, n, "cci_init failed")) {
		r->kind = PROBE_POWER;
		return;
	}
}

static int do_probe(int fd, int kfd, int camera, u16 addr8, int word,
		    long mclk, u16 reg, const struct step *seq, int nseq,
		    struct probe_result *r)
{
	struct msm_camera_sensor_slave_info info;
	struct msm_sensor_power_setting up[12];
	struct sensor_init_cfg_data cfg;
	char buf[8192];
	int i, len;

	zero(&info, sizeof(info));
	zero(up, sizeof(up));
	zero(&cfg, sizeof(cfg));

	info.sensor_name[0] = 'c';
	info.sensor_name[1] = 'c';
	info.sensor_name[2] = 'i';
	info.camera_id = (enum msm_sensor_camera_id_t)camera;
	info.slave_addr = addr8;
	info.i2c_freq_mode = I2C_FAST_PLUS_MODE;
	info.addr_type = word ? MSM_CAMERA_I2C_WORD_ADDR : MSM_CAMERA_I2C_BYTE_ADDR;
	info.sensor_id_info.sensor_id_reg_addr = reg;
	info.sensor_id_info.sensor_id = 0x0000;
	info.sensor_id_info.sensor_id_mask = 0xffff;
	info.is_init_params_valid = 0;
	info.is_flash_supported = 0;
	info.power_setting_array.size = (unsigned short)nseq;
	info.power_setting_array.size_down = 0;
	info.power_setting_array.power_setting = up;
	info.power_setting_array.power_down_setting = 0;

	for (i = 0; i < nseq; i++) {
		up[i].seq_type = (enum msm_sensor_power_seq_type_t)seq[i].type;
		up[i].seq_val = (unsigned short)seq[i].val;
		up[i].config_val = seq[i].cfg;
		up[i].delay = (unsigned short)seq[i].delay;
		if (seq[i].type == SENSOR_CLK)
			up[i].config_val = mclk;
	}

	cfg.cfgtype = CFG_SINIT_PROBE;
	cfg.cfg.setting = &info;

	kmsg_drain(kfd);
	sc(SYS_ioctl, fd, VIDIOC_MSM_SENSOR_INIT_CFG, (long)&cfg, 0);
	kmsg_collect(kfd, buf, sizeof(buf), &len);
	classify(buf, len, r);
	return 0;
}

static void report(u16 addr, const struct probe_result *r)
{
	put_hex16(addr);
	wr("  ");
	switch (r->kind) {
	case PROBE_ACK:
		wr("ACK  id ");
		put_hex16(r->id);
		wrn("");
		break;
	case PROBE_NACK:
		wrn("nessun ACK");
		break;
	case PROBE_ACK_OPAQUE:
		wrn("ACK, ID non stampato (kernel senza la patch)");
		break;
	case PROBE_POWER:
		wrn("alimentazione o GPIO falliti");
		break;
	case PROBE_LOCKED:
		wrn("slot gia' occupato, serve un reboot");
		break;
	case PROBE_BADSLOT:
		wrn("camera_id assente nel DTB");
		break;
	default:
		wrn("nessun messaggio nel kmsg");
		break;
	}
}

static int already(const u16 *list, int n, u16 a)
{
	int i;

	for (i = 0; i < n; i++) {
		if (list[i] == a)
			return 1;
	}
	return 0;
}

static void usage(void)
{
	wrn("cci_probe <0|1> [mclk=Hz] [addr=0xNN] [byte] [candidates]");
	wrn("  0 posteriore (CSI0, L23/L29/L25/LVS1)   1 frontale (CSI2, L17/LVS1)");
	wrn("  addr e' a 8 bit. Default: scan completo, MCLK 9.6 MHz, registri a 16 bit.");
	wrn("  Prima: stop cameraserver; stop qcamerasvr. Dopo: start di entrambi.");
}

int main(int argc, char **argv)
{
	int camera = 0;
	int word = 1;
	int only_cand = 0;
	int have_addr = 0;
	int have_standby = 0;
	int have_reset = 0;
	long standby_val = 1;
	long reset_val = 1;
	u32 reg_arg = 0;
	u32 addr_arg = 0;
	long mclk = 9600000;
	const struct step *seq;
	int nseq;
	char path[32];
	int fd, kfd, i, acks;
	struct step rear_local[7];
	struct step front_local[5];

	if (argc < 2) {
		usage();
		return 2;
	}
	if (!eq(argv[1], "0") && !eq(argv[1], "1")) {
		usage();
		return 2;
	}
	camera = argv[1][0] - '0';

	for (i = 2; i < argc; i++) {
		if (starts(argv[i], "mclk=")) {
			u32 v;

			if (parse_u32(argv[i] + 5, &v)) {
				usage();
				return 2;
			}
			mclk = (long)v;
		} else if (starts(argv[i], "addr=")) {
			if (parse_u32(argv[i] + 5, &addr_arg) || addr_arg > 0xff ||
			    (addr_arg & 1)) {
				wrn("addr deve essere un indirizzo CCI a 8 bit, pari");
				return 2;
			}
			have_addr = 1;
		} else if (eq(argv[i], "byte")) {
			word = 0;
		} else if (eq(argv[i], "word")) {
			word = 1;
		} else if (eq(argv[i], "candidates")) {
			only_cand = 1;
		} else if (starts(argv[i], "reg=")) {
			if (parse_u32(argv[i] + 4, &reg_arg) || reg_arg > 0xffff) {
				usage();
				return 2;
			}
		} else if (starts(argv[i], "standby=")) {
			u32 v;

			if (parse_u32(argv[i] + 8, &v) || v > 1) {
				usage();
				return 2;
			}
			standby_val = (long)v;
			have_standby = 1;
		} else if (starts(argv[i], "reset=")) {
			u32 v;

			if (parse_u32(argv[i] + 6, &v) || v > 1) {
				usage();
				return 2;
			}
			reset_val = (long)v;
			have_reset = 1;
		} else {
			usage();
			return 2;
		}
	}

	for (i = 0; i < 7; i++)
		rear_local[i] = rear_up[i];
	for (i = 0; i < 5; i++)
		front_local[i] = front_up[i];
	for (i = 0; i < 7; i++) {
		if (have_standby && rear_local[i].type == SENSOR_GPIO &&
		    rear_local[i].val == SENSOR_GPIO_STANDBY)
			rear_local[i].cfg = standby_val;
		if (have_reset && rear_local[i].type == SENSOR_GPIO &&
		    rear_local[i].val == SENSOR_GPIO_RESET)
			rear_local[i].cfg = reset_val;
		if (i < 5 && have_standby && front_local[i].type == SENSOR_GPIO &&
		    front_local[i].val == SENSOR_GPIO_STANDBY)
			front_local[i].cfg = standby_val;
		if (i < 5 && have_reset && front_local[i].type == SENSOR_GPIO &&
		    front_local[i].val == SENSOR_GPIO_RESET)
			front_local[i].cfg = reset_val;
	}
	if (camera == 0) {
		seq = rear_local;
		nseq = 7;
		wr("posteriore");
	} else {
		seq = front_local;
		nseq = 5;
		wr("frontale");
	}
	wr("  mclk ");
	{
		char num[16];
		long v = mclk;
		int p = 0;
		char tmp[16];
		int t = 0;

		if (v == 0)
			tmp[t++] = '0';
		while (v) {
			tmp[t++] = (char)('0' + (v % 10));
			v /= 10;
		}
		while (t)
			num[p++] = tmp[--t];
		num[p] = 0;
		wr(num);
	}
	wr(word ? "  registri 16 bit\n" : "  registri 8 bit\n");

	if (find_subdev(path, sizeof(path))) {
		wrn("msm_sensor_init non trovato in /sys/class/video4linux");
		return 3;
	}
	wr("nodo ");
	wrn(path);

	fd = (int)sc(SYS_openat, AT_FDCWD, (long)path, O_RDWR, 0);
	if (fd < 0) {
		wrn("open del subdev fallita");
		return 3;
	}
	kfd = (int)sc(SYS_openat, AT_FDCWD, (long)"/dev/kmsg",
		      O_RDONLY | O_NONBLOCK, 0);
	if (kfd < 0)
		wrn("kmsg non apribile: il risultato va letto da dmesg");

	acks = 0;
	if (have_addr) {
		struct probe_result r;

		do_probe(fd, kfd, camera, (u16)addr_arg, word, mclk, (u16)reg_arg,
			 seq, nseq, &r);
		report((u16)addr_arg, &r);
		if (r.kind == PROBE_ACK || r.kind == PROBE_ACK_OPAQUE)
			acks++;
		if (r.kind == PROBE_POWER || r.kind == PROBE_BADSLOT ||
		    r.kind == PROBE_LOCKED)
			acks = -1;
	} else {
		u16 seen[14];
		int ns = (int)(sizeof(candidates) / sizeof(candidates[0]));
		int stop = 0;

		for (i = 0; i < ns; i++)
			seen[i] = candidates[i];
		for (i = 0; i < ns && !stop; i++) {
			struct probe_result r;

			do_probe(fd, kfd, camera, candidates[i], word, mclk,
				 (u16)reg_arg, seq, nseq, &r);
			report(candidates[i], &r);
			if (r.kind == PROBE_ACK || r.kind == PROBE_ACK_OPAQUE)
				acks++;
			if (r.kind == PROBE_POWER || r.kind == PROBE_BADSLOT ||
			    r.kind == PROBE_LOCKED)
				stop = 1;
		}
		if (!only_cand && !stop) {
			u16 a;

			for (a = 0x08; a <= 0xee; a = (u16)(a + 2)) {
				struct probe_result r;

				if (already(seen, ns, a))
					continue;
				do_probe(fd, kfd, camera, a, word, mclk, (u16)reg_arg,
					 seq, nseq, &r);
				report(a, &r);
				if (r.kind == PROBE_ACK || r.kind == PROBE_ACK_OPAQUE)
					acks++;
				if (r.kind == PROBE_POWER || r.kind == PROBE_BADSLOT ||
				    r.kind == PROBE_LOCKED)
					break;
			}
		}
	}

	sc(SYS_close, fd, 0, 0, 0);
	if (kfd >= 0)
		sc(SYS_close, kfd, 0, 0, 0);

	if (acks > 0) {
		wrn("fatto: almeno un ACK");
		return 0;
	}
	if (acks < 0)
		return 4;
	wrn("nessun ACK");
	return 1;
}
