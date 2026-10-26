/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Scoped userspace shim: report ro.board.platform as msm8994 to the process
 * it is preloaded into (qmuxd, netmgrd, rild and the UIM helper), so the
 * vendor data services pick the MSM8994 configuration on Cityman.
 *
 * Both the libcutils property_get() and the libc __system_property_get()
 * entry points are covered. Readers that go through
 * __system_property_find()/__system_property_read_callback() (for example
 * android::base::GetProperty) are not: none of the preloaded vendor
 * binaries is known to use them for this key.
 */
#include <dlfcn.h>

#define PLATFORM_KEY	"ro.board.platform"
#define PLATFORM_VALUE	"msm8994"
#define VALUE_MAX	92	/* PROP_VALUE_MAX, including the terminator */

typedef int (*property_get_fn)(const char *, char *, const char *);
typedef int (*system_property_get_fn)(const char *, char *);

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int copy(char *out, const char *in)
{
	int n = 0;

	if (!in) {
		out[0] = 0;
		return 0;
	}
	while (n < VALUE_MAX - 1 && in[n]) {
		out[n] = in[n];
		n++;
	}
	out[n] = 0;
	return n;
}

int property_get(const char *key, char *value, const char *fallback)
{
	static property_get_fn real_get;

	if (key && same(key, PLATFORM_KEY))
		return copy(value, PLATFORM_VALUE);
	if (!real_get)
		real_get = (property_get_fn)dlsym(RTLD_NEXT, "property_get");
	return real_get ? real_get(key, value, fallback) : copy(value, fallback);
}

int __system_property_get(const char *key, char *value)
{
	static system_property_get_fn real_get;

	if (key && same(key, PLATFORM_KEY))
		return copy(value, PLATFORM_VALUE);
	if (!real_get)
		real_get = (system_property_get_fn)dlsym(RTLD_NEXT,
							 "__system_property_get");
	if (real_get)
		return real_get(key, value);
	value[0] = 0;
	return 0;
}
