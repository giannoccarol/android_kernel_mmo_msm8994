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
#ifndef MSM_FLASH_H
#define MSM_FLASH_H

#include <linux/leds.h>
#include <linux/platform_device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ioctl.h>
#include <media/msm_cam_sensor.h>
#include <soc/qcom/camera2.h>
#include "msm_camera_i2c.h"
#include "msm_sd.h"

#define DEFINE_MSM_MUTEX(mutexname) \
	static struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

enum msm_camera_flash_state_t {
	MSM_CAMERA_FLASH_INIT,
	MSM_CAMERA_FLASH_RELEASE,
};

struct msm_flash_ctrl_t;

struct msm_flash_func_t {
	int32_t (*camera_flash_init)(struct msm_flash_ctrl_t *,
		struct msm_flash_cfg_data_t *);
	int32_t (*camera_flash_release)(struct msm_flash_ctrl_t *);
	int32_t (*camera_flash_off)(struct msm_flash_ctrl_t *,
		struct msm_flash_cfg_data_t *);
	int32_t (*camera_flash_low)(struct msm_flash_ctrl_t *,
		struct msm_flash_cfg_data_t *);
	int32_t (*camera_flash_high)(struct msm_flash_ctrl_t *,
		struct msm_flash_cfg_data_t *);
};

struct msm_flash_table {
	enum msm_flash_driver_type flash_driver_type;
	struct msm_flash_func_t func_tbl;
};

struct msm_flash_reg_t {
	struct msm_camera_i2c_reg_setting *init_setting;
	struct msm_camera_i2c_reg_setting *off_setting;
	struct msm_camera_i2c_reg_setting *release_setting;
	struct msm_camera_i2c_reg_setting *low_setting;
	struct msm_camera_i2c_reg_setting *high_setting;
};

struct msm_flash_ctrl_t {
	struct msm_camera_i2c_client flash_i2c_client;
	struct msm_sd_subdev msm_sd;
	struct platform_device *pdev;
	struct msm_flash_func_t *func_tbl;
	struct msm_camera_power_ctrl_t power_info;

	/* Switch node to trigger led */
	const char *switch_trigger_name;
	struct led_trigger *switch_trigger;

	/* Flash */
	uint32_t flash_num_sources;
	const char *flash_trigger_name[MAX_LED_TRIGGERS];
	struct led_trigger *flash_trigger[MAX_LED_TRIGGERS];
	uint32_t flash_op_current[MAX_LED_TRIGGERS];
	uint32_t flash_max_current[MAX_LED_TRIGGERS];
	uint32_t flash_max_duration[MAX_LED_TRIGGERS];

	/* Torch */
	uint32_t torch_num_sources;
	const char *torch_trigger_name[MAX_LED_TRIGGERS];
	struct led_trigger *torch_trigger[MAX_LED_TRIGGERS];
	uint32_t torch_op_current[MAX_LED_TRIGGERS];
	uint32_t torch_max_current[MAX_LED_TRIGGERS];

	void *data;
	enum msm_camera_device_type_t flash_device_type;
	enum cci_i2c_master_t cci_i2c_master;
	uint32_t subdev_id;
	struct mutex *flash_mutex;
	struct msm_sensor_power_setting_array power_setting_array;

	/* flash driver type */
	enum msm_flash_driver_type flash_driver_type;

	/* flash state */
	enum msm_camera_flash_state_t flash_state;

	/*
	 * A self-describing I2C flash: slave address and register sequences
	 * read from the device tree, indexed by msm_flash_cfg_type_t.
	 *
	 * The userspace flash library only reaches this driver through the
	 * camera daemon.  The torch does not go that way -- QCameraFlash in
	 * the HAL opens this subdev directly and sends a current with
	 * cfg.settings NULL, which is all a PMIC flash needs and nothing an
	 * I2C part can use.  These let the board describe the part instead,
	 * so the driver stays generic and no chip is named in this file.
	 */
	uint32_t dt_slave_addr;
	struct msm_camera_i2c_reg_setting_array dt_setting[CFG_FLASH_HIGH + 1];
	uint8_t dt_setting_valid[CFG_FLASH_HIGH + 1];
	/* CCI was brought up here rather than by a power sequence */
	uint8_t dt_cci_inited;
	/* msm_camera_power_up() really ran, so power_down() may undo it */
	uint8_t dt_powered_up;
	/*
	 * Optional identity check (qcom,flash-id = <reg value>): nothing is
	 * written to the slave until that register has read back the value,
	 * so a board whose part differs from its description is left alone.
	 */
	uint8_t dt_id_valid;
	uint8_t dt_id_state;
	uint16_t dt_id_reg;
	uint16_t dt_id_val;
};

int msm_flash_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id);

int msm_flash_probe(struct platform_device *pdev, const void *data);

int32_t msm_flash_create_v4lsubdev(struct platform_device *pdev,
	void *data);
int32_t msm_led_i2c_flash_create_v4lsubdev(void *data);

int32_t msm_led_i2c_trigger_get_subdev_id(struct msm_flash_ctrl_t *fctrl,
	void *arg);

int32_t msm_led_i2c_trigger_config(struct msm_flash_ctrl_t *fctrl,
	void *data);

int msm_flash_led_init(struct msm_flash_ctrl_t *fctrl);
int msm_flash_led_release(struct msm_flash_ctrl_t *fctrl);
int msm_flash_led_off(struct msm_flash_ctrl_t *fctrl);
int msm_flash_led_low(struct msm_flash_ctrl_t *fctrl);
int msm_flash_led_high(struct msm_flash_ctrl_t *fctrl);
#endif
