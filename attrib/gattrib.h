/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
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
#ifndef __GATTRIB_H
#define __GATTRIB_H

#ifdef __cplusplus
extern "C" {
#endif

#define GATTRIB_ALL_EVENTS 0xFF

struct _GAttrib;
typedef struct _GAttrib GAttrib;

typedef void (*GAttribResultFunc) (guint8 status, const guint8 *pdu,
					guint16 len, gpointer user_data);
typedef void (*GAttribDisconnectFunc)(gpointer user_data);
typedef void (*GAttribDebugFunc)(const char *str, gpointer user_data);
typedef void (*GAttribNotifyFunc)(const guint8 *pdu, guint16 len,
							gpointer user_data);

GAttrib *g_attrib_new(GIOChannel *io);
GAttrib *g_attrib_ref(GAttrib *attrib);
void g_attrib_unref(GAttrib *attrib);

gboolean g_attrib_set_disconnect_function(GAttrib *attrib,
		GAttribDisconnectFunc disconnect, gpointer user_data);

guint g_attrib_send(GAttrib *attrib, guint8 opcode, const guint8 *pdu,
				guint16 len, GAttribResultFunc func,
				gpointer user_data, GDestroyNotify notify);
gboolean g_attrib_cancel(GAttrib *attrib, guint id);
gboolean g_attrib_cancel_all(GAttrib *attrib);

gboolean g_attrib_set_debug(GAttrib *attrib,
		GAttribDebugFunc func, gpointer user_data);

guint g_attrib_register(GAttrib *attrib, guint8 opcode,
		GAttribNotifyFunc func, gpointer user_data,
					GDestroyNotify notify);

gboolean g_attrib_unregister(GAttrib *attrib, guint id);
gboolean g_attrib_unregister_all(GAttrib *attrib);

#ifdef __cplusplus
}
#endif
#endif
