/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2025 Axis Communications AB.
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Martin Nordholts, Axis Communications AB, 2025.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

#ifndef _NICE_TEST_COMMON_H
#define _NICE_TEST_COMMON_H

#include <agent.h>
#include <glib.h>

typedef struct _TestTurnServer {
  GSubprocess *sp;
  guint port;
} TestTurnServer;

TestTurnServer *
test_common_turn_server_new (const gchar *server_ip, const gchar *user, const gchar *pass);

void
test_common_turn_server_destroy (TestTurnServer *turn_server);

void
test_common_set_credentials (NiceAgent *lagent, guint lstream, NiceAgent *ragent, guint rstream);

gboolean
test_common_wait_for_tcp_socket (const gchar *service_name, const gchar *host, guint16 port);

gboolean
test_common_turnserver_available (void);

void
test_common_set_candidates (NiceAgent *from, guint from_stream, NiceAgent *to, guint to_stream,
    guint component, gboolean remove_non_relay, gboolean force_relay);

#endif /* _NICE_TEST_COMMON_H */
