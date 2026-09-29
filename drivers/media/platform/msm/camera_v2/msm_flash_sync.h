/* Frame-synchronised flash trigger.
 *
 * Windows fires this phone's flash exactly once, timed against the frame: the
 * sensor driver computes the strobe length as `exposure_time + readout_time`
 * and issues the trigger through the CCI hardware sequencer
 * (rearsensor.sys, SMIApp_UpdateFlashConfigAndTriggerCciHw).  Nothing polls
 * and nothing repeats, which is why it never flickers.
 *
 * The camera daemon on this port issues CFG_FLASH_HIGH about 300-400 ms
 * before the snapshot is exposed, and Hill's own duration field is five bits
 * (max ~124 ms), so a pulse fired when the ioctl arrives is always dark by the
 * time the frame is taken.  These two hooks let msm_flash defer the trigger to
 * the frame that the capture actually lands on, which is the same result
 * Windows gets from the CCI sequencer.
 *
 * Both are no-ops unless a flash is armed, and both are safe to call from the
 * ISP's interrupt path -- they only record state and schedule work.
 */
#ifndef __MSM_FLASH_SYNC_H__
#define __MSM_FLASH_SYNC_H__

#include <linux/types.h>

/* A controllable-output frame request was queued for this frame. */
void msm_flash_capture_request(uint32_t frame_id);

/* Start of frame. */
void msm_flash_sof_notify(uint32_t frame_id);

#endif
