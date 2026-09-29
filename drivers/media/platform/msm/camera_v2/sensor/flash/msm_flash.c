/* Copyright (c) 2009-2015, The Linux Foundation. All rights reserved.
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
 */

#define pr_fmt(fmt) "%s:%d " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/workqueue.h>
#include "msm_flash.h"
#include "msm_camera_dt_util.h"
#include "msm_cci.h"
#include "../../msm_flash_sync.h"

#undef CDBG
#define CDBG(fmt, args...) pr_debug(fmt, ##args)

DEFINE_MSM_MUTEX(msm_flash_mutex);

/*
 * Trace every flash config the daemon issues.
 *
 * The main flash fires on the first capture of a session and never again:
 * hardware read-back shows the preflash sequence (0x02 = level, 0x07 = 0) on
 * every shot but the high sequence (0x02 = 0xe0, 0x07 = 0xff) only once.  The
 * kernel's dispatch is unconditional on flash_state == MSM_CAMERA_FLASH_INIT
 * and CFG_FLASH_LOW provably works every time, so either the daemon stops
 * sending CFG_FLASH_HIGH or it is being rejected here.  This says which.
 *
 *   echo 1 > /sys/module/msm_flash/parameters/flash_dbg
 */
static int flash_dbg;
module_param(flash_dbg, int, 0644);

/*
 * S182: a software safety timeout for the MAIN flash.
 *
 * ⚠ Both halves of the original reasoning here were wrong, and the conclusion
 * survives anyway -- this timeout is needed MORE than S182 thought.
 *
 * S182 said Hill's safety timer "only counts in FLASH mode -- enable 7 reverts
 * itself to standby 3 when it expires, which is how register 0x07 was
 * identified", and that the main flash therefore had to run in TORCH mode,
 * which has no timer.  Measured since:
 *
 *   - Flash mode DOES strobe, when it is entered from torch rather than from
 *     standby (the boost is inductive and flash mode will not start it).  The
 *     main flash is back in flash mode -- see flash_lib_hill.c.
 *   - The enable register NEVER reverts itself, in flash mode or any other,
 *     at any duration code: 0x07 still read 0x07 past the 124 ms code 0x1f
 *     implies, past 8 ms at code 0x02, and ten seconds later.  So there is no
 *     hardware auto-off to have lost, and register 0x07's identification as a
 *     self-clearing safety timer was never observed on a part that was
 *     actually firing.
 *
 * The light itself is a brief pulse on the 0x06 -> 0x07 transition, but the
 * part stays armed and drawing current afterwards regardless of mode.
 *
 * So a CFG_FLASH_HIGH with no CFG_FLASH_OFF behind it leaves three LEDs at full
 * current indefinitely -- observed after an ordinary capture, 0x08 = 0x06 and
 * 0x02 = 0xe0 long afterwards, with CFG HIGH the last ioctl in the log.  That
 * is a battery and thermal problem, not a cosmetic one.
 *
 * We removed the hardware net when we changed modes, so put one back in
 * software.  Only CFG_FLASH_HIGH arms it: the flashlight tile drives
 * CFG_FLASH_LOW and must stay on until the user turns it off.  0 disables.
 */
static int flash_high_timeout_ms = 1500;
module_param(flash_high_timeout_ms, int, 0644);

static struct msm_flash_ctrl_t *flash_wd_ctrl;

static void msm_flash_high_watchdog(struct work_struct *work)
{
	struct msm_flash_ctrl_t *fctrl = flash_wd_ctrl;
	struct msm_flash_cfg_data_t off;

	if (!fctrl || !fctrl->func_tbl || !fctrl->func_tbl->camera_flash_off)
		return;

	memset(&off, 0, sizeof(off));
	off.cfg_type = CFG_FLASH_OFF;	/* cfg.settings NULL -> the DT sequence */

	mutex_lock(fctrl->flash_mutex);
	if (fctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
		pr_err("msm_flash: main flash still on %d ms after CFG_FLASH_HIGH with no CFG_FLASH_OFF -- forcing off\n",
			flash_high_timeout_ms);
		fctrl->func_tbl->camera_flash_off(fctrl, &off);
	}
	mutex_unlock(fctrl->flash_mutex);
}

static DECLARE_DELAYED_WORK(flash_wd_work, msm_flash_high_watchdog);

/*
 * REPEATING THE MAIN FLASH so it is still lit when the snapshot is exposed.
 *
 * Measured (S188): the snapshot frame is exposed **300-400 ms** after
 * CFG_FLASH_HIGH -- bisecting flash_high_timeout_ms gives a photo mean of 50.5
 * at 150 ms, 49.0 at 250 ms, 73.7 at 400 ms and 81.0 at 800 ms.  Hill's own
 * duration field is five bits, `(ms-4)/4+1` clamped to 0x1f, so the longest
 * strobe the part can be asked for is ~124 ms.  A flash-mode pulse fired when
 * the ioctl arrives is therefore always dark by the time the frame is exposed,
 * and no choice of duration can fix that.
 *
 * Re-issuing the high setting on a timer covers the whole window instead: each
 * repeat re-triggers the strobe, so wherever the exposure lands a pulse is
 * lit.  It is worth the trouble because flash mode is much brighter than
 * torch -- fixed-exposure RAW puts torch level 7 at +9.5 grey levels over
 * ambient and a flash-mode pulse at +42.4, i.e. 4.5x, about 2.2 stops.
 *
 * Generic on purpose: this re-applies whatever sequence userspace supplied, so
 * the chip's behaviour stays in libflash_<name>.so and the device tree.  For a
 * torch-style setting re-applying is idempotent and harmless; only a sequence
 * that ends in a one-shot trigger turns it into a train.
 *
 * 0 disables (the default).  The repeat stops when flash_high_timeout_ms
 * expires, so that parameter still bounds how long the LEDs can be lit.
 */
static int flash_high_repeat_ms;	/* default off: torch needs no train */
module_param(flash_high_repeat_ms, int, 0644);
MODULE_PARM_DESC(flash_high_repeat_ms,
	"re-issue the CFG_FLASH_HIGH sequence every N ms (0 = off)");

/*
 * How long the train is allowed to run.  The snapshot is exposed 300-400 ms
 * after CFG_FLASH_HIGH, so 600 ms covers it with margin and then stops.
 *
 * This is deliberately much shorter than flash_high_timeout_ms.  That
 * parameter exists to guarantee the LEDs are eventually OFF and is 1500 ms;
 * running a 2400 mA strobe train for that long would put roughly three times
 * the energy of a single flash into the LEDs on every photo.
 *
 * ⚠ There is still no COOLDOWN here.  Windows' own driver tracks one --
 * "Current flash heatload : %d", "Cooldown safety mechanism triggered,
 * load : %d", "Hill_GetStatus: flash cooldown is active" -- and refuses to
 * fire when the accumulated load is too high.  Nothing in this port does, so
 * back-to-back flash photos are bounded only by this window.  That is the
 * reason flash_high_repeat_ms defaults to 0.
 */
static int flash_high_repeat_window_ms = 600;
module_param(flash_high_repeat_window_ms, int, 0644);
MODULE_PARM_DESC(flash_high_repeat_window_ms,
	"how long the CFG_FLASH_HIGH repeat train may run");

/*
 * FLASH HEAT LOAD -- Windows' own cooldown model, decoded from
 * qccamflash8992.sys fcn.00406b1c (the function carrying "Current flash
 * heatload : %d", "Cooldown safety mechanism triggered, load : %d" and
 * "Forced flash cooldown is active, remaining load : %d").
 *
 * Its accumulator lives at 0x444760: a 64-bit load at +8 and a 64-bit
 * timestamp at +0x10.  Per call it does
 *
 *     load -= (now - last)            clamped at 0, which also clears the latch
 *     last  = now
 *     if (latched)                    deny
 *     if (load > 8000000 + arg3*10000) latch, load += 16000000, deny
 *     load += arg2*10000
 *     if (load > arg3*10000)          latch for the NEXT request
 *
 * The times are Windows' 100 ns ticks, so 10000 is exactly 1 ms: **the load is
 * milliseconds of flash-on time, draining in real time**, with an 800 ms
 * budget and a 1600 ms penalty when it trips.  Bounded iff on-time <= off-time,
 * i.e. a duty cycle of 50%, with the 800 ms allowing a burst above that.
 *
 * This replaces the flat two-second gap that first stood in here.  A flat gap
 * is both too strict (it refuses a second photo 700 ms later that costs
 * nothing) and too lax (it permits an indefinite 600-on/1400-off cycle, a 30%
 * duty cycle for ever).  The load model is what the hardware vendor shipped.
 *
 * ⚠ Windows has a SECOND, independent cooldown keyed on measured temperature
 * ("Camflashdrv: Temperature cooldown activated, temp: %d",
 * "Hill_GetStatus: flash temperature cooldown is active").  This port has no
 * thermal reading for the LEDs, so that half is NOT implemented -- the budget
 * here is the only protection, and it is open-loop.
 */
static int flash_load_budget_ms = 800;
module_param(flash_load_budget_ms, int, 0644);
MODULE_PARM_DESC(flash_load_budget_ms,
	"flash-on milliseconds allowed before the cooldown latches (0 = no limit)");

static int flash_load_penalty_ms = 1600;
module_param(flash_load_penalty_ms, int, 0644);
MODULE_PARM_DESC(flash_load_penalty_ms,
	"extra load added when the cooldown trips, i.e. how long it stays latched");

/*
 * How long the train will actually run.  The watchdog bounds it, so a window
 * set longer than flash_high_timeout_ms does not extend it -- and the heat
 * load must be charged the real figure, not the requested one, or raising the
 * window alone would inflate the load without lighting the LEDs any longer.
 */
static int msm_flash_train_ms(void)
{
	return flash_high_repeat_window_ms < flash_high_timeout_ms ?
		flash_high_repeat_window_ms : flash_high_timeout_ms;
}

/*
 * FRAME-SYNCHRONISED TRIGGER -- fire once, when the capture is exposed.
 *
 * This is what removes the flicker.  The repeat train (flash_high_repeat_ms)
 * covers the exposure by firing ~5 strobes across 600 ms, and because each
 * re-trigger must begin with a standby write that extinguishes the LED, the
 * result is a visible ~8 Hz flicker and a photo whose brightness depends on
 * which pulse the exposure caught (means swung 108.9-140.7 on one scene).
 *
 * Windows has neither problem because it fires exactly once, timed against the
 * frame, with the LED on for `exposure_time + readout_time` -- the sensor
 * driver programs the CCI hardware sequencer to issue the trigger
 * (rearsensor.sys, SMIApp_UpdateFlashConfigAndTriggerCciHw; the strobe length
 * string is "flash strobe_length (exposure_time_us + current_readout_time_us)").
 *
 * We reach the same place from the ISP: msm_isp_request_frame() tells us which
 * frame the capture will land on, and msm_isp_notify() tells us when that
 * frame starts.  CFG_FLASH_HIGH then only arms; the sequence is written from a
 * work item at that frame's SOF.  One pulse, no repeat, no flicker.
 *
 * The SOF hook runs in the ISP's interrupt path, so it may not touch CCI --
 * it only schedules the work.
 */
static int flash_sof_sync;	/* default off: only useful with flash mode */
module_param(flash_sof_sync, int, 0644);
MODULE_PARM_DESC(flash_sof_sync,
	"defer CFG_FLASH_HIGH to the SOF of the frame the capture lands on (0 = fire immediately)");

/*
 * If the request or the SOF never arrives -- a path that does not go through
 * a controllable output, a dropped frame -- fire anyway rather than silently
 * losing the flash.  400 ms is past the 300-400 ms the snapshot normally
 * takes, so the fallback only runs when the frame hooks did not.
 */
static int flash_sof_fallback_ms = 400;
module_param(flash_sof_fallback_ms, int, 0644);
MODULE_PARM_DESC(flash_sof_fallback_ms,
	"fire the armed flash after this long if no capture frame SOF arrived");

static struct msm_flash_ctrl_t *flash_sync_ctrl;
static struct msm_camera_i2c_reg_setting_array flash_sync_setting;
static atomic_t flash_sync_armed = ATOMIC_INIT(0);
static uint32_t flash_sync_frame;	/* for the log only */
static bool flash_sync_ready;		/* the capture request has been queued */

static uint32_t flash_last_req_frame;
static unsigned long flash_last_req_jiffies;

static int flash_sof_count;		/* read-only: proves the hook is wired */
module_param(flash_sof_count, int, 0444);
MODULE_PARM_DESC(flash_sof_count, "SOFs seen by the flash driver since boot");

static void msm_flash_arm_repeat(struct msm_flash_ctrl_t *fctrl);

static void msm_flash_sync_fire(struct work_struct *work)
{
	struct msm_flash_ctrl_t *fctrl = flash_sync_ctrl;
	struct msm_flash_cfg_data_t high;
	int32_t rc;

	if (!fctrl || !fctrl->func_tbl || !fctrl->func_tbl->camera_flash_high)
		return;

	memset(&high, 0, sizeof(high));
	high.cfg_type = CFG_FLASH_HIGH;
	high.cfg.settings = &flash_sync_setting;

	mutex_lock(fctrl->flash_mutex);
	if (fctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
		if (flash_dbg && flash_sync_setting.size &&
			flash_sync_setting.size <= MAX_I2C_REG_SET)
			pr_err("%s: cached seq size %d first %02x=%02x last %02x=%02x addr_t %d data_t %d\n",
				__func__, flash_sync_setting.size,
				flash_sync_setting.reg_setting_a[0].reg_addr,
				flash_sync_setting.reg_setting_a[0].reg_data,
				flash_sync_setting.reg_setting_a[
					flash_sync_setting.size - 1].reg_addr,
				flash_sync_setting.reg_setting_a[
					flash_sync_setting.size - 1].reg_data,
				flash_sync_setting.addr_type,
				flash_sync_setting.data_type);

		rc = fctrl->func_tbl->camera_flash_high(fctrl, &high);
		if (flash_dbg) {
			uint16_t v = 0;

			fctrl->flash_i2c_client.i2c_func_tbl->i2c_read(
				&fctrl->flash_i2c_client, 0x08, &v,
				MSM_CAMERA_I2C_BYTE_DATA);
			pr_err("%s: fired at frame %u rc %d, enable reads 0x%02x\n",
				__func__, flash_sync_frame, rc, v);
		}
		/*
		 * One pulse is not enough light.  Windows keeps the LED on for
		 * `exposure_time + readout_time` (rearsensor.sys), which on a
		 * rolling shutter is what every row needs -- a single strobe
		 * shorter than the readout lights only a band of rows, and the
		 * picture comes out barely brighter than ambient.
		 *
		 * Hill's duration field caps at ~124 ms, under the ~74 ms
		 * exposure plus readout only with no margin, so the sequence is
		 * re-issued for flash_high_repeat_window_ms from HERE -- at the
		 * capture frame, not 400 ms before it.  That is a far shorter
		 * train than arming at CFG_FLASH_HIGH (which spanned 600 ms and
		 * flickered visibly at ~8 Hz), and it sits inside the exposure
		 * rather than across the whole pre-capture window.
		 */
		if (rc >= 0)
			msm_flash_arm_repeat(fctrl);
	}
	mutex_unlock(fctrl->flash_mutex);
}

static DECLARE_WORK(flash_sync_work, msm_flash_sync_fire);

static void msm_flash_sync_disarm(void)
{
	atomic_set(&flash_sync_armed, 0);
	flash_sync_frame = 0;
	WRITE_ONCE(flash_sync_ready, false);
}

/*
 * ⚠ Do NOT try to match the request's frame_id against the SOF's.  They are
 * not the same numbering here: a request logged by reqlog as "frame=366"
 * arrived at this hook as 3, so a comparison silently never matched and every
 * capture fell through to the fallback -- firing 400 ms late, after the
 * exposure, which reads exactly like the flash not working.
 *
 * The rule that does hold: the request is queued FOR the upcoming frame, so
 * the first SOF after it is the frame the capture lands on.
 */
void msm_flash_capture_request(uint32_t frame_id)
{
	flash_last_req_frame = frame_id;
	flash_last_req_jiffies = jiffies;
	if (flash_dbg)
		pr_err("msm_flash: capture request frame %u (armed %d)\n",
			frame_id, atomic_read(&flash_sync_armed));
	if (!atomic_read(&flash_sync_armed))
		return;
	flash_sync_frame = frame_id;
	WRITE_ONCE(flash_sync_ready, true);
}
EXPORT_SYMBOL(msm_flash_capture_request);

void msm_flash_sof_notify(uint32_t frame_id)
{
	flash_sof_count++;
	if (!atomic_read(&flash_sync_armed))
		return;
	if (!READ_ONCE(flash_sync_ready))
		return;		/* capture not requested yet */
	if (atomic_cmpxchg(&flash_sync_armed, 1, 0) != 1)
		return;
	schedule_work(&flash_sync_work);
}
EXPORT_SYMBOL(msm_flash_sof_notify);

static void msm_flash_sync_fallback(struct work_struct *work)
{
	if (atomic_cmpxchg(&flash_sync_armed, 1, 0) != 1)
		return;		/* the frame hooks got there first */
	pr_info("msm_flash: no capture SOF within %d ms, firing anyway\n",
		flash_sof_fallback_ms);
	schedule_work(&flash_sync_work);
}

static DECLARE_DELAYED_WORK(flash_sync_fb_work, msm_flash_sync_fallback);

static int flash_load_ms;		/* read-only instrumentation */
module_param(flash_load_ms, int, 0444);
MODULE_PARM_DESC(flash_load_ms, "current accumulated flash heat load, in ms");

static unsigned long flash_load_stamp;
static bool flash_load_latched;

/*
 * Charge `ms` of flash-on time against the budget.  Returns true if the flash
 * may fire at full strength.  Called with flash_mutex held.
 *
 * A denial is not "no flash": the caller still issues the single strobe the
 * sequence carries, and only the repeat train is withheld, so a capture inside
 * a cooldown comes out dimmer rather than unlit.
 */
static bool msm_flash_load_charge(int ms)
{
	unsigned long now = jiffies;
	unsigned int elapsed;

	if (flash_load_budget_ms <= 0)
		return true;

	elapsed = flash_load_stamp ?
		jiffies_to_msecs(now - flash_load_stamp) : 0;
	flash_load_stamp = now;

	if ((unsigned int)flash_load_ms > elapsed) {
		flash_load_ms -= elapsed;
	} else {
		flash_load_ms = 0;
		flash_load_latched = false;	/* drained -- release the latch */
	}

	if (flash_load_latched) {
		pr_info("msm_flash: flash cooldown active, remaining load %d ms\n",
			flash_load_ms);
		return false;
	}

	if (flash_load_ms > flash_load_budget_ms) {
		flash_load_latched = true;
		flash_load_ms += flash_load_penalty_ms;
		pr_info("msm_flash: cooldown triggered, load %d ms\n",
			flash_load_ms);
		return false;
	}

	flash_load_ms += ms;

	/*
	 * Windows' second, pre-emptive latch: having charged this flash, if the
	 * load is already over budget, arm the block now so the NEXT request is
	 * refused rather than letting it push further into the red
	 * ("Flash will be blocked after main flash, load : %d", 0x406c36).
	 * The latch clears itself when the load drains to zero.
	 */
	if (flash_load_ms > flash_load_budget_ms) {
		flash_load_latched = true;
		pr_info("msm_flash: flash will be blocked after this one, load %d ms\n",
			flash_load_ms);
	}

	return true;
}

static struct msm_camera_i2c_reg_setting_array flash_rep_setting;
static bool flash_rep_valid;
static unsigned long flash_rep_deadline;

static void msm_flash_high_repeat(struct work_struct *work);
static DECLARE_DELAYED_WORK(flash_rep_work, msm_flash_high_repeat);

/*
 * Keep the LED lit across the exposure and the rolling-shutter readout by
 * re-issuing the sequence from the capture frame onwards.  Called from the
 * SOF trigger, so the train sits inside the capture rather than spanning the
 * 600 ms before it -- which is what made it flicker visibly.
 */
static void msm_flash_arm_repeat(struct msm_flash_ctrl_t *fctrl)
{
	if (flash_high_repeat_ms <= 0)
		return;
	memcpy(&flash_rep_setting, &flash_sync_setting,
		sizeof(flash_rep_setting));
	flash_rep_valid = true;
	flash_wd_ctrl = fctrl;
	flash_rep_deadline = jiffies +
		msecs_to_jiffies(flash_high_repeat_window_ms);
	mod_delayed_work(system_wq, &flash_rep_work,
		msecs_to_jiffies(flash_high_repeat_ms));
}

static void msm_flash_high_repeat(struct work_struct *work)
{
	struct msm_flash_ctrl_t *fctrl = flash_wd_ctrl;
	struct msm_flash_cfg_data_t high;

	if (!fctrl || !fctrl->func_tbl || !fctrl->func_tbl->camera_flash_high)
		return;
	if (!flash_rep_valid || flash_high_repeat_ms <= 0)
		return;

	memset(&high, 0, sizeof(high));
	high.cfg_type = CFG_FLASH_HIGH;
	high.cfg.settings = &flash_rep_setting;

	mutex_lock(fctrl->flash_mutex);
	if (fctrl->flash_state == MSM_CAMERA_FLASH_INIT &&
		time_before(jiffies, flash_rep_deadline)) {
		fctrl->func_tbl->camera_flash_high(fctrl, &high);
		mod_delayed_work(system_wq, &flash_rep_work,
			msecs_to_jiffies(flash_high_repeat_ms));
	}
	mutex_unlock(fctrl->flash_mutex);
}


/*
 * Cancel without _sync: this is called with flash_mutex held and the work takes
 * that same mutex, so the sync form would deadlock against a watchdog that has
 * already started.  A watchdog that does start concurrently finds the state it
 * checks and turns the LED off, which is harmless -- the LED is meant to be off
 * by then anyway.
 */
static inline void msm_flash_wd_cancel(void)
{
	cancel_delayed_work(&flash_wd_work);
	cancel_delayed_work(&flash_rep_work);
	cancel_delayed_work(&flash_sync_fb_work);
	msm_flash_sync_disarm();
	flash_rep_valid = false;
}
MODULE_PARM_DESC(flash_dbg, "log every VIDIOC_MSM_FLASH_CFG (cfg_type, state, rc)");

static const char *msm_flash_cfg_name(int t)
{
	switch (t) {
	case CFG_FLASH_INIT:    return "INIT";
	case CFG_FLASH_RELEASE: return "RELEASE";
	case CFG_FLASH_OFF:     return "OFF";
	case CFG_FLASH_LOW:     return "LOW";
	case CFG_FLASH_HIGH:    return "HIGH";
	default:                return "?";
	}
}

static struct v4l2_file_operations msm_flash_v4l2_subdev_fops;
static struct led_trigger *torch_trigger;

static const struct of_device_id msm_flash_dt_match[] = {
	{.compatible = "qcom,camera-flash", .data = NULL},
	{}
};

static struct msm_flash_table msm_i2c_flash_table;
static struct msm_flash_table msm_gpio_flash_table;
static struct msm_flash_table msm_pmic_flash_table;

static struct msm_flash_table *flash_table[] = {
	&msm_i2c_flash_table,
	&msm_gpio_flash_table,
	&msm_pmic_flash_table
};

static struct msm_camera_i2c_fn_t msm_sensor_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
	.i2c_poll =  msm_camera_cci_i2c_poll,
};

void msm_torch_brightness_set(struct led_classdev *led_cdev,
				enum led_brightness value)
{
	if (!torch_trigger) {
		pr_err("No torch trigger found, can't set brightness\n");
		return;
	}

	led_trigger_event(torch_trigger, value);
};

static struct led_classdev msm_torch_led[MAX_LED_TRIGGERS] = {
	{
		.name		= "torch-light0",
		.brightness_set	= msm_torch_brightness_set,
		.brightness	= LED_OFF,
	},
	{
		.name		= "torch-light1",
		.brightness_set	= msm_torch_brightness_set,
		.brightness	= LED_OFF,
	},
	{
		.name		= "torch-light2",
		.brightness_set	= msm_torch_brightness_set,
		.brightness	= LED_OFF,
	},
};

static int32_t msm_torch_create_classdev(struct platform_device *pdev,
				void *data)
{
	int32_t rc = 0;
	int32_t i = 0;
	struct msm_flash_ctrl_t *fctrl =
		(struct msm_flash_ctrl_t *)data;

	if (!fctrl) {
		pr_err("Invalid fctrl\n");
		return -EINVAL;
	}

	for (i = 0; i < fctrl->torch_num_sources; i++) {
		if (fctrl->torch_trigger[i]) {
			torch_trigger = fctrl->torch_trigger[i];
			CDBG("%s:%d msm_torch_brightness_set for torch %d",
				__func__, __LINE__, i);
			msm_torch_brightness_set(&msm_torch_led[i],
				LED_OFF);

			rc = led_classdev_register(&pdev->dev,
				&msm_torch_led[i]);
			if (rc) {
				pr_err("Failed to register %d led dev. rc = %d\n",
						i, rc);
				return rc;
			}
		} else {
			pr_err("Invalid fctrl->torch_trigger[%d]\n", i);
			return -EINVAL;
		}
	}

	return 0;
};

static int32_t msm_flash_get_subdev_id(
	struct msm_flash_ctrl_t *flash_ctrl, void *arg)
{
	uint32_t *subdev_id = (uint32_t *)arg;
	CDBG("Enter\n");
	if (!subdev_id) {
		pr_err("failed\n");
		return -EINVAL;
	}
	if (flash_ctrl->flash_device_type == MSM_CAMERA_PLATFORM_DEVICE)
		*subdev_id = flash_ctrl->pdev->id;
	else
		*subdev_id = flash_ctrl->subdev_id;

	CDBG("subdev_id %d\n", *subdev_id);
	CDBG("Exit\n");
	return 0;
}

/*
 * Write a setting array, honouring the per-register `delay` field by
 * splitting the array into segments at every entry that carries one and
 * sleeping between them.
 *
 * WHY THIS IS NEEDED
 *
 * Hill's flash mode will not start its inductive boost from cold standby.
 * Arming it as 0x08: 0x03 -> 0x07 leaves the part armed -- the register
 * reads 0x07, and keeps reading 0x07 indefinitely -- with the LEDs dark.
 * Entering flash from TORCH, where the boost is already running, strobes
 * every time.  So libflash_hill.so's high_setting must hold the torch enable
 * for a dwell before writing the flash enable, and that dwell has to live in
 * the register array, because on FLASH_DRIVER_I2C the array is the entire
 * chip description this path gets from userspace.
 *
 * The `delay` field already exists in the ABI and msm_cci.c does turn a
 * non-zero one into a hardware WAIT command -- but the count it programs is
 * `delay_us * cci_clk_MHz` (msm_cci_data_queue), which for a 10 ms dwell is
 * ~192000 with no clamp anywhere, well past what the queue's command field is
 * specified to hold.  A software split has no such ceiling and is exact.
 *
 * i2c_write_table_w_microdelay is NOT an alternative despite its name: it
 * applies no per-register delay at all, it merely drops the table-wide one.
 *
 * Device-tree settings are unaffected -- msm_flash_get_dt_setting() zeroes
 * every per-register delay, so those still go out as a single transfer and
 * take the identical path they did before.
 */
static int32_t msm_flash_i2c_write_table(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_camera_i2c_reg_setting_array *settings)
{
	struct msm_camera_i2c_reg_setting conf_array;
	struct msm_camera_i2c_reg_array seg[MAX_I2C_REG_SET];
	uint16_t start = 0, i, n;
	int32_t rc = 0;

	/* Validate the settings size */
	if ((!settings->size) || (settings->size > MAX_I2C_REG_SET)) {
		pr_err("failed: invalid size %d", settings->size);
		return -EINVAL;
	}

	conf_array.addr_type = settings->addr_type;
	conf_array.data_type = settings->data_type;

	for (i = 0; i < settings->size; i++) {
		uint32_t us = settings->reg_setting_a[i].delay;

		/* Not a split point -- keep accumulating into this segment. */
		if (!us && (i + 1 < settings->size))
			continue;

		/*
		 * Copy rather than pointing into the caller's array: the
		 * delay must be cleared so msm_cci.c does not *also* queue a
		 * hardware wait, and the caller's array is the persistent
		 * per-flash setting, reused by every later capture.
		 */
		n = i - start + 1;
		memcpy(seg, &settings->reg_setting_a[start],
			n * sizeof(seg[0]));
		seg[n - 1].delay = 0;

		conf_array.reg_setting = seg;
		conf_array.size = n;
		/* Only the final segment carries the table-wide delay. */
		conf_array.delay = ((i + 1) == settings->size) ?
					settings->delay : 0;

		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client, &conf_array);
		if (rc < 0) {
			pr_err("%s: regs %d..%d failed rc %d\n",
				__func__, start, i, rc);
			return rc;
		}

		if (us)
			usleep_range(us, us + (us >> 4) + 10);

		start = i + 1;
	}

	return rc;
}

#ifdef CONFIG_COMPAT
static void msm_flash_copy_power_settings_compat(
	struct msm_sensor_power_setting *ps,
	struct msm_sensor_power_setting32 *ps32, uint32_t size)
{
	uint16_t i = 0;

	for (i = 0; i < size; i++) {
		ps[i].config_val = ps32[i].config_val;
		ps[i].delay = ps32[i].delay;
		ps[i].seq_type = ps32[i].seq_type;
		ps[i].seq_val = ps32[i].seq_val;
	}
}
#endif

static int32_t msm_flash_i2c_init(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t rc = 0;
	struct msm_flash_init_info_t *flash_init_info =
		flash_data->cfg.flash_init_info;
	struct msm_camera_i2c_reg_setting_array *settings = NULL;
	struct msm_camera_cci_client *cci_client = NULL;
#ifdef CONFIG_COMPAT
	struct msm_sensor_power_setting_array32 *power_setting_array32 = NULL;
#endif
	if (!flash_init_info) {
		pr_err("%s:%d failed: Null pointer\n", __func__, __LINE__);
		return -EFAULT;
	}

	/*
	 * A power sequence is optional.  The camera daemon always supplies one
	 * out of the flash library, but the HAL's torch path sends an all-zero
	 * init -- driver type DEFAULT, slave address 0, no power settings and
	 * no register array -- because a PMIC flash needs none of it.  When the
	 * board has described the part in its device tree there is nothing to
	 * copy and nothing to power up, so skip both rather than failing.
	 */
	if (!flash_init_info->power_setting_array)
		goto skip_power_settings;

#ifdef CONFIG_COMPAT
	if (is_compat_task()) {
		power_setting_array32 = kzalloc(
			sizeof(struct msm_sensor_power_setting_array32),
			GFP_KERNEL);
		if (!power_setting_array32) {
			pr_err("%s mem allocation failed %d\n",
				__func__, __LINE__);
			return -ENOMEM;
		}

		if (copy_from_user(power_setting_array32,
			(void *)flash_init_info->power_setting_array,
			sizeof(struct msm_sensor_power_setting_array32))) {
			pr_err("%s copy_from_user failed %d\n",
				__func__, __LINE__);
			kfree(power_setting_array32);
			return -EFAULT;
		}

		flash_ctrl->power_setting_array.size =
			power_setting_array32->size;
		flash_ctrl->power_setting_array.size_down =
			power_setting_array32->size_down;
		flash_ctrl->power_setting_array.power_down_setting =
			compat_ptr(power_setting_array32->power_down_setting);
		flash_ctrl->power_setting_array.power_setting =
			compat_ptr(power_setting_array32->power_setting);

		/* Validate power_up array size and power_down array size */
		if ((!flash_ctrl->power_setting_array.size) ||
			(flash_ctrl->power_setting_array.size >
			MAX_POWER_CONFIG) ||
			(!flash_ctrl->power_setting_array.size_down) ||
			(flash_ctrl->power_setting_array.size_down >
			MAX_POWER_CONFIG)) {

			pr_err("failed: invalid size %d, size_down %d",
				flash_ctrl->power_setting_array.size,
				flash_ctrl->power_setting_array.size_down);
			kfree(power_setting_array32);
			power_setting_array32 = NULL;
			return -EINVAL;
		}
		/* Copy the settings from compat struct to regular struct */
		msm_flash_copy_power_settings_compat(
			flash_ctrl->power_setting_array.power_setting_a,
			power_setting_array32->power_setting_a,
			flash_ctrl->power_setting_array.size);

		msm_flash_copy_power_settings_compat(
			flash_ctrl->power_setting_array.power_down_setting_a,
			power_setting_array32->power_down_setting_a,
			flash_ctrl->power_setting_array.size_down);
	} else
#endif
	if (copy_from_user(&flash_ctrl->power_setting_array,
		(void *)flash_init_info->power_setting_array,
		sizeof(struct msm_sensor_power_setting_array))) {
		pr_err("%s copy_from_user failed %d\n", __func__, __LINE__);
		return -EFAULT;
	}

skip_power_settings:
	if (flash_ctrl->flash_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		uint32_t slave_addr = flash_init_info->slave_addr;

		/* Fall back to the address the board described */
		if (!slave_addr)
			slave_addr = flash_ctrl->dt_slave_addr;

		cci_client = flash_ctrl->flash_i2c_client.cci_client;
		cci_client->sid = slave_addr >> 1;
		cci_client->retries = 3;
		cci_client->id_map = 0;
	}

	if (!flash_init_info->power_setting_array) {
		/*
		 * msm_camera_power_up() is also what brings CCI up for this
		 * client (it ends with i2c_util(MSM_CCI_INIT)).  Skipping the
		 * power sequence must not mean skipping the bus, or every
		 * write below goes nowhere and CFG_FLASH_LOW comes back
		 * -EINVAL with nothing logged.
		 */
		if (flash_ctrl->flash_device_type ==
			MSM_CAMERA_PLATFORM_DEVICE) {
			rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_util(
				&flash_ctrl->flash_i2c_client, MSM_CCI_INIT);
			if (rc < 0) {
				pr_err("%s:%d cci_init failed rc %d\n",
					__func__, __LINE__, rc);
				return rc;
			}
			flash_ctrl->dt_cci_inited = 1;
		}
		goto skip_power_up;
	}

	flash_ctrl->power_info.power_setting =
		flash_ctrl->power_setting_array.power_setting_a;
	flash_ctrl->power_info.power_down_setting =
		flash_ctrl->power_setting_array.power_down_setting_a;
	flash_ctrl->power_info.power_setting_size =
		flash_ctrl->power_setting_array.size;
	flash_ctrl->power_info.power_down_setting_size =
		flash_ctrl->power_setting_array.size_down;

	if ((flash_ctrl->power_info.power_setting_size > MAX_POWER_CONFIG) ||
	(flash_ctrl->power_info.power_down_setting_size > MAX_POWER_CONFIG)) {
		pr_err("%s:%d invalid power setting size=%d size_down=%d\n",
			__func__, __LINE__,
			flash_ctrl->power_info.power_setting_size,
			flash_ctrl->power_info.power_down_setting_size);
		rc = -EINVAL;
		goto msm_flash_i2c_init_fail;
	}

	rc = msm_camera_power_up(&flash_ctrl->power_info,
		flash_ctrl->flash_device_type,
		&flash_ctrl->flash_i2c_client);
	if (rc < 0) {
		pr_err("%s msm_camera_power_up failed %d\n",
			__func__, __LINE__);
		goto msm_flash_i2c_init_fail;
	}
	flash_ctrl->dt_powered_up = 1;

skip_power_up:
	if (!flash_data->cfg.flash_init_info->settings) {
		/* Nothing from userspace -- use the board's own init sequence */
		if (flash_ctrl->dt_setting_valid[CFG_FLASH_INIT]) {
			rc = msm_flash_i2c_write_table(flash_ctrl,
				&flash_ctrl->dt_setting[CFG_FLASH_INIT]);
			if (rc < 0)
				pr_err("%s:%d device tree init sequence failed rc %d\n",
					__func__, __LINE__, rc);
		}
		return 0;
	}

	{
		settings = kzalloc(sizeof(
			struct msm_camera_i2c_reg_setting_array), GFP_KERNEL);
		if (!settings) {
			pr_err("%s mem allocation failed %d\n",
				__func__, __LINE__);
			return -ENOMEM;
		}

		if (copy_from_user(settings, (void *)flash_init_info->settings,
			sizeof(struct msm_camera_i2c_reg_setting_array))) {
			kfree(settings);
			pr_err("%s copy_from_user failed %d\n",
				__func__, __LINE__);
			return -EFAULT;
		}

		rc = msm_flash_i2c_write_table(flash_ctrl, settings);
		kfree(settings);

		if (rc < 0) {
			pr_err("%s:%d msm_flash_i2c_write_table rc %d failed\n",
				__func__, __LINE__, rc);
		}
	}

	return 0;

msm_flash_i2c_init_fail:
	return rc;
}

static int32_t msm_flash_gpio_init(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t i = 0;
	int32_t rc = 0;

	CDBG("Enter");
	for (i = 0; i < flash_ctrl->flash_num_sources; i++)
		flash_ctrl->flash_op_current[i] = LED_FULL;

	for (i = 0; i < flash_ctrl->torch_num_sources; i++)
		flash_ctrl->torch_op_current[i] = LED_HALF;

	for (i = 0; i < flash_ctrl->torch_num_sources; i++) {
		if (!flash_ctrl->torch_trigger[i]) {
			if (i < flash_ctrl->flash_num_sources)
				flash_ctrl->torch_trigger[i] =
					flash_ctrl->flash_trigger[i];
			else
				flash_ctrl->torch_trigger[i] =
					flash_ctrl->flash_trigger[
					flash_ctrl->flash_num_sources - 1];
		}
	}

	rc = flash_ctrl->func_tbl->camera_flash_off(flash_ctrl, flash_data);

	CDBG("Exit");
	return rc;
}

static int32_t msm_flash_i2c_release(
	struct msm_flash_ctrl_t *flash_ctrl)
{
	int32_t rc = 0;

	if (!(&flash_ctrl->power_info) || !(&flash_ctrl->flash_i2c_client)) {
		pr_err("%s:%d failed: %pK %pK\n",
			__func__, __LINE__, &flash_ctrl->power_info,
			&flash_ctrl->flash_i2c_client);
		return -EINVAL;
	}

	/*
	 * Put the flash back in the RELEASE state.
	 *
	 * msm_flash_release() -- the PMIC/GPIO release -- does this; this one
	 * never did, so on an I2C flash flash_state went to INIT once and
	 * stayed there for the life of the device.  Every later CFG_FLASH_INIT
	 * then hit msm_flash_init()'s "Invalid flash state = 0" shortcut and
	 * returned without powering anything up, while every CFG_FLASH_RELEASE
	 * still ran the power-down below.  msm_camera_power_down() starts with
	 * an unconditional MSM_CCI_RELEASE for a platform device, so each of
	 * those dropped a CCI reference this driver had never taken -- which is
	 * what powered the whole CCI block down underneath a live sensor
	 * ("msm_cci_release invalid ref count 0 / cci state 1", then
	 * "msm_cci_i2c_write invalid cci state 1" on the sensor's own writes).
	 */
	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;

	/* Symmetric with the skip in msm_flash_i2c_init() */
	if (flash_ctrl->dt_cci_inited) {
		flash_ctrl->dt_cci_inited = 0;
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_util(
			&flash_ctrl->flash_i2c_client, MSM_CCI_RELEASE);
		if (rc < 0)
			pr_err("%s:%d cci_release failed rc %d\n",
				__func__, __LINE__, rc);
		return 0;
	}

	/* Only undo a power-up that happened */
	if (!flash_ctrl->dt_powered_up)
		return 0;
	flash_ctrl->dt_powered_up = 0;

	rc = msm_camera_power_down(&flash_ctrl->power_info,
		flash_ctrl->flash_device_type,
		&flash_ctrl->flash_i2c_client);
	if (rc < 0) {
		pr_err("%s msm_camera_power_down failed %d\n",
			__func__, __LINE__);
		return -EINVAL;
	}
	return 0;
}

static int32_t msm_flash_off(struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t i = 0;

	CDBG("Enter\n");

	for (i = 0; i < flash_ctrl->flash_num_sources; i++)
		if (flash_ctrl->flash_trigger[i])
			led_trigger_event(flash_ctrl->flash_trigger[i], 0);

	for (i = 0; i < flash_ctrl->torch_num_sources; i++)
		if (flash_ctrl->torch_trigger[i])
			led_trigger_event(flash_ctrl->torch_trigger[i], 0);
	if (flash_ctrl->switch_trigger)
		led_trigger_event(flash_ctrl->switch_trigger, 0);

	CDBG("Exit\n");
	return 0;
}

static int32_t msm_flash_i2c_write_setting_array(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t rc = 0, cci_rc;
	struct msm_camera_i2c_reg_setting_array *settings = NULL;
	uint32_t cfg_type = flash_data->cfg_type;

	/*
	 * Own the bus for the duration of the write, whoever supplied the
	 * sequence.
	 *
	 * Nothing here can assume CCI is up.  msm_flash_init() returns early
	 * whenever the flash is already in the INIT state, so whichever client
	 * initialises the flash first -- the camera daemon at session start, or
	 * the HAL's torch path -- leaves every later CFG_FLASH_INIT a no-op,
	 * and with it the msm_camera_power_up() that would otherwise have
	 * called i2c_util(MSM_CCI_INIT).  Turning the torch on and then taking
	 * a photo was enough to hit this: the capture's flash write came back
	 * -EINVAL with "msm_cci_irq: MASTER_0 error 0x8000000" beside it.
	 *
	 * msm_cci_init() is reference counted (cci_dev->ref_count), so taking
	 * it here nests safely inside a session that already holds it.
	 */
	if (!flash_ctrl->dt_cci_inited) {
		cci_rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_util(
			&flash_ctrl->flash_i2c_client, MSM_CCI_INIT);
		if (cci_rc < 0) {
			pr_err("%s:%d cci_init failed rc %d\n", __func__,
				__LINE__, cci_rc);
			return cci_rc;
		}
		flash_ctrl->dt_cci_inited = 1;
	}

	if (!flash_data->cfg.settings) {
		/*
		 * No sequence from userspace.  Not necessarily an error: the
		 * HAL's torch path (QCameraFlash) opens this subdev directly
		 * and sends a current with cfg.settings NULL, because for a
		 * PMIC flash a current is all the kernel needs.  An I2C part
		 * needs registers, so fall back to the ones the board
		 * described in the device tree.
		 */
		if (cfg_type <= CFG_FLASH_HIGH &&
			flash_ctrl->dt_setting_valid[cfg_type]) {
			struct msm_camera_i2c_reg_setting_array *a =
				&flash_ctrl->dt_setting[cfg_type];
			uint16_t i;

			CDBG("%s:%d using the device tree sequence for cfg_type %d\n",
				__func__, __LINE__, cfg_type);

			/*
			 * One register at a time rather than a burst.  The
			 * burst form was returning 0 while changing nothing in
			 * the part -- a silent success, which is what made this
			 * hard to find.  The cause was almost certainly
			 * flash_i2c_client.addr_type being left at 0, now set
			 * at probe (msm_camera_cci_i2c_write_table() validates
			 * write_setting->addr_type but then transfers with
			 * client->addr_type); the single-write form is simply
			 * what was verified against a read-back.  These
			 * sequences are a handful of bytes to non-adjacent
			 * registers, so there is nothing to gain from a burst.
			 */
			for (i = 0; i < a->size; i++) {
				rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write(
					&flash_ctrl->flash_i2c_client,
					a->reg_setting_a[i].reg_addr,
					a->reg_setting_a[i].reg_data,
					a->data_type);
				if (rc < 0) {
					pr_err("%s:%d write %02x = %02x failed rc %d\n",
						__func__, __LINE__,
						a->reg_setting_a[i].reg_addr,
						a->reg_setting_a[i].reg_data,
						rc);
					break;
				}
			}
		} else {
			pr_err("%s:%d failed: no settings from userspace and none in DT for cfg_type %d\n",
				__func__, __LINE__, cfg_type);
			rc = -EFAULT;
		}
		goto release;
	}

	settings = kzalloc(sizeof(struct msm_camera_i2c_reg_setting_array),
		GFP_KERNEL);
	if (!settings) {
		pr_err("%s mem allocation failed %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto release;
	}

	if (copy_from_user(settings, (void *)flash_data->cfg.settings,
		sizeof(struct msm_camera_i2c_reg_setting_array))) {
		pr_err("%s copy_from_user failed %d\n", __func__, __LINE__);
		kfree(settings);
		rc = -EFAULT;
		goto release;
	}

	rc = msm_flash_i2c_write_table(flash_ctrl, settings);
	kfree(settings);

	if (rc < 0)
		pr_err("%s:%d msm_flash_i2c_write_table rc = %d failed\n",
			__func__, __LINE__, rc);

release:
	/*
	 * Deliberately not released here.
	 *
	 * msm_cci_init()'s reference count is per CCI device, not per master,
	 * so dropping the last reference powers the whole block down -- master
	 * 1 and the streaming sensor with it.  Taking and releasing around
	 * every write killed the camera session outright
	 * (mm_camera_intf_error_close, CAMERA_ERROR_UNKNOWN).  Hold it from
	 * the first write until msm_flash_i2c_release(), which is what the
	 * power_up/power_down pair would have done.
	 */
	return rc;
}

static int32_t msm_flash_init(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	uint32_t i = 0;
	int32_t rc = -EFAULT;
	enum msm_flash_driver_type flash_driver_type = FLASH_DRIVER_DEFAULT;

	CDBG("Enter");

	if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
		pr_err("%s:%d Invalid flash state = %d",
			__func__, __LINE__, flash_ctrl->flash_state);
		return 0;
	}

	if (flash_data->cfg.flash_init_info->flash_driver_type ==
		FLASH_DRIVER_DEFAULT) {
		flash_driver_type = flash_ctrl->flash_driver_type;
		for (i = 0; i < MAX_LED_TRIGGERS; i++) {
			flash_data->flash_current[i] =
				flash_ctrl->flash_max_current[i];
			flash_data->flash_duration[i] =
				flash_ctrl->flash_max_duration[i];
		}
	} else if (flash_data->cfg.flash_init_info->flash_driver_type ==
		flash_ctrl->flash_driver_type) {
		flash_driver_type = flash_ctrl->flash_driver_type;
		for (i = 0; i < MAX_LED_TRIGGERS; i++) {
			flash_ctrl->flash_max_current[i] =
				flash_data->flash_current[i];
			flash_data->flash_duration[i] =
				flash_ctrl->flash_max_duration[i];
		}
	}

	if (flash_driver_type == FLASH_DRIVER_DEFAULT) {
		pr_err("%s:%d invalid flash_driver_type", __func__, __LINE__);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(flash_table); i++) {
		if (flash_driver_type == flash_table[i]->flash_driver_type) {
			flash_ctrl->func_tbl = &flash_table[i]->func_tbl;
			rc = 0;
		}
	}

	if (rc < 0) {
		pr_err("%s:%d failed invalid flash_driver_type %d\n",
			__func__, __LINE__,
			flash_data->cfg.flash_init_info->flash_driver_type);
	}

	rc = flash_ctrl->func_tbl->camera_flash_init(
			flash_ctrl, flash_data);
	if (rc < 0) {
		pr_err("%s:%d camera_flash_init failed rc = %d",
			__func__, __LINE__, rc);
		return rc;
	}

	flash_ctrl->flash_state = MSM_CAMERA_FLASH_INIT;

	CDBG("Exit");
	return 0;
}

static int32_t msm_flash_init_prepare(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
#ifdef CONFIG_COMPAT
	struct msm_flash_cfg_data_t flash_data_k;
	struct msm_flash_init_info_t flash_init_info;
	int32_t i = 0;
	if(!is_compat_task()) {
		/*for 64-bit usecase,it need copy the data to local memory*/
		flash_data_k.cfg_type = flash_data->cfg_type;
		for (i = 0; i < MAX_LED_TRIGGERS; i++) {
			flash_data_k.flash_current[i] =
				flash_data->flash_current[i];
			flash_data_k.flash_duration[i] =
				flash_data->flash_duration[i];
		}

		flash_data_k.cfg.flash_init_info = &flash_init_info;
		if (copy_from_user(&flash_init_info,
			(void *)(flash_data->cfg.flash_init_info),
			sizeof(struct msm_flash_init_info_t))) {
			pr_err("%s copy_from_user failed %d\n",
				__func__, __LINE__);
			return -EFAULT;
		}
		return msm_flash_init(flash_ctrl, &flash_data_k);
	}
	/*
	 * for 32-bit usecase,it already copy the userspace
	 * data to local memory in msm_flash_subdev_do_ioctl()
	 * so here do not need copy from user
	 */
	return msm_flash_init(flash_ctrl, flash_data);
#else
	struct msm_flash_cfg_data_t flash_data_k;
	struct msm_flash_init_info_t flash_init_info;
	int32_t i = 0;
	flash_data_k.cfg_type = flash_data->cfg_type;
	for (i = 0; i < MAX_LED_TRIGGERS; i++) {
		flash_data_k.flash_current[i] =
			flash_data->flash_current[i];
		flash_data_k.flash_duration[i] =
			flash_data->flash_duration[i];
	}

	flash_data_k.cfg.flash_init_info = &flash_init_info;
	if (copy_from_user(&flash_init_info,
			   (void *)(flash_data->cfg.flash_init_info),
			   sizeof(struct msm_flash_init_info_t))) {
		pr_err("%s copy_from_user failed %d\n",
		       __func__, __LINE__);
		return -EFAULT;
	}
	return msm_flash_init(flash_ctrl, &flash_data_k);
#endif
}

static int32_t msm_flash_low(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	uint32_t curr = 0, max_current = 0;
	int32_t i = 0;

	CDBG("Enter\n");
	/* Turn off flash triggers */
	for (i = 0; i < flash_ctrl->flash_num_sources; i++)
		if (flash_ctrl->flash_trigger[i])
			led_trigger_event(flash_ctrl->flash_trigger[i], 0);

	/* Turn on flash triggers */
	for (i = 0; i < flash_ctrl->torch_num_sources; i++) {
		if (flash_ctrl->torch_trigger[i]) {
			max_current = flash_ctrl->torch_max_current[i];
			if (flash_data->flash_current[i] >= 0 &&
				flash_data->flash_current[i] <
				max_current) {
				curr = flash_data->flash_current[i];
			} else {
				curr = flash_ctrl->torch_op_current[i];
				pr_debug("LED current clamped to %d\n",
					curr);
			}
			CDBG("low_flash_current[%d] = %d", i, curr);
			led_trigger_event(flash_ctrl->torch_trigger[i],
				curr);
		}
	}
	if (flash_ctrl->switch_trigger)
		led_trigger_event(flash_ctrl->switch_trigger, 1);
	CDBG("Exit\n");
	return 0;
}

static int32_t msm_flash_high(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t curr = 0;
	int32_t max_current = 0;
	int32_t i = 0;

	/* Turn off torch triggers */
	for (i = 0; i < flash_ctrl->torch_num_sources; i++)
		if (flash_ctrl->torch_trigger[i])
			led_trigger_event(flash_ctrl->torch_trigger[i], 0);

	/* Turn on flash triggers */
	for (i = 0; i < flash_ctrl->flash_num_sources; i++) {
		if (flash_ctrl->flash_trigger[i]) {
			max_current = flash_ctrl->flash_max_current[i];
			if (flash_data->flash_current[i] >= 0 &&
				flash_data->flash_current[i] <
				max_current) {
				curr = flash_data->flash_current[i];
			} else {
				curr = flash_ctrl->flash_op_current[i];
				pr_debug("LED flash_current[%d] clamped %d\n",
					i, curr);
			}
			CDBG("high_flash_current[%d] = %d", i, curr);
			led_trigger_event(flash_ctrl->flash_trigger[i],
				curr);
		}
	}
	if (flash_ctrl->switch_trigger)
		led_trigger_event(flash_ctrl->switch_trigger, 1);
	return 0;
}

static int32_t msm_flash_release(
	struct msm_flash_ctrl_t *flash_ctrl)
{
	int32_t rc = 0;
	if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_RELEASE) {
		pr_err("%s:%d Invalid flash state = %d",
			__func__, __LINE__, flash_ctrl->flash_state);
		return 0;
	}

	rc = flash_ctrl->func_tbl->camera_flash_off(flash_ctrl, NULL);
	if (rc < 0) {
		pr_err("%s:%d camera_flash_init failed rc = %d",
			__func__, __LINE__, rc);
		return rc;
	}
	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;
	return 0;
}

static int32_t msm_flash_config(struct msm_flash_ctrl_t *flash_ctrl,
	void __user *argp)
{
	int32_t rc = -EINVAL;
	struct msm_flash_cfg_data_t *flash_data =
		(struct msm_flash_cfg_data_t *) argp;

	mutex_lock(flash_ctrl->flash_mutex);

	CDBG("Enter %s type %d\n", __func__, flash_data->cfg_type);

	if (flash_dbg)
		pr_info("CFG %s(%d) state %d\n",
			msm_flash_cfg_name(flash_data->cfg_type),
			flash_data->cfg_type, flash_ctrl->flash_state);

	switch (flash_data->cfg_type) {
	case CFG_FLASH_INIT:
		msm_flash_wd_cancel();
		rc = msm_flash_init_prepare(flash_ctrl, flash_data);
		break;
	case CFG_FLASH_RELEASE:
		msm_flash_wd_cancel();
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT)
			rc = flash_ctrl->func_tbl->camera_flash_release(
				flash_ctrl);
		break;
	case CFG_FLASH_OFF:
		msm_flash_wd_cancel();
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT)
			rc = flash_ctrl->func_tbl->camera_flash_off(
				flash_ctrl, flash_data);
		break;
	case CFG_FLASH_LOW:
		msm_flash_wd_cancel();
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT)
			rc = flash_ctrl->func_tbl->camera_flash_low(
				flash_ctrl, flash_data);
		break;
	case CFG_FLASH_HIGH:
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
			/*
			 * Frame-synchronised: arm now, write the sequence from
			 * the SOF of the frame the capture lands on.  One
			 * pulse, at the exposure -- see flash_sof_sync above.
			 *
			 * The heat load charged here is a single pulse, not a
			 * train: Hill's duration field caps at ~124 ms and the
			 * sequence fires once, so 150 ms covers it with margin.
			 */
			if (flash_sof_sync && flash_data->cfg.settings) {
				if (!msm_flash_load_charge(150)) {
					rc = 0;	/* cooling down; no flash */
					break;
				}
				memcpy(&flash_sync_setting,
					flash_data->cfg.settings,
					sizeof(flash_sync_setting));
				flash_sync_ctrl = flash_ctrl;
				flash_sync_frame = 0;
				WRITE_ONCE(flash_sync_ready, false);
				atomic_set(&flash_sync_armed, 1);
				mod_delayed_work(system_wq, &flash_sync_fb_work,
					msecs_to_jiffies(flash_sof_fallback_ms));
				flash_wd_ctrl = flash_ctrl;
				if (flash_high_timeout_ms > 0)
					mod_delayed_work(system_wq,
						&flash_wd_work,
						msecs_to_jiffies(
						  flash_sof_fallback_ms +
						  flash_high_timeout_ms));
				if (flash_dbg)
					pr_err("msm_flash: armed at sof %d, last capture request frame %u %u ms ago\n",
						flash_sof_count,
						flash_last_req_frame,
						jiffies_to_msecs(jiffies -
						  flash_last_req_jiffies));
				rc = 0;
				break;
			}
			rc = flash_ctrl->func_tbl->camera_flash_high(
				flash_ctrl, flash_data);
			/* arm the software safety timeout -- see above */
			if (rc >= 0 && flash_high_timeout_ms > 0) {
				flash_wd_ctrl = flash_ctrl;
				mod_delayed_work(system_wq, &flash_wd_work,
					msecs_to_jiffies(flash_high_timeout_ms));
			}
			/*
			 * ...and, if asked, keep re-issuing it so a one-shot
			 * strobe is still lit when the snapshot is exposed
			 * 300-400 ms from now.  Cache the sequence: the NULL
			 * fallback in msm_flash_i2c_write_setting_array() uses
			 * the DEVICE TREE setting, not the one userspace just
			 * supplied, so replaying it needs a copy.
			 */
			if (rc >= 0 && flash_high_repeat_ms > 0 &&
				flash_high_timeout_ms > 0 &&
				flash_data->cfg.settings &&
				msm_flash_load_charge(msm_flash_train_ms())) {
				flash_wd_ctrl = flash_ctrl;
				memcpy(&flash_rep_setting,
					flash_data->cfg.settings,
					sizeof(flash_rep_setting));
				flash_rep_valid = true;
				flash_rep_deadline = jiffies +
					msecs_to_jiffies(msm_flash_train_ms());
				mod_delayed_work(system_wq, &flash_rep_work,
					msecs_to_jiffies(flash_high_repeat_ms));
			}
		}
		break;
	default:
		rc = -EFAULT;
		break;
	}

	mutex_unlock(flash_ctrl->flash_mutex);

	if (flash_dbg)
		pr_info("CFG %s(%d) -> rc %d\n",
			msm_flash_cfg_name(flash_data->cfg_type),
			flash_data->cfg_type, rc);

	CDBG("Exit %s type %d\n", __func__, flash_data->cfg_type);

	return rc;
}

static long msm_flash_subdev_ioctl(struct v4l2_subdev *sd,
	unsigned int cmd, void *arg)
{
	struct msm_flash_ctrl_t *fctrl = NULL;
	void __user *argp = (void __user *)arg;

	CDBG("Enter\n");

	if (!sd) {
		pr_err("sd NULL\n");
		return -EINVAL;
	}
	fctrl = v4l2_get_subdevdata(sd);
	if (!fctrl) {
		pr_err("fctrl NULL\n");
		return -EINVAL;
	}
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_flash_get_subdev_id(fctrl, argp);
	case VIDIOC_MSM_FLASH_CFG:
		return msm_flash_config(fctrl, argp);
	case MSM_SD_SHUTDOWN:
		if (!fctrl->func_tbl) {
			pr_err("fctrl->func_tbl NULL\n");
			return -EINVAL;
		} else {
			*(int *)argp = CFG_FLASH_RELEASE;
			return msm_flash_config(fctrl, argp);
		}
	default:
		pr_err_ratelimited("invalid cmd %d\n", cmd);
		return -ENOIOCTLCMD;
	}
	CDBG("Exit\n");
}

static struct v4l2_subdev_core_ops msm_flash_subdev_core_ops = {
	.ioctl = msm_flash_subdev_ioctl,
};

static struct v4l2_subdev_ops msm_flash_subdev_ops = {
	.core = &msm_flash_subdev_core_ops,
};

static const struct v4l2_subdev_internal_ops msm_flash_internal_ops;

static int32_t msm_flash_get_gpio_dt_data(struct device_node *of_node,
		struct msm_flash_ctrl_t *fctrl)
{
	int32_t rc = 0, i = 0;
	uint16_t *gpio_array = NULL;
	int16_t gpio_array_size = 0;
	struct msm_camera_gpio_conf *gconf = NULL;

	gpio_array_size = of_gpio_count(of_node);
	CDBG("%s gpio count %d\n", __func__, gpio_array_size);

	if (gpio_array_size > 0) {
		fctrl->power_info.gpio_conf =
			 kzalloc(sizeof(struct msm_camera_gpio_conf),
				 GFP_KERNEL);
		if (!fctrl->power_info.gpio_conf) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -ENOMEM;
			return rc;
		}
		gconf = fctrl->power_info.gpio_conf;

		gpio_array = kzalloc(sizeof(uint16_t) * gpio_array_size,
			GFP_KERNEL);
		if (!gpio_array) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -ENOMEM;
			goto free_gpio_conf;
		}
		for (i = 0; i < gpio_array_size; i++) {
			gpio_array[i] = of_get_gpio(of_node, i);
			if (((int16_t)gpio_array[i]) < 0) {
				pr_err("%s failed %d\n", __func__, __LINE__);
				rc = -EINVAL;
				goto free_gpio_array;
			}
			CDBG("%s gpio_array[%d] = %d\n", __func__, i,
				gpio_array[i]);
		}

		rc = msm_camera_get_dt_gpio_req_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto free_gpio_array;
		}

		rc = msm_camera_get_dt_gpio_set_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto free_cam_gpio_req_tbl;
		}

		rc = msm_camera_init_gpio_pin_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto free_cam_gpio_set_tbl;
		}
		if (fctrl->flash_driver_type == FLASH_DRIVER_DEFAULT)
			fctrl->flash_driver_type = FLASH_DRIVER_GPIO;
		CDBG("%s:%d fctrl->flash_driver_type = %d", __func__, __LINE__,
			fctrl->flash_driver_type);
	}

	return 0;

free_cam_gpio_set_tbl:
	kfree(gconf->cam_gpio_set_tbl);
free_cam_gpio_req_tbl:
	kfree(gconf->cam_gpio_req_tbl);
free_gpio_array:
	kfree(gpio_array);
free_gpio_conf:
	kfree(fctrl->power_info.gpio_conf);
	return rc;
}

static int32_t msm_flash_get_pmic_source_info(
	struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl)
{
	int32_t rc = 0;
	uint32_t count = 0, i = 0;
	struct device_node *flash_src_node = NULL;
	struct device_node *torch_src_node = NULL;
	struct device_node *switch_src_node = NULL;

	switch_src_node = of_parse_phandle(of_node, "qcom,switch-source", 0);
	if (!switch_src_node) {
		CDBG("%s:%d switch_src_node NULL\n", __func__, __LINE__);
	} else {
		rc = of_property_read_string(switch_src_node,
			"qcom,default-led-trigger",
			&fctrl->switch_trigger_name);
		if (rc < 0) {
			rc = of_property_read_string(switch_src_node,
				"linux,default-trigger",
				&fctrl->switch_trigger_name);
			if (rc < 0)
				pr_err("default-trigger read failed\n");
		}
		of_node_put(switch_src_node);
		switch_src_node = NULL;
		if (!rc) {
			CDBG("switch trigger %s\n",
				fctrl->switch_trigger_name);
			led_trigger_register_simple(
				fctrl->switch_trigger_name,
				&fctrl->switch_trigger);
		}
	}

	if (of_get_property(of_node, "qcom,flash-source", &count)) {
		count /= sizeof(uint32_t);
		CDBG("count %d\n", count);
		if (count > MAX_LED_TRIGGERS) {
			pr_err("invalid count\n");
			return -EINVAL;
		}
		fctrl->flash_num_sources = count;
		CDBG("%s:%d flash_num_sources = %d",
			__func__, __LINE__, fctrl->flash_num_sources);
		for (i = 0; i < count; i++) {
			flash_src_node = of_parse_phandle(of_node,
				"qcom,flash-source", i);
			if (!flash_src_node) {
				pr_err("flash_src_node NULL\n");
				continue;
			}

			rc = of_property_read_string(flash_src_node,
				"qcom,default-led-trigger",
				&fctrl->flash_trigger_name[i]);
			if (rc < 0) {
				rc = of_property_read_string(flash_src_node,
					"linux,default-trigger",
					&fctrl->flash_trigger_name[i]);
				if (rc < 0) {
					pr_err("default-trigger read failed\n");
					of_node_put(flash_src_node);
					continue;
				}
			}

			CDBG("default trigger %s\n",
				fctrl->flash_trigger_name[i]);

			/* Read operational-current */
			rc = of_property_read_u32(flash_src_node,
				"qcom,current",
				&fctrl->flash_op_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(flash_src_node);
				continue;
			}

			/* Read max-current */
			rc = of_property_read_u32(flash_src_node,
				"qcom,max-current",
				&fctrl->flash_max_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(flash_src_node);
				continue;
			}

			of_node_put(flash_src_node);

			CDBG("max_current[%d] %d\n",
				i, fctrl->flash_op_current[i]);

			led_trigger_register_simple(
				fctrl->flash_trigger_name[i],
				&fctrl->flash_trigger[i]);
		}
		if (fctrl->flash_driver_type == FLASH_DRIVER_DEFAULT)
			fctrl->flash_driver_type = FLASH_DRIVER_PMIC;
		CDBG("%s:%d fctrl->flash_driver_type = %d", __func__, __LINE__,
			fctrl->flash_driver_type);
	}

	if (of_get_property(of_node, "qcom,torch-source", &count)) {
		count /= sizeof(uint32_t);
		CDBG("count %d\n", count);
		if (count > MAX_LED_TRIGGERS) {
			pr_err("invalid count\n");
			return -EINVAL;
		}
		fctrl->torch_num_sources = count;
		CDBG("%s:%d torch_num_sources = %d",
			__func__, __LINE__, fctrl->torch_num_sources);
		for (i = 0; i < count; i++) {
			torch_src_node = of_parse_phandle(of_node,
				"qcom,torch-source", i);
			if (!torch_src_node) {
				pr_err("torch_src_node NULL\n");
				continue;
			}

			rc = of_property_read_string(torch_src_node,
				"qcom,default-led-trigger",
				&fctrl->torch_trigger_name[i]);
			if (rc < 0) {
				rc = of_property_read_string(torch_src_node,
					"linux,default-trigger",
					&fctrl->torch_trigger_name[i]);
				if (rc < 0) {
					pr_err("default-trigger read failed\n");
					of_node_put(torch_src_node);
					continue;
				}
			}

			CDBG("default trigger %s\n",
				fctrl->torch_trigger_name[i]);

			/* Read operational-current */
			rc = of_property_read_u32(torch_src_node,
				"qcom,current",
				&fctrl->torch_op_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(torch_src_node);
				continue;
			}

			/* Read max-current */
			rc = of_property_read_u32(torch_src_node,
				"qcom,max-current",
				&fctrl->torch_max_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(torch_src_node);
				continue;
			}

			of_node_put(torch_src_node);

			CDBG("max_current[%d] %d\n",
				i, fctrl->torch_op_current[i]);

			led_trigger_register_simple(
				fctrl->torch_trigger_name[i],
				&fctrl->torch_trigger[i]);
		}
		if (fctrl->flash_driver_type == FLASH_DRIVER_DEFAULT)
			fctrl->flash_driver_type = FLASH_DRIVER_PMIC;
		CDBG("%s:%d fctrl->flash_driver_type = %d", __func__, __LINE__,
			fctrl->flash_driver_type);
	}

	return 0;
}

/*
 * Read one register sequence out of the device tree.
 *
 * The property is a flat list of (register, value) pairs as 16-bit cells:
 *
 *     qcom,flash-low-setting = /bits/ 16 <0x02 0x80 0x05 0x19 0x08 0x06>;
 *
 * Nothing here knows which part it is describing -- the board does.
 */
static int32_t msm_flash_get_dt_setting(struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl, const char *propname,
	enum msm_flash_cfg_type_t cfg_type, uint16_t addr_type,
	uint16_t data_type)
{
	struct msm_camera_i2c_reg_setting_array *array;
	uint16_t *cells;
	int32_t count, pairs, i, rc;

	if (!of_get_property(of_node, propname, &count))
		return 0;               /* absent is fine, not every board has one */

	count /= sizeof(uint16_t);
	if (count <= 0 || (count & 1)) {
		pr_err("%s: %s must be (reg, value) pairs, got %d cells\n",
			__func__, propname, count);
		return -EINVAL;
	}

	pairs = count / 2;
	if (pairs > MAX_I2C_REG_SET) {
		pr_err("%s: %s has %d pairs, max %d\n", __func__, propname,
			pairs, MAX_I2C_REG_SET);
		return -EINVAL;
	}

	cells = kzalloc(count * sizeof(uint16_t), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;

	rc = of_property_read_u16_array(of_node, propname, cells, count);
	if (rc < 0) {
		pr_err("%s: failed to read %s rc %d\n", __func__, propname, rc);
		kfree(cells);
		return rc;
	}

	array = &fctrl->dt_setting[cfg_type];
	for (i = 0; i < pairs; i++) {
		array->reg_setting_a[i].reg_addr = cells[2 * i];
		array->reg_setting_a[i].reg_data = cells[2 * i + 1];
		array->reg_setting_a[i].delay = 0;
	}
	array->size = pairs;
	array->addr_type = addr_type;
	array->data_type = data_type;
	array->delay = 0;
	fctrl->dt_setting_valid[cfg_type] = 1;

	CDBG("%s: %s -> %d pairs for cfg_type %d\n", __func__, propname, pairs,
		cfg_type);
	kfree(cells);
	return 0;
}

/*
 * Pick up a board-described I2C flash, if the device tree has one.
 *
 * Absent these properties the driver behaves exactly as before, so boards
 * whose flash is on the PMIC are unaffected.
 */
static int32_t msm_flash_get_dt_i2c_flash(struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl)
{
	uint32_t addr_type = MSM_CAMERA_I2C_BYTE_ADDR;
	uint32_t data_type = MSM_CAMERA_I2C_BYTE_DATA;
	int32_t rc;

	of_property_read_u32(of_node, "qcom,flash-slave-addr",
		&fctrl->dt_slave_addr);
	of_property_read_u32(of_node, "qcom,flash-addr-type", &addr_type);
	of_property_read_u32(of_node, "qcom,flash-data-type", &data_type);

	rc = msm_flash_get_dt_setting(of_node, fctrl, "qcom,flash-init-setting",
		CFG_FLASH_INIT, addr_type, data_type);
	if (rc < 0)
		return rc;
	rc = msm_flash_get_dt_setting(of_node, fctrl, "qcom,flash-off-setting",
		CFG_FLASH_OFF, addr_type, data_type);
	if (rc < 0)
		return rc;
	rc = msm_flash_get_dt_setting(of_node, fctrl, "qcom,flash-low-setting",
		CFG_FLASH_LOW, addr_type, data_type);
	if (rc < 0)
		return rc;
	rc = msm_flash_get_dt_setting(of_node, fctrl, "qcom,flash-high-setting",
		CFG_FLASH_HIGH, addr_type, data_type);
	return rc;
}

static int32_t msm_flash_get_dt_data(struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl)
{
	int32_t rc = 0;

	CDBG("called\n");

	if (!of_node) {
		pr_err("of_node NULL\n");
		return -EINVAL;
	}

	/* Read the sub device */
	rc = of_property_read_u32(of_node, "cell-index", &fctrl->pdev->id);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		return rc;
	}

	CDBG("subdev id %d\n", fctrl->subdev_id);

	fctrl->flash_driver_type = FLASH_DRIVER_DEFAULT;

	/* Read the CCI master. Use M0 if not available in the node */
	rc = of_property_read_u32(of_node, "qcom,cci-master",
		&fctrl->cci_i2c_master);
	CDBG("%s qcom,cci-master %d, rc %d\n", __func__, fctrl->cci_i2c_master,
		rc);
	if (rc < 0) {
		/* Set default master 0 */
		fctrl->cci_i2c_master = MASTER_0;
		rc = 0;
	} else {
		fctrl->flash_driver_type = FLASH_DRIVER_I2C;
	}

	/* Read the gpio information from device tree */
	rc = msm_flash_get_gpio_dt_data(of_node, fctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_gpio_dt_data failed rc %d\n",
			__func__, __LINE__, rc);
		return rc;
	}

	/* Read the flash and torch source info from device tree node */
	rc = msm_flash_get_pmic_source_info(of_node, fctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_pmic_source_info failed rc %d\n",
			__func__, __LINE__, rc);
		return rc;
	}

	/* Optional: a board-described I2C flash (slave address + sequences) */
	rc = msm_flash_get_dt_i2c_flash(of_node, fctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_dt_i2c_flash failed rc %d\n",
			__func__, __LINE__, rc);
		return rc;
	}
	return rc;
}

#ifdef CONFIG_COMPAT
static long msm_flash_subdev_do_ioctl(
	struct file *file, unsigned int cmd, void *arg)
{
	int32_t i = 0;
	int32_t rc = 0;
	struct video_device *vdev;
	struct v4l2_subdev *sd;
	struct msm_flash_cfg_data_t32 *u32;
	struct msm_flash_cfg_data_t flash_data;
	struct msm_flash_init_info_t32 flash_init_info32;
	struct msm_flash_init_info_t flash_init_info;

	CDBG("Enter");

	if (!file || !arg) {
		pr_err("%s:failed NULL parameter\n", __func__);
		return -EINVAL;
	}
	vdev = video_devdata(file);
	sd = vdev_to_v4l2_subdev(vdev);
	u32 = (struct msm_flash_cfg_data_t32 *)arg;

	flash_data.cfg_type = u32->cfg_type;
	for (i = 0; i < MAX_LED_TRIGGERS; i++) {
		flash_data.flash_current[i] = u32->flash_current[i];
		flash_data.flash_duration[i] = u32->flash_duration[i];
	}
	switch (cmd) {
	case VIDIOC_MSM_FLASH_CFG32:
		cmd = VIDIOC_MSM_FLASH_CFG;
		switch (flash_data.cfg_type) {
		case CFG_FLASH_OFF:
		case CFG_FLASH_LOW:
		case CFG_FLASH_HIGH:
			flash_data.cfg.settings = compat_ptr(u32->cfg.settings);
			break;
		case CFG_FLASH_INIT:
			flash_data.cfg.flash_init_info = &flash_init_info;
			if (copy_from_user(&flash_init_info32,
				(void *)compat_ptr(u32->cfg.flash_init_info),
				sizeof(struct msm_flash_init_info_t32))) {
				pr_err("%s copy_from_user failed %d\n",
					__func__, __LINE__);
				return -EFAULT;
			}
			flash_init_info.flash_driver_type =
				flash_init_info32.flash_driver_type;
			flash_init_info.slave_addr =
				flash_init_info32.slave_addr;
			flash_init_info.settings =
				compat_ptr(flash_init_info32.settings);
			flash_init_info.power_setting_array =
				compat_ptr(
				flash_init_info32.power_setting_array);
			break;
		default:
			break;
		}
		break;
	case VIDIOC_MSM_FLASH_CFG:
		pr_err("invalid cmd 0x%x received\n", cmd);
		return -EINVAL;
	default:
		return msm_flash_subdev_ioctl(sd, cmd, arg);
	}

	rc =  msm_flash_subdev_ioctl(sd, cmd, &flash_data);
	for (i = 0; i < MAX_LED_TRIGGERS; i++) {
		u32->flash_current[i] = flash_data.flash_current[i];
		u32->flash_duration[i] = flash_data.flash_duration[i];
	}
	CDBG("Exit");
	return rc;
}

static long msm_flash_subdev_fops_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_flash_subdev_do_ioctl);
}
#endif
static int32_t msm_flash_platform_probe(struct platform_device *pdev)
{
	int32_t rc = 0;
	struct msm_flash_ctrl_t *flash_ctrl = NULL;
	struct msm_camera_cci_client *cci_client = NULL;

	CDBG("Enter");
	if (!pdev->dev.of_node) {
		pr_err("of_node NULL\n");
		return -EINVAL;
	}

	flash_ctrl = kzalloc(sizeof(struct msm_flash_ctrl_t), GFP_KERNEL);
	if (!flash_ctrl) {
		pr_err("%s:%d failed no memory\n", __func__, __LINE__);
		return -ENOMEM;
	}

	memset(flash_ctrl, 0, sizeof(struct msm_flash_ctrl_t));

	flash_ctrl->pdev = pdev;

	rc = msm_flash_get_dt_data(pdev->dev.of_node, flash_ctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_dt_data failed\n",
			__func__, __LINE__);
		kfree(flash_ctrl);
		return -EINVAL;
	}

	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;
	flash_ctrl->power_info.dev = &flash_ctrl->pdev->dev;
	flash_ctrl->flash_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	flash_ctrl->flash_mutex = &msm_flash_mutex;
	flash_ctrl->flash_i2c_client.i2c_func_tbl = &msm_sensor_cci_func_tbl;
	flash_ctrl->flash_i2c_client.cci_client = kzalloc(
		sizeof(struct msm_camera_cci_client), GFP_KERNEL);
	if (!flash_ctrl->flash_i2c_client.cci_client) {
		kfree(flash_ctrl);
		pr_err("failed no memory\n");
		return -ENOMEM;
	}

	cci_client = flash_ctrl->flash_i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = flash_ctrl->cci_i2c_master;

	/*
	 * Give the CCI client its address now, if the board described one.
	 *
	 * Otherwise sid is only ever set inside msm_flash_i2c_init(), and that
	 * is not reached on the path that matters here: msm_flash_init()
	 * returns early whenever the flash is already in the INIT state, which
	 * it normally is because the camera daemon initialised it first.  The
	 * torch's own CFG_FLASH_INIT then does nothing, sid stays 0, and every
	 * subsequent write is addressed to I2C address 0 -- which CCI reports
	 * as success, so the sequence goes out, returns 0, and lights nothing.
	 */
	if (flash_ctrl->dt_slave_addr) {
		cci_client->sid = flash_ctrl->dt_slave_addr >> 1;
		cci_client->retries = 3;
		cci_client->id_map = 0;
		/*
		 * msm_camera_cci_i2c_write() takes no address width -- it
		 * reads client->addr_type, which this driver otherwise never
		 * sets, leaving it 0 (not a valid msm_camera_i2c_reg_addr_type).
		 */
		flash_ctrl->flash_i2c_client.addr_type =
			flash_ctrl->dt_setting[CFG_FLASH_LOW].addr_type ?
			flash_ctrl->dt_setting[CFG_FLASH_LOW].addr_type :
			MSM_CAMERA_I2C_BYTE_ADDR;
		CDBG("%s: sid 0x%x addr_type %d from device tree\n", __func__,
			cci_client->sid, flash_ctrl->flash_i2c_client.addr_type);
	}

	/* Initialize sub device */
	v4l2_subdev_init(&flash_ctrl->msm_sd.sd, &msm_flash_subdev_ops);
	v4l2_set_subdevdata(&flash_ctrl->msm_sd.sd, flash_ctrl);

	flash_ctrl->msm_sd.sd.internal_ops = &msm_flash_internal_ops;
	flash_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(flash_ctrl->msm_sd.sd.name,
		ARRAY_SIZE(flash_ctrl->msm_sd.sd.name),
		"msm_camera_flash");
	media_entity_init(&flash_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	flash_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	flash_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_FLASH;
	flash_ctrl->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x1;
	msm_sd_register(&flash_ctrl->msm_sd);

	CDBG("%s:%d flash sd name = %s", __func__, __LINE__,
		flash_ctrl->msm_sd.sd.entity.name);
	msm_flash_v4l2_subdev_fops = v4l2_subdev_fops;
#ifdef CONFIG_COMPAT
	msm_flash_v4l2_subdev_fops.compat_ioctl32 =
		msm_flash_subdev_fops_ioctl;
#endif
	flash_ctrl->msm_sd.sd.devnode->fops = &msm_flash_v4l2_subdev_fops;

	if (flash_ctrl->flash_driver_type == FLASH_DRIVER_PMIC)
		rc = msm_torch_create_classdev(pdev, flash_ctrl);

	CDBG("probe success\n");
	return rc;
}

MODULE_DEVICE_TABLE(of, msm_flash_dt_match);

static struct platform_driver msm_flash_platform_driver = {
	.probe = msm_flash_platform_probe,
	.driver = {
		.name = "qcom,camera-flash",
		.owner = THIS_MODULE,
		.of_match_table = msm_flash_dt_match,
	},
};

static int __init msm_flash_init_module(void)
{
	int32_t rc = 0;
	CDBG("Enter\n");
	rc = platform_driver_register(&msm_flash_platform_driver);
	if (rc)
		pr_err("platform probe for flash failed");

	return rc;
}

static void __exit msm_flash_exit_module(void)
{
	platform_driver_unregister(&msm_flash_platform_driver);
	return;
}

static struct msm_flash_table msm_pmic_flash_table = {
	.flash_driver_type = FLASH_DRIVER_PMIC,
	.func_tbl = {
		.camera_flash_init = msm_flash_off,
		.camera_flash_release = msm_flash_release,
		.camera_flash_off = msm_flash_off,
		.camera_flash_low = msm_flash_low,
		.camera_flash_high = msm_flash_high,
	},
};

static struct msm_flash_table msm_gpio_flash_table = {
	.flash_driver_type = FLASH_DRIVER_GPIO,
	.func_tbl = {
		.camera_flash_init = msm_flash_gpio_init,
		.camera_flash_release = msm_flash_release,
		.camera_flash_off = msm_flash_off,
		.camera_flash_low = msm_flash_low,
		.camera_flash_high = msm_flash_high,
	},
};

static struct msm_flash_table msm_i2c_flash_table = {
	.flash_driver_type = FLASH_DRIVER_I2C,
	.func_tbl = {
		.camera_flash_init = msm_flash_i2c_init,
		.camera_flash_release = msm_flash_i2c_release,
		.camera_flash_off = msm_flash_i2c_write_setting_array,
		.camera_flash_low = msm_flash_i2c_write_setting_array,
		.camera_flash_high = msm_flash_i2c_write_setting_array,
	},
};

module_init(msm_flash_init_module);
module_exit(msm_flash_exit_module);
MODULE_DESCRIPTION("MSM FLASH");
MODULE_LICENSE("GPL v2");
