/* SPDX-License-Identifier: GPL-2.0-only */
/* Scoped userspace test: correct the SoC property only for the launched process. */
extern void *dlsym(void *, const char *);
typedef int (*property_get_fn)(const char *, char *, const char *);

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static int copy(char *out, const char *in)
{
	int n = 0;
	if (!in) { out[0] = 0; return 0; }
	while (n < 91 && in[n]) { out[n] = in[n]; n++; }
	out[n] = 0;
	return n;
}

int property_get(const char *key, char *value, const char *fallback)
{
	static property_get_fn real_get;
	if (same(key, "ro.board.platform"))
		return copy(value, "msm8994");
	if (!real_get)
		real_get = (property_get_fn)dlsym((void *)-1L, "property_get");
	return real_get ? real_get(key, value, fallback) : copy(value, fallback);
}
