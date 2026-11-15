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
 * On a dual SIM phone (persist.radio.multisim.config=dsds, set at boot for
 * the RM-1116 and RM-1118) the second card gets the same two steps on the
 * secondary GW session and the secondary NAS subscription. Which card goes
 * on which session follows qcril's binding of the rild instances to the
 * modem stacks (persist.radio.msim.stackid_N): normally slot 1 on the
 * primary session, but when Android moves the LTE stack to the data SIM
 * (setRadioCapability) qcril binds rild 0 to stack 1 and moves the cards
 * between the sessions, and so does this helper.
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
#define SESSION_PRIMARY_GW	0x00
#define SESSION_SECONDARY_GW	0x02
#define SUBS_PRIMARY		0x01
#define SUBS_SECONDARY		0x02

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
/*
 * Dual SIM: qcril opens the sessions itself when it is asked to activate a
 * subscription, and moves the cards between them on a stack swap. Open a
 * session only after it has been empty this long, so as not to race it.
 */
#define DUAL_ACTIVATION_GRACE_S	5

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
 * Walk the card status TLV and pick the USIM application that drives a GW
 * session: the one the session index names (index_gw_pri at INDEX_OFF 0,
 * index_gw_sec at 4) if it is a USIM, else the first USIM on the first
 * present card. SLOT_ONLY >= 0 limits the search to that slot (dual SIM).
 * Multi-app cards (USIM + ISIM/CSIM) are handled; returns 1 when a USIM was
 * found.
 *
 * TLV 0x10 layout: index_gw_pri, index_1x_pri, index_gw_sec, index_1x_sec
 * (u16 each, slot << 8 | app, 0xffff = none), num_slots, then per slot:
 * card_state, upin_state, upin_retries, upuk_retries, error_code, num_app,
 * and per app: type, state, perso_state, perso_feature, perso_retries,
 * perso_unblock_retries, aid_len, aid[], univ_pin, pin1_state, pin1_retries,
 * puk1_retries, pin2_state, pin2_retries, puk2_retries.
 */
static int find_usim(const uint8_t *p, unsigned int n, unsigned int index_off,
		     int slot_only, unsigned int *gw_index, struct usim *out)
{
	unsigned int off = 9, slots, slot, found = 0;

	if (n < 9)
		return 0;
	*gw_index = le16(p + index_off);
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
			    (slot_only < 0 || slot == (unsigned int)slot_only) &&
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

/*
 * Dual SIM: the USIM a GW session drives. While the session is open it is
 * the application its index names, in whichever slot. While it is empty, the
 * USIM in EXPECTED_SLOT, the slot whose rild instance is bound to that modem
 * stack; if the other session (index at OTHER_OFF) still holds that card, as
 * during a stack swap, the card in the other slot.
 */
static int find_session_usim(const uint8_t *p, unsigned int n,
			     unsigned int index_off, unsigned int other_off,
			     unsigned int expected_slot, unsigned int *gw_index,
			     struct usim *out)
{
	unsigned int other;

	if (n < 9)
		return 0;
	other = le16(p + other_off);
	if (find_usim(p, n, index_off, -1, gw_index, out) &&
	    *gw_index != SESSION_INDEX_NONE &&
	    *gw_index == (out->slot << 8 | out->app))
		return 1;
	if (!find_usim(p, n, index_off, (int)expected_slot, gw_index, out))
		return 0;
	if (other != (out->slot << 8 | out->app))
		return 1;
	return find_usim(p, n, index_off, (int)!expected_slot, gw_index, out) &&
	       other != (out->slot << 8 | out->app);
}

/*
 * The slot (0-based) whose card belongs on the primary GW session: the one
 * whose rild instance qcril bound to modem stack 0. qcril keeps the binding
 * in persist.radio.msim.stackid_<instance> and swaps it when Android moves
 * the LTE capability to the other SIM.
 */
static unsigned int primary_slot(void)
{
	return cityman_property_is("persist.radio.msim.stackid_0", "1") ? 1 : 0;
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

/*
 * The two provisioning steps the Windows modem waits for, on one GW session:
 * open the session for a DETECTED USIM, then, once the PIN is verified or
 * disabled and personalization is ready, send Subscription OK. Returns 1
 * while a step is still pending (poll fast), 0 when nothing is left to do.
 */
static int provision(void *uim, unsigned int session, unsigned int gw_index,
		     const struct usim *usim, struct retry *activation,
		     struct retry *subscription, const char *name,
		     int may_activate)
{
	char what[64];

	if (gw_index == SESSION_INDEX_NONE &&
	    usim->state == APP_STATE_DETECTED) {
		if (!may_activate)
			return 1;
		/* TLV 0x01: session type, activate. TLV 0x10: slot, AID. */
		uint8_t msg[10 + MAX_AID_LEN] = {
			0x01, 2, 0, 0x00, 1,
			0x10, 0, 0, 0, 0,
		};

		snprintf(what, sizeof(what), "%s GW USIM activation", name);
		if (!usim->aid_len) {
			LOG("%s USIM without application identifier; no activation",
			    name);
		} else if (retry_due(activation, what)) {
			msg[3] = session;
			msg[6] = usim->aid_len + 2;
			msg[8] = usim->slot + 1;
			msg[9] = usim->aid_len;
			memcpy(msg + 10, usim->aid, usim->aid_len);
			if (!request(uim, QMI_UIM_CHANGE_PROVISIONING_SESSION,
				     msg, 10 + usim->aid_len))
				LOG("%s GW USIM activated", name);
			else
				retry_failed(activation);
		}
		return 1;
	}
	if (gw_index == (usim->slot << 8 | usim->app) &&
	    usim->state == APP_STATE_PERSO_CHECK &&
	    usim->perso == PERSO_STATE_READY &&
	    (usim->pin1 == PIN_STATE_VERIFIED ||
	     usim->pin1 == PIN_STATE_DISABLED)) {
		/*
		 * Publish only after PIN verification/disablement and
		 * personalization ready. TLV 0x01: session type, no AID.
		 * TLV 0x02: OK for subscription.
		 */
		uint8_t ok[] = { 0x01, 2, 0, 0x00, 0, 0x02, 1, 0, 1 };

		ok[3] = session;
		snprintf(what, sizeof(what), "%s Subscription OK", name);
		if (retry_due(subscription, what)) {
			if (!request(uim, QMI_UIM_SUBSCRIPTION_OK, ok,
				     sizeof(ok)))
				LOG("%s GW Subscription OK accepted", name);
			else
				retry_failed(subscription);
		}
		return 1;
	}
	return usim->state != APP_STATE_READY;
}

static int open_client(void *object, void *params, void **client)
{
	return qmi_init(object, 0xffff, NULL, NULL, params, 8000, client);
}

/* The NAS and DMS clients used to enable the radio subscriptions. */
struct radio {
	void *nas, *dms, *nas_object, *dms_object, *nas_params, *dms_params;
	unsigned int attempted;		/* mask last tried while online */
};

static void release_radio(struct radio *r)
{
	if (r->nas) {
		qmi_release(r->nas);
		r->nas = NULL;
	}
	if (r->dms) {
		qmi_release(r->dms);
		r->dms = NULL;
	}
	r->attempted = 0;
}

/*
 * Enable the NAS subscriptions in WANT once the modem is online, following
 * Android's radio power decision (DMS is never forced online): one attempt
 * per change of WANT or of the online state, not a loop. Returns the poll
 * interval in seconds.
 */
static unsigned int activate_radio(struct radio *r, unsigned int want)
{
	const uint8_t *mode, *mask;
	uint8_t active[11] = { TLV_SET_ACTIVE_SUBS, 8, 0 };
	unsigned int n;

	if (!r->dms && open_client(r->dms_object, r->dms_params, &r->dms)) {
		r->dms = NULL;
		return POLL_RADIO_PENDING_S;
	}
	if (!r->nas && open_client(r->nas_object, r->nas_params, &r->nas)) {
		r->nas = NULL;
		return POLL_RADIO_PENDING_S;
	}
	if (request(r->dms, QMI_DMS_GET_OPERATING_MODE, NULL, 0))
		goto drop;
	mode = find_tlv(TLV_OPERATING_MODE, &n);
	if (!mode || n != 1)
		goto drop;
	if (*mode != DMS_MODE_ONLINE) {
		r->attempted = 0;
		return POLL_STABLE_S;
	}
	if (r->attempted == want)
		return POLL_STABLE_S;

	r->attempted = want;
	if (request(r->nas, QMI_NAS_GET_ACTIVE_SUBSCRIPTIONS, NULL, 0))
		goto drop;
	mask = find_tlv(TLV_GET_ACTIVE_SUBS, &n);
	if (!mask || n != 8)
		goto drop;
	memcpy(active + 3, mask, 8);
	if ((active[3] & want) != want) {
		active[3] |= want;
		if (!request(r->nas, QMI_NAS_SET_ACTIVE_SUBSCRIPTIONS, active,
			     sizeof(active)))
			LOG("%s radio subscription%s enabled",
			    want == SUBS_PRIMARY ? "Primary" :
			    want == SUBS_SECONDARY ? "Secondary" :
			    "Primary and secondary",
			    want == (SUBS_PRIMARY | SUBS_SECONDARY) ? "s" : "");
	}
	return POLL_STABLE_S;

drop:
	/* Usually a modem restart: reopen the clients next round. */
	release_radio(r);
	return POLL_RADIO_PENDING_S;
}

int main(int argc, char **argv)
{
	union {
		long double align;
		uint8_t bytes[1024];
	} params[3] = { 0 };
	void *cci, *services, *uim_object, *nas_object, *dms_object;
	void *uim = NULL;
	struct radio r = { 0 };
	struct retry activation = { 0 }, subscription = { 0 };
	struct retry activation2 = { 0 }, subscription2 = { 0 };
	int observe = 0, radio = 0, done = 1, dual;
	int last_signature = -1, last_signature2 = -1, no_usim_logged = 0;
	unsigned int failures = 0, attempt, pslot = 0, last_pslot = 0;
	time_t empty_since = 0, empty_since2 = 0;

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
	dual = cityman_property_is("persist.radio.multisim.config", "dsds");
	if (dual)
		LOG("Dual SIM: GW sessions follow the modem stacks");

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
	r.nas_object = nas_object;
	r.dms_object = dms_object;
	r.dms_params = &params[1];
	r.nas_params = &params[2];

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
		struct usim usim, usim2;
		unsigned int n, gw_index, gw_index2 = SESSION_INDEX_NONE;
		unsigned int poll, poll_ms = 0;
		int signature, found2 = 0, pending2 = 0, ready2 = 0, found;
		time_t now = now_s();

		if (request(uim, QMI_UIM_GET_CARD_STATUS, NULL, 0)) {
			/*
			 * A transport error usually means qmuxd or the modem
			 * restarted: the client is dead, so open a new one at
			 * once instead of waiting. While the PIN is being
			 * entered a slow recovery delays Subscription OK and
			 * Android shows the PIN screen a second time.
			 */
			if (++failures == 30)
				break;
			qmi_release(uim);
			uim = NULL;
			usleep(500 * 1000);
			if (open_client(uim_object, &params[0], &uim)) {
				uim = NULL;
				sleep(1);
				for (attempt = 0; !uim && attempt < 10; attempt++) {
					if (!open_client(uim_object, &params[0], &uim))
						break;
					uim = NULL;
					sleep(1);
				}
				if (!uim)
					break;
			}
			LOG("UIM client reopened");
			continue;
		}
		failures = 0;

		status = find_tlv(TLV_CARD_STATUS, &n);
		if (dual) {
			pslot = primary_slot();
			if (pslot != last_pslot) {
				LOG("Modem stacks %s: slot %u on the primary GW session",
				    pslot ? "swapped" : "in order", pslot + 1);
				last_pslot = pslot;
			}
		}
		if (status && dual) {
			found2 = find_session_usim(status, n, 4, 0, !pslot,
						   &gw_index2, &usim2);
			if (gw_index2 != SESSION_INDEX_NONE)
				empty_since2 = 0;
			else if (!empty_since2)
				empty_since2 = now;
			if (found2) {
				int s2 = usim2.state | usim2.pin1 << 8 |
					 usim2.perso << 16 | usim2.slot << 24;

				if (s2 != last_signature2) {
					LOG("Secondary session USIM slot %u app %u: state=%u PIN-state=%u personalization=%u",
					    usim2.slot + 1, usim2.app, usim2.state,
					    usim2.pin1, usim2.perso);
					last_signature2 = s2;
					retry_reset(&activation2);
					retry_reset(&subscription2);
				}
				ready2 = usim2.state == APP_STATE_READY;
				if (!observe)
					pending2 = provision(uim, SESSION_SECONDARY_GW,
							     gw_index2, &usim2,
							     &activation2,
							     &subscription2,
							     "Secondary",
							     now - empty_since2 >=
							     DUAL_ACTIVATION_GRACE_S);
			}
		}
		found = status && (dual ?
			find_session_usim(status, n, 0, 4, pslot, &gw_index, &usim) :
			find_usim(status, n, 0, -1, &gw_index, &usim));
		/* Both sessions on one card cannot happen; never act twice on it. */
		if (found && found2 && usim.slot == usim2.slot &&
		    usim.app == usim2.app)
			found = 0;
		if (dual && found && gw_index != SESSION_INDEX_NONE)
			empty_since = 0;
		else if (dual && !empty_since)
			empty_since = now;
		if (!found) {
			if (!no_usim_logged) {
				LOG("No present USIM application");
				no_usim_logged = 1;
			}
			if (observe)
				break;
			/* Dual SIM with a card in the second slot only. */
			poll = POLL_NO_USIM_S;
			if (radio && ready2)
				poll = activate_radio(&r, SUBS_SECONDARY);
			else
				release_radio(&r);
			if (pending2)
				usleep(POLL_TRANSITION_MS * 1000);
			else
				sleep(poll);
			continue;
		}
		no_usim_logged = 0;

		signature = usim.state | usim.pin1 << 8 | usim.perso << 16 |
			    usim.slot << 24;
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
			if (!radio) {
				done = 0;
				break;
			}
			poll = activate_radio(&r, SUBS_PRIMARY |
						  (ready2 ? SUBS_SECONDARY : 0));
			goto next;
		}

		if (radio && ready2)
			activate_radio(&r, SUBS_SECONDARY);
		else
			release_radio(&r);
		if (observe) {
			done = 0;
			break;
		}

		provision(uim, SESSION_PRIMARY_GW, gw_index, &usim, &activation,
			  &subscription, "Primary",
			  !dual || now - empty_since >= DUAL_ACTIVATION_GRACE_S);
		/* PIN- and PUK-required states are only observed; the user handles them. */
		poll = 0;
		poll_ms = (usim.state == APP_STATE_PIN_REQUIRED ||
			   usim.state == APP_STATE_PUK_REQUIRED) ?
			  POLL_PIN_MS : POLL_TRANSITION_MS;
next:
		if (pending2 && poll) {
			/* The second SIM is starting: do not wait 5-30 s. */
			poll = 0;
			poll_ms = POLL_TRANSITION_MS;
		}
		if (poll)
			sleep(poll);
		else
			usleep(poll_ms * 1000);
	}

	release_radio(&r);
	if (uim)
		qmi_release(uim);
	return done;
}
