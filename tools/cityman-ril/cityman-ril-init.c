/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional rild-only workaround for the absent LG VSS NV service. */
#include <android/log.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>

#ifndef CITYMAN_SOC_ID_PATH
#define CITYMAN_SOC_ID_PATH "/sys/devices/soc0/soc_id"
#endif

static int is_cityman(void)
{
	char index[PROP_VALUE_MAX] = {0}, extra;
	unsigned soc = 0;
	FILE *file = fopen(CITYMAN_SOC_ID_PATH, "r");
	if (!file)
		return 0;
	int fields = fscanf(file, "%u %c", &soc, &extra);
	fclose(file);
	if (fields != 1 || soc != 207)
		return 0;
	/* Boot properties are readable by rild's radio UID; /proc/cmdline is not. */
	if (__system_property_get("ro.boot.dtb_idx", index) <= 0)
		return 0;
	return strcmp(index, "1") == 0;
}

int qcci_qmi_vs_nv_init(void)
{
	typedef int (*init_fn)(void);
	if (is_cityman()) {
		/* The original library returns -1 after its absent-service timeout.
		 * Preserve that result, rather than pretending LG NV is initialized.
		 */
		__android_log_print(ANDROID_LOG_INFO, "CitymanRIL",
			"LG VSS NV service absent on Cityman; returning -1 without waiting");
		return -1;
	}
	init_fn original = (init_fn)dlsym(RTLD_NEXT, "qcci_qmi_vs_nv_init");
	return original ? original() : -1;
}
