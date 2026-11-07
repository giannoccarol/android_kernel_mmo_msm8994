/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cityman-ims-profile: let Android bring up the IMS PDN on the Windows modem.
 *
 * The Lumia MPSS ships a 3GPP data profile for the "ims" APN with PDP type
 * IPv4. Android asks for the IMS APN as IPv6 (or IPv4v6), and the vendor
 * profile lookup (libqdp, inside rild) refuses an existing modem profile
 * whose IP type does not match: SETUP_DATA_CALL fails before reaching the
 * network. Iliad, for one, only accepts IPv6 on that PDN (ESM cause #51).
 *
 * This one-shot helper sets every 3GPP profile whose APN is "ims" to IPv4v6,
 * which the lookup accepts for IPv4, IPv6 and IPv4v6 requests. It only writes
 * when the type differs, so the modem profile is changed once, not at every
 * boot. Nothing else in the profile is touched. It talks to qmuxd through
 * libqmi_cci like cityman-uim-bootstrap.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <android/log.h>

#include "cityman-detect.h"

#define TAG "CitymanIMS"
#define LOG(...)							\
	do {								\
		__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); \
		fprintf(stderr, __VA_ARGS__);				\
		fputc('\n', stderr);					\
	} while (0)

#define QMI_WDS_MODIFY_PROFILE_SETTINGS	0x0028
#define QMI_WDS_GET_PROFILE_LIST	0x002a
#define QMI_WDS_GET_PROFILE_SETTINGS	0x002b

#define TLV_RESULT		0x02
#define TLV_PROFILE_ID		0x01	/* request: type, index */
#define TLV_PROFILE_LIST	0x01	/* response */
#define TLV_PDP_TYPE		0x11
#define TLV_APN			0x14

#define PROFILE_3GPP		0
#define PDP_IPV4		0
#define PDP_PPP			1
#define PDP_IPV4V6		3

#define IMS_APN			"ims"
#define MAX_PROFILES		32
#define INIT_TRIES		24	/* 8 s timeout each, plus 5 s pauses */

typedef int (*qmi_init_fn)(void *, unsigned int, void *, void *, void *,
			   unsigned int, void **);
typedef int (*qmi_raw_fn)(void *, unsigned int, void *, unsigned int, void *,
			  unsigned int, unsigned int *, unsigned int);
typedef int (*qmi_release_fn)(void *);

static qmi_raw_fn qmi_send_raw;
static uint8_t response[8192];
static unsigned int response_len;

static unsigned int le16(const uint8_t *p)
{
	return p[0] | ((unsigned int)p[1] << 8);
}

static const uint8_t *find_tlv(unsigned int tag, unsigned int *len)
{
	unsigned int i = 0;

	while (i + 3 <= response_len) {
		unsigned int n = le16(response + i + 1);

		if (n > response_len - i - 3)
			return NULL;
		if (response[i] == tag) {
			*len = n;
			return response + i + 3;
		}
		i += n + 3;
	}
	return NULL;
}

/* 0 only when the transport and the QMI result both succeed. */
static int request(void *client, unsigned int msg, const void *data,
		   unsigned int len)
{
	const uint8_t *result;
	unsigned int n;
	int ret;

	response_len = 0;
	ret = qmi_send_raw(client, msg, (void *)data, len, response,
			   sizeof(response), &response_len, 10000);
	if (ret) {
		LOG("QMI 0x%04x: transport error %d", msg, ret);
		return -1;
	}
	result = find_tlv(TLV_RESULT, &n);
	if (!result || n < 4) {
		LOG("QMI 0x%04x: no result TLV", msg);
		return -1;
	}
	if (le16(result)) {
		LOG("QMI 0x%04x: error 0x%04x", msg, le16(result + 2));
		return -1;
	}
	return 0;
}

static void fix_profile(void *client, unsigned int index)
{
	const uint8_t req[] = { TLV_PROFILE_ID, 2, 0, PROFILE_3GPP, index };
	const uint8_t mod[] = { TLV_PROFILE_ID, 2, 0, PROFILE_3GPP, index,
				TLV_PDP_TYPE, 1, 0, PDP_IPV4V6 };
	const uint8_t *apn, *pdp;
	unsigned int apn_len, pdp_len, type;

	if (request(client, QMI_WDS_GET_PROFILE_SETTINGS, req, sizeof(req)))
		return;
	apn = find_tlv(TLV_APN, &apn_len);
	pdp = find_tlv(TLV_PDP_TYPE, &pdp_len);
	if (!apn || !pdp || pdp_len < 1 || apn_len != strlen(IMS_APN) ||
	    strncasecmp((const char *)apn, IMS_APN, apn_len))
		return;
	type = pdp[0];
	if (type == PDP_IPV4V6 || type == PDP_PPP) {
		LOG("profile %u (APN %s): PDP type %u, nothing to do", index,
		    IMS_APN, type);
		return;
	}
	if (request(client, QMI_WDS_MODIFY_PROFILE_SETTINGS, mod, sizeof(mod)))
		return;
	LOG("profile %u (APN %s): PDP type %u -> IPv4v6", index, IMS_APN, type);
}

int main(void)
{
	union { long long align; uint8_t bytes[1024]; } params = { 0 };
	unsigned int indexes[MAX_PROFILES], count = 0, i, n, pos;
	qmi_release_fn qmi_release;
	qmi_init_fn qmi_init;
	void *cci, *services, *wds, *client = NULL;
	const uint8_t *list;
	int tries, ret = -1;

	if (!is_cityman()) {
		LOG("not a Cityman: nothing to do");
		return 0;
	}
	cci = dlopen("/vendor/lib64/libqmi_cci.so", RTLD_NOW | RTLD_GLOBAL);
	services = dlopen("/vendor/lib64/libqmiservices.so",
			  RTLD_NOW | RTLD_GLOBAL);
	if (!cci || !services) {
		LOG("dlopen: %s", dlerror());
		return 1;
	}
	qmi_init = (qmi_init_fn)dlsym(cci, "qmi_client_init_instance");
	qmi_send_raw = (qmi_raw_fn)dlsym(cci, "qmi_client_send_raw_msg_sync");
	qmi_release = (qmi_release_fn)dlsym(cci, "qmi_client_release");
	wds = dlsym(services, "wds_qmi_idl_service_object_v01");
	if (!qmi_init || !qmi_send_raw || !qmi_release || !wds) {
		LOG("QMI symbols missing");
		return 1;
	}

	/* qmuxd is up before the modem: wait for the WDS service. */
	for (tries = 0; tries < INIT_TRIES; tries++) {
		ret = qmi_init(wds, 0xffff, NULL, NULL, &params, 8000, &client);
		if (!ret)
			break;
		sleep(5);
	}
	if (ret) {
		LOG("WDS client not available (%d)", ret);
		return 1;
	}

	if (request(client, QMI_WDS_GET_PROFILE_LIST, NULL, 0) ||
	    !(list = find_tlv(TLV_PROFILE_LIST, &n)) || n < 1) {
		qmi_release(client);
		return 1;
	}
	/* count, then per profile: type, index, name length, name */
	for (i = 0, pos = 1; i < list[0] && pos + 3 <= n; i++) {
		if (list[pos] == PROFILE_3GPP && count < MAX_PROFILES)
			indexes[count++] = list[pos + 1];
		pos += 3 + list[pos + 2];
	}
	for (i = 0; i < count; i++)
		fix_profile(client, indexes[i]);
	qmi_release(client);
	return 0;
}
