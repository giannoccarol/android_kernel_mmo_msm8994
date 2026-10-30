/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cityman-uim-bootstrap: finish the Windows modem's primary USIM start-up.
 *
 * The Lumia MPSS leaves the USIM application DETECTED until a client opens
 * the primary GW provisioning session, and keeps it out of READY until the
 * client confirms the subscription. Android's RIL does neither, so this
 * helper does both through the existing qmuxd (libqmi_cci), then, with
 * --activate-radio, enables the primary NAS subscription once per
 * SIM-ready/radio-online transition.
 *
 * It never enters a PIN, writes NV, forces DMS online, touches the SMD
 * device directly or logs card identifiers. Requests the modem rejects are
 * retried a few times with backoff and then only observed: re-sending the
 * provisioning request in a loop resets the SMD channel and drops the SIM.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#include "cityman-detect.h"

#define TAG "CitymanUIM"
#define LOG(...)							\
	do {								\
		__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); \
		fprintf(stderr, __VA_ARGS__);				\
		fputc('\n', stderr);					\
	} while (0)

/* QMI messages (raw IDs, as sent through qmi_client_send_raw_msg_sync). */
#define QMI_UIM_GET_CARD_STATUS			0x002f
#define QMI_UIM_CHANGE_PROVISIONING_SESSION	0x0038
#define QMI_UIM_SUBSCRIPTION_OK			0x0040
#define QMI_DMS_GET_OPERATING_MODE		0x002d
#define QMI_NAS_GET_ACTIVE_SUBSCRIPTIONS	0x005c	/* mask in TLV 0x15 */
#define QMI_NAS_SET_ACTIVE_SUBSCRIPTIONS	0x004b	/* mask in TLV 0x14 */

#define TLV_RESULT		0x02
#define TLV_CARD_STATUS		0x10
#define TLV_OPERATING_MODE	0x01
#define TLV_GET_ACTIVE_SUBS	0x15
#define TLV_SET_ACTIVE_SUBS	0x14

#define CARD_STATE_PRESENT	1
#define APP_TYPE_USIM		2
#define APP_STATE_DETECTED	1
#define APP_STATE_PIN_REQUIRED	2
#define APP_STATE_PUK_REQUIRED	3
#define APP_STATE_PERSO_CHECK	4
#define APP_STATE_READY		7
#define PERSO_STATE_READY	2
#define PIN_STATE_VERIFIED	2
#define PIN_STATE_DISABLED	3
#define DMS_MODE_ONLINE		0
#define SESSION_INDEX_NONE	0xffff
#define SUBS_PRIMARY		0x01

#define MAX_AID_LEN		32
#define MAX_RETRIES		3
/*
 * While the user unlocks the card, poll fast: after a correct PIN the modem
 * stays in the personalization check until Subscription OK is sent, and
 * Android 16 keeps the card in PIN_REQUIRED meanwhile, so the PIN screen came
 * back for a couple of seconds after it was entered.
 */
#define POLL_TRANSITION_MS	250
#define POLL_PIN_MS		500
#define POLL_RADIO_PENDING_S	5
#define POLL_STABLE_S		30
#define POLL_NO_USIM_S		10

typedef int (*qmi_init_fn)(void *, unsigned int, void *, void *, void *,
			   unsigned int, void **);
typedef int (*qmi_raw_fn)(void *, unsigned int, void *, unsigned int, void *,
			  unsigned int, unsigned int *, unsigned int);
typedef int (*qmi_release_fn)(void *);

static qmi_init_fn qmi_init;
static qmi_raw_fn qmi_send_raw;
static qmi_release_fn qmi_release;

static uint8_t response[8192];
static unsigned int response_len;

/* The parts of one USIM application from the card status TLV that we use. */
struct usim {
	unsigned int slot;		/* 0-based */
	unsigned int app;		/* 0-based, within the slot */
	unsigned int state;
	unsigned int perso;
	unsigned int pin1;
	unsigned int aid_len;
	uint8_t aid[MAX_AID_LEN];
};

/* A request that may be rejected: a few tries with backoff, then observe. */
struct retry {
	unsigned int tries;
	time_t next;
	int exhausted_logged;
};

static unsigned int le16(const uint8_t *p)
{
	return p[0] | ((unsigned int)p[1] << 8);
}

static time_t now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec;
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

/* Send one request; 0 only when the transport and the QMI result succeed. */
static int request(void *client, unsigned int msg, const void *data,
		   unsigned int size)
{
	const uint8_t *result;
	unsigned int n;
	int ret;

	response_len = 0;
	ret = qmi_send_raw(client, msg, (void *)data, size, response,
			   sizeof(response), &response_len, 5000);
	if (ret || response_len > sizeof(response)) {
		LOG("QMI message 0x%04x: transport error %d", msg, ret);
		return -1;
	}
	result = find_tlv(TLV_RESULT, &n);
	if (!result || n != 4)
		return -1;
	if (le16(result)) {
		LOG("QMI message 0x%04x: error %u", msg, le16(result + 2));
		return -1;
	}
	return 0;
}

/*
 * Walk the card status TLV and pick the USIM application that drives the
 * primary GW session: the one index_gw_pri names if it is a USIM, else the
 * first USIM on the first present card. Multi-app cards (USIM + ISIM/CSIM)
 * and dual-SIM layouts are handled; returns 1 when a USIM was found.
 *
 * TLV 0x10 layout: index_gw_pri, index_1x_pri, index_gw_sec, index_1x_sec
 * (u16 each, slot << 8 | app, 0xffff = none), num_slots, then per slot:
 * card_state, upin_state, upin_retries, upuk_retries, error_code, num_app,
 * and per app: type, state, perso_state, perso_feature, perso_retries,
 * perso_unblock_retries, aid_len, aid[], univ_pin, pin1_state, pin1_retries,
 * puk1_retries, pin2_state, pin2_retries, puk2_retries.
 */
static int find_usim(const uint8_t *p, unsigned int n, unsigned int *gw_index,
		     struct usim *out)
{
	unsigned int off = 9, slots, slot, found = 0;

	if (n < 9)
		return 0;
	*gw_index = le16(p);
	slots = p[8];

	for (slot = 0; slot < slots; slot++) {
		unsigned int card_state, apps, app;

		if (off + 6 > n)
			return found;
		card_state = p[off];
		apps = p[off + 5];
		off += 6;

		for (app = 0; app < apps; app++) {
			unsigned int aid_len, type;

			if (off + 7 > n)
				return found;
			type = p[off];
			aid_len = p[off + 6];
			if (aid_len > MAX_AID_LEN || off + 7 + aid_len + 7 > n)
				return found;

			if (card_state == CARD_STATE_PRESENT &&
			    type == APP_TYPE_USIM &&
			    (!found || *gw_index == (slot << 8 | app))) {
				out->slot = slot;
				out->app = app;
				out->state = p[off + 1];
				out->perso = p[off + 2];
				out->aid_len = aid_len;
				memcpy(out->aid, p + off + 7, aid_len);
				out->pin1 = p[off + 7 + aid_len + 1];
				found = 1;
			}
			off += 7 + aid_len + 7;
		}
	}
	return found;
}

static int retry_due(struct retry *r, const char *what)
{
	if (r->tries >= MAX_RETRIES) {
		if (!r->exhausted_logged) {
			LOG("%s rejected %u times; observing only", what,
			    MAX_RETRIES);
			r->exhausted_logged = 1;
		}
		return 0;
	}
	return now_s() >= r->next;
}

static void retry_failed(struct retry *r)
{
	/* 5 s, 15 s, 45 s */
	static const unsigned int backoff[MAX_RETRIES] = { 5, 15, 45 };

	r->next = now_s() + backoff[r->tries < MAX_RETRIES ? r->tries : 0];
	r->tries++;
}

static void retry_reset(struct retry *r)
{
	memset(r, 0, sizeof(*r));
}

static int open_client(void *object, void *params, void **client)
{
	return qmi_init(object, 0xffff, NULL, NULL, params, 8000, client);
}

int main(int argc, char **argv)
{
	union {
		long double align;
		uint8_t bytes[1024];
	} params[3] = { 0 };
	void *cci, *services, *uim_object, *nas_object, *dms_object;
	void *uim = NULL, *nas = NULL, *dms = NULL;
	struct retry activation = { 0 }, subscription = { 0 };
	int observe = 0, radio = 0, done = 1, radio_attempted = 0;
	int last_signature = -1, no_usim_logged = 0;
	unsigned int failures = 0, attempt;

	if (argc == 2 && !strcmp(argv[1], "--observe-only"))
		observe = 1;
	else if (argc == 2 && !strcmp(argv[1], "--activate-radio"))
		radio = 1;
	else if (argc != 1) {
		fprintf(stderr, "usage: %s [--observe-only|--activate-radio]\n",
			argv[0]);
		return 2;
	}

	if (!is_cityman()) {
		LOG("Unsupported device; no QMI changes");
		return 1;
	}

	cci = dlopen("/vendor/lib64/libqmi_cci.so", RTLD_NOW | RTLD_GLOBAL);
	services = dlopen("/vendor/lib64/libqmiservices.so",
			  RTLD_NOW | RTLD_GLOBAL);
	if (!cci || !services) {
		LOG("Cannot load vendor QMI libraries");
		return 1;
	}
	qmi_init = (qmi_init_fn)dlsym(cci, "qmi_client_init_instance");
	qmi_send_raw = (qmi_raw_fn)dlsym(cci, "qmi_client_send_raw_msg_sync");
	qmi_release = (qmi_release_fn)dlsym(cci, "qmi_client_release");
	uim_object = dlsym(services, "uim_qmi_idl_service_object_v01");
	nas_object = dlsym(services, "nas_qmi_idl_service_object_v01");
	dms_object = dlsym(services, "dms_qmi_idl_service_object_v01");
	if (!qmi_init || !qmi_send_raw || !qmi_release || !uim_object ||
	    (radio && (!nas_object || !dms_object))) {
		LOG("Vendor QMI symbols missing");
		return 1;
	}

	for (attempt = 0; attempt < 30; attempt++) {
		int ret = open_client(uim_object, &params[0], &uim);

		if (!ret)
			break;
		LOG("Waiting for UIM transport: %d", ret);
		sleep(2);
	}
	if (!uim)
		return 1;

	for (;;) {
		const uint8_t *status;
		struct usim usim;
		unsigned int n, gw_index, poll, poll_ms = 0;
		int signature;

		if (request(uim, QMI_UIM_GET_CARD_STATUS, NULL, 0)) {
			if (++failures == 6)
				break;
			sleep(5);
			continue;
		}
		failures = 0;

		status = find_tlv(TLV_CARD_STATUS, &n);
		if (!status || !find_usim(status, n, &gw_index, &usim)) {
			if (!no_usim_logged) {
				LOG("No present USIM application");
				no_usim_logged = 1;
			}
			if (observe)
				break;
			sleep(POLL_NO_USIM_S);
			continue;
		}
		no_usim_logged = 0;

		signature = usim.state | usim.pin1 << 8 | usim.perso << 16;
		if (signature != last_signature) {
			LOG("USIM slot %u app %u: state=%u PIN-state=%u personalization=%u",
			    usim.slot + 1, usim.app, usim.state, usim.pin1,
			    usim.perso);
			if (usim.state == APP_STATE_READY)
				LOG("USIM ready");
			last_signature = signature;
			/* A new state earns the rejected requests new tries. */
			retry_reset(&activation);
			retry_reset(&subscription);
		}

		if (usim.state == APP_STATE_READY) {
			const uint8_t *mode, *mask;
			uint8_t active[11] = { TLV_SET_ACTIVE_SUBS, 8, 0 };

			if (!radio) {
				done = 0;
				break;
			}
			poll = POLL_RADIO_PENDING_S;
			if (!dms && open_client(dms_object, &params[1], &dms))
				goto next;
			if (!nas && open_client(nas_object, &params[2], &nas))
				goto next;

			/* Follow Android's radio power decision; never force DMS online. */
			if (request(dms, QMI_DMS_GET_OPERATING_MODE, NULL, 0))
				goto drop_radio_clients;
			mode = find_tlv(TLV_OPERATING_MODE, &n);
			if (!mode || n != 1)
				goto drop_radio_clients;
			if (*mode != DMS_MODE_ONLINE) {
				radio_attempted = 0;
				poll = POLL_STABLE_S;
				goto next;
			}
			if (radio_attempted) {
				poll = POLL_STABLE_S;
				goto next;
			}

			/* One activation per ready/online transition, not a loop. */
			radio_attempted = 1;
			if (request(nas, QMI_NAS_GET_ACTIVE_SUBSCRIPTIONS, NULL, 0))
				goto drop_radio_clients;
			mask = find_tlv(TLV_GET_ACTIVE_SUBS, &n);
			if (!mask || n != 8)
				goto drop_radio_clients;
			memcpy(active + 3, mask, 8);
			if (!(active[3] & SUBS_PRIMARY)) {
				active[3] |= SUBS_PRIMARY;
				if (!request(nas, QMI_NAS_SET_ACTIVE_SUBSCRIPTIONS,
					     active, sizeof(active)))
					LOG("Primary radio subscription enabled");
			}
			poll = POLL_STABLE_S;
			goto next;

drop_radio_clients:
			/* Usually a modem restart: reopen the clients next round. */
			if (nas) {
				qmi_release(nas);
				nas = NULL;
			}
			if (dms) {
				qmi_release(dms);
				dms = NULL;
			}
			radio_attempted = 0;
			goto next;
		}

		if (nas) {
			qmi_release(nas);
			nas = NULL;
		}
		if (dms) {
			qmi_release(dms);
			dms = NULL;
		}
		radio_attempted = 0;
		if (observe) {
			done = 0;
			break;
		}

		if (gw_index == SESSION_INDEX_NONE &&
		    usim.state == APP_STATE_DETECTED) {
			/* TLV 0x01: primary GW session, activate. TLV 0x10: slot, AID. */
			uint8_t msg[10 + MAX_AID_LEN] = {
				0x01, 2, 0, 0x00, 1,
				0x10, 0, 0, 0, 0,
			};

			if (!usim.aid_len) {
				LOG("USIM without application identifier; no activation");
			} else if (retry_due(&activation, "Primary GW USIM activation")) {
				msg[6] = usim.aid_len + 2;
				msg[8] = usim.slot + 1;
				msg[9] = usim.aid_len;
				memcpy(msg + 10, usim.aid, usim.aid_len);
				if (!request(uim, QMI_UIM_CHANGE_PROVISIONING_SESSION,
					     msg, 10 + usim.aid_len))
					LOG("Primary GW USIM activated");
				else
					retry_failed(&activation);
			}
		} else if (gw_index == (usim.slot << 8 | usim.app) &&
			   usim.state == APP_STATE_PERSO_CHECK &&
			   usim.perso == PERSO_STATE_READY &&
			   (usim.pin1 == PIN_STATE_VERIFIED ||
			    usim.pin1 == PIN_STATE_DISABLED)) {
			/*
			 * Publish only after PIN verification/disablement and
			 * personalization ready. TLV 0x01: primary GW session,
			 * no AID. TLV 0x02: OK for subscription.
			 */
			static const uint8_t ok[] = { 0x01, 2, 0, 0x00, 0,
						      0x02, 1, 0, 1 };

			if (retry_due(&subscription, "Subscription OK")) {
				if (!request(uim, QMI_UIM_SUBSCRIPTION_OK, ok,
					     sizeof(ok)))
					LOG("Primary GW Subscription OK accepted");
				else
					retry_failed(&subscription);
			}
		}
		/* PIN- and PUK-required states are only observed; the user handles them. */
		poll = 0;
		poll_ms = (usim.state == APP_STATE_PIN_REQUIRED ||
			   usim.state == APP_STATE_PUK_REQUIRED) ?
			  POLL_PIN_MS : POLL_TRANSITION_MS;
next:
		if (poll)
			sleep(poll);
		else
			usleep(poll_ms * 1000);
	}

	if (nas)
		qmi_release(nas);
	if (dms)
		qmi_release(dms);
	qmi_release(uim);
	return done;
}
