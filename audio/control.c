/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/l2cap.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "log.h"
#include "error.h"
#include "uinput.h"
#include "adapter.h"
#include "../src/device.h"
#include "device.h"
#include "manager.h"
#include "avdtp.h"
#include "control.h"
#include "sdpd.h"
#include "glib-helper.h"
#include "btio.h"
#include "dbus-common.h"

#define AVCTP_PSM 23

/* Message types */
#define AVCTP_COMMAND		0
#define AVCTP_RESPONSE		1

/* Packet types */
#define AVCTP_PACKET_SINGLE	0
#define AVCTP_PACKET_START	1
#define AVCTP_PACKET_CONTINUE	2
#define AVCTP_PACKET_END	3

/* ctype entries */
#define CTYPE_CONTROL		0x0
#define CTYPE_STATUS		0x1
#define CTYPE_NOT_IMPLEMENTED	0x8
#define CTYPE_ACCEPTED		0x9
#define CTYPE_REJECTED		0xA
#define CTYPE_STABLE		0xC

/* opcodes */
#define OP_VENDORDEPENDENT	0x00
#define OP_UNITINFO		0x30
#define OP_SUBUNITINFO		0x31
#define OP_PASSTHROUGH		0x7c

/* subunits of interest */
#define SUBUNIT_PANEL		0x09

/* operands in passthrough commands */
#define VOL_UP_OP		0x41
#define VOL_DOWN_OP		0x42
#define MUTE_OP			0x43
#define PLAY_OP			0x44
#define STOP_OP			0x45
#define PAUSE_OP		0x46
#define RECORD_OP		0x47
#define REWIND_OP		0x48
#define FAST_FORWARD_OP		0x49
#define EJECT_OP		0x4a
#define FORWARD_OP		0x4b
#define BACKWARD_OP		0x4c

#define QUIRK_NO_RELEASE	1 << 0

/* Company IDs for vendor dependent commands */
#define IEEEID_BTSIG		0x001958

/* Error codes */
#define E_INVALID_COMMAND	0x00
#define E_INVALID_PARAM		0x01
#define E_PARAM_NOT_FOUND	0x02
#define E_INTERNAL		0x03

/* PDU types for metadata transfer */
#define GET_CAPABILITIES			0x10
#define LIST_PLAYER_SETTING_ATTRIBUTES		0x11
#define LIST_PLAYER_SETTING_VALUES		0x12
#define GET_CURRENT_PLAYER_SETTING_VALUE	0x13
#define SET_PLAYER_SETTING_VALUE		0x14
#define GET_PLAYER_SETTING_ATTRIBUTE_TEXT	0x15
#define GET_PLAYER_SETTING_VALUE_TEXT		0x16
#define INFORM_DISPLAYABLE_CHARSET		0x17
#define INFORM_BATT_STATUS_OF_CT		0x18
#define GET_ELEMENT_ATTRIBUTES			0x20
#define GET_PLAY_STATUS				0x30

/* Capabilities */
#define CAP_COMPANY_ID		0x2
#define CAP_EVENTS_SUPPORTED	0x3

/* Player setting attribute IDs */
#define ATTRIBUTE_ILLEGAL	0x0
#define ATTRIBUTE_EQUALIZER	0x1
#define ATTRIBUTE_REPEAT	0x2
#define ATTRIBUTE_SHUFFLE	0x3
#define ATTRIBUTE_SCAN		0x4

/* Player setting attribute values */
#define ATTRIBUTE_EQUALIZER_OFF	0x1
#define ATTRIBUTE_EQUALIZER_ON	0x2
#define ATTRIBUTE_REPEAT_OFF	0x1
#define ATTRIBUTE_REPEAT_SINGLE	0x2
#define ATTRIBUTE_REPEAT_ALL	0x3
#define ATTRIBUTE_REPEAT_GROUP	0x4
#define ATTRIBUTE_SHUFFLE_OFF	0x1
#define ATTRIBUTE_SHUFFLE_ALL	0x2
#define ATTRIBUTE_SHUFFLE_GROUP	0x3
#define ATTRIBUTE_SCAN_OFF	0x1
#define ATTRIBUTE_SCAN_ALL	0x2
#define ATTRIBUTE_SCAN_GROUP	0x3

/* Element IDs */
#define ELEMENT_PLAYING		0x0000

/* Metadata attributes */
#define METADATA_ILLEGAL	0x0
#define METADATA_TITLE		0x1
#define METADATA_ARTIST		0x2
#define METADATA_ALBUM		0x3
#define METADATA_NUMBER		0x4
#define METADATA_TOTAL		0x5
#define METADATA_GENRE		0x6
#define METADATA_PLAY_TIME	0x7

/* Play status */
#define PLAY_STOPPED		0x00
#define PLAY_PLAYING		0x01
#define PLAY_PAUSED		0x02
#define PLAY_FWDSEEK		0x03
#define PLAY_REVSEEK		0x04
#define PLAY_ERROR		0xFF

/* Character sets */
#define CHARSET_UTF8		0x6A

/* Metadata transfer events */
#define EVENT_PLAYBACK_STATUS_CHANGED	0x01
#define EVENT_TRACK_CHANGED		0x02
#define EVENT_TRACK_REACHED_END		0x03
#define EVENT_TRACK_REACHED_START	0x04
#define EVENT_PLAYBACK_POS_CHANGED	0x05
#define EVENT_BATT_STATUS_CHANGED	0x06
#define EVENT_SYSTEM_STATUS_CHANGED	0x07
#define EVENT_PLAYER_SETTING_CHANGED	0x08

/* MPRIS player capabilities */
#define MPRIS_CAN_REPEAT	1 << 7
#define MPRIS_CAN_LOOP		1 << 8
#define MPRIS_CAN_SHUFFLE	1 << 9
#define MPRIS_CAN_SCAN		1 << 10

static DBusConnection *connection = NULL;

static GSList *servers = NULL;

#if __BYTE_ORDER == __LITTLE_ENDIAN

struct avctp_header {
	uint8_t ipid:1;
	uint8_t cr:1;
	uint8_t packet_type:2;
	uint8_t transaction:4;
	uint16_t pid;
} __attribute__ ((packed));
#define AVCTP_HEADER_LENGTH 3

struct avrcp_header {
	uint8_t code:4;
	uint8_t _hdr0:4;
	uint8_t subunit_id:3;
	uint8_t subunit_type:5;
	uint8_t opcode;
} __attribute__ ((packed));
#define AVRCP_HEADER_LENGTH 3

struct metadata_header {
	int8_t pdu_id;
	uint8_t packet_type:2;
	int8_t _rfa:6;
	uint16_t parameter_length;
} __attribute__ ((packed));
#define METADATA_HEADER_LENGTH 4

#elif __BYTE_ORDER == __BIG_ENDIAN

struct avctp_header {
	uint8_t transaction:4;
	uint8_t packet_type:2;
	uint8_t cr:1;
	uint8_t ipid:1;
	uint16_t pid;
} __attribute__ ((packed));
#define AVCTP_HEADER_LENGTH 3

struct avrcp_header {
	uint8_t _hdr0:4;
	uint8_t code:4;
	uint8_t subunit_type:5;
	uint8_t subunit_id:3;
	uint8_t opcode;
} __attribute__ ((packed));
#define AVRCP_HEADER_LENGTH 3

struct metadata_header {
	int8_t pdu_id;
	int8_t _rfa:6;
	uint8_t packet_type:2;
	uint16_t parameter_length;
} __attribute__ ((packed));
#define METADATA_HEADER_LENGTH 4

#else
#error "Unknown byte order"
#endif

struct avctp_state_callback {
	avctp_state_cb cb;
	void *user_data;
	unsigned int id;
};

struct avctp_server {
	bdaddr_t src;
	GIOChannel *io;
	uint32_t tg_record_id;
	uint32_t ct_record_id;
};

struct control {
	struct audio_device *dev;

	avctp_state_t state;

	int uinput;

	GIOChannel *io;
	guint io_id;

	uint16_t mtu;

	gboolean target;

	uint8_t key_quirks[256];

	uint32_t mpris_caps;
	gboolean mpris_play_state;
	gboolean mpris_shuffle_state;
	gboolean mpris_repeat_state;
	gboolean mpris_endless_state;

	char *mpris_title;
	char *mpris_artist;
	char *mpris_album;
	char *mpris_number;
	char *mpris_genre;
	uint32_t mpris_total;
};

static struct {
	const char *name;
	uint8_t avrcp;
	uint16_t uinput;
} key_map[] = {
	{ "PLAY",		PLAY_OP,		KEY_PLAYCD },
	{ "STOP",		STOP_OP,		KEY_STOPCD },
	{ "PAUSE",		PAUSE_OP,		KEY_PAUSECD },
	{ "FORWARD",		FORWARD_OP,		KEY_NEXTSONG },
	{ "BACKWARD",		BACKWARD_OP,		KEY_PREVIOUSSONG },
	{ "REWIND",		REWIND_OP,		KEY_REWIND },
	{ "FAST FORWARD",	FAST_FORWARD_OP,	KEY_FASTFORWARD },
	{ NULL }
};

static GSList *avctp_callbacks = NULL;

static void auth_cb(DBusError *derr, void *user_data);

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static sdp_record_t *avrcp_ct_record()
{
	sdp_list_t *svclass_id, *pfseq, *apseq, *root;
	uuid_t root_uuid, l2cap, avctp, avrct;
	sdp_profile_desc_t profile[1];
	sdp_list_t *aproto, *proto[2];
	sdp_record_t *record;
	sdp_data_t *psm, *version, *features;
	uint16_t lp = AVCTP_PSM;
	uint16_t avrcp_ver = 0x0100, avctp_ver = 0x0103, feat = 0x000f;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(0, &root_uuid);
	sdp_set_browse_groups(record, root);

	/* Service Class ID List */
	sdp_uuid16_create(&avrct, AV_REMOTE_SVCLASS_ID);
	svclass_id = sdp_list_append(0, &avrct);
	sdp_set_service_classes(record, svclass_id);

	/* Protocol Descriptor List */
	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(0, &l2cap);
	psm = sdp_data_alloc(SDP_UINT16, &lp);
	proto[0] = sdp_list_append(proto[0], psm);
	apseq = sdp_list_append(0, proto[0]);

	sdp_uuid16_create(&avctp, AVCTP_UUID);
	proto[1] = sdp_list_append(0, &avctp);
	version = sdp_data_alloc(SDP_UINT16, &avctp_ver);
	proto[1] = sdp_list_append(proto[1], version);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(0, apseq);
	sdp_set_access_protos(record, aproto);

	/* Bluetooth Profile Descriptor List */
	sdp_uuid16_create(&profile[0].uuid, AV_REMOTE_PROFILE_ID);
	profile[0].version = avrcp_ver;
	pfseq = sdp_list_append(0, &profile[0]);
	sdp_set_profile_descs(record, pfseq);

	features = sdp_data_alloc(SDP_UINT16, &feat);
	sdp_attr_add(record, SDP_ATTR_SUPPORTED_FEATURES, features);

	sdp_set_info_attr(record, "AVRCP CT", 0, 0);

	free(psm);
	free(version);
	sdp_list_free(proto[0], 0);
	sdp_list_free(proto[1], 0);
	sdp_list_free(apseq, 0);
	sdp_list_free(pfseq, 0);
	sdp_list_free(aproto, 0);
	sdp_list_free(root, 0);
	sdp_list_free(svclass_id, 0);

	return record;
}

static sdp_record_t *avrcp_tg_record()
{
	sdp_list_t *svclass_id, *pfseq, *apseq, *root;
	uuid_t root_uuid, l2cap, avctp, avrtg;
	sdp_profile_desc_t profile[1];
	sdp_list_t *aproto, *proto[2];
	sdp_record_t *record;
	sdp_data_t *psm, *version, *features;
	uint16_t lp = AVCTP_PSM;
	uint16_t avrcp_ver = 0x0100, avctp_ver = 0x0103, feat = 0x000f;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(0, &root_uuid);
	sdp_set_browse_groups(record, root);

	/* Service Class ID List */
	sdp_uuid16_create(&avrtg, AV_REMOTE_TARGET_SVCLASS_ID);
	svclass_id = sdp_list_append(0, &avrtg);
	sdp_set_service_classes(record, svclass_id);

	/* Protocol Descriptor List */
	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(0, &l2cap);
	psm = sdp_data_alloc(SDP_UINT16, &lp);
	proto[0] = sdp_list_append(proto[0], psm);
	apseq = sdp_list_append(0, proto[0]);

	sdp_uuid16_create(&avctp, AVCTP_UUID);
	proto[1] = sdp_list_append(0, &avctp);
	version = sdp_data_alloc(SDP_UINT16, &avctp_ver);
	proto[1] = sdp_list_append(proto[1], version);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(0, apseq);
	sdp_set_access_protos(record, aproto);

	/* Bluetooth Profile Descriptor List */
	sdp_uuid16_create(&profile[0].uuid, AV_REMOTE_PROFILE_ID);
	profile[0].version = avrcp_ver;
	pfseq = sdp_list_append(0, &profile[0]);
	sdp_set_profile_descs(record, pfseq);

	features = sdp_data_alloc(SDP_UINT16, &feat);
	sdp_attr_add(record, SDP_ATTR_SUPPORTED_FEATURES, features);

	sdp_set_info_attr(record, "AVRCP TG", 0, 0);

	free(psm);
	free(version);
	sdp_list_free(proto[0], 0);
	sdp_list_free(proto[1], 0);
	sdp_list_free(apseq, 0);
	sdp_list_free(aproto, 0);
	sdp_list_free(pfseq, 0);
	sdp_list_free(root, 0);
	sdp_list_free(svclass_id, 0);

	return record;
}

static int send_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct uinput_event event;

	memset(&event, 0, sizeof(event));
	event.type	= type;
	event.code	= code;
	event.value	= value;

	return write(fd, &event, sizeof(event));
}

static void send_key(int fd, uint16_t key, int pressed)
{
	if (fd < 0)
		return;

	send_event(fd, EV_KEY, key, pressed);
	send_event(fd, EV_SYN, SYN_REPORT, 0);
}

static void handle_panel_passthrough(struct control *control,
					const unsigned char *operands,
					int operand_count)
{
	const char *status;
	int pressed, i;

	if (operand_count == 0)
		return;

	if (operands[0] & 0x80) {
		status = "released";
		pressed = 0;
	} else {
		status = "pressed";
		pressed = 1;
	}

	for (i = 0; key_map[i].name != NULL; i++) {
		uint8_t key_quirks;

		if ((operands[0] & 0x7F) != key_map[i].avrcp)
			continue;

		DBG("AVRCP: %s %s", key_map[i].name, status);

		key_quirks = control->key_quirks[key_map[i].avrcp];

		if (key_quirks & QUIRK_NO_RELEASE) {
			if (!pressed) {
				DBG("AVRCP: Ignoring release");
				break;
			}

			DBG("AVRCP: treating key press as press + release");
			send_key(control->uinput, key_map[i].uinput, 1);
			send_key(control->uinput, key_map[i].uinput, 0);
			break;
		}

		send_key(control->uinput, key_map[i].uinput, pressed);
		break;
	}

	if (key_map[i].name == NULL)
		DBG("AVRCP: unknown button 0x%02X %s",
						operands[0] & 0x7F, status);
}

static void avctp_disconnected(struct audio_device *dev)
{
	struct control *control = dev->control;

	if (!control)
		return;

	if (control->io) {
		g_io_channel_shutdown(control->io, TRUE, NULL);
		g_io_channel_unref(control->io);
		control->io = NULL;
	}

	if (control->io_id) {
		g_source_remove(control->io_id);
		control->io_id = 0;

		if (control->state == AVCTP_STATE_CONNECTING)
			audio_device_cancel_authorization(dev, auth_cb,
								control);
	}

	if (control->uinput >= 0) {
		char address[18];

		ba2str(&dev->dst, address);
		DBG("AVRCP: closing uinput for %s", address);

		ioctl(control->uinput, UI_DEV_DESTROY);
		close(control->uinput);
		control->uinput = -1;
	}
}

static void avctp_set_state(struct control *control, avctp_state_t new_state)
{
	GSList *l;
	struct audio_device *dev = control->dev;
	avctp_state_t old_state = control->state;
	gboolean value;

	switch (new_state) {
	case AVCTP_STATE_DISCONNECTED:
		DBG("AVCTP Disconnected");

		avctp_disconnected(control->dev);

		if (old_state != AVCTP_STATE_CONNECTED)
			break;

		value = FALSE;
		g_dbus_emit_signal(dev->conn, dev->path,
					AUDIO_CONTROL_INTERFACE,
					"Disconnected", DBUS_TYPE_INVALID);
		emit_property_changed(dev->conn, dev->path,
					AUDIO_CONTROL_INTERFACE, "Connected",
					DBUS_TYPE_BOOLEAN, &value);

		if (!audio_device_is_active(dev, NULL))
			audio_device_set_authorized(dev, FALSE);

		break;
	case AVCTP_STATE_CONNECTING:
		DBG("AVCTP Connecting");
		break;
	case AVCTP_STATE_CONNECTED:
		DBG("AVCTP Connected");
		value = TRUE;
		g_dbus_emit_signal(control->dev->conn, control->dev->path,
				AUDIO_CONTROL_INTERFACE, "Connected",
				DBUS_TYPE_INVALID);
		emit_property_changed(control->dev->conn, control->dev->path,
				AUDIO_CONTROL_INTERFACE, "Connected",
				DBUS_TYPE_BOOLEAN, &value);
		break;
	default:
		error("Invalid AVCTP state %d", new_state);
		return;
	}

	control->state = new_state;

	for (l = avctp_callbacks; l != NULL; l = l->next) {
		struct avctp_state_callback *cb = l->data;
		cb->cb(control->dev, old_state, new_state, cb->user_data);
	}
}

static void handle_metadata_pdu(struct control *control,
				struct avrcp_header *avrcp, int operand_count)
{
	uint8_t i, rsp_i = 0;
	size_t metadata_length = operand_count - 3;
	size_t params_count = metadata_length - METADATA_HEADER_LENGTH;
	struct metadata_header *metadata;
	uint8_t *metadata_params, rsp[params_count];
	uint32_t element_id;

	metadata = (struct metadata_header *) avrcp + AVRCP_HEADER_LENGTH + 3;
	metadata_params = (unsigned char *) metadata + METADATA_HEADER_LENGTH;

	/* metadata segmentation */
	if (metadata->packet_type != AVCTP_PACKET_SINGLE) {
		avrcp->code = CTYPE_NOT_IMPLEMENTED;
		return;
	}

	switch (metadata->pdu_id) {
	case GET_CAPABILITIES:
		if (metadata->parameter_length < 1) {
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}

		switch(metadata_params[0]) { /* capability id */
		case CAP_COMPANY_ID:
			avrcp->code = CTYPE_STABLE;
			metadata->parameter_length = 5;
			metadata_params[1] = 1; /* capability count */
			metadata_params[2] = (uint8_t) IEEEID_BTSIG & 0xFF0000;
			metadata_params[3] = (uint8_t) IEEEID_BTSIG & 0x00FF00;
			metadata_params[4] = IEEEID_BTSIG & 0x0000FF;
			break;
		case CAP_EVENTS_SUPPORTED:
			avrcp->code = CTYPE_STABLE;
			metadata->parameter_length = 5;
			metadata_params[1] = 3; /* capability count */
			metadata_params[2] = EVENT_PLAYBACK_STATUS_CHANGED;
			metadata_params[3] = EVENT_TRACK_CHANGED;
			metadata_params[4] = EVENT_TRACK_REACHED_END;
			break;
		default:
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}
		break;
	case LIST_PLAYER_SETTING_ATTRIBUTES:
		avrcp->code = CTYPE_STABLE;

		metadata->parameter_length = 1;
		metadata_params[0] = 0; /* num player setting attributes */
		if (control->mpris_caps & MPRIS_CAN_REPEAT ||
			control->mpris_caps & MPRIS_CAN_LOOP) {
			metadata->parameter_length++;
			metadata_params[0]++;
			metadata_params[metadata_params[0]] = ATTRIBUTE_REPEAT;
		}
		if (control->mpris_caps & MPRIS_CAN_SHUFFLE) {
			metadata->parameter_length++;
			metadata_params[0]++;
			metadata_params[metadata_params[0]] = ATTRIBUTE_SHUFFLE;
		}
		if (control->mpris_caps & MPRIS_CAN_SCAN) {
			metadata->parameter_length++;
			metadata_params[0]++;
			metadata_params[metadata_params[0]] = ATTRIBUTE_SCAN;
		}
		break;
	case LIST_PLAYER_SETTING_VALUES:
		if (metadata->parameter_length < 1) {
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}

		avrcp->code = CTYPE_STABLE;
		switch (metadata_params[0]) {
		case ATTRIBUTE_REPEAT:
			if (!(control->mpris_caps & MPRIS_CAN_REPEAT ||
				control->mpris_caps & MPRIS_CAN_LOOP)) {
				avrcp->code = CTYPE_REJECTED;
				metadata->parameter_length = 1;
				metadata_params[0] = E_INVALID_PARAM;
				break;
			}
			metadata->parameter_length = 2;
			metadata_params[0] = 1; /* num player setting values */
			metadata_params[1] = ATTRIBUTE_REPEAT_OFF;
			if (control->mpris_caps & MPRIS_CAN_REPEAT) {
				metadata->parameter_length++;
				metadata_params[0]++;
				metadata_params[metadata_params[0]] =
					ATTRIBUTE_REPEAT_SINGLE;
			}
			if (control->mpris_caps & MPRIS_CAN_LOOP) {
				metadata->parameter_length++;
				metadata_params[0]++;
				metadata_params[metadata_params[0]] =
					ATTRIBUTE_REPEAT_GROUP;
				/* AVRCP spec is not clear if ALL is refers
				 * to the playlist or to the media colection.
				 * For MPRIS CAN_LOOP refers to the playlist. */
			}
			break;
		case ATTRIBUTE_SHUFFLE:
			if (!(control->mpris_caps & MPRIS_CAN_SHUFFLE)) {
				avrcp->code = CTYPE_REJECTED;
				metadata->parameter_length = 1;
				metadata_params[0] = E_INVALID_PARAM;
				break;
			}
			metadata->parameter_length = 3;
			metadata_params[0] = 2; /* num player setting values */
			metadata_params[1] = ATTRIBUTE_SHUFFLE_OFF;
			metadata_params[2] = ATTRIBUTE_SHUFFLE_GROUP;
			/* same note for REPEAT_GROUP applies here */
			break;
		case ATTRIBUTE_SCAN:
			if (!(control->mpris_caps & MPRIS_CAN_SCAN)) {
				avrcp->code = CTYPE_REJECTED;
				metadata->parameter_length = 1;
				metadata_params[0] = E_INVALID_PARAM;
				break;
			}
			metadata->parameter_length = 3;
			metadata_params[0] = 2; /* num player setting values */
			metadata_params[1] = ATTRIBUTE_SCAN_OFF;
			metadata_params[2] = ATTRIBUTE_SCAN_GROUP;
			/* same note for REPEAT_GROUP applies here */
			break;
		default:
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}
		break;
	case GET_CURRENT_PLAYER_SETTING_VALUE:
		if (metadata->parameter_length < 1) {
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}

		avrcp->code = CTYPE_STABLE;
		metadata->parameter_length = 1;
		rsp[0] = metadata_params[0];
		rsp_i = 1;

		for (i = 1; i <= metadata_params[0]; i++) {
			switch (metadata_params[i]) {
			case ATTRIBUTE_REPEAT:
				if (!(control->mpris_caps & MPRIS_CAN_REPEAT ||
					control->mpris_caps & MPRIS_CAN_LOOP)) {
					i = metadata_params[0] + 1;
					avrcp->code = CTYPE_REJECTED;
					metadata->parameter_length = 1;
					metadata_params[0] = E_INVALID_PARAM;
					break;
				}
				rsp[rsp_i++] = ATTRIBUTE_REPEAT;
				if (control->mpris_repeat_state)
					rsp[rsp_i++] = ATTRIBUTE_REPEAT_SINGLE;
				else if (control->mpris_endless_state)
					rsp[rsp_i++] = ATTRIBUTE_REPEAT_GROUP;
				else
					rsp[rsp_i++] = ATTRIBUTE_REPEAT_OFF;
				break;
			case ATTRIBUTE_SHUFFLE:
				if (!(control->mpris_caps & MPRIS_CAN_SHUFFLE)) {
					i = metadata_params[0] + 1;
					avrcp->code = CTYPE_REJECTED;
					metadata->parameter_length = 1;
					metadata_params[0] = E_INVALID_PARAM;
					break;
				}
				rsp[rsp_i++] = ATTRIBUTE_SHUFFLE;
				if (control->mpris_shuffle_state)
					rsp[rsp_i++] = ATTRIBUTE_SHUFFLE_GROUP;
				else
					rsp[rsp_i++] = ATTRIBUTE_SHUFFLE_OFF;
				break;
			case ATTRIBUTE_SCAN:
				if (!(control->mpris_caps & MPRIS_CAN_SCAN)) {
					i = metadata_params[0] + 1;
					avrcp->code = CTYPE_REJECTED;
					metadata->parameter_length = 1;
					metadata_params[0] = E_INVALID_PARAM;
					break;
				}
				/* not supported on MPRIS 1.0 */
				rsp[rsp_i++] = ATTRIBUTE_SCAN;
				rsp[rsp_i++] = ATTRIBUTE_SCAN_OFF;
				break;
			default:
				i = metadata_params[0] + 1;
				avrcp->code = CTYPE_REJECTED;
				metadata->parameter_length = 1;
				metadata_params[0] = E_INVALID_PARAM;
				break;
			}
		}
		memcpy(metadata_params, rsp, rsp_i);
		break;
	case SET_PLAYER_SETTING_VALUE:
		if (metadata->parameter_length < 1) {
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}
		for (i = 0; i <= metadata_params[0]; i++) {
			switch (metadata_params[2 * i + 1]) {
			case ATTRIBUTE_REPEAT:
				if (!(control->mpris_caps & MPRIS_CAN_REPEAT ||
					control->mpris_caps & MPRIS_CAN_LOOP) ||
					metadata_params[2 * i + 2] == ATTRIBUTE_REPEAT_ALL) {
					i = metadata_params[0] + 1;
					avrcp->code = CTYPE_REJECTED;
					metadata->parameter_length = 1;
					metadata_params[0] = E_INVALID_PARAM;
					break;
				}

				if (metadata_params[2 * i + 2] == ATTRIBUTE_REPEAT_OFF) {
					control->mpris_repeat_state = FALSE;
					control->mpris_endless_state = FALSE;
				} else if (metadata_params[2 * i + 2] == ATTRIBUTE_REPEAT_SINGLE) {
					control->mpris_repeat_state = TRUE;
					control->mpris_endless_state = FALSE;
				} else if (metadata_params[2 * i + 2] == ATTRIBUTE_REPEAT_GROUP) {
					control->mpris_repeat_state = FALSE;
					control->mpris_endless_state = TRUE;
				}

				/* is uinput better appropriate for this? */
				emit_property_changed(control->dev->conn,
					control->dev->path,
					AUDIO_CONTROL_INTERFACE,
					"SetRepeatState",
					DBUS_TYPE_BOOLEAN,
					&metadata_params[2 * i + 2]);

				DBG("repeat 0x%1X", metadata_params[2 * i + 2]);

				break;

			case ATTRIBUTE_SHUFFLE:
				if (!(control->mpris_caps & MPRIS_CAN_SHUFFLE)) {
					i = metadata_params[0] + 1;
					avrcp->code = CTYPE_REJECTED;
					metadata->parameter_length = 1;
					metadata_params[0] = E_INVALID_PARAM;
					break;
				}

				if (metadata_params[2 * i + 2] == ATTRIBUTE_SHUFFLE_OFF)
					control->mpris_shuffle_state = FALSE;
				else if (metadata_params[2 * i + 2] == ATTRIBUTE_SHUFFLE_GROUP)
					control->mpris_shuffle_state = TRUE;

				/* is uinput better appropriate for this? */
				emit_property_changed(control->dev->conn,
					control->dev->path,
					AUDIO_CONTROL_INTERFACE,
					"SetShuffleState",
					DBUS_TYPE_BOOLEAN,
					&metadata_params[2 * i + 2]);

				DBG("shuffle 0x%1X", metadata_params[2 * i + 2]);

				break;

			case ATTRIBUTE_SCAN:
				if (!(control->mpris_caps & MPRIS_CAN_SCAN)) {
					i = metadata_params[0] + 1;
					avrcp->code = CTYPE_REJECTED;
					metadata->parameter_length = 1;
					metadata_params[0] = E_INVALID_PARAM;
					break;
				}
				/* not supported on MPRIS 1.0 */
				DBG("scan 0x%1X", metadata_params[2 * i + 2]);
				break;
			default:
				i = metadata_params[0] + 1;
				avrcp->code = CTYPE_REJECTED;
				metadata->parameter_length = 1;
				metadata_params[0] = E_INVALID_PARAM;
				break;
			}
		}
		break;
	case GET_PLAYER_SETTING_ATTRIBUTE_TEXT:
	case GET_PLAYER_SETTING_VALUE_TEXT:
	case INFORM_DISPLAYABLE_CHARSET:
	case INFORM_BATT_STATUS_OF_CT:
		avrcp->code = CTYPE_NOT_IMPLEMENTED;
		break;
	case GET_ELEMENT_ATTRIBUTES:
		element_id = (metadata_params[0] << 24) |
				(metadata_params[1] << 16) |
				(metadata_params[2] << 8) |
				metadata_params[3];
		if (element_id != ELEMENT_PLAYING) {
			avrcp->code = CTYPE_REJECTED;
			metadata->parameter_length = 1;
			metadata_params[0] = E_INVALID_PARAM;
			break;
		}
		avrcp->code = CTYPE_STABLE;
		rsp[rsp_i++] = metadata_params[4];
		for (i = 0; i < metadata_params[4]; i++) {
			char metainfo[65536];
			uint16_t attribute_id = (metadata_params[i*2+5] << 8) |
						metadata_params[i*2+5+1];
			switch (attribute_id) {
			case METADATA_TITLE:
				rsp[rsp_i++] = (uint8_t) METADATA_TITLE &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_TITLE &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get title from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			case METADATA_ARTIST:
				rsp[rsp_i++] = (uint8_t) METADATA_ARTIST &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_ARTIST &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get artist from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			case METADATA_ALBUM:
				rsp[rsp_i++] = (uint8_t) METADATA_ALBUM &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_ALBUM &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get album from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			case METADATA_NUMBER:
				rsp[rsp_i++] = (uint8_t) METADATA_NUMBER &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_NUMBER &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get number from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			case METADATA_TOTAL:
				rsp[rsp_i++] = (uint8_t) METADATA_TOTAL &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_TOTAL &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get total from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			case METADATA_GENRE:
				rsp[rsp_i++] = (uint8_t) METADATA_GENRE &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_GENRE &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get genre from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			case METADATA_PLAY_TIME:
				rsp[rsp_i++] = (uint8_t) METADATA_PLAY_TIME &
						0xFF00;
				rsp[rsp_i++] = (uint8_t) METADATA_PLAY_TIME &
						0x00FF;
				snprintf(metainfo, 65536, "%s", "Get time from MPRIS");
				rsp[rsp_i++] = strlen(metainfo);
				memcpy(rsp + rsp_i, metainfo, rsp[rsp_i-1]);
				rsp_i += rsp[rsp_i-1];
				break;
			default:
				avrcp->code = CTYPE_REJECTED;
				metadata->parameter_length = 1;
				metadata_params[0] = E_INVALID_PARAM;
				return;
			}
			rsp[rsp_i++] = (uint8_t) CHARSET_UTF8 & 0xFF00;
			rsp[rsp_i++] = (uint8_t) CHARSET_UTF8 & 0x00FF;
			/* Attribute value length (2 bytes) */
			rsp[rsp_i++] = 0;
			rsp[rsp_i++] = 0;
			memcpy(metadata_params, rsp, rsp_i);
		}
	case GET_PLAY_STATUS:
		avrcp->code = CTYPE_STABLE;
		/* get song length, position and player status from MPRIS */
		for (i = 0; i < 8; i++)
			metadata_params[i] = 0xFF;
		metadata_params[8] = PLAY_STOPPED;
		break;
	default:
		avrcp->code = CTYPE_REJECTED;
		metadata->parameter_length = 1;
		metadata_params[0] = E_INVALID_COMMAND;
		break;
	}
}

static gboolean control_cb(GIOChannel *chan, GIOCondition cond,
				gpointer data)
{
	struct control *control = data;
	unsigned char buf[1024], *operands;
	struct avctp_header *avctp;
	struct avrcp_header *avrcp;
	int ret, packet_size, operand_count, sock;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		goto failed;

	sock = g_io_channel_unix_get_fd(control->io);

	ret = read(sock, buf, sizeof(buf));
	if (ret <= 0)
		goto failed;

	DBG("Got %d bytes of data for AVCTP session %p", ret, control);

	if ((unsigned int) ret < sizeof(struct avctp_header)) {
		error("Too small AVCTP packet");
		goto failed;
	}

	packet_size = ret;

	avctp = (struct avctp_header *) buf;

	DBG("AVCTP transaction %u, packet type %u, C/R %u, IPID %u, "
			"PID 0x%04X",
			avctp->transaction, avctp->packet_type,
			avctp->cr, avctp->ipid, ntohs(avctp->pid));

	ret -= sizeof(struct avctp_header);
	if ((unsigned int) ret < sizeof(struct avrcp_header)) {
		error("Too small AVRCP packet");
		goto failed;
	}

	avrcp = (struct avrcp_header *) (buf + sizeof(struct avctp_header));

	ret -= sizeof(struct avrcp_header);

	operands = buf + sizeof(struct avctp_header) + sizeof(struct avrcp_header);
	operand_count = ret;

	DBG("AVRCP %s 0x%01X, subunit_type 0x%02X, subunit_id 0x%01X, "
			"opcode 0x%02X, %d operands",
			avctp->cr ? "response" : "command",
			avrcp->code, avrcp->subunit_type, avrcp->subunit_id,
			avrcp->opcode, operand_count);

	if (avctp->packet_type != AVCTP_PACKET_SINGLE) {
		avctp->cr = AVCTP_RESPONSE;
		avrcp->code = CTYPE_NOT_IMPLEMENTED;
	} else if (avctp->pid != htons(AV_REMOTE_SVCLASS_ID)) {
		avctp->ipid = 1;
		avctp->cr = AVCTP_RESPONSE;
		avrcp->code = CTYPE_REJECTED;
	} else if (avctp->cr == AVCTP_COMMAND &&
			avrcp->code == CTYPE_CONTROL &&
			avrcp->subunit_type == SUBUNIT_PANEL &&
			avrcp->opcode == OP_PASSTHROUGH) {
		handle_panel_passthrough(control, operands, operand_count);
		avctp->cr = AVCTP_RESPONSE;
		avrcp->code = CTYPE_ACCEPTED;
	} else if (avctp->cr == AVCTP_COMMAND &&
			avrcp->code == CTYPE_STATUS &&
			(avrcp->opcode == OP_UNITINFO
			|| avrcp->opcode == OP_SUBUNITINFO)) {
		avctp->cr = AVCTP_RESPONSE;
		avrcp->code = CTYPE_STABLE;
		/* The first operand should be 0x07 for the UNITINFO response.
		 * Neither AVRCP (section 22.1, page 117) nor AVC Digital
		 * Interface Command Set (section 9.2.1, page 45) specs
		 * explain this value but both use it */
		if (operand_count >= 1 && avrcp->opcode == OP_UNITINFO)
			operands[0] = 0x07;
		if (operand_count >= 2)
			operands[1] = SUBUNIT_PANEL << 3;
		DBG("reply to %s", avrcp->opcode == OP_UNITINFO ?
				"OP_UNITINFO" : "OP_SUBUNITINFO");
	} else if (avctp->cr == AVCTP_COMMAND &&
			(avrcp->code == CTYPE_STATUS ||
				avrcp->code == CTYPE_CONTROL) &&
			avrcp->subunit_type == SUBUNIT_PANEL &&
			avrcp->opcode == OP_VENDORDEPENDENT &&
			operand_count >= 3) {
		uint32_t company_id = (operands[0] << 16) |
					(operands[1] << 8) |
					(operands[2]);
		DBG("AVRCP vendor 0x%06X dependent command", company_id);
		if (company_id == IEEEID_BTSIG) {
			DBG("AVRCP metadata PDU");
			avctp->cr = AVCTP_RESPONSE;
			handle_metadata_pdu(control, avrcp, operand_count);
		} else {
			avctp->cr = AVCTP_RESPONSE;
			avrcp->code = CTYPE_NOT_IMPLEMENTED;
		}
	} else if (avctp->cr == AVCTP_RESPONSE) {
		goto noresponse;
	} else {
		avctp->cr = AVCTP_RESPONSE;
		avrcp->code = CTYPE_REJECTED;
	}

	ret = write(sock, buf, packet_size);

noresponse:

	return TRUE;

failed:
	DBG("AVCTP session %p got disconnected", control);
	avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
	return FALSE;
}

static int uinput_create(char *name)
{
	struct uinput_dev dev;
	int fd, err, i;

	fd = open("/dev/uinput", O_RDWR);
	if (fd < 0) {
		fd = open("/dev/input/uinput", O_RDWR);
		if (fd < 0) {
			fd = open("/dev/misc/uinput", O_RDWR);
			if (fd < 0) {
				err = errno;
				error("Can't open input device: %s (%d)",
							strerror(err), err);
				return -err;
			}
		}
	}

	memset(&dev, 0, sizeof(dev));
	if (name)
		strncpy(dev.name, name, UINPUT_MAX_NAME_SIZE - 1);

	dev.id.bustype = BUS_BLUETOOTH;
	dev.id.vendor  = 0x0000;
	dev.id.product = 0x0000;
	dev.id.version = 0x0000;

	if (write(fd, &dev, sizeof(dev)) < 0) {
		err = errno;
		error("Can't write device information: %s (%d)",
						strerror(err), err);
		close(fd);
		errno = err;
		return -err;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_EVBIT, EV_REP);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);

	for (i = 0; key_map[i].name != NULL; i++)
		ioctl(fd, UI_SET_KEYBIT, key_map[i].uinput);

	if (ioctl(fd, UI_DEV_CREATE, NULL) < 0) {
		err = errno;
		error("Can't create uinput device: %s (%d)",
						strerror(err), err);
		close(fd);
		errno = err;
		return -err;
	}

	return fd;
}

static void init_uinput(struct control *control)
{
	struct audio_device *dev = control->dev;
	char address[18], name[248 + 1];

	device_get_name(dev->btd_dev, name, sizeof(name));
	if (g_str_equal(name, "Nokia CK-20W")) {
		control->key_quirks[FORWARD_OP] |= QUIRK_NO_RELEASE;
		control->key_quirks[BACKWARD_OP] |= QUIRK_NO_RELEASE;
		control->key_quirks[PLAY_OP] |= QUIRK_NO_RELEASE;
		control->key_quirks[PAUSE_OP] |= QUIRK_NO_RELEASE;
	}

	ba2str(&dev->dst, address);

	control->uinput = uinput_create(address);
	if (control->uinput < 0)
		error("AVRCP: failed to init uinput for %s", address);
	else
		DBG("AVRCP: uinput initialized for %s", address);
}

static void avctp_connect_cb(GIOChannel *chan, GError *err, gpointer data)
{
	struct control *control = data;
	char address[18];
	uint16_t imtu;
	GError *gerr = NULL;

	if (err) {
		avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
		error("%s", err->message);
		return;
	}

	bt_io_get(chan, BT_IO_L2CAP, &gerr,
			BT_IO_OPT_DEST, &address,
			BT_IO_OPT_IMTU, &imtu,
			BT_IO_OPT_INVALID);
	if (gerr) {
		avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
		error("%s", gerr->message);
		g_error_free(gerr);
		return;
	}

	DBG("AVCTP: connected to %s", address);

	if (!control->io)
		control->io = g_io_channel_ref(chan);

	init_uinput(control);

	avctp_set_state(control, AVCTP_STATE_CONNECTED);
	control->mtu = imtu;
	control->io_id = g_io_add_watch(chan,
				G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
				(GIOFunc) control_cb, control);
}

static void auth_cb(DBusError *derr, void *user_data)
{
	struct control *control = user_data;
	GError *err = NULL;

	if (control->io_id) {
		g_source_remove(control->io_id);
		control->io_id = 0;
	}

	if (derr && dbus_error_is_set(derr)) {
		error("Access denied: %s", derr->message);
		avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
		return;
	}

	if (!bt_io_accept(control->io, avctp_connect_cb, control,
								NULL, &err)) {
		error("bt_io_accept: %s", err->message);
		g_error_free(err);
		avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
	}
}

static void avctp_confirm_cb(GIOChannel *chan, gpointer data)
{
	struct control *control = NULL;
	struct audio_device *dev;
	char address[18];
	bdaddr_t src, dst;
	GError *err = NULL;

	bt_io_get(chan, BT_IO_L2CAP, &err,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_DEST, address,
			BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		g_io_channel_shutdown(chan, TRUE, NULL);
		return;
	}

	dev = manager_get_device(&src, &dst, TRUE);
	if (!dev) {
		error("Unable to get audio device object for %s", address);
		goto drop;
	}

	if (!dev->control) {
		btd_device_add_uuid(dev->btd_dev, AVRCP_REMOTE_UUID);
		if (!dev->control)
			goto drop;
	}

	control = dev->control;

	if (control->io) {
		error("Refusing unexpected connect from %s", address);
		goto drop;
	}

	avctp_set_state(control, AVCTP_STATE_CONNECTING);
	control->io = g_io_channel_ref(chan);

	if (audio_device_request_authorization(dev, AVRCP_TARGET_UUID,
						auth_cb, dev->control) < 0)
		goto drop;

	control->io_id = g_io_add_watch(chan, G_IO_ERR | G_IO_HUP | G_IO_NVAL,
							control_cb, control);
	return;

drop:
	if (!control || !control->io)
		g_io_channel_shutdown(chan, TRUE, NULL);
	if (control)
		avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
}

static GIOChannel *avctp_server_socket(const bdaddr_t *src, gboolean master)
{
	GError *err = NULL;
	GIOChannel *io;

	io = bt_io_listen(BT_IO_L2CAP, NULL, avctp_confirm_cb, NULL,
				NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, src,
				BT_IO_OPT_PSM, AVCTP_PSM,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
				BT_IO_OPT_MASTER, master,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("%s", err->message);
		g_error_free(err);
	}

	return io;
}

gboolean avrcp_connect(struct audio_device *dev)
{
	struct control *control = dev->control;
	GError *err = NULL;
	GIOChannel *io;

	if (control->state > AVCTP_STATE_DISCONNECTED)
		return TRUE;

	avctp_set_state(control, AVCTP_STATE_CONNECTING);

	io = bt_io_connect(BT_IO_L2CAP, avctp_connect_cb, control, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, &dev->src,
				BT_IO_OPT_DEST_BDADDR, &dev->dst,
				BT_IO_OPT_PSM, AVCTP_PSM,
				BT_IO_OPT_INVALID);
	if (err) {
		avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
		error("%s", err->message);
		g_error_free(err);
		return FALSE;
	}

	control->io = io;

	return TRUE;
}

void avrcp_disconnect(struct audio_device *dev)
{
	struct control *control = dev->control;

	if (!(control && control->io))
		return;

	avctp_set_state(control, AVCTP_STATE_DISCONNECTED);
}

int avrcp_register(DBusConnection *conn, const bdaddr_t *src, GKeyFile *config)
{
	sdp_record_t *record;
	gboolean tmp, master = TRUE;
	GError *err = NULL;
	struct avctp_server *server;

	if (config) {
		tmp = g_key_file_get_boolean(config, "General",
							"Master", &err);
		if (err) {
			DBG("audio.conf: %s", err->message);
			g_error_free(err);
		} else
			master = tmp;
	}

	server = g_new0(struct avctp_server, 1);
	if (!server)
		return -ENOMEM;

	if (!connection)
		connection = dbus_connection_ref(conn);

	record = avrcp_tg_record();
	if (!record) {
		error("Unable to allocate new service record");
		g_free(server);
		return -1;
	}

	if (add_record_to_server(src, record) < 0) {
		error("Unable to register AVRCP target service record");
		g_free(server);
		sdp_record_free(record);
		return -1;
	}
	server->tg_record_id = record->handle;

	record = avrcp_ct_record();
	if (!record) {
		error("Unable to allocate new service record");
		g_free(server);
		return -1;
	}

	if (add_record_to_server(src, record) < 0) {
		error("Unable to register AVRCP controller service record");
		sdp_record_free(record);
		g_free(server);
		return -1;
	}
	server->ct_record_id = record->handle;

	server->io = avctp_server_socket(src, master);
	if (!server->io) {
		remove_record_from_server(server->ct_record_id);
		remove_record_from_server(server->tg_record_id);
		g_free(server);
		return -1;
	}

	bacpy(&server->src, src);

	servers = g_slist_append(servers, server);

	return 0;
}

static struct avctp_server *find_server(GSList *list, const bdaddr_t *src)
{
	GSList *l;

	for (l = list; l; l = l->next) {
		struct avctp_server *server = l->data;

		if (bacmp(&server->src, src) == 0)
			return server;
	}

	return NULL;
}

void avrcp_unregister(const bdaddr_t *src)
{
	struct avctp_server *server;

	server = find_server(servers, src);
	if (!server)
		return;

	servers = g_slist_remove(servers, server);

	remove_record_from_server(server->ct_record_id);
	remove_record_from_server(server->tg_record_id);

	g_io_channel_shutdown(server->io, TRUE, NULL);
	g_io_channel_unref(server->io);
	g_free(server);

	if (servers)
		return;

	dbus_connection_unref(connection);
	connection = NULL;
}

static DBusMessage *control_is_connected(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	DBusMessage *reply;
	dbus_bool_t connected;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	connected = (control->state == AVCTP_STATE_CONNECTED);

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &connected,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *control_connect(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	DBusMessage *reply;
	int err;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	avrcp_connect(device);

	return dbus_message_new_method_return(msg);
}

static int avctp_send_passthrough(struct control *control, uint8_t op)
{
	unsigned char buf[AVCTP_HEADER_LENGTH + AVRCP_HEADER_LENGTH + 2];
	struct avctp_header *avctp = (void *) buf;
	struct avrcp_header *avrcp = (void *) &buf[AVCTP_HEADER_LENGTH];
	uint8_t *operands = &buf[AVCTP_HEADER_LENGTH + AVRCP_HEADER_LENGTH];
	int err, sk = g_io_channel_unix_get_fd(control->io);
	static uint8_t transaction = 0;

	memset(buf, 0, sizeof(buf));

	avctp->transaction = transaction++;
	avctp->packet_type = AVCTP_PACKET_SINGLE;
	avctp->cr = AVCTP_COMMAND;
	avctp->pid = htons(AV_REMOTE_SVCLASS_ID);

	avrcp->code = CTYPE_CONTROL;
	avrcp->subunit_type = SUBUNIT_PANEL;
	avrcp->opcode = OP_PASSTHROUGH;

	operands[0] = op & 0x7f;
	operands[1] = 0;

	err = write(sk, buf, sizeof(buf));
	if (err < 0)
		return err;

	/* Button release */
	avctp->transaction = transaction++;
	operands[0] |= 0x80;

	return write(sk, buf, sizeof(buf));
}

static DBusMessage *volume_up(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	DBusMessage *reply;
	int err;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (control->state != AVCTP_STATE_CONNECTED)
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".NotConnected",
					"Device not Connected");

	if (!control->target)
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".NotSupported",
					"AVRCP Target role not supported");

	err = avctp_send_passthrough(control, VOL_UP_OP);
	if (err < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
							"%s", strerror(-err));

	return dbus_message_new_method_return(msg);
}

static DBusMessage *volume_down(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	DBusMessage *reply;
	int err;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	if (control->state != AVCTP_STATE_CONNECTED)
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".NotConnected",
					"Device not Connected");

	if (!control->target)
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".NotSupported",
					"AVRCP Target role not supported");

	err = avctp_send_passthrough(control, VOL_DOWN_OP);
	if (err < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
							"%s", strerror(-err));

	return dbus_message_new_method_return(msg);
}

static DBusMessage *control_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct audio_device *device = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	gboolean value;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Connected */
	value = (device->control->state == AVCTP_STATE_CONNECTED);
	dict_append_entry(&dict, "Connected", DBUS_TYPE_BOOLEAN, &value);

	/* PlayerCapabilities */
	value = device->control->mpris_caps;
	dict_append_entry(&dict, "PlayerCapabilities", DBUS_TYPE_UINT32, &value);

	/* PlayState */
	value = device->control->mpris_play_state;
	dict_append_entry(&dict, "PlayState", DBUS_TYPE_UINT32, &value);

	/* ShuffleState */
	value = device->control->mpris_shuffle_state;
	dict_append_entry(&dict, "ShuffleState", DBUS_TYPE_BOOLEAN, &value);

	/* RepeatState */
	value = device->control->mpris_repeat_state;
	dict_append_entry(&dict, "RepeatState", DBUS_TYPE_BOOLEAN, &value);

	/* EndlessState */
	value = device->control->mpris_endless_state;
	dict_append_entry(&dict, "EndlessState", DBUS_TYPE_BOOLEAN, &value);

	/* MediaTitle */
	dict_append_entry(&dict, "MediaTitle", DBUS_TYPE_STRING,
				&device->control->mpris_title);
	/* MediaArtist */
	dict_append_entry(&dict, "MediaArtist", DBUS_TYPE_STRING,
				&device->control->mpris_artist);
	/* MediaAlbum */
	dict_append_entry(&dict, "MediaAlbum", DBUS_TYPE_STRING,
				&device->control->mpris_album);
	/* MediaNumber */
	dict_append_entry(&dict, "MediaNumber", DBUS_TYPE_STRING,
				&device->control->mpris_number);
	/* MediaGenre */
	dict_append_entry(&dict, "MediaGenre", DBUS_TYPE_STRING,
				&device->control->mpris_genre);

	/* MediaLength */
	dict_append_entry(&dict, "MediaLength", DBUS_TYPE_UINT32,
				&device->control->mpris_total);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *control_set_property(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	const char *property;
	DBusMessageIter iter;
	DBusMessageIter sub;

	if (!dbus_message_iter_init(msg, &iter))
		return invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return invalid_args(msg);
	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("PlayerCapabilities", property) ||
			g_str_equal("PlayState", property) ||
			g_str_equal("MediaLength", property)) {
		uint32_t value;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT32)
			return invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &value);

		if (g_str_equal("PlayerCapabilities", property))
			control->mpris_caps = value;
		else if (g_str_equal("PlayState", property))
			control->mpris_play_state = value;
		else if (g_str_equal("MediaLength", property))
			control->mpris_total = value;

		emit_property_changed(conn, dbus_message_get_path(msg),
					AUDIO_CONTROL_INTERFACE, property,
					DBUS_TYPE_UINT32, &value);

		return dbus_message_new_method_return(msg);

	} else if (g_str_equal("ShuffleState", property) ||
			g_str_equal("RepeatState", property) ||
			g_str_equal("EndlessState", property)) {
		gboolean value;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &value);

		if (g_str_equal("ShuffleState", property))
			control->mpris_shuffle_state = value;
		else if (g_str_equal("RepeatState", property))
			control->mpris_repeat_state = value;
		else if (g_str_equal("EndlessState", property))
			control->mpris_endless_state = value;

		emit_property_changed(conn, dbus_message_get_path(msg),
					AUDIO_CONTROL_INTERFACE, property,
					DBUS_TYPE_BOOLEAN, &value);

		return dbus_message_new_method_return(msg);
	} else if (g_str_equal("MediaTitle", property) ||
			g_str_equal("MediaArtist", property) ||
			g_str_equal("MediaAlbum", property) ||
			g_str_equal("MediaNumber", property) ||
			g_str_equal("MediaGenre", property)) {
		const char *value;
		int size;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &value);
		size = strlen(value) + 1;

		if (g_str_equal("MediaTitle", property)) {
			control->mpris_title = malloc(size * sizeof(char));
			strncpy(control->mpris_title, value, size);
		} else if (g_str_equal("MediaArtist", property)) {
			control->mpris_artist = malloc(size * sizeof(char));
			strncpy(control->mpris_artist, value, size);
		} else if (g_str_equal("MediaAlbum", property)) {
			control->mpris_album = malloc(size * sizeof(char));
			strncpy(control->mpris_album, value, size);
		} else if (g_str_equal("MediaNumber", property)) {
			control->mpris_number = malloc(size * sizeof(char));
			strncpy(control->mpris_number, value, size);
		} else if (g_str_equal("MediaGenre", property)) {
			control->mpris_genre = malloc(size * sizeof(char));
			strncpy(control->mpris_genre, value, size);
		}

		emit_property_changed(conn, dbus_message_get_path(msg),
					AUDIO_CONTROL_INTERFACE, property,
					DBUS_TYPE_STRING, &value);

		return dbus_message_new_method_return(msg);
	}

	return invalid_args(msg);
}

static GDBusMethodTable control_methods[] = {
	{ "Connect",		"", "",	control_connect },
	{ "IsConnected",	"",	"b",	control_is_connected,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "GetProperties",	"",	"a{sv}",control_get_properties },
	{ "SetProperty",	"sv",	"",	control_set_property },
	{ "VolumeUp",		"",	"",	volume_up },
	{ "VolumeDown",		"",	"",	volume_down },
	{ NULL, NULL, NULL, NULL }
};

static GDBusSignalTable control_signals[] = {
	{ "Connected",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED},
	{ "Disconnected",		"",	G_DBUS_SIGNAL_FLAG_DEPRECATED},
	{ "PropertyChanged",		"sv"	},
	{ "SetRepeatState",		"b"	},
	{ "SetShuffleState",		"b"	},
	{ "SetScanState",		"b"	},
	{ NULL, NULL }
};

static void path_unregister(void *data)
{
	struct audio_device *dev = data;
	struct control *control = dev->control;

	DBG("Unregistered interface %s on path %s",
		AUDIO_CONTROL_INTERFACE, dev->path);

	if (control->state != AVCTP_STATE_DISCONNECTED)
		avctp_disconnected(dev);

	g_free(control);
	dev->control = NULL;
}

void control_unregister(struct audio_device *dev)
{
	g_dbus_unregister_interface(dev->conn, dev->path,
		AUDIO_CONTROL_INTERFACE);
}

void control_update(struct audio_device *dev, uint16_t uuid16)
{
	struct control *control = dev->control;

	if (uuid16 == AV_REMOTE_TARGET_SVCLASS_ID)
		control->target = TRUE;
}

struct control *control_init(struct audio_device *dev, uint16_t uuid16)
{
	struct control *control;

	if (!g_dbus_register_interface(dev->conn, dev->path,
					AUDIO_CONTROL_INTERFACE,
					control_methods, control_signals, NULL,
					dev, path_unregister))
		return NULL;

	DBG("Registered interface %s on path %s",
		AUDIO_CONTROL_INTERFACE, dev->path);

	control = g_new0(struct control, 1);
	control->dev = dev;
	control->state = AVCTP_STATE_DISCONNECTED;
	control->uinput = -1;

	if (uuid16 == AV_REMOTE_TARGET_SVCLASS_ID)
		control->target = TRUE;

	return control;
}

gboolean control_is_active(struct audio_device *dev)
{
	struct control *control = dev->control;

	if (control && control->state != AVCTP_STATE_DISCONNECTED)
		return TRUE;

	return FALSE;
}

unsigned int avctp_add_state_cb(avctp_state_cb cb, void *user_data)
{
	struct avctp_state_callback *state_cb;
	static unsigned int id = 0;

	state_cb = g_new(struct avctp_state_callback, 1);
	state_cb->cb = cb;
	state_cb->user_data = user_data;
	state_cb->id = ++id;

	avctp_callbacks = g_slist_append(avctp_callbacks, state_cb);

	return state_cb->id;
}

gboolean avctp_remove_state_cb(unsigned int id)
{
	GSList *l;

	for (l = avctp_callbacks; l != NULL; l = l->next) {
		struct avctp_state_callback *cb = l->data;
		if (cb && cb->id == id) {
			avctp_callbacks = g_slist_remove(avctp_callbacks, cb);
			g_free(cb);
			return TRUE;
		}
	}

	return FALSE;
}
