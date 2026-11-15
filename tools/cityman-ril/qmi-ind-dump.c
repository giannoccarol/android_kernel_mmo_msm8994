/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qmi-ind-dump: print the raw (still encoded) QMI indications of one service,
 * through qmuxd and libqmi_cci, for a number of seconds. Diagnostics only: it
 * never decodes, so it shows what the modem sends even when the vendor IDL
 * cannot decode it (qcril "Indication decode failed for msg N").
 *
 *   qmi-ind-dump SERVICE_OBJECT SECONDS [MSG_ID [REQ_ID REQ_TLV_HEX]]
 *
 * MSG_ID (0 = all) filters the indications. REQ_ID/REQ_TLV_HEX send one raw
 * request first on the same client (for example an indication registration).
 * Card and subscriber identifiers can appear in the payload: share the output
 * privately.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*ind_fn)(void *, unsigned int, void *, unsigned int, void *);
typedef int (*init_fn)(void *, unsigned int, ind_fn, void *, void *, unsigned int, void **);
typedef int (*raw_fn)(void *, unsigned int, void *, unsigned int, void *, unsigned int,
		      unsigned int *, unsigned int);
typedef int (*release_fn)(void *);

static unsigned int filter;

static void on_ind(void *client, unsigned int msg, void *buf, unsigned int len, void *data)
{
	const unsigned char *p = buf;
	struct timespec ts;
	unsigned int i;

	(void)client;
	(void)data;
	if (filter && msg != filter)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	printf("%ld.%03ld ind 0x%04x len %u:", (long)ts.tv_sec, ts.tv_nsec / 1000000, msg, len);
	for (i = 0; i < len; i++)
		printf("%s%02x", i % 32 ? " " : "\n  ", p[i]);
	printf("\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	union {
		long double align;
		unsigned char bytes[1024];
	} params = { 0 };
	unsigned char request[256], response[2048];
	unsigned int request_len = 0, response_len = 0, i;
	void *cci, *services, *object, *client = NULL;
	unsigned long seconds;
	init_fn init;
	raw_fn raw;
	release_fn release;
	int ret;

	if (argc != 3 && argc != 4 && argc != 6) {
		fprintf(stderr, "usage: %s SERVICE_OBJECT SECONDS [MSG_ID [REQ_ID REQ_TLV_HEX]]\n",
			argv[0]);
		return 2;
	}
	seconds = strtoul(argv[2], NULL, 10);
	if (!seconds || seconds > 3600)
		return 2;
	if (argc >= 4)
		filter = strtoul(argv[3], NULL, 0);

	cci = dlopen("/vendor/lib64/libqmi_cci.so", RTLD_NOW | RTLD_GLOBAL);
	services = dlopen("/vendor/lib64/libqmiservices.so", RTLD_NOW | RTLD_GLOBAL);
	if (!cci || !services) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	object = dlsym(services, argv[1]);
	init = (init_fn)dlsym(cci, "qmi_client_init_instance");
	raw = (raw_fn)dlsym(cci, "qmi_client_send_raw_msg_sync");
	release = (release_fn)dlsym(cci, "qmi_client_release");
	if (!object || !init || !raw || !release) {
		fprintf(stderr, "missing symbol\n");
		return 1;
	}
	ret = init(object, 0xffff, on_ind, NULL, &params, 8000, &client);
	printf("client_init=%d\n", ret);
	fflush(stdout);
	if (ret)
		return 1;

	if (argc == 6) {
		size_t n = strlen(argv[5]);

		if (n % 2 || n > sizeof(request) * 2)
			return 2;
		for (i = 0; i < n; i += 2) {
			char pair[3] = { argv[5][i], argv[5][i + 1], 0 };

			request[request_len++] = (unsigned char)strtoul(pair, NULL, 16);
		}
		ret = raw(client, strtoul(argv[4], NULL, 0), request, request_len, response,
			  sizeof(response), &response_len, 10000);
		printf("request=%d bytes=%u:", ret, response_len);
		for (i = 0; i < response_len && i < sizeof(response); i++)
			printf(" %02x", response[i]);
		printf("\n");
		fflush(stdout);
	}

	sleep(seconds);
	release(client);
	return 0;
}
