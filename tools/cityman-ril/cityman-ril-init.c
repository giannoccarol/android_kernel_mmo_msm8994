/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional rild-only workaround for the absent LG VSS NV service. */
#include <android/log.h>
#include <dlfcn.h>

#include "cityman-detect.h"

int qcci_qmi_vs_nv_init(void)
{
	typedef int (*init_fn)(void);
	init_fn original;

	if (is_cityman()) {
		/* The original library returns -1 after its absent-service timeout.
		 * Preserve that result, rather than pretending LG NV is initialized.
		 */
		__android_log_print(ANDROID_LOG_INFO, "CitymanRIL",
			"LG VSS NV service absent on Cityman; returning -1 without waiting");
		return -1;
	}
	original = (init_fn)dlsym(RTLD_NEXT, "qcci_qmi_vs_nv_init");
	return original ? original() : -1;
}
