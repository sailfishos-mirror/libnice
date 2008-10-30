/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2006, 2007 Collabora Ltd.
 *  Contact: Dafydd Harries
 * (C) 2006, 2007 Nokia Corporation. All rights reserved.
 *  Contact: Kai Vehmanen
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
 *   Dafydd Harries, Collabora Ltd.
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

#ifndef _ADDRESS_H
#define _ADDRESS_H

#include <glib.h>

#ifdef G_OS_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

G_BEGIN_DECLS

#define NICE_ADDRESS_STRING_LEN INET6_ADDRSTRLEN

typedef struct _NiceAddress NiceAddress;

/* note: clients need to know the storage size, so needs to be public */
struct _NiceAddress
{
  union
  {
    struct sockaddr     addr;
    struct sockaddr_in  ip4;
    struct sockaddr_in6 ip6;
  } s;
};

static inline void
nice_address_init (NiceAddress *addr)
{
  addr->s.addr.sa_family = AF_UNSPEC;
}

NiceAddress *
nice_address_new (void);

void
nice_address_free (NiceAddress *addr);

NiceAddress *
nice_address_dup (const NiceAddress *a);

void
nice_address_set_ipv4 (NiceAddress *addr, guint32 addr_ipv4);

void
nice_address_set_ipv6 (NiceAddress *addr, const guchar *addr_ipv6);

void
nice_address_set_port (NiceAddress *addr, guint port);

guint
nice_address_get_port (const NiceAddress *addr);

gboolean
nice_address_set_from_string (NiceAddress *addr, const gchar *str);

void
nice_address_set_from_sockaddr (NiceAddress *addr, const struct sockaddr *sin);

void
nice_address_copy_to_sockaddr (const NiceAddress *addr, struct sockaddr *sin);

gboolean
nice_address_equal (const NiceAddress *a, const NiceAddress *b);

void
nice_address_to_string (const NiceAddress *addr, gchar *dst);

gboolean
nice_address_is_private (const NiceAddress *a);

G_GNUC_WARN_UNUSED_RESULT
gboolean
nice_address_is_valid (const NiceAddress *a);

G_END_DECLS

#endif /* _ADDRESS_H */
