/*
 * "Hill" -- the Lumia 950's triple-LED camera flash.
 *
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 * Register map recovered from this phone's own Windows Phone driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * Where this comes from
 * ---------------------
 * The phone's WP flash KMD (qccamflash8992.sys) is Nokia's
 * WhiteLEDFlashDriver, which supports six different flash parts named after
 * F1 drivers -- Hill, Prost, Hakkinen, Mansell, Rosberg -- plus a fallback
 * onto the PMIC's own flash block.  It picks one by reading register 0 at
 * 8-bit I2C address 0x60, then 0x50, and switching on the byte
 * ("Camflashdrv: IC support found for flash device context %d, id 0x%x"):
 * 0x16 is Hill, and 0x16 is what this phone returns.
 *
 * Hill is a three-channel inductive driver -- its own fault strings name
 * SW1/SW2/SW3 and "all three inductors reported faulty - check output
 * capacitor" -- which is what the 950's triple-LED "natural flash" needs, and
 * more channels than the PMIC's two.  That is why the PMI8994 qpnp-flash-led
 * nodes this kernel inherits from the Qualcomm reference DT accept a
 * brightness write and light nothing: their current outputs are not connected
 * to these LEDs.
 *
 * It hangs off **CCI master 0**, not a BLSP I2C bus and not the rear sensor's
 * master 1.  Every /dev/i2c-N was swept for 0x30/0x28 and neither is there;
 * master 1 NACKs both; master 0 answers 0x16 at 0x30.  WP reaches it the same
 * way, through the camera platform driver's I2C function table
 * (qccamplatform8992.sys is the CCI driver), which is also why ACPI declares
 * the flash device (QCOM244B) with no I2C resource of its own.
 *
 *
 * Register map
 * ------------
 *   0x00  chip id, 0x16
 *   0x01  revision, 0x02 here
 *   0x02  LED1 current      \
 *   0x03  LED2 current       > bits [7:5] are the level, 0..7; the low five
 *   0x04  LED3 current      /  bits read back but do nothing
 *   0x05  mode.  Powers up 0xcc; WP writes 0 to power the part on, 4 to power
 *         it off, and 0x19 / 0xd9 / 0x99 for its three flash modes (bits
 *         [7:6] select, the rest is constant).  0x19 is the one used here.
 *   0x06  VPH_PWR droop compensation.  WP computes it by banding the summed
 *         current against 1750 / 1900 / 2050 mA and leaves it 0 below that,
 *         which is where an assist light sits.
 *   0x07  safety-timer code, five bits: WP computes (ms - 4) / 4 + 1 and
 *         clamps to 0x1f, so ~128 ms is the longest it can express
 *   0x08  enable, three bits: 3 standby, 6 torch, 7 flash
 *   0x20  0x6a, 0x22  0x1c -- untouched, unknown
 *
 * The three channels are independent, and not identical: driven alone at the
 * same level, LED1/LED2/LED3 gave preview exposures of 228 / 175 / 378 lines,
 * so LED3 is the warm phosphor of the "natural flash" pair.  Mixing them to a
 * measured illuminant is what WP's "B algorithm" and its RGBC sensor are for;
 * this driver just drives all three together.
 *
 * Levels are measured, not assumed.  With AEC live and pinned at its maximum
 * 3292 lines in a dark room, raising the level dropped the exposure it settled
 * on -- which is the only remote way to see the light:
 *
 *   level    0    1    2    4    5    6    7
 *   lines  3292  197  127   77   54   34   20
 *
 * Nothing below level 1 (raw 0x20) emits at all.
 *
 * Enable 7 (flash) reverts itself to 3 after a couple of seconds, which is the
 * safety timer expiring against register 0x07 -- so the high setting carries a
 * duration.  Enable 6 (torch) is continuous and is what the LED class device
 * below uses.
 */
#include <linux/module.h>
#include <linux/export.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include "msm_led_flash.h"

#define FLASH_NAME "qcom,led-flash-hill"

#undef CDBG
#ifdef CONFIG_MSMB_CAMERA_DEBUG
#define CDBG(fmt, args...) pr_err(fmt, ##args)
#else
#define CDBG(fmt, args...) do { } while (0)
#endif

#define HILL_REG_CURRENT1	0x02
#define HILL_REG_CURRENT2	0x03
#define HILL_REG_CURRENT3	0x04
#define HILL_REG_MODE		0x05
#define HILL_REG_DROOP		0x06
#define HILL_REG_DURATION	0x07
#define HILL_REG_ENABLE		0x08

#define HILL_MODE_NORMAL	0x19
#define HILL_MODE_POWER_ON	0x00
#define HILL_MODE_POWER_OFF	0x04

#define HILL_EN_STANDBY		0x03
#define HILL_EN_TORCH		0x06
#define HILL_EN_FLASH		0x07

#define HILL_LEVEL_MAX		7
#define HILL_CUR(level)		((uint8_t)((level) << 5))

static struct msm_led_flash_ctrl_t fctrl;
static struct i2c_driver hill_i2c_driver;

/*
 * Per-LED level for the two operating points, 0..7.  Runtime-settable because
 * neither has a calibrated source: WP takes its currents from the B algorithm
 * against the module's own NVM, which this port does not read.  4 is a bright
 * flashlight; 7 is the part's maximum and what a still capture should want.
 */
static int hill_torch_level = 4;
module_param_named(torch_level, hill_torch_level, int, 0644);
MODULE_PARM_DESC(torch_level, "per-LED current level for torch/assist light, 0..7 (0 = off)");

static int hill_flash_level = HILL_LEVEL_MAX;
module_param_named(flash_level, hill_flash_level, int, 0644);
MODULE_PARM_DESC(flash_level, "per-LED current level for a flash pulse, 0..7");

/*
 * Register 0x07 is a safety timer whose code Windows computes as
 * `(ms - 4) / 4 + 1`, clamped to 0x1f (qccamflash8992.sys 0x00408450) -- a
 * five-bit field.  0xff, which this used to default to, is not a legal code
 * and also set three unknown bits above it.  0x19 is 100 ms.
 */
static int hill_flash_duration = 0x19;
module_param_named(flash_duration, hill_flash_duration, int, 0644);
MODULE_PARM_DESC(flash_duration, "register 0x07, the flash safety-timer code (5 bits, (ms-4)/4+1); enable 7 reverts to standby when it expires");

/*
 * WP powers the part on by writing mode 0, before it reads the LED module's
 * calibration over the part's one-wire interface.  Nothing here needs that
 * calibration, so this is the whole of it.
 */
static struct msm_camera_i2c_reg_array hill_init_array[] = {
	{HILL_REG_MODE, HILL_MODE_POWER_ON},
};

static struct msm_camera_i2c_reg_array hill_off_array[] = {
	{HILL_REG_ENABLE, HILL_EN_STANDBY},
};

/* WP's power-off order: standby first, then drop the mode. */
static struct msm_camera_i2c_reg_array hill_release_array[] = {
	{HILL_REG_ENABLE, HILL_EN_STANDBY},
	{HILL_REG_MODE,   HILL_MODE_POWER_OFF},
};

/*
 * Torch and flash differ only in the enable byte and the duration, so both
 * arrays are the same shape and are patched from the level params before each
 * use.  The order is WP's: currents, mode, droop, duration, then enable --
 * enable last, so the part never runs a frame with a stale current.
 */
static struct msm_camera_i2c_reg_array hill_low_array[] = {
	{HILL_REG_CURRENT1, 0},
	{HILL_REG_CURRENT2, 0},
	{HILL_REG_CURRENT3, 0},
	{HILL_REG_MODE,     HILL_MODE_NORMAL},
	{HILL_REG_DROOP,    0},
	{HILL_REG_DURATION, 0},
	{HILL_REG_ENABLE,   HILL_EN_TORCH},
};

static struct msm_camera_i2c_reg_array hill_high_array[] = {
	{HILL_REG_CURRENT1, 0},
	{HILL_REG_CURRENT2, 0},
	{HILL_REG_CURRENT3, 0},
	{HILL_REG_MODE,     HILL_MODE_NORMAL},
	{HILL_REG_DROOP,    0},
	{HILL_REG_DURATION, 0},
	{HILL_REG_ENABLE,   HILL_EN_FLASH},
};

static void hill_fill(struct msm_camera_i2c_reg_array *a, int level,
		      int duration)
{
	if (level < 0)
		level = 0;
	if (level > HILL_LEVEL_MAX)
		level = HILL_LEVEL_MAX;
	a[0].reg_data = HILL_CUR(level);
	a[1].reg_data = HILL_CUR(level);
	a[2].reg_data = HILL_CUR(level);
	a[5].reg_data = (uint8_t)duration;
}

static int32_t hill_led_low(struct msm_led_flash_ctrl_t *f)
{
	hill_fill(hill_low_array, hill_torch_level, 0);
	return msm_flash_led_low(f);
}

static int32_t hill_led_high(struct msm_led_flash_ctrl_t *f)
{
	hill_fill(hill_high_array, hill_flash_level, hill_flash_duration);
	return msm_flash_led_high(f);
}

/*
 * A plain flashlight, as /sys/class/leds/flash-torch.
 *
 * The camera daemon reaches the same hardware through the v4l2 subdev below,
 * but only while a camera session is open, and this kernel's I2C flash
 * framework -- unlike its PMIC one -- never creates an LED class device
 * (msm_led_torch_create_classdev is only wired into msm_led_trigger.c).  So
 * there is nothing for a torch app, or for a bench measurement, to write.
 *
 * Brightness maps onto the part's eight levels rather than being thrown away:
 * 1..255 spans levels 1..7, so the sysfs default of 255 is the maximum and
 * anything non-zero lights.  CCI has to be initialised before the first write
 * and released after the last, and the daemon may already have done it -- so
 * this only initialises when it finds the part idle, and only releases what it
 * initialised itself.  Driven from sysfs, i.e. process context, so the I2C
 * write here may sleep; do not wire this classdev to an LED trigger.
 */
static DEFINE_MUTEX(hill_torch_lock);
static bool hill_torch_did_init;

static int hill_level_from_brightness(enum led_brightness b)
{
	int level;

	if (b == LED_OFF)
		return 0;
	level = 1 + ((int)b - 1) * (HILL_LEVEL_MAX - 1) / (LED_FULL - 1);
	if (level > HILL_LEVEL_MAX)
		level = HILL_LEVEL_MAX;
	return level;
}

static void hill_torch_brightness_set(struct led_classdev *cdev,
				      enum led_brightness value)
{
	int rc = 0;

	if (!fctrl.func_tbl || !fctrl.flash_i2c_client) {
		pr_err("hill: no flash controller\n");
		return;
	}

	mutex_lock(&hill_torch_lock);
	if (value != LED_OFF) {
		if (fctrl.led_state != MSM_CAMERA_LED_INIT) {
			rc = fctrl.func_tbl->flash_led_init(&fctrl);
			if (rc < 0) {
				pr_err("hill: torch init failed %d\n", rc);
				goto out;
			}
			hill_torch_did_init = true;
		}
		hill_fill(hill_low_array,
			  hill_level_from_brightness(value), 0);
		rc = msm_flash_led_low(&fctrl);
		if (rc < 0)
			pr_err("hill: torch on failed %d\n", rc);
	} else {
		msm_flash_led_off(&fctrl);
		if (hill_torch_did_init) {
			fctrl.func_tbl->flash_led_release(&fctrl);
			hill_torch_did_init = false;
		}
	}
out:
	mutex_unlock(&hill_torch_lock);
}

static struct led_classdev hill_torch_classdev = {
	.name		= "flash-torch",
	.brightness_set	= hill_torch_brightness_set,
	.brightness	= LED_OFF,
	.max_brightness	= LED_FULL,
};

static void __exit msm_flash_hill_i2c_remove(void)
{
	i2c_del_driver(&hill_i2c_driver);
	return;
}

static const struct of_device_id hill_trigger_dt_match[] = {
	{.compatible = FLASH_NAME, .data = &fctrl},
	{}
};

MODULE_DEVICE_TABLE(of, hill_trigger_dt_match);

static const struct i2c_device_id hill_i2c_id[] = {
	{FLASH_NAME, (kernel_ulong_t)&fctrl},
	{ }
};

static int msm_flash_hill_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	if (!id) {
		pr_err("msm_flash_hill_i2c_probe: id is NULL");
		id = hill_i2c_id;
	}

	return msm_flash_i2c_probe(client, id);
}

static struct i2c_driver hill_i2c_driver = {
	.id_table = hill_i2c_id,
	.probe  = msm_flash_hill_i2c_probe,
	.remove = __exit_p(msm_flash_hill_i2c_remove),
	.driver = {
		.name = FLASH_NAME,
		.owner = THIS_MODULE,
		.of_match_table = hill_trigger_dt_match,
	},
};

static int msm_flash_hill_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	int rc;

	match = of_match_device(hill_trigger_dt_match, &pdev->dev);
	if (!match)
		return -EFAULT;
	rc = msm_flash_probe(pdev, match->data);
	if (rc < 0)
		return rc;

	rc = led_classdev_register(&pdev->dev, &hill_torch_classdev);
	if (rc < 0)
		pr_err("hill: led_classdev_register failed %d\n", rc);
	else
		pr_err("hill: flash at 8-bit 0x%02x on cci master %d, /sys/class/leds/%s\n",
		       fctrl.flashdata && fctrl.flashdata->slave_info ?
			       fctrl.flashdata->slave_info->sensor_slave_addr : 0,
		       fctrl.cci_i2c_master, hill_torch_classdev.name);
	return 0;
}

static struct platform_driver hill_platform_driver = {
	.probe = msm_flash_hill_platform_probe,
	.driver = {
		.name = FLASH_NAME,
		.owner = THIS_MODULE,
		.of_match_table = hill_trigger_dt_match,
	},
};

static int __init msm_flash_hill_init_module(void)
{
	int32_t rc = 0;

	rc = platform_driver_register(&hill_platform_driver);
	if (!rc)
		return rc;
	pr_debug("%s:%d rc %d\n", __func__, __LINE__, rc);
	return i2c_add_driver(&hill_i2c_driver);
}

static void __exit msm_flash_hill_exit_module(void)
{
	if (fctrl.pdev)
		platform_driver_unregister(&hill_platform_driver);
	else
		i2c_del_driver(&hill_i2c_driver);
}

static struct msm_camera_i2c_client hill_i2c_client = {
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
};

static struct msm_camera_i2c_reg_setting hill_init_setting = {
	.reg_setting = hill_init_array,
	.size = ARRAY_SIZE(hill_init_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting hill_off_setting = {
	.reg_setting = hill_off_array,
	.size = ARRAY_SIZE(hill_off_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting hill_release_setting = {
	.reg_setting = hill_release_array,
	.size = ARRAY_SIZE(hill_release_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting hill_low_setting = {
	.reg_setting = hill_low_array,
	.size = ARRAY_SIZE(hill_low_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting hill_high_setting = {
	.reg_setting = hill_high_array,
	.size = ARRAY_SIZE(hill_high_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_led_flash_reg_t hill_regs = {
	.init_setting = &hill_init_setting,
	.off_setting = &hill_off_setting,
	.low_setting = &hill_low_setting,
	.high_setting = &hill_high_setting,
	.release_setting = &hill_release_setting,
};

static struct msm_flash_fn_t hill_func_tbl = {
	.flash_get_subdev_id = msm_led_i2c_trigger_get_subdev_id,
	.flash_led_config = msm_led_i2c_trigger_config,
	.flash_led_init = msm_flash_led_init,
	.flash_led_release = msm_flash_led_release,
	.flash_led_off = msm_flash_led_off,
	.flash_led_low = hill_led_low,
	.flash_led_high = hill_led_high,
};

static struct msm_led_flash_ctrl_t fctrl = {
	.flash_i2c_client = &hill_i2c_client,
	.reg_setting = &hill_regs,
	.func_tbl = &hill_func_tbl,
};

module_init(msm_flash_hill_init_module);
module_exit(msm_flash_hill_exit_module);
MODULE_DESCRIPTION("Hill triple-LED camera flash");
MODULE_LICENSE("GPL v2");
