/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared Cityman check for the RIL helpers.
 *
 * Works for the radio UID: /proc/cmdline and the device tree are not
 * readable there, but boot properties and soc_id are. A Cityman ROM sets
 * androidboot.hardware=cityman; a ROM built for Talkman but running on a
 * Cityman (MSM8994, soc_id 207, DTB index 1) is recognised as well.
 */
#ifndef CITYMAN_DETECT_H
#define CITYMAN_DETECT_H

#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>

#ifndef CITYMAN_SOC_ID_PATH
#define CITYMAN_SOC_ID_PATH "/sys/devices/soc0/soc_id"
#endif

#define CITYMAN_MSM8994_SOC_ID	207

static inline int cityman_property_is(const char *key, const char *want)
{
	char value[PROP_VALUE_MAX] = { 0 };

	if (__system_property_get(key, value) <= 0)
		return 0;
	return strcmp(value, want) == 0;
}

static inline int is_cityman(void)
{
	unsigned int soc = 0;
	char extra;
	int fields;
	FILE *file;

	if (cityman_property_is("ro.boot.hardware", "cityman"))
		return 1;

	file = fopen(CITYMAN_SOC_ID_PATH, "r");
	if (!file)
		return 0;
	fields = fscanf(file, "%u %c", &soc, &extra);
	fclose(file);
	if (fields != 1 || soc != CITYMAN_MSM8994_SOC_ID)
		return 0;

	return cityman_property_is("ro.boot.dtb_idx", "1");
}

#endif /* CITYMAN_DETECT_H */
