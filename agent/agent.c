/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2006-2010, 2013 Collabora Ltd.
 *  Contact: Youness Alaoui
 * (C) 2006-2010 Nokia Corporation. All rights reserved.
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
 *   Youness Alaoui, Collabora Ltd.
 *   Kai Vehmanen, Nokia
 *   Philip Withnall, Collabora Ltd.
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


#ifdef HAVE_CONFIG_H
# include <config.h>
#else
#define NICEAPI_EXPORT
#endif

#include <glib.h>
#include <gobject/gvaluecollector.h>

#include <string.h>
#include <errno.h>

#ifndef G_OS_WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "debug.h"

#include "socket.h"
#include "socket-priv.h"
#include "stun/usages/turn.h"
#include "candidate-priv.h"
#include "component.h"
#include "conncheck.h"
#include "discovery.h"
#include "agent.h"
#include "agent-priv.h"
#include "iostream.h"

#include "stream.h"
#include "interfaces.h"

#include "pseudotcp.h"
#include "agent-enum-types.h"

#define DEFAULT_STUN_PORT  3478
#define DEFAULT_UPNP_TIMEOUT 200  /* milliseconds */
#define DEFAULT_IDLE_TIMEOUT 5000 /* milliseconds */

#define MAX_TCP_MTU 1400 /* Use 1400 because of VPNs and we assume IEE 802.3 */


static void agent_consume_next_rfc4571_chunk (NiceAgent *agent,
    NiceComponent *component, NiceInputMessage *messages, guint n_messages,
    NiceInputMessageIter *iter);
static void
nice_debug_input_message_composition (const NiceInputMessage *messages,
    guint n_messages);
static gsize append_buffer_to_input_messages (gboolean bytestream_tcp,
    NiceInputMessage *messages, guint n_messages, NiceInputMessageIter *iter,
    const guint8 *buffer, gsize buffer_length);
static gsize nice_input_message_iter_get_message_capacity (
    NiceInputMessageIter *iter, NiceInputMessage *messages, guint n_messages);
static const gchar *_cand_type_to_sdp (NiceCandidateType type);

G_DEFINE_TYPE (NiceAgent, nice_agent, G_TYPE_OBJECT);

enum
{
  PROP_COMPATIBILITY = 1,
  PROP_MAIN_CONTEXT,
  PROP_STUN_SERVER,
  PROP_STUN_SERVER_PORT,
  PROP_CONTROLLING_MODE,
  PROP_FULL_MODE,
  PROP_STUN_PACING_TIMER,
  PROP_MAX_CONNECTIVITY_CHECKS,
  PROP_PROXY_TYPE,
  PROP_PROXY_IP,
  PROP_PROXY_PORT,
  PROP_PROXY_USERNAME,
  PROP_PROXY_PASSWORD,
  PROP_PROXY_EXTRA_HEADERS,
  PROP_UPNP,
  PROP_UPNP_TIMEOUT,
  PROP_RELIABLE,
  PROP_ICE_UDP,
  PROP_ICE_TCP,
  PROP_BYTESTREAM_TCP,
  PROP_KEEPALIVE_CONNCHECK,
  PROP_FORCE_RELAY,
  PROP_STUN_MAX_RETRANSMISSIONS,
  PROP_STUN_INITIAL_TIMEOUT,
  PROP_STUN_RELIABLE_TIMEOUT,
  PROP_NOMINATION_MODE,
  PROP_ICE_TRICKLE,
  PROP_SUPPORT_RENOMINATION,
  PROP_IDLE_TIMEOUT,
  PROP_CONSENT_FRESHNESS,
};


enum
{
  SIGNAL_COMPONENT_STATE_CHANGED,
  SIGNAL_CANDIDATE_GATHERING_DONE,
  SIGNAL_NEW_SELECTED_PAIR,
  SIGNAL_NEW_CANDIDATE,
  SIGNAL_NEW_REMOTE_CANDIDATE,
  SIGNAL_INITIAL_BINDING_REQUEST_RECEIVED,
  SIGNAL_RELIABLE_TRANSPORT_WRITABLE,
  SIGNAL_STREAMS_REMOVED,
  SIGNAL_NEW_SELECTED_PAIR_FULL,
  SIGNAL_NEW_CANDIDATE_FULL,
  SIGNAL_NEW_REMOTE_CANDIDATE_FULL,

  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void pseudo_tcp_socket_opened (PseudoTcpSocket *sock, gpointer user_data);
static void pseudo_tcp_socket_readable (PseudoTcpSocket *sock, gpointer user_data);
static void pseudo_tcp_socket_writable (PseudoTcpSocket *sock, gpointer user_data);
static void pseudo_tcp_socket_closed (PseudoTcpSocket *sock, guint32 err,
    gpointer user_data);
static PseudoTcpWriteResult pseudo_tcp_socket_write_packet (PseudoTcpSocket *sock,
    const gchar *buffer, guint32 len, gpointer user_data);
static void adjust_tcp_clock (NiceAgent *agent, NiceStream *stream, NiceComponent *component);

static void nice_agent_constructed (GObject *object);
static void nice_agent_dispose (GObject *object);
static void nice_agent_get_property (GObject *object,
  guint property_id, GValue *value, GParamSpec *pspec);
static void nice_agent_set_property (GObject *object,
  guint property_id, const GValue *value, GParamSpec *pspec);

void agent_lock (NiceAgent *agent)
{
  g_mutex_lock (&agent->agent_mutex);
}

void agent_unlock (NiceAgent *agent)
{
  g_mutex_unlock (&agent->agent_mutex);
}

static GType _nice_agent_stream_ids_get_type (void);

G_DEFINE_POINTER_TYPE (_NiceAgentStreamIds, _nice_agent_stream_ids);

#define NICE_TYPE_AGENT_STREAM_IDS _nice_agent_stream_ids_get_type ()

typedef struct {
  guint signal_id;
  GSignalQuery query;
  GValue *params;
} QueuedSignal;


static void
free_queued_signal (QueuedSignal *sig)
{
  guint i;

  g_value_unset (&sig->params[0]);

  for (i = 0; i < sig->query.n_params; i++) {
    if (G_VALUE_HOLDS(&sig->params[i + 1], NICE_TYPE_AGENT_STREAM_IDS))
      g_free (g_value_get_pointer (&sig->params[i + 1]));
    g_value_unset (&sig->params[i + 1]);
  }

  g_slice_free1 (sizeof(GValue) * (sig->query.n_params + 1), sig->params);
  g_slice_free (QueuedSignal, sig);
}

void
agent_unlock_and_emit (NiceAgent *agent)
{
  GQueue queue = G_QUEUE_INIT;
  QueuedSignal *sig;

  queue = agent->pending_signals;
  g_queue_init (&agent->pending_signals);

  agent_unlock (agent);

  while ((sig = g_queue_pop_head (&queue))) {
    g_signal_emitv (sig->params, sig->signal_id, 0, NULL);

    free_queued_signal (sig);
  }
}

static void
agent_queue_signal (NiceAgent *agent, guint signal_id, ...)
{
  QueuedSignal *sig;
  guint i;
  gchar *error = NULL;
  va_list var_args;

  sig = g_slice_new (QueuedSignal);
  g_signal_query (signal_id, &sig->query);

  sig->signal_id = signal_id;
  sig->params = g_slice_alloc0 (sizeof(GValue) * (sig->query.n_params + 1));

  g_value_init (&sig->params[0], G_TYPE_OBJECT);
  g_value_set_object (&sig->params[0], agent);

  va_start (var_args, signal_id);
  for (i = 0; i < sig->query.n_params; i++) {
    G_VALUE_COLLECT_INIT (&sig->params[i + 1], sig->query.param_types[i],
        var_args, 0, &error);
    if (error)
      break;
  }
  va_end (var_args);

  if (error) {
    free_queued_signal (sig);
    g_critical ("Error collecting values for signal: %s", error);
    g_free (error);
    return;
  }

  g_queue_push_tail (&agent->pending_signals, sig);
}


StunUsageIceCompatibility
agent_to_ice_compatibility (NiceAgent *agent)
{
  return agent->compatibility == NICE_COMPATIBILITY_GOOGLE ?
      STUN_USAGE_ICE_COMPATIBILITY_GOOGLE :
      agent->compatibility == NICE_COMPATIBILITY_MSN ?
      STUN_USAGE_ICE_COMPATIBILITY_MSN :
      agent->compatibility == NICE_COMPATIBILITY_WLM2009 ?
      STUN_USAGE_ICE_COMPATIBILITY_MSICE2 :
      agent->compatibility == NICE_COMPATIBILITY_OC2007 ?
      STUN_USAGE_ICE_COMPATIBILITY_MSN :
      agent->compatibility == NICE_COMPATIBILITY_OC2007R2 ?
      STUN_USAGE_ICE_COMPATIBILITY_MSICE2 :
      STUN_USAGE_ICE_COMPATIBILITY_RFC5245;
}


StunUsageTurnCompatibility
agent_to_turn_compatibility (NiceAgent *agent)
{
  return agent->compatibility == NICE_COMPATIBILITY_GOOGLE ?
      STUN_USAGE_TURN_COMPATIBILITY_GOOGLE :
      agent->compatibility == NICE_COMPATIBILITY_MSN ?
      STUN_USAGE_TURN_COMPATIBILITY_MSN :
      agent->compatibility == NICE_COMPATIBILITY_WLM2009 ?
      STUN_USAGE_TURN_COMPATIBILITY_MSN :
      agent->compatibility == NICE_COMPATIBILITY_OC2007 ?
      STUN_USAGE_TURN_COMPATIBILITY_OC2007 :
      agent->compatibility == NICE_COMPATIBILITY_OC2007R2 ?
      STUN_USAGE_TURN_COMPATIBILITY_OC2007 :
      STUN_USAGE_TURN_COMPATIBILITY_RFC5766;
}

NiceTurnSocketCompatibility
agent_to_turn_socket_compatibility (NiceAgent *agent)
{
  return agent->compatibility == NICE_COMPATIBILITY_GOOGLE ?
      NICE_TURN_SOCKET_COMPATIBILITY_GOOGLE :
      agent->compatibility == NICE_COMPATIBILITY_MSN ?
      NICE_TURN_SOCKET_COMPATIBILITY_MSN :
      agent->compatibility == NICE_COMPATIBILITY_WLM2009 ?
      NICE_TURN_SOCKET_COMPATIBILITY_MSN :
      agent->compatibility == NICE_COMPATIBILITY_OC2007 ?
      NICE_TURN_SOCKET_COMPATIBILITY_OC2007 :
      agent->compatibility == NICE_COMPATIBILITY_OC2007R2 ?
      NICE_TURN_SOCKET_COMPATIBILITY_OC2007 :
      NICE_TURN_SOCKET_COMPATIBILITY_RFC5766;
}

NiceStream *agent_find_stream (NiceAgent *agent, guint stream_id)
{
  GSList *i;

  for (i = agent->streams; i; i = i->next)
    {
      NiceStream *s = i->data;

      if (s->id == stream_id)
        return s;
    }

  return NULL;
}


gboolean
agent_find_component (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  NiceStream **stream,
  NiceComponent **component)
{
  NiceStream *s;
  NiceComponent *c;

  s = agent_find_stream (agent, stream_id);

  if (s == NULL)
    return FALSE;

  c = nice_stream_find_component_by_id (s, component_id);

  if (c == NULL)
    return FALSE;

  if (stream)
    *stream = s;

  if (component)
    *component = c;

  return TRUE;
}

static void
nice_agent_class_init (NiceAgentClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructed = nice_agent_constructed;
  gobject_class->get_property = nice_agent_get_property;
  gobject_class->set_property = nice_agent_set_property;
  gobject_class->dispose = nice_agent_dispose;

  /* install properties */
  /**
   * NiceAgent:main-context:
   *
   * A GLib main context is needed for all timeouts used by libnice.
   * This is a property being set by the nice_agent_new() call.
   */
  g_object_class_install_property (gobject_class, PROP_MAIN_CONTEXT,
      g_param_spec_pointer (
         "main-context",
         "The GMainContext to use for timeouts",
         "The GMainContext to use for timeouts",
         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * NiceAgent:compatibility:
   *
   * The Nice agent can work in various compatibility modes depending on
   * what the application/peer needs.
   * <para> See also: #NiceCompatibility</para>
   */
  g_object_class_install_property (gobject_class, PROP_COMPATIBILITY,
      g_param_spec_uint (
         "compatibility",
         "ICE specification compatibility",
         "The compatibility mode for the agent",
         NICE_COMPATIBILITY_RFC5245, NICE_COMPATIBILITY_LAST,
         NICE_COMPATIBILITY_RFC5245,
         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_STUN_SERVER,
      g_param_spec_string (
        "stun-server",
        "STUN server IP address",
        "The IP address (or hostname) of the STUN server to use",
        NULL,
        G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_STUN_SERVER_PORT,
      g_param_spec_uint (
        "stun-server-port",
        "STUN server port",
        "Port of the STUN server used to gather server-reflexive candidates",
        1, 65536,
	1, /* not a construct property, ignored */
        G_PARAM_READWRITE));

  /**
   * NiceAgent:controlling-mode:
   *
   * Whether the agent has the controlling role. This property should
   * be modified before gathering candidates, any modification occuring
   * later will be hold until ICE is restarted.
   */
  g_object_class_install_property (gobject_class, PROP_CONTROLLING_MODE,
      g_param_spec_boolean (
        "controlling-mode",
        "ICE controlling mode",
        "Whether the agent is in controlling mode",
	FALSE, /* not a construct property, ignored */
        G_PARAM_READWRITE));

   g_object_class_install_property (gobject_class, PROP_FULL_MODE,
      g_param_spec_boolean (
        "full-mode",
        "ICE full mode",
        "Whether agent runs in ICE full mode",
	TRUE, /* use full mode by default */
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_STUN_PACING_TIMER,
      g_param_spec_uint (
        "stun-pacing-timer",
        "STUN pacing timer",
        "Timer 'Ta' (msecs) used in the IETF ICE specification for pacing "
        "candidate gathering and sending of connectivity checks",
        1, 0xffffffff,
	NICE_AGENT_TIMER_TA_DEFAULT,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  /* note: according to spec recommendation in sect 5.7.3 (ID-19) */
  g_object_class_install_property (gobject_class, PROP_MAX_CONNECTIVITY_CHECKS,
      g_param_spec_uint (
        "max-connectivity-checks",
        "Maximum number of connectivity checks",
        "Upper limit for the total number of connectivity checks performed",
        0, 0xffffffff,
	0, /* default set in init */
        G_PARAM_READWRITE));

  /**
   * NiceAgent:nomination-mode:
   *
   * The nomination mode used in the ICE specification for describing
   * the selection of valid pairs to be used upstream.
   * <para> See also: #NiceNominationMode </para>
   *
   * Since: 0.1.15
   */
  g_object_class_install_property (gobject_class, PROP_NOMINATION_MODE,
      g_param_spec_enum (
         "nomination-mode",
         "ICE nomination mode",
         "Nomination mode used in the ICE specification for describing "
         "the selection of valid pairs to be used upstream",
         NICE_TYPE_NOMINATION_MODE, NICE_NOMINATION_MODE_AGGRESSIVE,
         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * NiceAgent:support-renomination:
   *
   * Support RENOMINATION STUN attribute proposed here:
   * https://tools.ietf.org/html/draft-thatcher-ice-renomination-00 As
   * soon as RENOMINATION attribute is received from remote
   * candidate's address, corresponding candidates pair gets
   * selected. This is specific to Google Chrome/libWebRTC.
   */
  g_object_class_install_property (gobject_class, PROP_SUPPORT_RENOMINATION,
      g_param_spec_boolean (
         "support-renomination",
         "Support RENOMINATION STUN attribute",
         "As soon as RENOMINATION attribute is received from remote candidate's address, "
         "corresponding candidates pair gets selected.",
         FALSE,
         G_PARAM_READWRITE));

  /**
   * NiceAgent:idle-timeout
   *
   * A final timeout in msec, launched when the agent becomes idle,
   * before stopping its activity.
   *
   * This timer will delay the decision to set a component as failed.
   * This delay is added to reduce the chance to see the agent receiving
   * new stun activity just after the conncheck list has been declared
   * failed (some valid pairs, no nominated pair, and no in-progress
   * pairs), reactiviting conncheck activity, and causing a (valid)
   * state transitions like that: connecting -> failed -> connecting ->
   * connected -> ready.  Such transitions are not buggy per-se, but may
   * break the test-suite, that counts precisely the number of time each
   * state has been set, and doesnt expect these transcient failed
   * states.
   *
   * This timer is also useful when the agent is in controlled mode and
   * the other controlling peer takes some time to elect its nominated
   * pair (this may be the case for SfB peers).
   *
   * This timer is *NOT* part if the RFC5245, as this situation is not
   * covered in sect 8.1.2 "Updating States", but deals with a real
   * use-case, where a controlled agent can not wait forever for the
   * other peer to make a nomination decision.
   *
   * Also note that the value of this timeout will not delay the
   * emission of 'connected' and 'ready' agent signals, and will not
   * slow down the behaviour of the agent when the peer agent works
   * in a timely manner.
   *
   * Since: 0.1.17
   */

  g_object_class_install_property (gobject_class, PROP_IDLE_TIMEOUT,
      g_param_spec_uint (
         "idle-timeout",
         "Timeout before stopping the agent when being idle",
         "A final timeout in msecs, launched when the agent becomes idle, "
         "with no in-progress pairs to wait for, before stopping its activity, "
         "and declaring a component as failed in needed.",
         50, 60000,
	 DEFAULT_IDLE_TIMEOUT,
         G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  /**
   * NiceAgent:proxy-ip:
   *
   * The proxy server IP used to bypass a proxy firewall
   *
   * Since: 0.0.4
   */
  g_object_class_install_property (gobject_class, PROP_PROXY_IP,
      g_param_spec_string (
        "proxy-ip",
        "Proxy server IP",
        "The proxy server IP used to bypass a proxy firewall",
        NULL,
        G_PARAM_READWRITE));

  /**
   * NiceAgent:proxy-port:
   *
   * The proxy server port used to bypass a proxy firewall
   *
   * Since: 0.0.4
   */
  g_object_class_install_property (gobject_class, PROP_PROXY_PORT,
      g_param_spec_uint (
        "proxy-port",
        "Proxy server port",
        "The Proxy server port used to bypass a proxy firewall",
        1, 65536,
	1,
        G_PARAM_READWRITE));

  /**
   * NiceAgent:proxy-type:
   *
   * The type of proxy set in the proxy-ip property
   *
   * Since: 0.0.4
   */
  g_object_class_install_property (gobject_class, PROP_PROXY_TYPE,
      g_param_spec_uint (
         "proxy-type",
         "Type of proxy to use",
         "The type of proxy set in the proxy-ip property",
         NICE_PROXY_TYPE_NONE, NICE_PROXY_TYPE_LAST,
         NICE_PROXY_TYPE_NONE,
         G_PARAM_READWRITE));

  /**
   * NiceAgent:proxy-username:
   *
   * The username used to authenticate with the proxy
   *
   * Since: 0.0.4
   */
  g_object_class_install_property (gobject_class, PROP_PROXY_USERNAME,
      g_param_spec_string (
        "proxy-username",
        "Proxy server username",
        "The username used to authenticate with the proxy",
        NULL,
        G_PARAM_READWRITE));

  /**
   * NiceAgent:proxy-password:
   *
   * The password used to authenticate with the proxy
   *
   * Since: 0.0.4
   */
  g_object_class_install_property (gobject_class, PROP_PROXY_PASSWORD,
      g_param_spec_string (
        "proxy-password",
        "Proxy server password",
        "The password used to authenticate with the proxy",
        NULL,
        G_PARAM_READWRITE));

  /**
   * NiceAgent:proxy-extra-headers: (type GLib.HashTable(utf8,utf8))
   *
   * Optional extra headers to append to the HTTP proxy CONNECT request.
   * Provided as key/value-pairs in hash table corresponding to
   * header-name/header-value.
   *
   * Since: 0.1.20
   */
  g_object_class_install_property (gobject_class, PROP_PROXY_EXTRA_HEADERS,
      g_param_spec_boxed (
        "proxy-extra-headers",
        "Extra headers for HTTP proxy connect",
        "Extra headers to append to the HTTP proxy CONNECT request",
        G_TYPE_HASH_TABLE,
        G_PARAM_READWRITE));

  /**
   * NiceAgent:upnp:
   *
   * Whether the agent should use UPnP to open a port in the router and
   * get the external IP
   *
   * Since: 0.0.7
   */
   g_object_class_install_property (gobject_class, PROP_UPNP,
      g_param_spec_boolean (
        "upnp",
#ifdef HAVE_GUPNP
        "Use UPnP",
        "Whether the agent should use UPnP to open a port in the router and "
        "get the external IP",
#else
        "Use UPnP (disabled in build)",
        "Does nothing because libnice was not built with UPnP support",
#endif
	TRUE, /* enable UPnP by default */
        G_PARAM_READWRITE| G_PARAM_CONSTRUCT));

  /**
   * NiceAgent:upnp-timeout:
   *
   * The maximum amount of time (in milliseconds) to wait for UPnP discovery to
   * finish before signaling the #NiceAgent::candidate-gathering-done signal
   *
   * Since: 0.0.7
   */
  g_object_class_install_property (gobject_class, PROP_UPNP_TIMEOUT,
      g_param_spec_uint (
        "upnp-timeout",
#ifdef HAVE_GUPNP
        "Timeout for UPnP discovery",
        "The maximum amount of time to wait for UPnP discovery to finish before "
        "signaling the candidate-gathering-done signal",
#else
        "Timeout for UPnP discovery (disabled in build)",
        "Does nothing because libnice was not built with UPnP support",
#endif
        100, 60000,
	DEFAULT_UPNP_TIMEOUT,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  /**
   * NiceAgent:reliable:
   *
   * Whether the agent is providing a reliable transport of messages (through
   * ICE-TCP or PseudoTCP over ICE-UDP)
   *
   * Since: 0.0.11
   */
   g_object_class_install_property (gobject_class, PROP_RELIABLE,
      g_param_spec_boolean (
        "reliable",
        "reliable mode",
        "Whether the agent provides a reliable transport of messages",
	FALSE,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * NiceAgent:ice-udp:
   *
   * Whether the agent should use ICE-UDP when gathering candidates.
   * If the option is disabled, no UDP candidates will be generated. If the
   * agent is in reliable mode, then pseudotcp will not be used since pseudotcp
   * works on top of UDP candidates.
   * <para>
   * This option should be set before gathering candidates and should not be
   * modified afterwards.
   * </para>
   * The #NiceAgent:ice-udp property can be set at the same time as the
   * #NiceAgent:ice-tcp property, but both cannot be unset at the same time.
   * If #NiceAgent:ice-tcp is set to %FALSE, then this property cannot be set
   * to %FALSE as well.
   *
   * Since: 0.1.8
   */
   g_object_class_install_property (gobject_class, PROP_ICE_UDP,
      g_param_spec_boolean (
        "ice-udp",
        "Use ICE-UDP",
        "Use ICE-UDP specification to generate UDP candidates",
        TRUE, /* use ice-udp by default */
        G_PARAM_READWRITE));

  /**
   * NiceAgent:ice-tcp:
   *
   * Whether the agent should use ICE-TCP when gathering candidates.
   * If the option is disabled, no TCP candidates will be generated. If the
   * agent is in reliable mode, then pseudotcp will need to be used over UDP
   * candidates.
   * <para>
   * This option should be set before gathering candidates and should not be
   * modified afterwards.
   * </para>
   * The #NiceAgent:ice-tcp property can be set at the same time as the
   * #NiceAgent:ice-udp property, but both cannot be unset at the same time.
   * If #NiceAgent:ice-udp is set to %FALSE, then this property cannot be set
   * to %FALSE as well.
   * <note>
   <para>
   ICE-TCP is only supported for %NICE_COMPATIBILITY_RFC5245,
   %NICE_COMPATIBILITY_OC2007 and %NICE_COMPATIBILITY_OC2007R2 compatibility
   modes.
   </para>
   * </note>
   *
   * Since: 0.1.8
   */
   g_object_class_install_property (gobject_class, PROP_ICE_TCP,
      g_param_spec_boolean (
        "ice-tcp",
        "Use ICE-TCP",
        "Use ICE-TCP specification to generate TCP candidates",
        TRUE, /* use ice-tcp by default */
        G_PARAM_READWRITE));

  /**
   * NiceAgent:bytestream-tcp:
   *
   * This property defines whether receive/send operations over a TCP socket, in
   * reliable mode, are considered as packetized or as bytestream.
   * In unreliable mode, every send/recv is considered as packetized, and
   * this property is ignored and cannot be set.
   * <para>
   * In reliable mode, this property will always return %TRUE in the
   * %NICE_COMPATIBILITY_GOOGLE compatibility mode.
   * </para>
   * If the property is %TRUE, the stream is considered in bytestream mode
   * and data can be read with any receive size. If the property is %FALSE, then
   * the stream is considered packetized and each receive will return one packet
   * of the same size as what was sent from the peer. If in packetized mode,
   * then doing a receive with a size smaller than the packet, will cause the
   * remaining bytes in the packet to be dropped, breaking the reliability
   * of the stream.
   *
   * Since: 0.1.8
   */
   g_object_class_install_property (gobject_class, PROP_BYTESTREAM_TCP,
      g_param_spec_boolean (
        "bytestream-tcp",
        "Bytestream TCP",
        "Use bytestream mode for reliable TCP connections",
        FALSE,
        G_PARAM_READWRITE));

  /**
   * NiceAgent:keepalive-conncheck:
   *
   * Use binding requests as keepalives instead of binding
   * indications. This means that the keepalives may time out which
   * will change the component state to %NICE_COMPONENT_STATE_FAILED.
   *
   * Enabing this is a slight violation of RFC 5245 section 10 which
   * recommends using Binding Indications for keepalives.
   *
   * This is always enabled if the compatibility mode is
   * %NICE_COMPATIBILITY_GOOGLE.
   *
   * This is always enabled if the 'consent-freshness' property is %TRUE
   *
   * Since: 0.1.8
   */
   g_object_class_install_property (gobject_class, PROP_KEEPALIVE_CONNCHECK,
      g_param_spec_boolean (
        "keepalive-conncheck",
        "Use conncheck as keepalives",
        "Use binding requests which require a reply as keepalives instead of "
        "binding indications which don't.",
	FALSE,
        G_PARAM_READWRITE));

   /**
   * NiceAgent:force-relay
   *
   * Force all traffic to go through a relay for added privacy, this
   * allows hiding the local IP address. When this is enabled, so
   * local candidates are available before relay servers have been set
   * with nice_agent_set_relay_info().
   *
   * Since: 0.1.14
   */
   g_object_class_install_property (gobject_class, PROP_FORCE_RELAY,
      g_param_spec_boolean (
        "force-relay",
        "Force Relay",
        "Force all traffic to go through a relay for added privacy.",
	FALSE,
        G_PARAM_READWRITE));

   /**
   * NiceAgent:stun-max-retransmissions
   *
   * The maximum number of retransmissions of the STUN binding requests
   * used in the gathering stage, to find our local candidates, and used
   * in the connection check stage, to test the validity of each
   * constructed pair. This property is described as 'Rc' in the RFC
   * 5389, with a default value of 7. The timeout of each STUN request
   * is doubled for each retransmission, so the choice of this value has
   * a direct impact on the time needed to move from the CONNECTED state
   * to the READY state, and on the time needed to complete the GATHERING
   * state.
   *
   * Since: 0.1.15
   */

   g_object_class_install_property (gobject_class, PROP_STUN_MAX_RETRANSMISSIONS,
      g_param_spec_uint (
        "stun-max-retransmissions",
        "STUN Max Retransmissions",
        "Maximum number of STUN binding requests retransmissions "
        "described as 'Rc' in the STUN specification.",
        1, 99,
        STUN_TIMER_DEFAULT_MAX_RETRANSMISSIONS,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

   /**
   * NiceAgent:stun-initial-timeout
   *
   * The initial timeout (msecs) of the STUN binding requests
   * used in the gathering stage, to find our local candidates.
   * This property is described as 'RTO' in the RFC 5389 and RFC 5245.
   * This timeout is doubled for each retransmission, until
   * #NiceAgent:stun-max-retransmissions have been done,
   * with an exception for the last restransmission, where the timeout is
   * divided by two instead (RFC 5389 indicates that a customisable
   * multiplier 'Rm' to 'RTO' should be used).
   *
   * Since: 0.1.15
   */

   g_object_class_install_property (gobject_class, PROP_STUN_INITIAL_TIMEOUT,
      g_param_spec_uint (
        "stun-initial-timeout",
        "STUN Initial Timeout",
        "STUN timeout in msecs of the initial binding requests used in the "
        "gathering state, described as 'RTO' in the ICE specification.",
        20, 9999,
        STUN_TIMER_DEFAULT_TIMEOUT,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

   /**
   * NiceAgent:stun-reliable-timeout
   *
   * The initial timeout of the STUN binding requests used
   * for a reliable timer.
   *
   * Since: 0.1.15
   */

   g_object_class_install_property (gobject_class, PROP_STUN_RELIABLE_TIMEOUT,
      g_param_spec_uint (
        "stun-reliable-timeout",
        "STUN Reliable Timeout",
        "STUN timeout in msecs of the initial binding requests used for "
        "a reliable timer.",
        20, 99999,
        STUN_TIMER_DEFAULT_RELIABLE_TIMEOUT,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

   /**
    * NiceAgent:ice-trickle
    *
    * Whether to perform Trickle ICE as per draft-ietf-ice-trickle-ice-21.
    * When %TRUE, the agent will postpone changing a component state to
    * %NICE_COMPONENT_STATE_FAILED until nice_agent_peer_candidate_gathering_done()
    * has been called with the ID of the component's stream.
    *
    * Since: 0.1.16
    */
   g_object_class_install_property (gobject_class, PROP_ICE_TRICKLE,
      g_param_spec_boolean (
        "ice-trickle",
        "Trickle ICE",
        "Whether to perform Trickle ICE as per draft-ietf-ice-trickle-ice-21.",
        FALSE,
        G_PARAM_READWRITE));

   /**
    * NiceAgent:consent-freshness
    *
    * Whether to perform periodic consent freshness checks as specified in
    * RFC 7675.  When %TRUE, the agent will periodically send binding requests
    * to the peer to maintain the consent to send with the peer.  On receipt
    * of any authenticated error response, a component will immediately move
    * to the failed state.
    *
    * Setting this property to %TRUE implies that 'keepalive-conncheck' should
    * be %TRUE as well.
    *
    * Since: 0.1.19
    */
   g_object_class_install_property (gobject_class, PROP_CONSENT_FRESHNESS,
      g_param_spec_boolean (
        "consent-freshness",
        "Consent Freshness",
        "Whether to perform the consent freshness checks as specified in RFC 7675",
        FALSE,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /* install signals */

  /**
   * NiceAgent::component-state-changed
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   * @component_id: The ID of the component
   * @state: The new #NiceComponentState of the component
   *
   * This signal is fired whenever a component’s state changes. There are many
   * valid state transitions.
   *
   * ![State transition diagram](states.png)
   */
  signals[SIGNAL_COMPONENT_STATE_CHANGED] =
      g_signal_new (
          "component-state-changed",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          3,
          G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
          G_TYPE_INVALID);

  /**
   * NiceAgent::candidate-gathering-done:
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   *
   * This signal is fired whenever a stream has finished gathering its
   * candidates after a call to nice_agent_gather_candidates()
   */
  signals[SIGNAL_CANDIDATE_GATHERING_DONE] =
      g_signal_new (
          "candidate-gathering-done",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          1,
          G_TYPE_UINT, G_TYPE_INVALID);

  /**
   * NiceAgent::new-selected-pair
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   * @component_id: The ID of the component
   * @lfoundation: The local foundation of the selected candidate pair
   * @rfoundation: The remote foundation of the selected candidate pair
   *
   * This signal is fired once a candidate pair is selected for data
   * transfer for a stream's component This is emitted along with
   * #NiceAgent::new-selected-pair-full which has the whole candidate,
   * the Foundation of a Candidate is not a unique identifier.
   *
   * See also: #NiceAgent::new-selected-pair-full
   * Deprecated: 0.1.8: Use #NiceAgent::new-selected-pair-full
   */
  signals[SIGNAL_NEW_SELECTED_PAIR] =
      g_signal_new (
          "new-selected-pair",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          4,
          G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING,
          G_TYPE_INVALID);

  /**
   * NiceAgent::new-candidate
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   * @component_id: The ID of the component
   * @foundation: The foundation of the new candidate
   *
   * This signal is fired when the agent discovers a new local candidate.
   * When this signal is emitted, a matching #NiceAgent::new-candidate-full is
   * also emitted with the candidate.
   *
   * See also: #NiceAgent::candidate-gathering-done,
   * #NiceAgent::new-candidate-full
   * Deprecated: 0.1.8: Use #NiceAgent::new-candidate-full
   */
  signals[SIGNAL_NEW_CANDIDATE] =
      g_signal_new (
          "new-candidate",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          3,
          G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING,
          G_TYPE_INVALID);

  /**
   * NiceAgent::new-remote-candidate
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   * @component_id: The ID of the component
   * @foundation: The foundation of the new candidate
   *
   * This signal is fired when the agent discovers a new remote
   * candidate.  This can happen with peer reflexive candidates.  When
   * this signal is emitted, a matching
   * #NiceAgent::new-remote-candidate-full is also emitted with the
   * candidate.
   *
   * See also: #NiceAgent::new-remote-candidate-full
   * Deprecated: 0.1.8: Use #NiceAgent::new-remote-candidate-full
   */
  signals[SIGNAL_NEW_REMOTE_CANDIDATE] =
      g_signal_new (
          "new-remote-candidate",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          3,
          G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING,
          G_TYPE_INVALID);

  /**
   * NiceAgent::initial-binding-request-received
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   *
   * This signal is fired when we received our first binding request from
   * the peer.
   */
  signals[SIGNAL_INITIAL_BINDING_REQUEST_RECEIVED] =
      g_signal_new (
          "initial-binding-request-received",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          1,
          G_TYPE_UINT,
          G_TYPE_INVALID);

  /**
   * NiceAgent::reliable-transport-writable
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   * @component_id: The ID of the component
   *
   * This signal is fired on #NiceAgent when the underlying transport becomes writable.
   * This signal is only emitted when the nice_agent_send() function returns less
   * bytes than requested to send (or -1) and once when the connection
   * is established.
   *
   * Note: Since 0.1.23 this signal also fires for non-reliable transports.
   * See https://gitlab.freedesktop.org/libnice/libnice/-/issues/202.
   *
   * Since: 0.0.11
   */
  signals[SIGNAL_RELIABLE_TRANSPORT_WRITABLE] =
      g_signal_new (
          // TODO: Rename to "transport-writable" now that non-reliable transports are supported.
          "reliable-transport-writable",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          2,
          G_TYPE_UINT, G_TYPE_UINT,
          G_TYPE_INVALID);

  /**
   * NiceAgent::streams-removed
   * @agent: The #NiceAgent object
   * @stream_ids: (array zero-terminated=1) (element-type uint): An array of
   * unsigned integer stream IDs, ending with a 0 ID
   *
   * This signal is fired whenever one or more streams are removed from the
   * @agent.
   *
   * Since: 0.1.5
   */
  signals[SIGNAL_STREAMS_REMOVED] =
      g_signal_new (
          "streams-removed",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          g_cclosure_marshal_VOID__POINTER,
          G_TYPE_NONE,
          1,
          NICE_TYPE_AGENT_STREAM_IDS,
          G_TYPE_INVALID);


  /**
   * NiceAgent::new-selected-pair-full
   * @agent: The #NiceAgent object
   * @stream_id: The ID of the stream
   * @component_id: The ID of the component
   * @lcandidate: The local #NiceCandidate of the selected candidate pair
   * @rcandidate: The remote #NiceCandidate of the selected candidate pair
   *
   * This signal is fired once a candidate pair is selected for data
   * transfer for a stream's component. This is emitted along with
   * #NiceAgent::new-selected-pair.
   *
   * See also: #NiceAgent::new-selected-pair
   * Since: 0.1.8
   */
  signals[SIGNAL_NEW_SELECTED_PAIR_FULL] =
      g_signal_new (
          "new-selected-pair-full",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          4, G_TYPE_UINT, G_TYPE_UINT, NICE_TYPE_CANDIDATE, NICE_TYPE_CANDIDATE,
          G_TYPE_INVALID);

  /**
   * NiceAgent::new-candidate-full
   * @agent: The #NiceAgent object
   * @candidate: The new #NiceCandidate
   *
   * This signal is fired when the agent discovers a new local candidate.
   * When this signal is emitted, a matching #NiceAgent::new-candidate is
   * also emitted with the candidate's foundation.
   *
   * See also: #NiceAgent::candidate-gathering-done,
   * #NiceAgent::new-candidate
   * Since: 0.1.8
   */
  signals[SIGNAL_NEW_CANDIDATE_FULL] =
      g_signal_new (
          "new-candidate-full",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          1,
          NICE_TYPE_CANDIDATE,
          G_TYPE_INVALID);

  /**
   * NiceAgent::new-remote-candidate-full
   * @agent: The #NiceAgent object
   * @candidate: The new #NiceCandidate
   *
   * This signal is fired when the agent discovers a new remote candidate.
   * This can happen with peer reflexive candidates.
   * When this signal is emitted, a matching #NiceAgent::new-remote-candidate is
   * also emitted with the candidate's foundation.
   *
   * See also: #NiceAgent::new-remote-candidate
   * Since: 0.1.8
   */
  signals[SIGNAL_NEW_REMOTE_CANDIDATE_FULL] =
      g_signal_new (
          "new-remote-candidate-full",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          NULL,
          G_TYPE_NONE,
          1,
          NICE_TYPE_CANDIDATE,
          G_TYPE_INVALID);

  /* Init debug options depending on env variables */
  nice_debug_init ();
}

static void priv_generate_tie_breaker (NiceAgent *agent)
{
  nice_rng_generate_bytes (agent->rng, 8, (gchar*)&agent->tie_breaker);
}

static void
priv_update_controlling_mode (NiceAgent *agent, gboolean value)
{
  gboolean update_controlling_mode;
  GSList *i, *j;

  agent->saved_controlling_mode = value;
  /* It is safe to update the agent controlling mode when all
   * components are still in state disconnected. When we leave
   * this state, the role must stay under the control of the
   * conncheck algorithm exclusively, until the conncheck is
   * eventually restarted. See RFC5245, sect 5.2. Determining Role
   */
  if (agent->controlling_mode != agent->saved_controlling_mode) {
    update_controlling_mode = TRUE;
    for (i = agent->streams;
         i && update_controlling_mode; i = i->next) {
      NiceStream *stream = i->data;
      for (j = stream->components;
           j && update_controlling_mode; j = j->next) {
        NiceComponent *component = j->data;
        if (component->state > NICE_COMPONENT_STATE_DISCONNECTED)
          update_controlling_mode = FALSE;
      }
    }
    if (update_controlling_mode) {
      agent->controlling_mode = agent->saved_controlling_mode;
      nice_debug ("Agent %p : Property set, changing role to \"%s\".",
          agent, agent->controlling_mode ? "controlling" : "controlled");
    } else {
      nice_debug ("Agent %p : Property set, role switch requested "
          "but conncheck already started.", agent);
      nice_debug ("Agent %p : Property set, staying with role \"%s\" "
          "until restart.", agent,
          agent->controlling_mode ? "controlling" : "controlled");
    }
  } else
    nice_debug ("Agent %p : Property set, role is already \"%s\".", agent,
        agent->controlling_mode ? "controlling" : "controlled");
}

static void
nice_agent_init (NiceAgent *agent)
{
  agent->next_candidate_id = 1;
  agent->next_stream_id = 1;

  /* set defaults; not construct params, so set here */
  agent->stun_server_port = DEFAULT_STUN_PORT;
  agent->controlling_mode = TRUE;
  agent->saved_controlling_mode = TRUE;
  agent->max_conn_checks = NICE_AGENT_MAX_CONNECTIVITY_CHECKS_DEFAULT;
  agent->nomination_mode = NICE_NOMINATION_MODE_AGGRESSIVE;
  agent->support_renomination = FALSE;
  agent->idle_timeout = DEFAULT_IDLE_TIMEOUT;

  agent->discovery_list = NULL;
  agent->discovery_unsched_items = 0;
  agent->discovery_timer_source = NULL;
  agent->conncheck_timer_source = NULL;
  agent->keepalive_timer_source = NULL;
  agent->refresh_list = NULL;
  agent->media_after_tick = FALSE;
  agent->software_attribute = NULL;

  agent->compatibility = NICE_COMPATIBILITY_RFC5245;
  agent->reliable = FALSE;
  agent->bytestream_tcp = FALSE;
  agent->use_ice_udp = TRUE;
  agent->use_ice_tcp = TRUE;

  agent->stun_resolving_cancellable = g_cancellable_new();

  agent->rng = nice_rng_new ();
  priv_generate_tie_breaker (agent);

  g_queue_init (&agent->pending_signals);

  g_mutex_init (&agent->agent_mutex);
}

static void
nice_agent_constructed (GObject *object)
{
  NiceAgent *agent = NICE_AGENT (object);

  if (agent->reliable && agent->compatibility == NICE_COMPATIBILITY_GOOGLE)
    agent->bytestream_tcp = TRUE;

  G_OBJECT_CLASS (nice_agent_parent_class)->constructed (object);
}


NICEAPI_EXPORT NiceAgent *
nice_agent_new (GMainContext *ctx, NiceCompatibility compat)
{
  return nice_agent_new_full (ctx, compat, NICE_AGENT_OPTION_NONE);
}


NICEAPI_EXPORT NiceAgent *
nice_agent_new_reliable (GMainContext *ctx, NiceCompatibility compat)
{
  return nice_agent_new_full (ctx, compat, NICE_AGENT_OPTION_RELIABLE);
}


NICEAPI_EXPORT NiceAgent *
nice_agent_new_full (GMainContext *ctx,
  NiceCompatibility compat,
  NiceAgentOption flags)
{
  NiceAgent *agent = g_object_new (NICE_TYPE_AGENT,
      "compatibility", compat,
      "main-context", ctx,
      "reliable", (flags & NICE_AGENT_OPTION_RELIABLE) ? TRUE : FALSE,
      "bytestream-tcp", (flags & NICE_AGENT_OPTION_BYTESTREAM_TCP) ? TRUE : FALSE,
      "nomination-mode", (flags & NICE_AGENT_OPTION_REGULAR_NOMINATION) ?
      NICE_NOMINATION_MODE_REGULAR : NICE_NOMINATION_MODE_AGGRESSIVE,
      "full-mode", (flags & NICE_AGENT_OPTION_LITE_MODE) ? FALSE : TRUE,
      "ice-trickle", (flags & NICE_AGENT_OPTION_ICE_TRICKLE) ? TRUE : FALSE,
      "support-renomination", (flags & NICE_AGENT_OPTION_SUPPORT_RENOMINATION) ? TRUE : FALSE,
      "consent-freshness", (flags & NICE_AGENT_OPTION_CONSENT_FRESHNESS) ? TRUE : FALSE,
      NULL);

  return agent;
}


static void
nice_agent_get_property (
  GObject *object,
  guint property_id,
  GValue *value,
  GParamSpec *pspec)
{
  NiceAgent *agent = NICE_AGENT (object);

  agent_lock (agent);

  switch (property_id)
    {
    case PROP_MAIN_CONTEXT:
      g_value_set_pointer (value, agent->main_context);
      break;

    case PROP_COMPATIBILITY:
      g_value_set_uint (value, agent->compatibility);
      break;

    case PROP_STUN_SERVER:
      g_value_set_string (value, agent->stun_server_ip);
      break;

    case PROP_STUN_SERVER_PORT:
      g_value_set_uint (value, agent->stun_server_port);
      break;

    case PROP_CONTROLLING_MODE:
      g_value_set_boolean (value, agent->saved_controlling_mode);
      break;

    case PROP_FULL_MODE:
      g_value_set_boolean (value, agent->full_mode);
      break;

    case PROP_STUN_PACING_TIMER:
      g_value_set_uint (value, agent->timer_ta);
      break;

    case PROP_MAX_CONNECTIVITY_CHECKS:
      g_value_set_uint (value, agent->max_conn_checks);
      /* XXX: should we prune the list of already existing checks? */
      break;

    case PROP_NOMINATION_MODE:
      g_value_set_enum (value, agent->nomination_mode);
      break;

    case PROP_SUPPORT_RENOMINATION:
      g_value_set_boolean (value, agent->support_renomination);
      break;

    case PROP_IDLE_TIMEOUT:
      g_value_set_uint (value, agent->idle_timeout);
      break;

    case PROP_PROXY_IP:
      g_value_set_string (value, agent->proxy_ip);
      break;

    case PROP_PROXY_PORT:
      g_value_set_uint (value, agent->proxy_port);
      break;

    case PROP_PROXY_TYPE:
      g_value_set_uint (value, agent->proxy_type);
      break;

    case PROP_PROXY_USERNAME:
      g_value_set_string (value, agent->proxy_username);
      break;

    case PROP_PROXY_PASSWORD:
      g_value_set_string (value, agent->proxy_password);
      break;

    case PROP_PROXY_EXTRA_HEADERS:
      g_value_set_boxed (value, agent->proxy_extra_headers);
      break;

    case PROP_UPNP:
#ifdef HAVE_GUPNP
      g_value_set_boolean (value, agent->upnp_enabled);
#else
      g_value_set_boolean (value, FALSE);
#endif
      break;

    case PROP_UPNP_TIMEOUT:
#ifdef HAVE_GUPNP
      g_value_set_uint (value, agent->upnp_timeout);
#else
      g_value_set_uint (value, DEFAULT_UPNP_TIMEOUT);
#endif
      break;

    case PROP_RELIABLE:
      g_value_set_boolean (value, agent->reliable);
      break;

    case PROP_ICE_UDP:
      g_value_set_boolean (value, agent->use_ice_udp);
      break;

    case PROP_ICE_TCP:
      g_value_set_boolean (value, agent->use_ice_tcp);
      break;

    case PROP_BYTESTREAM_TCP:
      g_value_set_boolean (value, agent->bytestream_tcp);
      break;

    case PROP_KEEPALIVE_CONNCHECK:
      if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE || agent->consent_freshness)
        g_value_set_boolean (value, TRUE);
      else
        g_value_set_boolean (value, agent->keepalive_conncheck);
      break;

    case PROP_FORCE_RELAY:
      g_value_set_boolean (value, agent->force_relay);
      break;

    case PROP_STUN_MAX_RETRANSMISSIONS:
      g_value_set_uint (value, agent->stun_max_retransmissions);
      break;

    case PROP_STUN_INITIAL_TIMEOUT:
      g_value_set_uint (value, agent->stun_initial_timeout);
      break;

    case PROP_STUN_RELIABLE_TIMEOUT:
      g_value_set_uint (value, agent->stun_reliable_timeout);
      break;

    case PROP_ICE_TRICKLE:
      g_value_set_boolean (value, agent->use_ice_trickle);
      break;

    case PROP_CONSENT_FRESHNESS:
      g_value_set_boolean (value, agent->consent_freshness);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }

  agent_unlock_and_emit(agent);
}

void
nice_agent_init_stun_agent (NiceAgent *agent, StunAgent *stun_agent)
{
  if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE) {
    stun_agent_init (stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC3489,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_IGNORE_CREDENTIALS);
  } else if (agent->compatibility == NICE_COMPATIBILITY_MSN) {
    stun_agent_init (stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC3489,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_FORCE_VALIDATER);
  } else if (agent->compatibility == NICE_COMPATIBILITY_WLM2009) {
    stun_agent_init (stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_MSICE2,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_USE_FINGERPRINT);
  } else if (agent->compatibility == NICE_COMPATIBILITY_OC2007) {
    stun_agent_init (stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC3489,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_FORCE_VALIDATER |
        STUN_AGENT_USAGE_NO_ALIGNED_ATTRIBUTES);
  } else if (agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
    stun_agent_init (stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_MSICE2,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_USE_FINGERPRINT |
        STUN_AGENT_USAGE_NO_ALIGNED_ATTRIBUTES);
  } else {
    StunAgentUsageFlags stun_usage = 0;

    if (agent->consent_freshness)
      stun_usage |= STUN_AGENT_USAGE_CONSENT_FRESHNESS;

    stun_agent_init (stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC5389,
        stun_usage | STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_USE_FINGERPRINT);
  }
  stun_agent_set_software (stun_agent, agent->software_attribute);
}

static void
nice_agent_reset_all_stun_agents (NiceAgent *agent, gboolean only_software)
{
  GSList *stream_item, *component_item;

  for (stream_item = agent->streams; stream_item;
       stream_item = stream_item->next) {
    NiceStream *stream = stream_item->data;

    for (component_item = stream->components; component_item;
         component_item = component_item->next) {
      NiceComponent *component = component_item->data;

      if (only_software)
        stun_agent_set_software (&component->stun_agent,
            agent->software_attribute);
      else
        nice_agent_init_stun_agent(agent, &component->stun_agent);
    }
  }
}

static void
copy_hash_entry (const gchar *key, const gchar *value, GHashTable *hdest)
{
    g_hash_table_insert (hdest, g_strdup (key), g_strdup (value));
}

static void
nice_agent_set_property (
  GObject *object,
  guint property_id,
  const GValue *value,
  GParamSpec *pspec)
{
  NiceAgent *agent = NICE_AGENT (object);

  agent_lock (agent);

  switch (property_id)
    {
    case PROP_MAIN_CONTEXT:
      agent->main_context = g_value_get_pointer (value);
      if (agent->main_context != NULL)
        g_main_context_ref (agent->main_context);
      break;

    case PROP_COMPATIBILITY:
      agent->compatibility = g_value_get_uint (value);
      if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE ||
          agent->compatibility == NICE_COMPATIBILITY_MSN ||
          agent->compatibility == NICE_COMPATIBILITY_WLM2009)
        agent->use_ice_tcp = FALSE;

      nice_agent_reset_all_stun_agents (agent, FALSE);
      break;

    case PROP_STUN_SERVER:
      g_free (agent->stun_server_ip);
      agent->stun_server_ip = g_value_dup_string (value);
      break;

    case PROP_STUN_SERVER_PORT:
      agent->stun_server_port = g_value_get_uint (value);
      break;

    case PROP_CONTROLLING_MODE:
      priv_update_controlling_mode (agent, g_value_get_boolean (value));
      break;

    case PROP_FULL_MODE:
      agent->full_mode = g_value_get_boolean (value);
      break;

    case PROP_STUN_PACING_TIMER:
      agent->timer_ta = g_value_get_uint (value);
      break;

    case PROP_MAX_CONNECTIVITY_CHECKS:
      agent->max_conn_checks = g_value_get_uint (value);
      break;

    case PROP_NOMINATION_MODE:
      agent->nomination_mode = g_value_get_enum (value);
      break;

    case PROP_SUPPORT_RENOMINATION:
      agent->support_renomination = g_value_get_boolean (value);
      break;

    case PROP_IDLE_TIMEOUT:
      agent->idle_timeout = g_value_get_uint (value);
      break;

    case PROP_PROXY_IP:
      g_free (agent->proxy_ip);
      agent->proxy_ip = g_value_dup_string (value);
      break;

    case PROP_PROXY_PORT:
      agent->proxy_port = g_value_get_uint (value);
      break;

    case PROP_PROXY_TYPE:
      agent->proxy_type = g_value_get_uint (value);
      break;

    case PROP_PROXY_USERNAME:
      g_free (agent->proxy_username);
      agent->proxy_username = g_value_dup_string (value);
      break;

    case PROP_PROXY_PASSWORD:
      g_free (agent->proxy_password);
      agent->proxy_password = g_value_dup_string (value);
      break;

    case PROP_PROXY_EXTRA_HEADERS:{
      GHashTable *h = g_value_get_boxed (value);
      if (agent->proxy_extra_headers) {
        g_hash_table_unref (agent->proxy_extra_headers);
      }
      agent->proxy_extra_headers = g_hash_table_new_full (g_str_hash,
          g_str_equal, g_free, g_free);
      g_hash_table_foreach (h, (GHFunc)copy_hash_entry,
          agent->proxy_extra_headers);
      break;
    }
    case PROP_UPNP_TIMEOUT:
#ifdef HAVE_GUPNP
      agent->upnp_timeout = g_value_get_uint (value);
#endif
      break;

    case PROP_UPNP:
      agent->upnp_enabled = g_value_get_boolean (value);
      break;

    case PROP_RELIABLE:
      agent->reliable = g_value_get_boolean (value);
      break;

      /* Don't allow ice-udp and ice-tcp to be disabled at the same time */
    case PROP_ICE_UDP:
      if (agent->use_ice_tcp == TRUE || g_value_get_boolean (value) == TRUE)
        agent->use_ice_udp = g_value_get_boolean (value);
      break;

    case PROP_ICE_TCP:
      if ((agent->compatibility == NICE_COMPATIBILITY_RFC5245 ||
              agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
              agent->compatibility == NICE_COMPATIBILITY_OC2007R2) &&
          (agent->use_ice_udp == TRUE || g_value_get_boolean (value) == TRUE))
        agent->use_ice_tcp = g_value_get_boolean (value);
      break;

    case PROP_BYTESTREAM_TCP:
      if (agent->reliable && agent->compatibility != NICE_COMPATIBILITY_GOOGLE)
        agent->bytestream_tcp = g_value_get_boolean (value);
      break;

    case PROP_KEEPALIVE_CONNCHECK:
      agent->keepalive_conncheck = g_value_get_boolean (value);
      break;

    case PROP_FORCE_RELAY:
      agent->force_relay = g_value_get_boolean (value);
      break;

    case PROP_STUN_MAX_RETRANSMISSIONS:
      agent->stun_max_retransmissions = g_value_get_uint (value);
      break;

    case PROP_STUN_INITIAL_TIMEOUT:
      agent->stun_initial_timeout = g_value_get_uint (value);
      break;

    case PROP_STUN_RELIABLE_TIMEOUT:
      agent->stun_reliable_timeout = g_value_get_uint (value);
      break;

    case PROP_ICE_TRICKLE:
      agent->use_ice_trickle = g_value_get_boolean (value);
      break;

    case PROP_CONSENT_FRESHNESS:
      agent->consent_freshness = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }

  agent_unlock_and_emit (agent);

}


static void
 agent_signal_socket_writable (NiceAgent *agent, NiceComponent *component)
{
  g_cancellable_cancel (component->tcp_writable_cancellable);

  agent_queue_signal (agent, signals[SIGNAL_RELIABLE_TRANSPORT_WRITABLE],
      component->stream_id, component->id);
}

static void
pseudo_tcp_socket_create (NiceAgent *agent, NiceStream *stream, NiceComponent *component)
{
  PseudoTcpCallbacks tcp_callbacks = {component,
                                      pseudo_tcp_socket_opened,
                                      pseudo_tcp_socket_readable,
                                      pseudo_tcp_socket_writable,
                                      pseudo_tcp_socket_closed,
                                      pseudo_tcp_socket_write_packet};
  component->tcp = pseudo_tcp_socket_new (0, &tcp_callbacks);
  component->tcp_writable_cancellable = g_cancellable_new ();
  nice_debug ("Agent %p: Create Pseudo Tcp Socket for component %d",
      agent, component->id);
}

static void priv_pseudo_tcp_error (NiceAgent *agent, NiceComponent *component)
{
  if (component->tcp_writable_cancellable) {
    g_cancellable_cancel (component->tcp_writable_cancellable);
    g_clear_object (&component->tcp_writable_cancellable);
  }

  if (component->tcp) {
    agent_signal_component_state_change (agent, component->stream_id,
        component->id, NICE_COMPONENT_STATE_FAILED);
    nice_component_detach_all_sockets (component);
    pseudo_tcp_socket_close (component->tcp, TRUE);
  }

  if (component->tcp_clock) {
    g_source_destroy (component->tcp_clock);
    g_source_unref (component->tcp_clock);
    component->tcp_clock = NULL;
  }
}

static void
pseudo_tcp_socket_opened (PseudoTcpSocket *sock, gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceAgent *agent;

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return;

  nice_debug ("Agent %p: s%d:%d pseudo Tcp socket Opened", agent,
      component->stream_id, component->id);

  agent_signal_socket_writable (agent, component);

  g_object_unref (agent);
}

/* Will attempt to queue all @n_messages into the pseudo-TCP transmission
 * buffer. This is always used in reliable mode, so essentially treats @messages
 * as a massive flat array of buffers.
 *
 * Returns the number of messages successfully sent on success (which may be
 * zero if sending the first buffer of the message would have blocked), or
 * a negative number on error. If "allow_partial" is TRUE, then it returns
 * the number of bytes sent
 */
static gint
pseudo_tcp_socket_send_messages (PseudoTcpSocket *self,
    const NiceOutputMessage *messages, guint n_messages, gboolean allow_partial,
    GError **error)
{
  guint i;
  gint bytes_sent = 0;

  for (i = 0; i < n_messages; i++) {
    const NiceOutputMessage *message = &messages[i];
    guint j;

    /* If allow_partial is FALSE and there’s not enough space for the
     * entire message, bail now before queuing anything. This doesn’t
     * gel with the fact this function is only used in reliable mode,
     * and there is no concept of a ‘message’, but is necessary
     * because the calling API has no way of returning to the client
     * and indicating that a message was partially sent. */
    if (!allow_partial &&
        output_message_get_size (message) >
        pseudo_tcp_socket_get_available_send_space (self)) {
      return i;
    }

    for (j = 0;
         (message->n_buffers >= 0 && j < (guint) message->n_buffers) ||
         (message->n_buffers < 0 && message->buffers[j].buffer != NULL);
         j++) {
      const GOutputVector *buffer = &message->buffers[j];
      gssize ret;

      /* Send on the pseudo-TCP socket. */
      ret = pseudo_tcp_socket_send (self, buffer->buffer, buffer->size);

      /* In case of -1, the error is either EWOULDBLOCK or ENOTCONN, which both
       * need the user to wait for the reliable-transport-writable signal */
      if (ret < 0) {
        if (pseudo_tcp_socket_get_error (self) == EWOULDBLOCK)
          goto out;

        if (pseudo_tcp_socket_get_error (self) == ENOTCONN ||
            pseudo_tcp_socket_get_error (self) == EPIPE)
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
              "TCP connection is not yet established.");
        else
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Error writing data to pseudo-TCP socket.");
        return -1;
      } else {
        bytes_sent += ret;
      }
    }
  }

 out:

  return allow_partial ? bytes_sent : (gint) i;
}

/* Will fill up @messages from the first free byte onwards (as determined using
 * @iter). This is always used in reliable mode, so it essentially treats
 * @messages as a massive flat array of buffers.
 *
 * Updates @iter in place. @iter and @messages are left in invalid states if
 * an error is returned.
 *
 * Returns the number of valid messages in @messages on success (which may be
 * zero if no data is pending and the peer has disconnected), or a negative
 * number on error (including if the request would have blocked returning no
 * messages). */
static gint
pseudo_tcp_socket_recv_messages (PseudoTcpSocket *self,
    NiceInputMessage *messages, guint n_messages, NiceInputMessageIter *iter,
    GError **error)
{
  for (; iter->message < n_messages; iter->message++) {
    NiceInputMessage *message = &messages[iter->message];

    if (iter->buffer == 0 && iter->offset == 0) {
      message->length = 0;
    }

    for (;
         (message->n_buffers >= 0 && iter->buffer < (guint) message->n_buffers) ||
         (message->n_buffers < 0 && message->buffers[iter->buffer].buffer != NULL);
         iter->buffer++) {
      GInputVector *buffer = &message->buffers[iter->buffer];

      do {
        gssize len;

        len = pseudo_tcp_socket_recv (self,
            (gchar *) buffer->buffer + iter->offset,
            buffer->size - iter->offset);

        nice_debug_verbose ("%s: Received %" G_GSSIZE_FORMAT " bytes into "
            "buffer %p (offset %" G_GSIZE_FORMAT ", length %" G_GSIZE_FORMAT
            ").", G_STRFUNC, len, buffer->buffer, iter->offset, buffer->size);

        if (len == 0) {
          /* Reached EOS. */
          goto done;
        } else if (len < 0 &&
            pseudo_tcp_socket_get_error (self) == EWOULDBLOCK) {
          /* EWOULDBLOCK. If we’ve already received something, return that;
           * otherwise, error. */
          if (nice_input_message_iter_get_n_valid_messages (iter) > 0) {
            goto done;
          }
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
              "Error reading data from pseudo-TCP socket: would block.");
          return len;
        } else if (len < 0 && pseudo_tcp_socket_get_error (self) == ENOTCONN) {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
              "Error reading data from pseudo-TCP socket: not connected.");
          return len;
        } else if (len < 0) {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
              "Error reading data from pseudo-TCP socket.");
          return len;
        } else {
          /* Got some data! */
          message->length += len;
          iter->offset += len;
        }
      } while (iter->offset < buffer->size);

      iter->offset = 0;
    }

    iter->buffer = 0;
  }

done:
  return nice_input_message_iter_get_n_valid_messages (iter);
}

/* This is called with the agent lock held. */
static void
pseudo_tcp_socket_readable (PseudoTcpSocket *sock, gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceAgent *agent;
  gboolean has_io_callback;
  NiceStream *stream = NULL;
  guint stream_id = component->stream_id;
  guint component_id = component->id;

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return;

  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component)) {
    goto out;
  }

  nice_debug_verbose ("Agent %p: s%d:%d pseudo Tcp socket readable", agent,
      stream_id, component->id);

  component->tcp_readable = TRUE;

  has_io_callback = nice_component_has_io_callback (component);

  /* Only dequeue pseudo-TCP data if we can reliably inform the client. The
   * agent lock is held here, so has_io_callback can only change during
   * nice_component_emit_io_callback(), after which it’s re-queried. This ensures
   * no data loss of packets already received and dequeued. */
  if (has_io_callback) {
    do {
      gssize len;

      /* FIXME: Why copy into a temporary buffer here? Why can’t the I/O
       * callbacks be emitted directly from the pseudo-TCP receive buffer? */
      len = pseudo_tcp_socket_recv (sock, (gchar *) component->recv_buffer,
          component->recv_buffer_size);

      nice_debug ("%s: I/O callback case: Received %" G_GSSIZE_FORMAT " bytes",
          G_STRFUNC, len);

      if (len == 0) {
        /* Reached EOS. */
        component->tcp_readable = FALSE;
        pseudo_tcp_socket_close (component->tcp, FALSE);
        break;
      } else if (len < 0) {
        /* Handle errors. */
        if (pseudo_tcp_socket_get_error (sock) != EWOULDBLOCK) {
          nice_debug ("%s: calling priv_pseudo_tcp_error()", G_STRFUNC);
          priv_pseudo_tcp_error (agent, component);
        }

        if (component->recv_buf_error != NULL) {
          GIOErrorEnum error_code;

          if (pseudo_tcp_socket_get_error (sock) == ENOTCONN)
            error_code = G_IO_ERROR_BROKEN_PIPE;
          else if (pseudo_tcp_socket_get_error (sock) == EWOULDBLOCK)
            error_code = G_IO_ERROR_WOULD_BLOCK;
          else
            error_code = G_IO_ERROR_FAILED;

          g_set_error (component->recv_buf_error, G_IO_ERROR, error_code,
              "Error reading data from pseudo-TCP socket.");
        }

        break;
      }

      nice_component_emit_io_callback (agent, component, len);

      if (!agent_find_component (agent, stream_id, component_id,
              &stream, &component)) {
        nice_debug ("Stream or Component disappeared during the callback");
        goto out;
      }
      if (pseudo_tcp_socket_is_closed (component->tcp)) {
        nice_debug ("PseudoTCP socket got destroyed in readable callback!");
        goto out;
      }

      has_io_callback = nice_component_has_io_callback (component);
    } while (has_io_callback);
  } else if (component->recv_messages != NULL) {
    gint n_valid_messages;
    GError *child_error = NULL;

    /* Fill up every buffer in every message until the connection closes or an
     * error occurs. Copy the data directly into the client’s receive message
     * array without making any callbacks. Update component->recv_messages_iter
     * as we go. */
    n_valid_messages = pseudo_tcp_socket_recv_messages (sock,
        component->recv_messages, component->n_recv_messages,
        &component->recv_messages_iter, &child_error);

    nice_debug_verbose ("%s: Client buffers case: Received %d valid messages:",
        G_STRFUNC, n_valid_messages);
    nice_debug_input_message_composition (component->recv_messages,
        component->n_recv_messages);

    if (n_valid_messages < 0) {
      g_propagate_error (component->recv_buf_error, child_error);
    } else {
      g_clear_error (&child_error);
    }

    if (n_valid_messages < 0 &&
        g_error_matches (child_error, G_IO_ERROR,
            G_IO_ERROR_WOULD_BLOCK)) {
      component->tcp_readable = FALSE;
    } else if (n_valid_messages < 0) {
      nice_debug ("%s: calling priv_pseudo_tcp_error()", G_STRFUNC);
      priv_pseudo_tcp_error (agent, component);
    } else if (n_valid_messages == 0) {
      /* Reached EOS. */
      component->tcp_readable = FALSE;
      pseudo_tcp_socket_close (component->tcp, FALSE);
    }
  } else {
    nice_debug ("%s: no data read", G_STRFUNC);
  }

  g_assert (stream);
  g_assert (component);
  adjust_tcp_clock (agent, stream, component);

out:

  g_object_unref (agent);
}

static void
pseudo_tcp_socket_writable (PseudoTcpSocket *sock, gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceAgent *agent;

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return;

  nice_debug_verbose ("Agent %p: s%d:%d pseudo Tcp socket writable", agent,
      component->stream_id, component->id);

  agent_signal_socket_writable (agent, component);

  g_object_unref (agent);
}

static void
pseudo_tcp_socket_closed (PseudoTcpSocket *sock, guint32 err,
    gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceAgent *agent;

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return;

  nice_debug ("Agent %p: s%d:%d pseudo Tcp socket closed. "
      "Calling priv_pseudo_tcp_error().",  agent, component->stream_id,
      component->id);
  priv_pseudo_tcp_error (agent, component);

  g_object_unref (agent);
}


static PseudoTcpWriteResult
pseudo_tcp_socket_write_packet (PseudoTcpSocket *psocket,
    const gchar *buffer, guint32 len, gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceAgent *agent;

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return WR_FAIL;

  if (component->selected_pair.local != NULL) {
    NiceSocket *sock;
    NiceAddress *addr;

    sock = component->selected_pair.local->sockptr;
    addr = &component->selected_pair.remote->c.addr;

    if (nice_debug_is_enabled ()) {
      gchar tmpbuf[INET6_ADDRSTRLEN];
      nice_address_to_string (addr, tmpbuf);

      nice_debug_verbose (
          "Agent %p : s%d:%d: sending %d bytes on socket %p (FD %d) to [%s]:%d",
          agent, component->stream_id, component->id, len,
          sock->fileno, g_socket_get_fd (sock->fileno), tmpbuf,
          nice_address_get_port (addr));
    }

    /* Send the segment. nice_socket_send() returns 0 on EWOULDBLOCK; in that
     * case the segment is not sent on the wire, but we return WR_SUCCESS
     * anyway. This effectively drops the segment. The pseudo-TCP state machine
     * will eventually pick up this loss and go into recovery mode, reducing
     * its transmission rate and, hopefully, the usage of system resources
     * which caused the EWOULDBLOCK in the first place. */
    if (nice_socket_send (sock, addr, len, buffer) >= 0) {
      g_object_unref (agent);
      return WR_SUCCESS;
    }
  } else {
    nice_debug ("%s: WARNING: Failed to send pseudo-TCP packet from agent %p "
        "as no pair has been selected yet.", G_STRFUNC, agent);
  }

  g_object_unref (agent);

  return WR_FAIL;
}


static gboolean
notify_pseudo_tcp_socket_clock_agent_locked (NiceAgent *agent,
    gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceStream *stream;

  stream = agent_find_stream (agent, component->stream_id);
  if (!stream)
    return G_SOURCE_REMOVE;

  pseudo_tcp_socket_notify_clock (component->tcp);
  adjust_tcp_clock (agent, stream, component);

  return G_SOURCE_CONTINUE;
}

static void
adjust_tcp_clock (NiceAgent *agent, NiceStream *stream, NiceComponent *component)
{
  if (!pseudo_tcp_socket_is_closed (component->tcp)) {
    guint64 timeout = component->last_clock_timeout;

    if (pseudo_tcp_socket_get_next_clock (component->tcp, &timeout)) {
      if (timeout != component->last_clock_timeout) {
        component->last_clock_timeout = timeout;
        if (component->tcp_clock) {
          g_source_set_ready_time (component->tcp_clock, timeout * 1000);
        }
        if (!component->tcp_clock) {
          long interval = timeout - (guint32) (g_get_monotonic_time () / 1000);

          /* Prevent integer overflows */
          if (interval < 0 || interval > G_MAXINT)
            interval = G_MAXINT;
          agent_timeout_add_with_context (agent, &component->tcp_clock,
              "Pseudo-TCP clock", interval,
              notify_pseudo_tcp_socket_clock_agent_locked, component);
        }
      }
    } else {
      nice_debug ("Agent %p: component %d pseudo-TCP socket should be "
          "destroyed. Calling priv_pseudo_tcp_error().",
          agent, component->id);
      priv_pseudo_tcp_error (agent, component);
    }
  }
}

void
_tcp_sock_is_writable (NiceSocket *sock, gpointer user_data)
{
  NiceComponent *component = user_data;
  NiceAgent *agent;

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return;

  agent_lock (agent);

  /* Don't signal writable if the socket that has become writable is not
   * the selected pair */
  if (component->selected_pair.local == NULL ||
      !nice_socket_is_based_on (component->selected_pair.local->sockptr, sock)) {
    agent_unlock (agent);
    g_object_unref (agent);
    return;
  }

  nice_debug ("Agent %p: s%d:%d Tcp socket writable", agent,
      component->stream_id, component->id);
  agent_signal_socket_writable (agent, component);

  agent_unlock_and_emit (agent);

  g_object_unref (agent);
}

static const gchar *
_transport_to_string (NiceCandidateTransport type) {
  switch(type) {
    case NICE_CANDIDATE_TRANSPORT_UDP:
      return "UDP";
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      return "TCP-ACT";
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      return "TCP-PASS";
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
      return "TCP-SO";
    default:
      return "???";
  }
}

void agent_gathering_done (NiceAgent *agent)
{
  gboolean upnp_running = FALSE;
  gboolean dns_resolution_ongoing = FALSE;
  GSList *i, *j, *k, *l, *m;

  if (agent->stun_resolving_list) {
    nice_debug ("Agent %p: Gathering not done, resolving names", agent);
  }

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;

    /* We ignore streams not in gathering state, typically already in
     * ready state. Such streams may have couples (local,remote)
     * candidates that have not resulted in the creation a new pair
     * during a previous conncheck session, and we don't want these new
     * pairs to be added now, because it would generate unneeded
     * transition changes for a stream unconcerned by this gathering.
     */
    if (!stream->gathering)
      continue;

#ifdef HAVE_GUPNP
    if (stream->upnp_timer_source != NULL)
      upnp_running = TRUE;
#endif

    for (j = stream->components; j; j = j->next) {
      NiceComponent *component = j->data;

      if (nice_component_resolving_turn (component)) {
        dns_resolution_ongoing = TRUE;
        continue;
      }

      for (k = component->local_candidates; k;) {
        NiceCandidate *local_candidate = k->data;
        GSList *next = k->next;

        if (agent->force_relay &&
            local_candidate->type != NICE_CANDIDATE_TYPE_RELAYED)
          goto next_cand;

	if (nice_debug_is_enabled ()) {
	  gchar tmpbuf[INET6_ADDRSTRLEN];
	  nice_address_to_string (&local_candidate->addr, tmpbuf);
          nice_debug ("Agent %p: gathered %s local candidate : [%s]:%u"
              " for s%d/c%d. U/P '%s'/'%s'", agent,
              _transport_to_string (local_candidate->transport),
              tmpbuf, nice_address_get_port (&local_candidate->addr),
              local_candidate->stream_id, local_candidate->component_id,
              local_candidate->username, local_candidate->password);
	}

        /* In addition to not contribute to the creation of a pair in the
         * conncheck list, according to RFC 5245, sect.  5.7.3 "Pruning the
         * Pairs", it can be guessed from SfB behavior, that server
         * reflexive pairs are expected to be also removed from the
         * candidates list, when pairs are formed, so they have no way to
         * become part of a selected pair with such type.
         *
         * It can be observed that, each time a valid pair is discovered and
         * nominated with a local candidate of type srv-rflx, is makes SfB
         * fails with a 500 Internal Error.
         *
         * On the contrary, when a local srv-rflx candidate is gathered,
         * normally announced in the sdp, but removed from the candidate
         * list, in that case, when the *same* candidate is discovered again
         * later during the conncheck, with peer-rflx type this time, then
         * it just works.
         */

        if (agent->compatibility == NICE_COMPATIBILITY_OC2007R2 &&
            local_candidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE) {
          nice_debug ("Agent %p: removing this previous srv-rflx candidate "
              "for OC2007R2 compatibility", agent);
          component->local_candidates =
              g_slist_remove (component->local_candidates, local_candidate);
          agent_remove_local_candidate (agent, stream, local_candidate);
          nice_candidate_free (local_candidate);
          goto next_cand;
        }

        for (l = component->remote_candidates; l; l = l->next) {
          NiceCandidate *remote_candidate = l->data;

          for (m = stream->conncheck_list; m; m = m->next) {
            CandidateCheckPair *p = m->data;

            if (p->local == local_candidate && p->remote == remote_candidate)
              break;
          }
          if (m == NULL) {
            conn_check_add_for_candidate_pair (agent, stream->id, component,
                local_candidate, remote_candidate);
          }
        }
next_cand:
        k = next;
      }
    }
  }

  if (agent->discovery_timer_source == NULL && !upnp_running &&
      !dns_resolution_ongoing)
    agent_signal_gathering_done (agent);
}

void agent_signal_gathering_done (NiceAgent *agent)
{
  GSList *i;

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;
    if (stream->gathering) {
      stream->gathering = FALSE;
      agent_queue_signal (agent, signals[SIGNAL_CANDIDATE_GATHERING_DONE],
          stream->id);
    }
  }
}

void
agent_signal_initial_binding_request_received (NiceAgent *agent,
    NiceStream *stream)
{
  if (stream->initial_binding_request_received != TRUE) {
    stream->initial_binding_request_received = TRUE;
    agent_queue_signal (agent, signals[SIGNAL_INITIAL_BINDING_REQUEST_RECEIVED],
        stream->id);
  }
}

/* If the Component now has a selected_pair, and has pending TCP packets which
 * it couldn’t receive before due to not being able to send out ACKs (or
 * SYNACKs, for the initial SYN packet), handle them now.
 *
 * Must be called with the agent lock held. */
static void
process_queued_tcp_packets (NiceAgent *agent, NiceStream *stream,
    NiceComponent *component)
{
  GOutputVector *vec;
  guint stream_id = stream->id;
  guint component_id = component->id;

  g_assert (agent->reliable);

  if (component->selected_pair.local == NULL ||
      pseudo_tcp_socket_is_closed (component->tcp) ||
      nice_socket_is_reliable (component->selected_pair.local->sockptr)) {
    return;
  }

  nice_debug_verbose ("%s: Sending outstanding packets for agent %p.", G_STRFUNC,
      agent);

  while ((vec = g_queue_peek_head (&component->queued_tcp_packets)) != NULL) {
    gboolean retval;

    nice_debug ("%s: Sending %" G_GSIZE_FORMAT " bytes.", G_STRFUNC, vec->size);
    retval =
        pseudo_tcp_socket_notify_packet (component->tcp, vec->buffer,
            vec->size);

    if (!agent_find_component (agent, stream_id, component_id,
            &stream, &component)) {
      nice_debug ("Stream or Component disappeared during "
          "pseudo_tcp_socket_notify_packet()");
      return;
    }
    if (pseudo_tcp_socket_is_closed (component->tcp)) {
      nice_debug ("PseudoTCP socket got destroyed in"
          " pseudo_tcp_socket_notify_packet()!");
      return;
    }

    adjust_tcp_clock (agent, stream, component);

    if (!retval) {
      /* Failed to send; try again later. */
      break;
    }

    g_queue_pop_head (&component->queued_tcp_packets);
    g_free ((gpointer) vec->buffer);
    g_slice_free (GOutputVector, vec);
  }
}

void agent_signal_new_selected_pair (NiceAgent *agent, guint stream_id,
    guint component_id, NiceCandidate *lcandidate, NiceCandidate *rcandidate)
{
  NiceComponent *component;
  NiceStream *stream;
  NiceCandidateImpl *lc = (NiceCandidateImpl *) lcandidate;

  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component))
    return;

  if (((NiceSocket *)lc->sockptr)->type == NICE_SOCKET_TYPE_UDP_TURN) {
    nice_udp_turn_socket_set_peer (lc->sockptr, &rcandidate->addr);
  }

  if(agent->reliable && !nice_socket_is_reliable (lc->sockptr)) {
    if (!component->tcp)
      pseudo_tcp_socket_create (agent, stream, component);
    process_queued_tcp_packets (agent, stream, component);

    pseudo_tcp_socket_connect (component->tcp);
    pseudo_tcp_socket_notify_mtu (component->tcp, MAX_TCP_MTU);
    adjust_tcp_clock (agent, stream, component);
  }

  if (nice_debug_is_enabled ()) {
    gchar ip[100];
    guint port;

    port = nice_address_get_port (&lcandidate->addr);
    nice_address_to_string (&lcandidate->addr, ip);

    nice_debug ("Agent %p: Local selected pair: %d:%d %s %s %s:%d %s",
        agent, stream_id, component_id, lcandidate->foundation,
        lcandidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE ?
        "TCP-ACT" :
        lcandidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE ?
        "TCP-PASS" :
        lcandidate->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "UDP" : "???",
        ip, port, lcandidate->type == NICE_CANDIDATE_TYPE_HOST ? "HOST" :
        lcandidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ?
        "SRV-RFLX" :
        lcandidate->type == NICE_CANDIDATE_TYPE_RELAYED ?
        "RELAYED" :
        lcandidate->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE ?
        "PEER-RFLX" : "???");

    port = nice_address_get_port (&rcandidate->addr);
    nice_address_to_string (&rcandidate->addr, ip);

    nice_debug ("Agent %p: Remote selected pair: %d:%d %s %s %s:%d %s",
        agent, stream_id, component_id, rcandidate->foundation,
        rcandidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE ?
        "TCP-ACT" :
        rcandidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE ?
        "TCP-PASS" :
        rcandidate->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "UDP" : "???",
        ip, port, rcandidate->type == NICE_CANDIDATE_TYPE_HOST ? "HOST" :
        rcandidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ?
        "SRV-RFLX" :
        rcandidate->type == NICE_CANDIDATE_TYPE_RELAYED ?
        "RELAYED" :
        rcandidate->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE ?
        "PEER-RFLX" : "???");
  }

  agent_queue_signal (agent, signals[SIGNAL_NEW_SELECTED_PAIR_FULL],
      stream_id, component_id, lcandidate, rcandidate);
  agent_queue_signal (agent, signals[SIGNAL_NEW_SELECTED_PAIR],
      stream_id, component_id, lcandidate->foundation, rcandidate->foundation);

  agent_signal_socket_writable (agent, component);
}

void agent_signal_new_candidate (NiceAgent *agent, NiceCandidate *candidate)
{
  agent_queue_signal (agent, signals[SIGNAL_NEW_CANDIDATE_FULL],
      candidate);
  agent_queue_signal (agent, signals[SIGNAL_NEW_CANDIDATE],
      candidate->stream_id, candidate->component_id, candidate->foundation);
}

void agent_signal_new_remote_candidate (NiceAgent *agent, NiceCandidate *candidate)
{
  agent_queue_signal (agent, signals[SIGNAL_NEW_REMOTE_CANDIDATE_FULL],
      candidate);
  agent_queue_signal (agent, signals[SIGNAL_NEW_REMOTE_CANDIDATE],
      candidate->stream_id, candidate->component_id, candidate->foundation);
}

NICEAPI_EXPORT const gchar *
nice_component_state_to_string (NiceComponentState state)
{
  switch (state)
    {
      case NICE_COMPONENT_STATE_DISCONNECTED:
        return "disconnected";
      case NICE_COMPONENT_STATE_GATHERING:
        return "gathering";
      case NICE_COMPONENT_STATE_CONNECTING:
        return "connecting";
      case NICE_COMPONENT_STATE_CONNECTED:
        return "connected";
      case NICE_COMPONENT_STATE_READY:
        return "ready";
      case NICE_COMPONENT_STATE_FAILED:
        return "failed";
      case NICE_COMPONENT_STATE_LAST:
      default:
        return "invalid";
    }
}

void agent_signal_component_state_change (NiceAgent *agent, guint stream_id, guint component_id, NiceComponentState new_state)
{
  NiceComponentState old_state;
  NiceComponent *component;
  NiceStream *stream;

  g_return_if_fail (new_state < NICE_COMPONENT_STATE_LAST);

  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component))
    return;

  /* Validate the state change. */
  old_state = component->state;

  if (new_state == old_state) {
    return;
  }

  nice_debug ("Agent %p : stream %u component %u STATE-CHANGE %s -> %s.", agent,
      stream_id, component_id, nice_component_state_to_string (old_state),
      nice_component_state_to_string (new_state));

  /* Check whether it’s a valid state transition. */
#define TRANSITION(OLD, NEW) \
  (old_state == NICE_COMPONENT_STATE_##OLD && \
   new_state == NICE_COMPONENT_STATE_##NEW)

  g_assert (/* Can (almost) always transition to FAILED (including
             * DISCONNECTED → FAILED which happens if one component fails
             * before another leaves DISCONNECTED): */
            (new_state == NICE_COMPONENT_STATE_FAILED) ||
            /* Standard progression towards a ready connection: */
            TRANSITION (DISCONNECTED, GATHERING) ||
            TRANSITION (GATHERING, CONNECTING) ||
            TRANSITION (CONNECTING, CONNECTED) ||
            TRANSITION (CONNECTED, READY) ||
            /* priv_conn_check_add_for_candidate_pair_matched(): */
            TRANSITION (READY, CONNECTED) ||
            /* If set_remote_candidates() is called with new candidates after
             * reaching FAILED: */
            TRANSITION (FAILED, CONNECTING) ||
            /* if new relay servers are added to a failed connection */
            TRANSITION (FAILED, GATHERING) ||
            /* Possible by calling set_remote_candidates() without calling
             * nice_agent_gather_candidates(): */
            TRANSITION (DISCONNECTED, CONNECTING) ||
            /* If a tcp socket of connected pair is disconnected, in
             * conn_check_prune_socket(): */
            TRANSITION (CONNECTED, CONNECTING) ||
            /* with ICE restart in nice_stream_restart(),
             * it can always go back to gathering */
            (new_state == NICE_COMPONENT_STATE_GATHERING));

#undef TRANSITION

  component->state = new_state;

  if (agent->reliable)
    process_queued_tcp_packets (agent, stream, component);

  agent_queue_signal (agent, signals[SIGNAL_COMPONENT_STATE_CHANGED],
      stream_id, component_id, new_state);
}

guint64
agent_candidate_pair_priority (NiceAgent *agent, NiceCandidate *local, NiceCandidate *remote)
{
  if (agent->controlling_mode)
    return nice_candidate_pair_priority (local->priority, remote->priority);
  else
    return nice_candidate_pair_priority (remote->priority, local->priority);
}



static void
priv_add_new_candidate_discovery_stun (NiceAgent *agent,
    NiceSocket *nicesock, NiceAddress server,
    NiceStream *stream, guint component_id)
{
  CandidateDiscovery *cdisco;

  /* note: no need to check for redundant candidates, as this is
   *       done later on in the process */

  cdisco = g_slice_new0 (CandidateDiscovery);

  cdisco->type = NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE;
  cdisco->nicesock = nicesock;
  cdisco->server = server;
  cdisco->stream_id = stream->id;
  cdisco->component_id = component_id;
  stun_agent_init (&cdisco->stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
		   agent->compatibility == NICE_COMPATIBILITY_RFC5245 ?
		   STUN_COMPATIBILITY_RFC5389 : STUN_COMPATIBILITY_RFC3489,
      (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
       agent->compatibility == NICE_COMPATIBILITY_OC2007R2) ?
        STUN_AGENT_USAGE_NO_ALIGNED_ATTRIBUTES : 0);

  nice_debug ("Agent %p : Adding new srv-rflx candidate discovery %p",
      agent, cdisco);

  agent->discovery_list = g_slist_append (agent->discovery_list, cdisco);
  ++agent->discovery_unsched_items;
}

struct StunResolverData {
  GWeakRef agent_ref;
  guint stream_id;
};

static void
stun_server_resolved_cb (GObject *src, GAsyncResult *result,
    gpointer user_data)
{
  GResolver *resolver = G_RESOLVER (src);
  GList *addresses, *item;
  GError *error = NULL;
  struct StunResolverData *data = user_data;
  guint stream_id;
  NiceAgent *agent;
  NiceStream *stream;

  agent = g_weak_ref_get (&data->agent_ref);
  g_weak_ref_clear (&data->agent_ref);

  stream_id = data->stream_id;
  g_slice_free (struct StunResolverData, data);
  if (agent == NULL)
    return;
  agent->stun_resolving_list = g_slist_remove_all (agent->stun_resolving_list,
      data);

  addresses = g_resolver_lookup_by_name_finish (resolver, result, &error);

  if (addresses == NULL) {
    g_warning ("Agent: %p: s:%d: Can't resolve STUN server: %s", agent,
        stream_id, error->message);
    g_clear_error (&error);
    goto done;
  }

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);

  for (item = addresses; item; item = item->next) {
    GInetAddress *addr = item->data;
    guint cid;
    NiceAddress stun_server;
    const guint8 *addr_bytes = g_inet_address_to_bytes (addr);

    if (nice_debug_is_enabled ()) {
      char *resolved_addr = g_inet_address_to_string (addr);

      nice_debug ("Agent %p: s:%d: Resolved STUN server %s to %s",
          agent, stream_id, agent->stun_server_ip, resolved_addr);
      g_free (resolved_addr);
    }

    switch (g_inet_address_get_family (addr)) {
    case G_SOCKET_FAMILY_IPV4:
      nice_address_set_ipv4 (&stun_server, ntohl (*((guint32 *) addr_bytes)));
      break;
    case G_SOCKET_FAMILY_IPV6:
      nice_address_set_ipv6 (&stun_server, addr_bytes);
      break;
    default:
      /* Ignore others */
      continue;
    }
    nice_address_set_port (&stun_server, agent->stun_server_port);

    for (cid = 1; cid <= stream->n_components; cid++) {
      NiceComponent *component = nice_stream_find_component_by_id (stream,
          cid);
      GSList *citem;

      if (component == NULL)
        continue;

      for (citem = component->local_candidates; citem; citem = citem->next) {
        NiceCandidateImpl *host_candidate = citem->data;

        if (host_candidate->c.type != NICE_CANDIDATE_TYPE_HOST)
          continue;

        if (nice_address_is_linklocal (&host_candidate->c.addr))
          continue;

        /* TODO: Add server-reflexive support for TCP candidates */
        if (host_candidate->c.transport != NICE_CANDIDATE_TRANSPORT_UDP)
          continue;
        if (nice_address_ip_version (&host_candidate->c.addr) !=
            nice_address_ip_version (&stun_server))
          continue;

        priv_add_new_candidate_discovery_stun (agent,
            host_candidate->sockptr,
            stun_server,
            stream,
            cid);
      }
    }
  }

  if (agent->discovery_unsched_items)
    discovery_schedule (agent);
  else
    agent_gathering_done (agent);
  agent_unlock_and_emit (agent);

 done:
  g_list_free_full (addresses, g_object_unref);
  g_object_unref (agent);
}


NiceSocket *
agent_create_tcp_turn_socket (NiceAgent *agent, NiceStream *stream,
    NiceComponent *component, NiceSocket *nicesock,
    NiceAddress *server, NiceRelayType type, gboolean reliable_tcp)
{
  NiceAddress proxy_server;
  NiceAddress local_address = nicesock->addr;

  nice_address_set_port (&local_address, 0);
  nicesock = NULL;

  /* TODO: add support for turn-tcp RFC 6062 */
  if (agent->proxy_type != NICE_PROXY_TYPE_NONE &&
      agent->proxy_ip != NULL &&
      nice_address_set_from_string (&proxy_server, agent->proxy_ip)) {
    nice_address_set_port (&proxy_server, agent->proxy_port);
    nicesock = nice_tcp_bsd_socket_new (agent->main_context, &local_address,
        &proxy_server, reliable_tcp);

    if (nicesock) {
      _priv_set_socket_tos (agent, nicesock, stream->tos);
      if (agent->proxy_type == NICE_PROXY_TYPE_SOCKS5) {
        nicesock = nice_socks5_socket_new (nicesock, server,
            agent->proxy_username, agent->proxy_password);
      } else if (agent->proxy_type == NICE_PROXY_TYPE_HTTP){
        nicesock = nice_http_socket_new (nicesock, server,
            agent->proxy_username, agent->proxy_password,
            agent->proxy_extra_headers);
      } else {
        nice_socket_free (nicesock);
        nicesock = NULL;
      }
    }
  }

  if (nicesock == NULL) {
    nicesock = nice_tcp_bsd_socket_new (agent->main_context, &local_address,
        server, reliable_tcp);

    if (nicesock)
      _priv_set_socket_tos (agent, nicesock, stream->tos);
  }

  /* The TURN server may be invalid or not listening */
  if (nicesock == NULL)
    return NULL;

  nice_socket_set_writable_callback (nicesock, _tcp_sock_is_writable,
      component);

  if (type ==  NICE_RELAY_TYPE_TURN_TLS &&
      agent->compatibility == NICE_COMPATIBILITY_GOOGLE) {
    nicesock = nice_pseudossl_socket_new (nicesock,
        NICE_PSEUDOSSL_SOCKET_COMPATIBILITY_GOOGLE);
  } else if (type == NICE_RELAY_TYPE_TURN_TLS &&
      (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
          agent->compatibility == NICE_COMPATIBILITY_OC2007R2)) {
    nicesock = nice_pseudossl_socket_new (nicesock,
        NICE_PSEUDOSSL_SOCKET_COMPATIBILITY_MSOC);
  }
  return nice_udp_turn_over_tcp_socket_new (nicesock,
      agent_to_turn_socket_compatibility (agent));
}

static void
priv_add_new_candidate_discovery_turn (NiceAgent *agent,
    NiceSocket *nicesock, TurnServer *turn,
    NiceStream *stream, guint component_id, gboolean turn_tcp)
{
  CandidateDiscovery *cdisco;
  NiceComponent *component = nice_stream_find_component_by_id (stream, component_id);

  /* note: no need to check for redundant candidates, as this is
   *       done later on in the process */

  cdisco = g_slice_new0 (CandidateDiscovery);
  cdisco->type = NICE_CANDIDATE_TYPE_RELAYED;

  if (turn->type == NICE_RELAY_TYPE_TURN_UDP) {
    if (agent->use_ice_udp == FALSE || turn_tcp == TRUE) {
      goto skip;
    }
    if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE) {
      NiceAddress addr = nicesock->addr;
      NiceSocket *new_socket;
      nice_address_set_port (&addr, 0);

      new_socket = nice_udp_bsd_socket_new (agent->main_context, &addr, NULL);
      if (new_socket) {
        _priv_set_socket_tos (agent, new_socket, stream->tos);
        nice_component_attach_socket (component, new_socket);
        nicesock = new_socket;
      }
    }
    cdisco->nicesock = nicesock;
  } else {
    gboolean reliable_tcp = FALSE;

    /* MS-TURN will allocate a transport with the same protocol it received
     * the allocate request. So if we are connecting in TCP, then the candidate
     * will be TCP-ACT/TCP-PASS which means it will be reliable all the way
     * to the peer.
     * [MS-TURN] : The transport address has the same transport protocol
     * over which the Allocate request was received; a request that is
     * received over TCP returns a TCP allocated transport address.
     */
    /* TURN-TCP is currently unsupport unless it's OC2007 compatibliity */
    /* TODO: Add support for TURN-TCP */
    if (turn_tcp &&
        (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
         agent->compatibility == NICE_COMPATIBILITY_OC2007R2))
      reliable_tcp = TRUE;

    /* Ignore reliable tcp candidates if we disabled ice-tcp */
    if (agent->use_ice_tcp == FALSE && reliable_tcp == TRUE) {
      goto skip;
    }

    cdisco->nicesock = agent_create_tcp_turn_socket (agent, stream,
        component, nicesock, &turn->server, turn->type, reliable_tcp);
    if (!cdisco->nicesock) {
      goto skip;
    }

    nice_component_attach_socket (component, cdisco->nicesock);
  }

  cdisco->turn = turn_server_ref (turn);
  cdisco->server = turn->server;

  cdisco->stream_id = stream->id;
  cdisco->component_id = component_id;

  if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE) {
    stun_agent_init (&cdisco->stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC3489,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_IGNORE_CREDENTIALS);
  } else if (agent->compatibility == NICE_COMPATIBILITY_MSN ||
      agent->compatibility == NICE_COMPATIBILITY_WLM2009) {
    stun_agent_init (&cdisco->stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC3489,
        STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS);
  } else if (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
      agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
    stun_agent_init (&cdisco->stun_agent, STUN_MSOC_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_OC2007,
        STUN_AGENT_USAGE_LONG_TERM_CREDENTIALS |
        STUN_AGENT_USAGE_NO_ALIGNED_ATTRIBUTES);
  } else {
    stun_agent_init (&cdisco->stun_agent, STUN_ALL_KNOWN_ATTRIBUTES,
        STUN_COMPATIBILITY_RFC5389,
        STUN_AGENT_USAGE_ADD_SOFTWARE |
        STUN_AGENT_USAGE_LONG_TERM_CREDENTIALS);
  }
  stun_agent_set_software (&cdisco->stun_agent, agent->software_attribute);

  nice_debug ("Agent %p : Adding new relay-rflx candidate discovery %p sock=%s",
      agent, cdisco, cdisco->nicesock ? nice_socket_type_to_string (cdisco->nicesock->type) : "");
  agent->discovery_list = g_slist_append (agent->discovery_list, cdisco);
  ++agent->discovery_unsched_items;

  return;

skip:
  g_slice_free (CandidateDiscovery, cdisco);
}

NICEAPI_EXPORT guint
nice_agent_add_stream (
  NiceAgent *agent,
  guint n_components)
{
  NiceStream *stream;
  guint ret = 0;
  guint i;

  g_return_val_if_fail (NICE_IS_AGENT (agent), 0);
  g_return_val_if_fail (n_components >= 1, 0);

  agent_lock (agent);
  stream = nice_stream_new (agent->next_stream_id++, n_components, agent);

  agent->streams = g_slist_append (agent->streams, stream);
  nice_debug ("Agent %p : allocating stream id %u (%p)", agent, stream->id, stream);
  if (agent->reliable) {
    nice_debug ("Agent %p : reliable stream", agent);
    for (i = 0; i < n_components; i++) {
      NiceComponent *component = nice_stream_find_component_by_id (stream, i + 1);
      if (component) {
        pseudo_tcp_socket_create (agent, stream, component);
      } else {
        nice_debug ("Agent %p: couldn't find component %d", agent, i+1);
      }
    }
  }

  nice_stream_initialize_credentials (stream, agent->rng);

  ret = stream->id;

  agent_unlock_and_emit (agent);
  return ret;
}

struct TurnResolverData {
  GWeakRef component_ref;
  TurnServer *turn;
};

static void
turn_server_resolved_cb (GObject *src, GAsyncResult *result,
    gpointer user_data)
{
  GResolver *resolver = G_RESOLVER (src);
  GList *addresses = NULL, *item;
  GError *error = NULL;
  struct TurnResolverData *rd = user_data;
  NiceAgent *agent;
  NiceStream *stream;
  NiceComponent *component;
  TurnServer *turn = rd->turn;
  gboolean first_filled = FALSE;

  component = g_weak_ref_get (&rd->component_ref);
  g_weak_ref_clear (&rd->component_ref);
  g_slice_free (struct TurnResolverData, rd);
  if (component == NULL) {
    turn_server_unref (turn);
    return;
  }

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL) {
    g_object_unref (component);
    turn_server_unref (turn);
    return;
  }

  agent_lock (agent);

  if (g_list_find (component->turn_servers, turn) == NULL) {
    /* No longer relevant turn server */
    goto done;
  }

  stream = agent_find_stream (agent, component->stream_id);

  addresses = g_resolver_lookup_by_name_finish (resolver, result, &error);

  if (addresses == NULL) {
    g_warning ("Agent: %p: s:%d/c:%d: Can't resolve TURN server %s: %s", agent,
        component->stream_id, component->id, turn->server_address,
        error->message);
    g_clear_error (&error);
    turn->resolution_failed = TRUE;
    goto done;
  }

  for (item = addresses; item; item = item->next) {
    GInetAddress *addr = item->data;
    const guint8 *addr_bytes = g_inet_address_to_bytes (addr);
    GSList *citem;

    if (nice_debug_is_enabled ()) {
      char *resolved_addr = g_inet_address_to_string (addr);

      nice_debug ("Agent %p: s:%d/c:%d: Resolved TURN server %s to %s",
          agent, component->stream_id, component->id, turn->server_address,
          resolved_addr);
      g_free (resolved_addr);
    }

    /* If there is already one resolved, duplicate it */
    if (first_filled) {
      TurnServer *copy = turn_server_copy (turn);

      turn_server_unref (turn);
      turn = copy;
      component->turn_servers = g_list_append (component->turn_servers,
          turn_server_ref (turn));
    }

    switch (g_inet_address_get_family (addr)) {
    case G_SOCKET_FAMILY_IPV4:
      nice_address_set_ipv4 (&turn->server, ntohl (*((guint32 *) addr_bytes)));
      break;
    case G_SOCKET_FAMILY_IPV6:
      nice_address_set_ipv6 (&turn->server, addr_bytes);
      break;
    default:
      /* Ignore others */
      continue;
    }
    nice_address_set_port (&turn->server, turn->server_port);

    first_filled = TRUE;

    if (stream->gathering_started) {
      for (citem = component->local_candidates; citem; citem = citem->next) {
        NiceCandidateImpl *host_candidate = citem->data;

        if (host_candidate->c.type != NICE_CANDIDATE_TYPE_HOST)
          continue;

        if (nice_address_is_linklocal (&host_candidate->c.addr))
          continue;

        /* TODO: Add server-reflexive support for TCP candidates */
        if (host_candidate->c.transport ==
            NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE)
          continue;
        if (nice_address_ip_version (&host_candidate->c.addr) !=
            nice_address_ip_version (&turn->server))
          continue;

        priv_add_new_candidate_discovery_turn (agent,
            host_candidate->sockptr, turn, stream, component->id,
            host_candidate->c.transport != NICE_CANDIDATE_TRANSPORT_UDP);
      }
    }
  }

  if (agent->discovery_unsched_items)
    discovery_schedule (agent);
  else
    agent_gathering_done (agent);

 done:
  agent_unlock_and_emit (agent);
  g_list_free_full (addresses, g_object_unref);
  turn_server_unref (turn);
  g_object_unref (component);
  g_object_unref (agent);
}

static gboolean
resolve_turn_in_context (NiceAgent *agent, gpointer data)
{
  struct TurnResolverData *rd = data;
  NiceComponent *component;
  GResolver *resolver;

  component = g_weak_ref_get (&rd->component_ref);
  if (component == NULL) {
    g_weak_ref_clear (&rd->component_ref);
    turn_server_unref (rd->turn);
    g_slice_free (struct TurnResolverData, rd);

    return G_SOURCE_REMOVE;
  }

  resolver = g_resolver_get_default ();

  g_main_context_push_thread_default (agent->main_context);
  g_resolver_lookup_by_name_async (resolver, rd->turn->server_address,
      component->turn_resolving_cancellable, turn_server_resolved_cb,
      rd);
  g_main_context_pop_thread_default (agent->main_context);

  g_object_unref (resolver);

  g_object_unref (component);

  return G_SOURCE_REMOVE;
}

NICEAPI_EXPORT gboolean
nice_agent_set_relay_info(NiceAgent *agent,
    guint stream_id, guint component_id,
    const gchar *server_ip, guint server_port,
    const gchar *username, const gchar *password,
    NiceRelayType type)
{

  NiceComponent *component = NULL;
  NiceStream *stream = NULL;
  gboolean ret = TRUE;
  TurnServer *turn;
  guint length;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);
  g_return_val_if_fail (component_id >= 1, FALSE);
  g_return_val_if_fail (server_ip, FALSE);
  g_return_val_if_fail (server_port, FALSE);
  g_return_val_if_fail (username, FALSE);
  g_return_val_if_fail (password, FALSE);
  g_return_val_if_fail (type <= NICE_RELAY_TYPE_TURN_TLS, FALSE);

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id, &stream,
          &component)) {
    ret = FALSE;
    goto done;
  }

  length = g_list_length (component->turn_servers);
  if (length == NICE_CANDIDATE_MAX_TURN_SERVERS) {
    g_warning ("Agent %p : cannot have more than %d turn servers per component.",
        agent, length);
    ret = FALSE;
    goto done;
  }

  turn = turn_server_new (server_ip, server_port, username, password, type);

  nice_debug ("Agent %p: added relay server [%s]:%d of type %d to s/c %d/%d "
      "with user/pass : %s -- %s", agent, server_ip, server_port, type,
      stream_id, component_id, username,
      nice_debug_is_verbose() ? password : "****");

  /* The turn server preference (used to setup its priority in the
   * conncheck) is simply its position in the list. The preference must
   * be unique for each one.
   */
  turn->preference = length;
  component->turn_servers = g_list_append (component->turn_servers, turn);

  if (!nice_address_is_valid (&turn->server)) {
    GSource *source = NULL;
    struct TurnResolverData *rd = g_slice_new (struct TurnResolverData);

    g_weak_ref_init (&rd->component_ref, component);
    rd->turn = turn_server_ref (turn);

    nice_debug("Agent:%p s:%d/%d: Resolving TURN server %s",
        agent, stream_id, component_id, server_ip);

    agent_timeout_add_with_context (agent, &source, "TURN resolution", 0,
        resolve_turn_in_context, rd);
    g_source_unref (source);
  }

  if (stream->gathering_started) {
    GSList *i;

    stream->gathering = TRUE;

    if (nice_address_is_valid (&turn->server)) {
      for (i = component->local_candidates; i; i = i->next) {
        NiceCandidateImpl *c = i->data;

        if  (c->c.type == NICE_CANDIDATE_TYPE_HOST &&
            c->c.transport != NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE &&
            nice_address_ip_version (&c->c.addr) ==
            nice_address_ip_version (&turn->server)) {
          priv_add_new_candidate_discovery_turn (agent,
              c->sockptr, turn, stream, component_id,
              c->c.transport != NICE_CANDIDATE_TRANSPORT_UDP);
        }
      }

      if (agent->discovery_unsched_items)
        discovery_schedule (agent);
    }
  }


 done:

  agent_unlock_and_emit (agent);
  return ret;
}

#ifdef HAVE_GUPNP

/* Check whether UPnP gathering is done, which is true when the list of pending
 * mappings (upnp_mapping) is empty. When it is empty, we have heard back from
 * gupnp-igd about each of the mappings we added, either successfully or not.
 *
 * Note that upnp_mapping has to be a list, rather than a counter, as the
 * mapped-external-port and error-mapping-port signals could be emitted multiple
 * times for each mapping. */
static void check_upnp_gathering_done (NiceAgent *agent,
                                       NiceStream *stream)
{
  if (stream->upnp_mapping != NULL)
    return;

  if (stream->upnp_timer_source != NULL) {
    g_source_destroy (stream->upnp_timer_source);
    g_source_unref (stream->upnp_timer_source);
    stream->upnp_timer_source = NULL;
  }

  agent_gathering_done (agent);
}

static gboolean priv_upnp_timeout_cb_agent_locked (NiceAgent *agent,
    gpointer user_data)
{
  NiceStream *stream = user_data;

  nice_debug ("Agent %p s:%d : UPnP port mapping timed out", agent,
              stream->id);

  /* Force it to be done */
  stream->upnp_mapped = g_slist_concat (stream->upnp_mapped,
      stream->upnp_mapping);
  stream->upnp_mapping = NULL;

  check_upnp_gathering_done (agent, stream);

  return G_SOURCE_REMOVE;
}


static GSList *
priv_find_upnp_candidate (GSList *upnp_list, NiceCandidate *host_candidate)
{
  GSList *item;

  for (item = upnp_list; item; item = item->next) {
    NiceCandidate *c = item->data;

    if (!nice_candidate_equal_target (host_candidate, c))
      continue;

    if ((host_candidate->transport == NICE_CANDIDATE_TRANSPORT_UDP) !=
        (c->transport == NICE_CANDIDATE_TRANSPORT_UDP))
      continue;

    return item;
  }

  return NULL;
}

static NiceStream *
priv_find_candidate_for_upnp_mapping (NiceAgent *agent, gchar *proto,
    gchar *local_ip, guint local_port, gboolean only_mapping,
    gboolean *was_mapping, GSList **item)
{
  GSList *i;
  NiceCandidate upnp_candidate = { .type = NICE_CANDIDATE_TYPE_HOST };

  if (!nice_address_set_from_string (&upnp_candidate.addr, local_ip))
    return NULL;

  nice_address_set_port (&upnp_candidate.addr, local_port);
  if (!g_strcmp0 (proto, "UDP"))
    upnp_candidate.transport = NICE_CANDIDATE_TRANSPORT_UDP;
  else
    upnp_candidate.transport = NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;
    GSList *j;

    j = priv_find_upnp_candidate (stream->upnp_mapping, &upnp_candidate);
    if (was_mapping)
      *was_mapping = (j != NULL);

    if (j == NULL && !only_mapping)
      j = priv_find_upnp_candidate (stream->upnp_mapped, &upnp_candidate);

    if (j) {
      *item = j;
      return stream;
    }
  }

  return NULL;
}


static void _upnp_mapped_external_port (GUPnPSimpleIgd *self, gchar *proto,
    gchar *external_ip, gchar *replaces_external_ip, guint external_port,
    gchar *local_ip, guint local_port, gchar *description, gpointer user_data)
{
  NiceAgent *agent = (NiceAgent*)user_data;
  NiceStream *stream = NULL;
  GSList *item;
  gboolean was_mapping = FALSE;
  NiceAddress externaddr;

  nice_debug ("Agent %p : Successfully mapped %s:%d to %s:%d", agent, local_ip,
      local_port, external_ip, external_port);

  if (!nice_address_set_from_string (&externaddr, external_ip))
    return;
  nice_address_set_port (&externaddr, external_port);

  agent_lock (agent);

  stream = priv_find_candidate_for_upnp_mapping (agent, proto,
      local_ip, local_port, FALSE, &was_mapping, &item);

  if (stream && stream->upnp_timer_source) {
    NiceCandidateImpl *host_candidate = item->data;

    if (was_mapping) {
      stream->upnp_mapping = g_slist_delete_link (stream->upnp_mapping, item);
      stream->upnp_mapped = g_slist_prepend (stream->upnp_mapped,
          host_candidate);
    }

    discovery_add_server_reflexive_candidate (agent,
        host_candidate->c.stream_id, host_candidate->c.component_id,
        &externaddr,
        host_candidate->c.transport,
        host_candidate->sockptr,
        NULL,
        TRUE);

    check_upnp_gathering_done (agent, stream);
  }

  agent_unlock_and_emit (agent);
}

static void _upnp_error_mapping_port (GUPnPSimpleIgd *self, GError *error,
    gchar *proto, guint external_port, gchar *local_ip, guint local_port,
    gchar *description, gpointer user_data)
{
  NiceAgent *agent = (NiceAgent *) user_data;
  NiceStream *stream;
  GSList *item;

  agent_lock (agent);

  nice_debug ("Agent %p : Error mapping %s:%d to %d (%d) : %s", agent, local_ip,
      local_port, external_port, error->domain, error->message);

  stream = priv_find_candidate_for_upnp_mapping (agent, proto,
      local_ip, local_port, TRUE, NULL, &item);

  if (stream) {
    NiceCandidate *host_candidate = item->data;

    stream->upnp_mapping = g_slist_delete_link (stream->upnp_mapping, item);
    stream->upnp_mapped =
        g_slist_prepend (stream->upnp_mapped, host_candidate);
    check_upnp_gathering_done (agent, stream);
  }

  agent_unlock_and_emit (agent);
}

static void
priv_add_upnp_discovery (NiceAgent *agent, NiceStream *stream,
    NiceCandidate *host_candidate)
{
  gchar local_ip[NICE_ADDRESS_STRING_LEN];

  if (!agent->upnp_enabled || agent->force_relay)
    return;

  if (agent->upnp == NULL) {
    agent->upnp = gupnp_simple_igd_thread_new ();

    if (agent->upnp == NULL) {
      nice_debug ("Agent %p : Could not initialize GUPnP library", agent);
      agent->upnp_enabled = FALSE;
      return;
    }

    g_signal_connect (agent->upnp, "mapped-external-port",
        G_CALLBACK (_upnp_mapped_external_port), agent);
    g_signal_connect (agent->upnp, "error-mapping-port",
        G_CALLBACK (_upnp_error_mapping_port), agent);
  }

  if (host_candidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE)
    return;

  if (priv_find_upnp_candidate (stream->upnp_mapping, host_candidate))
    return;
  if (priv_find_upnp_candidate (stream->upnp_mapped, host_candidate))
    return;

  nice_address_to_string (&host_candidate->addr, local_ip);

  gupnp_simple_igd_add_port (GUPNP_SIMPLE_IGD (agent->upnp),
      host_candidate->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "UDP" : "TCP",
      0, local_ip, nice_address_get_port (&host_candidate->addr),
      0, PACKAGE_STRING);
  stream->upnp_mapping = g_slist_prepend (stream->upnp_mapping,
      nice_candidate_copy (host_candidate));

  if (stream->upnp_timer_source == NULL)
    agent_timeout_add_with_context (agent, &stream->upnp_timer_source,
        "UPnP timeout", agent->upnp_timeout,
        priv_upnp_timeout_cb_agent_locked, stream);
}

static void
priv_remove_upnp_mapping (NiceAgent *agent, NiceCandidate *host_candidate)
{
  gchar local_ip[NICE_ADDRESS_STRING_LEN] = "";

  nice_address_to_string (&host_candidate->addr, local_ip);

  nice_debug ("Removing UPnP mapping %s: %d", local_ip,
      nice_address_get_port (&host_candidate->addr));

  gupnp_simple_igd_remove_port_local (GUPNP_SIMPLE_IGD (agent->upnp),
      host_candidate->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "UDP" :
      "TCP",
      local_ip, nice_address_get_port (&host_candidate->addr));
}

void
agent_remove_local_candidate (NiceAgent *agent, NiceStream *stream,
    NiceCandidate *local_candidate)
{
  GSList *item;

  if (agent->upnp == NULL)
    return;

  if (local_candidate->type != NICE_CANDIDATE_TYPE_HOST)
    return;

  if (local_candidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE)
    return;

  item = priv_find_upnp_candidate (stream->upnp_mapping, local_candidate);
  if (item) {
    nice_candidate_free (item->data);
    stream->upnp_mapping = g_slist_delete_link (stream->upnp_mapping, item);
  }

  item = priv_find_upnp_candidate (stream->upnp_mapped, local_candidate);
  if (item) {
    nice_candidate_free (item->data);
    stream->upnp_mapped = g_slist_delete_link (stream->upnp_mapped, item);
  }

  priv_remove_upnp_mapping (agent, local_candidate);
}

static void
priv_stop_upnp (NiceAgent *agent, NiceStream *stream)
{
  if (agent->upnp == NULL)
    return;

  if (stream->upnp_timer_source != NULL) {
    g_source_destroy (stream->upnp_timer_source);
    g_source_unref (stream->upnp_timer_source);
    stream->upnp_timer_source = NULL;
  }

  while (stream->upnp_mapping) {
    NiceCandidate *host_candidate = stream->upnp_mapping->data;

    priv_remove_upnp_mapping (agent, host_candidate);

    nice_candidate_free (host_candidate);
    stream->upnp_mapping = g_slist_delete_link (stream->upnp_mapping,
        stream->upnp_mapping);
  }

  while (stream->upnp_mapped) {
    NiceCandidate *host_candidate = stream->upnp_mapped->data;

    priv_remove_upnp_mapping (agent, host_candidate);

    nice_candidate_free (host_candidate);
    stream->upnp_mapped = g_slist_delete_link (stream->upnp_mapped,
        stream->upnp_mapped);
  }
}

#else /* HAVE_GUPNP */

static inline void
priv_add_upnp_discovery (NiceAgent *agent, NiceStream *stream,
    NiceCandidate *host_candidate)
{
  /* Use the upnp_enabled to print this only once */
  if (agent->upnp_enabled) {
    nice_debug ("Agent %p : libnice compiled without GUPnP support", agent);
    agent->upnp_enabled = FALSE;
  }
}

static void
priv_stop_upnp (NiceAgent *agent, NiceStream *stream) {
  /* Do nothing */
}

void
agent_remove_local_candidate (NiceAgent *agent, NiceStream *stream,
    NiceCandidate *local_candidate)
{
  /* Do nothing */
}

#endif

static const gchar *
priv_host_candidate_result_to_string (HostCandidateResult result)
{
  switch (result) {
    case HOST_CANDIDATE_SUCCESS:
      return "success";
    case HOST_CANDIDATE_FAILED:
      return "failed";
    case HOST_CANDIDATE_CANT_CREATE_SOCKET:
      return "can't create socket";
    case HOST_CANDIDATE_REDUNDANT:
      return "redundant";
    case HOST_CANDIDATE_DUPLICATE_PORT:
      return "duplicate port";
    default:
      g_assert_not_reached ();
  }
}

static gboolean
resolve_stun_in_context (NiceAgent *agent, gpointer data)
{
  GResolver *resolver = g_resolver_get_default ();
  struct StunResolverData *rd = data;

  nice_debug("Agent:%p s:%d: Resolving STUN server %s",
      agent, rd->stream_id, agent->stun_server_ip);

  g_main_context_push_thread_default (agent->main_context);
  g_resolver_lookup_by_name_async (resolver, agent->stun_server_ip,
      agent->stun_resolving_cancellable, stun_server_resolved_cb, rd);
  g_main_context_pop_thread_default (agent->main_context);

  g_object_unref (resolver);

  return G_SOURCE_REMOVE;
}

NICEAPI_EXPORT gboolean
nice_agent_gather_candidates (
  NiceAgent *agent,
  guint stream_id)
{
  guint cid;
  GSList *i;
  NiceStream *stream;
  GSList *local_addresses = NULL;
  gboolean ret = TRUE;
  guint length;
  gboolean resolving_turn = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream == NULL) {
    agent_unlock_and_emit (agent);
    return FALSE;
  }

  if (stream->gathering_started) {
    /* Stream is already gathering, ignore this call */
    agent_unlock_and_emit (agent);
    return TRUE;
  }

  nice_debug ("Agent %p : In %s mode, starting candidate gathering.", agent,
      agent->full_mode ? "ICE-FULL" : "ICE-LITE");

  /* if no local addresses added, generate them ourselves */
  if (agent->local_addresses == NULL) {
    GList *addresses = nice_interfaces_get_local_ips (FALSE);
    GList *item;

    for (item = addresses; item; item = g_list_next (item)) {
      const gchar *addr_string = item->data;
      NiceAddress *addr = nice_address_new ();

      if (nice_address_set_from_string (addr, addr_string)) {
        local_addresses = g_slist_append (local_addresses, addr);
      } else {
        nice_debug ("Error: Failed to parse local address ‘%s’.", addr_string);
        nice_address_free (addr);
      }
    }

    g_list_free_full (addresses, (GDestroyNotify) g_free);
  } else {
    for (i = agent->local_addresses; i; i = i->next) {
      NiceAddress *addr = i->data;
      NiceAddress *dupaddr = nice_address_dup (addr);

      local_addresses = g_slist_append (local_addresses, dupaddr);
    }
  }

  length = g_slist_length (local_addresses);
  if (length > NICE_CANDIDATE_MAX_LOCAL_ADDRESSES) {
    g_warning ("Agent %p : cannot have more than %d local addresses.",
        agent, NICE_CANDIDATE_MAX_LOCAL_ADDRESSES);
  }

  if (agent->full_mode && agent->stun_server_ip && !agent->force_relay)
  {
    struct StunResolverData *rd = g_slice_new (struct StunResolverData);
    GSource *source = NULL;

    g_weak_ref_init (&rd->agent_ref, agent);
    rd->stream_id = stream_id;

    nice_debug("Agent:%p s:%d: Resolving STUN server %s",
        agent, stream_id, agent->stun_server_ip);
    agent_timeout_add_with_context (agent, &source, "STUN resolution", 0,
        resolve_stun_in_context, rd);
    g_source_unref (source);
    agent->stun_resolving_list = g_slist_prepend (agent->stun_resolving_list,
        rd);
  }

  for (cid = 1; cid <= stream->n_components; cid++) {
    NiceComponent *component = nice_stream_find_component_by_id (stream, cid);
    gboolean found_local_address = FALSE;
    enum {
      ADD_HOST_MIN = 0,
      ADD_HOST_UDP = ADD_HOST_MIN,
      ADD_HOST_TCP_ACTIVE,
      ADD_HOST_TCP_PASSIVE,
      ADD_HOST_MAX = ADD_HOST_TCP_PASSIVE
    } add_type;

    if (component == NULL)
      continue;

    /* generate a local host candidate for each local address */
    length = 0;
    for (i = local_addresses;
        i && length < NICE_CANDIDATE_MAX_LOCAL_ADDRESSES;
        i = i->next, length++) {
      NiceAddress *addr = i->data;
      NiceCandidateImpl *host_candidate;

      for (add_type = ADD_HOST_MIN; add_type <= ADD_HOST_MAX; add_type++) {
        NiceCandidateTransport transport;
        guint current_port;
        guint start_port;
        gboolean accept_duplicate = FALSE;
        HostCandidateResult res = HOST_CANDIDATE_CANT_CREATE_SOCKET;

        if ((agent->use_ice_udp == FALSE && add_type == ADD_HOST_UDP) ||
            (agent->use_ice_tcp == FALSE && add_type != ADD_HOST_UDP))
          continue;

        switch (add_type) {
          default:
          case ADD_HOST_UDP:
            transport = NICE_CANDIDATE_TRANSPORT_UDP;
            break;
          case ADD_HOST_TCP_ACTIVE:
            transport = NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
            break;
          case ADD_HOST_TCP_PASSIVE:
            transport = NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
            break;
        }

        start_port = component->min_port;
        if(component->min_port != 0) {
          start_port = nice_rng_generate_int(agent->rng, component->min_port, component->max_port+1);
        }
        current_port = start_port;

        host_candidate = NULL;
        while (res == HOST_CANDIDATE_CANT_CREATE_SOCKET ||
            res == HOST_CANDIDATE_DUPLICATE_PORT) {
          nice_address_set_port (addr, current_port);
          res = discovery_add_local_host_candidate (agent, stream->id, cid,
              addr, transport, accept_duplicate, &host_candidate);
          if (nice_debug_is_enabled ()) {
            gchar ip[NICE_ADDRESS_STRING_LEN];
            nice_address_to_string (addr, ip);
            nice_debug ("Agent %p: s%d/c%d: creation of host candidate "
                "%s:[%s]:%u: %s%s", agent, stream->id, cid,
                nice_candidate_transport_to_string (transport), ip,
                transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE ?
                    0 : current_port,
                priv_host_candidate_result_to_string (res),
                accept_duplicate ? " (accept duplicate)" : "");
          }
          if (current_port > 0)
            current_port++;
          if (current_port > component->max_port)
            current_port = component->min_port;
          if (current_port == start_port) {
            if (accept_duplicate)
              break;
            accept_duplicate = TRUE;
          }
          if (current_port == 0 && res != HOST_CANDIDATE_DUPLICATE_PORT)
            break;
        }

        if (res == HOST_CANDIDATE_REDUNDANT ||
            res == HOST_CANDIDATE_FAILED ||
            res == HOST_CANDIDATE_CANT_CREATE_SOCKET)
          continue;
        else if (res == HOST_CANDIDATE_DUPLICATE_PORT) {
          ret = FALSE;
          goto error;
        }

        found_local_address = TRUE;
        nice_address_set_port (addr, 0);

        nice_socket_set_writable_callback (host_candidate->sockptr,
            _tcp_sock_is_writable, component);

        priv_add_upnp_discovery (agent, stream, (NiceCandidate *) host_candidate);

        if (agent->full_mode && component && !nice_address_is_linklocal (addr) &&
            transport != NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE) {
          GList *item;
          int host_ip_version = nice_address_ip_version (&host_candidate->c.addr);

          for (item = component->turn_servers; item; item = item->next) {
            TurnServer *turn = item->data;

            if (!nice_address_is_valid (&turn->server)) {
              if (!turn->resolution_failed)
                resolving_turn = TRUE;
              continue;
            }

            if (host_ip_version != nice_address_ip_version (&turn->server)) {
              continue;
            }

            priv_add_new_candidate_discovery_turn (agent,
                host_candidate->sockptr,
                turn,
                stream,
                cid,
                host_candidate->c.transport != NICE_CANDIDATE_TRANSPORT_UDP);
          }
        }
      }
    }
    /* Go to error if we could not find a local address for a given
     * component
     */
    if (!found_local_address) {
      ret = FALSE;
      goto error;
    }

    if (component->state == NICE_COMPONENT_STATE_DISCONNECTED ||
        component->state == NICE_COMPONENT_STATE_FAILED)
      agent_signal_component_state_change (agent,
          stream->id, component->id, NICE_COMPONENT_STATE_GATHERING);
  }

  stream->gathering = TRUE;
  stream->gathering_started = TRUE;

  /* Only signal the new candidates after we're sure that the gathering was
   * succesfful. But before sending gathering-done */
  for (cid = 1; cid <= stream->n_components; cid++) {
    NiceComponent *component = nice_stream_find_component_by_id (stream, cid);
    for (i = component->local_candidates; i; i = i->next) {
      NiceCandidate *candidate = i->data;

      if (agent->force_relay && candidate->type != NICE_CANDIDATE_TYPE_RELAYED)
        continue;

      agent_signal_new_candidate (agent, candidate);
    }
  }

  /* note: no async discoveries pending, signal that we are ready */
  if (agent->discovery_unsched_items == 0 &&
      agent->stun_resolving_list == NULL &&
      resolving_turn == FALSE &&
#ifdef HAVE_GUPNP
      stream->upnp_mapping == NULL) {
#else
      TRUE) {
#endif
    nice_debug ("Agent %p: Candidate gathering FINISHED, no scheduled items.",
        agent);
    agent_gathering_done (agent);
  } else if (agent->discovery_unsched_items) {
    discovery_schedule (agent);
  }

 error:
  for (i = local_addresses; i; i = i->next)
    nice_address_free (i->data);
  g_slist_free (local_addresses);

  if (ret == FALSE) {
    priv_stop_upnp (agent, stream);
    for (cid = 1; cid <= stream->n_components; cid++) {
      NiceComponent *component = nice_stream_find_component_by_id (stream, cid);

      nice_component_free_socket_sources (component);

      g_slist_free_full (component->local_candidates,
          (GDestroyNotify) nice_candidate_free);
      component->local_candidates = NULL;
    }
    discovery_prune_stream (agent, stream_id);
  }

  agent_unlock_and_emit (agent);

  return ret;
}

static void priv_remove_keepalive_timer (NiceAgent *agent)
{
  if (agent->keepalive_timer_source != NULL) {
    g_source_destroy (agent->keepalive_timer_source);
    g_source_unref (agent->keepalive_timer_source);
    agent->keepalive_timer_source = NULL;
  }
}

static gboolean
on_stream_refreshes_pruned (NiceAgent *agent, NiceStream *stream)
{
  // This is called from a timeout cb with agent lock held

  nice_stream_close (agent, stream);

  agent->pruning_streams = g_slist_remove (agent->pruning_streams, stream);

  agent_unlock (agent);

  /* Actually free the stream. This should be done with the lock released, as
   * it could end up disposing of a NiceIOStream, which tries to take the
   * agent lock itself. */
  g_object_unref (stream);

  agent_lock (agent);

  return G_SOURCE_REMOVE;
}

NICEAPI_EXPORT void
nice_agent_remove_stream (
  NiceAgent *agent,
  guint stream_id)
{
  guint stream_ids[] = { stream_id, 0 };

  /* note that streams/candidates can be in use by other threads */

  NiceStream *stream;

  g_return_if_fail (NICE_IS_AGENT (agent));
  g_return_if_fail (stream_id >= 1);

  agent_lock (agent);
  stream = agent_find_stream (agent, stream_id);

  if (!stream) {
    agent_unlock_and_emit (agent);
    return;
  }

  priv_stop_upnp (agent, stream);

  /* note: remove items with matching stream_ids from both lists */
  conn_check_prune_stream (agent, stream);
  discovery_prune_stream (agent, stream_id);

  /* Remove the stream and signal its removal. */
  agent->streams = g_slist_remove (agent->streams, stream);
  agent->pruning_streams = g_slist_prepend (agent->pruning_streams, stream);

  refresh_prune_stream_async (agent, stream,
      (NiceTimeoutLockedCallback) on_stream_refreshes_pruned);

  if (!agent->streams)
    priv_remove_keepalive_timer (agent);

  agent_queue_signal (agent, signals[SIGNAL_STREAMS_REMOVED],
      g_memdup (stream_ids, sizeof(stream_ids)));

  agent_unlock_and_emit (agent);
}

NICEAPI_EXPORT void
nice_agent_set_port_range (NiceAgent *agent, guint stream_id, guint component_id,
    guint min_port, guint max_port)
{
  NiceStream *stream;
  NiceComponent *component;

  g_return_if_fail (NICE_IS_AGENT (agent));
  g_return_if_fail (stream_id >= 1);
  g_return_if_fail (component_id >= 1);

  agent_lock (agent);

  if (agent_find_component (agent, stream_id, component_id, &stream,
          &component)) {
    if (stream->gathering_started) {
      g_critical ("nice_agent_gather_candidates (stream_id=%u) already called for this stream", stream_id);
    } else {
      component->min_port = min_port;
      component->max_port = max_port;
    }
  }

  agent_unlock_and_emit (agent);
}

NICEAPI_EXPORT gboolean
nice_agent_add_local_address (NiceAgent *agent, NiceAddress *addr)
{
  NiceAddress *dupaddr;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (addr != NULL, FALSE);

  agent_lock (agent);

  dupaddr = nice_address_dup (addr);
  nice_address_set_port (dupaddr, 0);
  agent->local_addresses = g_slist_append (agent->local_addresses, dupaddr);

  agent_unlock_and_emit (agent);
  return TRUE;
}

/* Recompute foundations of all candidate pairs from a given stream
 * having a specific remote candidate, and eventually update the
 * priority of the selected pair as well.
 */
static void priv_update_pair_foundations (NiceAgent *agent,
    guint stream_id, guint component_id, NiceCandidate *remote)
{
  NiceStream *stream;
  NiceComponent *component;

  if (agent_find_component (agent, stream_id, component_id, &stream,
      &component)) {
    GSList *i;

    for (i = stream->conncheck_list; i; i = i->next) {
      CandidateCheckPair *pair = i->data;

      if (pair->remote == remote) {
        gchar foundation[NICE_CANDIDATE_PAIR_MAX_FOUNDATION];
        g_snprintf (foundation, NICE_CANDIDATE_PAIR_MAX_FOUNDATION, "%s:%s",
            pair->local->foundation, pair->remote->foundation);
        if (strncmp (pair->foundation, foundation,
            NICE_CANDIDATE_PAIR_MAX_FOUNDATION)) {
          g_strlcpy (pair->foundation, foundation,
              NICE_CANDIDATE_PAIR_MAX_FOUNDATION);
          nice_debug ("Agent %p : Updating pair %p foundation to '%s'",
              agent, pair, pair->foundation);
          if (pair->state == NICE_CHECK_SUCCEEDED)
            conn_check_unfreeze_related (agent, pair);
          if ((NiceCandidate *) component->selected_pair.local == pair->local &&
              (NiceCandidate *) component->selected_pair.remote == pair->remote) {
            gchar priority[NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE];

            /* the foundation update of the selected pair also implies
             * an update of its priority. stun_priority doesn't change
             * because only the remote candidate foundation is modified.
             */
            nice_debug ("Agent %p : pair %p is the selected pair, updating "
                "its priority.", agent, pair);
            component->selected_pair.priority = pair->priority;

            nice_candidate_pair_priority_to_string (pair->priority, priority);
            nice_debug ("Agent %p : updating SELECTED PAIR for component "
                "%u: %s (prio:%s).", agent,
                component->id, foundation, priority);
            agent_signal_new_selected_pair (agent, pair->stream_id,
              component->id, pair->local, pair->remote);
          }
        }
      }
    }
  }
}

/* Returns the nominated pair with the highest priority.
 */
static CandidateCheckPair *priv_get_highest_priority_nominated_pair (
    NiceAgent *agent, guint stream_id, guint component_id)
{
  NiceStream *stream;
  NiceComponent *component;
  CandidateCheckPair *pair;
  GSList *i;

  if (agent_find_component (agent, stream_id, component_id, &stream,
      &component)) {

    for (i = stream->conncheck_list; i; i = i->next) {
      pair = i->data;
      if (pair->component_id == component_id && pair->nominated) {
        return pair;
      }
    }
  }
  return NULL;
}

static gboolean priv_add_remote_candidate (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  NiceCandidateType type,
  const NiceAddress *addr,
  const NiceAddress *base_addr,
  NiceCandidateTransport transport,
  guint32 priority,
  const gchar *username,
  const gchar *password,
  const gchar *foundation)
{
  NiceStream *stream;
  NiceComponent *component;
  NiceCandidate *candidate;
  NiceCandidateImpl *c;
  CandidateCheckPair *pair;

  if (type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE)
    return FALSE;
  if (transport == NICE_CANDIDATE_TRANSPORT_UDP &&
      !agent->use_ice_udp)
    return FALSE;
  if (transport != NICE_CANDIDATE_TRANSPORT_UDP &&
      !agent->use_ice_tcp)
    return FALSE;

  if (priority == 0)
    return FALSE;

  if (!agent_find_component (agent, stream_id, component_id, &stream,
      &component))
    return FALSE;

  /* step: check whether the candidate already exists */
  candidate = nice_component_find_remote_candidate (component, addr, transport);
  c = (NiceCandidateImpl *) candidate;

  /* If it was a discovered remote peer reflexive candidate, then it should
   * be updated according to RFC 5245 section 7.2.1.3 */
  if (candidate && candidate->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE) {
    nice_debug ("Agent %p : Updating existing peer-rfx remote candidate to %s",
        agent, _cand_type_to_sdp (type));
    candidate->type = type;
    /* The updated candidate is no more peer reflexive, so its
     * sockptr can be cleared
     */
    c->sockptr = NULL;
    /* If it got there, the next one will also be ran, so the foundation
     * will be set.
     */
  }

  if (candidate && candidate->type == type) {
    if (nice_debug_is_enabled ()) {
      gchar tmpbuf[INET6_ADDRSTRLEN];
      nice_address_to_string (addr, tmpbuf);
      nice_debug ("Agent %p : Updating existing remote candidate with addr [%s]:%u"
          " for s%d/c%d. U/P '%s'/'%s' prio: %08x", agent, tmpbuf,
          nice_address_get_port (addr), stream_id, component_id,
          username, password, priority);
    }
    /* case 1: an existing candidate, update the attributes */
    if (base_addr)
      candidate->base_addr = *base_addr;
    candidate->priority = priority;
    if (foundation)
      g_strlcpy(candidate->foundation, foundation,
          NICE_CANDIDATE_MAX_FOUNDATION);
    /* note: username and password must remain the same during
     *       a session; see sect 9.1.2 in ICE ID-19 */

    /* note: however, the user/pass in ID-19 is global, if the user/pass
     * are set in the candidate here, it means they need to be updated...
     * this is essential to overcome a race condition where we might receive
     * a valid binding request from a valid candidate that wasn't yet added to
     * our list of candidates.. this 'update' will make the peer-rflx a
     * server-rflx/host candidate again */
    if (username) {
      if (candidate->username == NULL)
        candidate->username = g_strdup (username);
      else if (g_strcmp0 (username, candidate->username))
        nice_debug ("Agent %p : Candidate username '%s' is not allowed "
            "to change to '%s' now (ICE restart only).", agent,
            candidate->username, username);
    }
    if (password) {
      if (candidate->password == NULL)
        candidate->password = g_strdup (password);
      else if (g_strcmp0 (password, candidate->password))
        nice_debug ("Agent %p : candidate password '%s' is not allowed "
            "to change to '%s' now (ICE restart only).", agent,
            candidate->password, password);
    }

    /* since the type of the existing candidate may have changed,
     * the pairs priority and foundation related to this candidate need
     * to be recomputed...
     */
    recalculate_pair_priorities (agent);
    priv_update_pair_foundations (agent, stream_id, component_id, candidate);
    /* ... and maybe we now have another nominated pair with a higher
     * priority as the result of this priorities update.
     */
    pair = priv_get_highest_priority_nominated_pair (agent,
        stream_id, component_id);
    if (pair &&
        (pair->local != (NiceCandidate *) component->selected_pair.local ||
         pair->remote != (NiceCandidate *) component->selected_pair.remote)) {
      /* If we have (at least) one pair with the nominated flag set, it
       * implies that this pair (or another) is set as the selected pair
       * for this component. In other words, this is really an *update*
       * of the selected pair.
       */
      g_assert (component->selected_pair.local != NULL);
      g_assert (component->selected_pair.remote != NULL);
      nice_debug ("Agent %p : Updating selected pair with higher "
          "priority nominated pair %p.", agent, pair);
      conn_check_update_selected_pair (agent, component, pair);
    }
    conn_check_update_check_list_state_for_ready (agent, stream, component);
  }
  else {
    /* case 2: add a new candidate */

    candidate = nice_candidate_new (type);

    candidate->stream_id = stream_id;
    candidate->component_id = component_id;

    candidate->type = type;
    if (addr)
      candidate->addr = *addr;

    if (nice_debug_is_enabled ()) {
      gchar tmpbuf[INET6_ADDRSTRLEN] = {0};
      if (addr)
        nice_address_to_string (addr, tmpbuf);
      nice_debug ("Agent %p : Adding %s remote candidate with addr [%s]:%u"
          " for s%d/c%d. U/P '%s'/'%s' prio: %08x", agent,
          _transport_to_string (transport), tmpbuf,
          addr? nice_address_get_port (addr) : 0, stream_id, component_id,
          username, password, priority);
    }

    if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent)) {
      /* note:  If there are TCP candidates for a media stream,
       * a controlling agent MUST use the regular selection algorithm,
       * RFC 6544, sect 8, "Concluding ICE Processing"
       */
      if (agent->controlling_mode &&
          agent->nomination_mode == NICE_NOMINATION_MODE_AGGRESSIVE &&
          transport != NICE_CANDIDATE_TRANSPORT_UDP) {
        if (conn_check_stun_transactions_count (agent) > 0) {
          /* changing nomination mode from aggressive to regular while
           * conncheck is ongoing may cause unexpected results (inflight
           * aggressive stun requests may nominate a pair unilaterally)
           */
          nice_debug ("Agent %p : we have a TCP candidate, but conncheck "
              "has started already in aggressive mode, ignore it", agent);
          goto errors;
        } else {
          nice_debug ("Agent %p : we have a TCP candidate, switching back "
              "to regular nomination mode", agent);
          agent->nomination_mode = NICE_NOMINATION_MODE_REGULAR;
        }
      }
    }

    if (base_addr)
      candidate->base_addr = *base_addr;

    candidate->transport = transport;
    candidate->priority = priority;
    candidate->username = g_strdup (username);
    candidate->password = g_strdup (password);

    if (foundation)
      g_strlcpy (candidate->foundation, foundation,
          NICE_CANDIDATE_MAX_FOUNDATION);

    /* We only create a pair when a candidate is new, and not when
     * updating an existing one.
     */
    if (conn_check_add_for_candidate (agent, stream_id,
        component, candidate) < 0)
      goto errors;

    component->remote_candidates = g_slist_append (component->remote_candidates,
        candidate);
  }
  return TRUE;

errors:
  nice_candidate_free (candidate);
  return FALSE;
}

NICEAPI_EXPORT gboolean
nice_agent_set_remote_credentials (
  NiceAgent *agent,
  guint stream_id,
  const gchar *ufrag, const gchar *pwd)
{
  NiceStream *stream;
  gboolean ret = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);

  nice_debug ("Agent %p: set_remote_credentials %d", agent, stream_id);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  /* note: oddly enough, ufrag and pwd can be empty strings */
  if (stream && ufrag && pwd) {

    g_strlcpy (stream->remote_ufrag, ufrag, NICE_STREAM_MAX_UFRAG);
    g_strlcpy (stream->remote_password, pwd, NICE_STREAM_MAX_PWD);

    conn_check_remote_credentials_set(agent, stream);

    ret = TRUE;
    goto done;
  }

 done:
  agent_unlock_and_emit (agent);
  return ret;
}

NICEAPI_EXPORT gboolean
nice_agent_set_local_credentials (
  NiceAgent *agent,
  guint stream_id,
  const gchar *ufrag,
  const gchar *pwd)
{
  NiceStream *stream;
  gboolean ret = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);

  /* note: oddly enough, ufrag and pwd can be empty strings */
  if (stream && ufrag && pwd) {
    g_strlcpy (stream->local_ufrag, ufrag, NICE_STREAM_MAX_UFRAG);
    g_strlcpy (stream->local_password, pwd, NICE_STREAM_MAX_PWD);

    ret = TRUE;
    goto done;
  }

 done:
  agent_unlock_and_emit (agent);
  return ret;
}


NICEAPI_EXPORT gboolean
nice_agent_get_local_credentials (
  NiceAgent *agent,
  guint stream_id,
  gchar **ufrag, gchar **pwd)
{
  NiceStream *stream;
  gboolean ret = TRUE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream == NULL) {
    goto done;
  }

  if (!ufrag || !pwd) {
    goto done;
  }

  *ufrag = g_strdup (stream->local_ufrag);
  *pwd = g_strdup (stream->local_password);
  ret = TRUE;

 done:

  agent_unlock_and_emit (agent);
  return ret;
}

static int
_set_remote_candidates_locked (NiceAgent *agent, NiceStream *stream,
    NiceComponent *component, const GSList *candidates)
{
  const GSList *i;
  int added = 0;

  for (i = candidates; i && added >= 0; i = i->next) {
    NiceCandidate *d = (NiceCandidate*) i->data;

    if (nice_address_is_valid (&d->addr) == TRUE) {
      gboolean res =
          priv_add_remote_candidate (agent,
              stream->id,
              component->id,
              d->type,
              &d->addr,
              &d->base_addr,
              d->transport,
              d->priority,
              d->username,
              d->password,
              d->foundation);
      if (res)
        ++added;
    }
  }

  if (added > 0)
    conn_check_remote_candidates_set(agent, stream, component);

  return added;
}


NICEAPI_EXPORT int
nice_agent_set_remote_candidates (NiceAgent *agent, guint stream_id, guint component_id, const GSList *candidates)
{
  int added = 0;
  NiceStream *stream;
  NiceComponent *component;

  g_return_val_if_fail (NICE_IS_AGENT (agent), 0);
  g_return_val_if_fail (stream_id >= 1, 0);
  g_return_val_if_fail (component_id >= 1, 0);

  nice_debug ("Agent %p: set_remote_candidates %d %d", agent, stream_id, component_id);

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component)) {
    g_warning ("Could not find component %u in stream %u", component_id,
        stream_id);
    added = -1;
    goto done;
  }

  added = _set_remote_candidates_locked (agent, stream, component, candidates);

 done:
  agent_unlock_and_emit (agent);

  return added;
}

/* Return values for agent_recv_message_unlocked(). Needed purely because it
 * must differentiate between RECV_OOB and RECV_SUCCESS. */
typedef enum {
  RECV_ERROR = -2,
  RECV_WOULD_BLOCK = -1,
  RECV_OOB = 0,
  RECV_SUCCESS = 1,
} RecvStatus;

/* returns TRUE if nicesock is turn type */
static gboolean
_agent_recv_turn_message_unlocked (
  NiceAgent *agent,
  NiceStream *stream,
  NiceComponent *component,
  NiceSocket **nicesock,
  NiceInputMessage *message,
  RecvStatus * recv_status)
{
  GList *item;
  gboolean is_turn = FALSE;

  if ((*nicesock)->type == NICE_SOCKET_TYPE_UDP_TURN)
    return TRUE;

  if (component->turn_candidate &&
      nice_socket_is_based_on (component->turn_candidate->sockptr, *nicesock) &&
      nice_address_equal (message->from,
          &component->turn_candidate->turn->server)) {
    *recv_status = nice_udp_turn_socket_parse_recv_message (
        component->turn_candidate->sockptr, nicesock, message);
    return TRUE;
  }

  for (item = component->turn_servers; item; item = g_list_next (item)) {
    TurnServer *turn = item->data;
    GSList *i = NULL;

    if (!nice_address_equal (message->from, &turn->server))
      continue;

    is_turn = TRUE;

    for (i = component->local_candidates; i; i = i->next) {
      NiceCandidateImpl *cand = i->data;

      if (cand->c.type == NICE_CANDIDATE_TYPE_RELAYED &&
          cand->turn == turn &&
          cand->c.stream_id == stream->id &&
          nice_socket_is_based_on (cand->sockptr, *nicesock)) {
        nice_debug_verbose ("Agent %p : Packet received from TURN server candidate.",
            agent);
        *recv_status = nice_udp_turn_socket_parse_recv_message (cand->sockptr, nicesock,
            message);
        return TRUE;
      }
    }
  }

  return is_turn;
}

/*
 * agent_recv_message_unlocked:
 * @agent: a #NiceAgent
 * @stream: the stream to receive from
 * @component: the component to receive from
 * @socket: the socket to receive on
 * @message: the message to write into (must have at least 65536 bytes of buffer
 * space)
 *
 * Receive a single message of data from the given @stream, @component and
 * @socket tuple, in a non-blocking fashion. The caller must ensure that
 * @message contains enough buffers to provide at least 65536 bytes of buffer
 * space, but the buffers may be split as the caller sees fit.
 *
 * This must be called with the agent’s lock held.
 *
 * Returns: number of valid messages received on success (i.e. %RECV_SUCCESS or
 * 1), %RECV_OOB if data was successfully received but was handled out-of-band
 * (e.g. due to being a STUN control packet), %RECV_WOULD_BLOCK if no data is
 * available and the call would block, or %RECV_ERROR on error
 */
static RecvStatus
agent_recv_message_unlocked (
  NiceAgent *agent,
  NiceStream *stream,
  NiceComponent *component,
  NiceSocket *nicesock,
  NiceInputMessage *message)
{
  NiceInputMessage *provided_message = message;
  NiceInputMessage rfc4571_message;
  GInputVector rfc4571_buf;
  NiceAddress from;
  RecvStatus retval;
  gint sockret;
  gboolean is_turn;

  /* We need an address for packet parsing, below. */
  if (provided_message->from == NULL) {
    nice_address_init (&from);
    provided_message->from = &from;
  }

  /* ICE-TCP requires that all packets be framed with RFC4571 */
  if (nice_socket_is_reliable (nicesock)) {
    /* In the case of OC2007 and OC2007R2 which uses UDP TURN for TCP-ACTIVE
     * and TCP-PASSIVE candidates, the recv_messages will be packetized and
     * always return an entire frame, so we must read it as is */
    if (nicesock->type == NICE_SOCKET_TYPE_UDP_TURN_OVER_TCP ||
        nicesock->type == NICE_SOCKET_TYPE_UDP_TURN) {
      GSList *cand_i;
      GInputVector *local_bufs;
      NiceInputMessage local_message;
      guint n_bufs = 0;
      guint16 rfc4571_frame;
      guint i;

      /* In case of ICE-TCP on UDP-TURN (OC2007 compat), we need to do the recv
       * on the UDP_TURN socket, but it's possible we receive the source event
       * on the UDP_TURN_OVER_TCP socket, so in that case, we need to replace
       * the socket we do the recv on to the topmost socket
       */
      for (cand_i = component->local_candidates; cand_i; cand_i = cand_i->next) {
        NiceCandidateImpl *cand = cand_i->data;

        if (cand->c.type == NICE_CANDIDATE_TYPE_RELAYED &&
            cand->c.stream_id == stream->id &&
            cand->c.component_id == component->id &&
            nice_socket_is_based_on(cand->sockptr, nicesock)) {
          nice_debug ("Agent %p : Packet received from a TURN socket.",
              agent);
          nicesock = cand->sockptr;
          break;
        }
      }
      /* Count the number of buffers. */
      if (message->n_buffers == -1) {
        for (i = 0; message->buffers[i].buffer != NULL; i++)
          n_bufs++;
      } else {
        n_bufs = message->n_buffers;
      }

      local_bufs = g_alloca ((n_bufs + 1) * sizeof (GInputVector));
      local_message.buffers = local_bufs;
      local_message.n_buffers = n_bufs + 1;
      local_message.from = message->from;
      local_message.length = 0;

      local_bufs[0].buffer = &rfc4571_frame;
      local_bufs[0].size = sizeof (guint16);

      for (i = 0; i < n_bufs; i++) {
        local_bufs[i + 1].buffer = message->buffers[i].buffer;
        local_bufs[i + 1].size = message->buffers[i].size;
      }
      sockret = nice_socket_recv_messages (nicesock, &local_message, 1);
      if (sockret == 1 && local_message.length >= sizeof (guint16)) {
        message->length = ntohs (rfc4571_frame);
      }
    } else {
      if (nicesock->type == NICE_SOCKET_TYPE_TCP_PASSIVE) {
        NiceSocket *new_socket;

        /* Passive candidates when readable should accept and create a new
         * socket. When established, the connchecks will create a peer reflexive
         * candidate for it */
        new_socket = nice_tcp_passive_socket_accept (nicesock);
        if (new_socket) {
          _priv_set_socket_tos (agent, new_socket, stream->tos);
          nice_debug ("Agent %p: add to tcp-pass socket %p a new "
              "tcp accept socket %p in s/c %d/%d",
              agent, nicesock, new_socket, stream->id, component->id);
          nice_component_attach_socket (component, new_socket);
        }
        sockret = 0;
      } else {
        /* In the case of a real ICE-TCP connection, we can use the socket as a
         * bytestream and do the read here with caching of data being read
         */
        guint headroom;
        gboolean missing_cached_data, have_whole_frame;

        sockret = 0;
        message->length = 0;

        headroom = nice_component_compute_rfc4571_headroom (component);
        missing_cached_data = component->rfc4571_frame_size == 0 ||
            headroom < component->rfc4571_frame_size;

        if (missing_cached_data) {
          gssize available = g_socket_get_available_bytes (nicesock->fileno);

          if (available <= 0) {
            sockret = available;

            /* If we don't call check_connect_result on an outbound connection,
             * then is_connected will always return FALSE. That's why we check
             * both conditions to make sure g_socket_is_connected returns the
             * correct result, otherwise we end up closing valid connections
             */
            if (!g_socket_check_connect_result (nicesock->fileno, NULL) ||
                !g_socket_is_connected (nicesock->fileno)) {
              /* If we receive a readable event on a TCP_BSD socket which is
               * not connected, it means that it failed to connect, so we must
               * return an error to make the socket fail/closed
               */
              sockret = -1;
            } else {
              gint flags = G_SOCKET_MSG_PEEK;

              /* If available bytes are 0, but the socket is still considered
               * connected, then either we're just trying to see if there's more
               * data available or the peer closed the connection.
               * The only way to know is to do a read, so we do here a peek and
               * check the return value, if it's 0, it means the peer has closed
               * the connection, so we must return an error instead of
               * WOULD_BLOCK
               */
              if (g_socket_receive_message (nicesock->fileno, NULL,
                      NULL, 0, NULL, NULL, &flags, NULL, NULL) == 0)
                sockret = -1;
            }
          } else {
            GInputVector local_buf = {
              component->rfc4571_buffer,
              component->rfc4571_buffer_size
            };
            NiceInputMessage local_message = {
              &local_buf, 1, &component->rfc4571_remote_addr, 0
            };

            if (headroom > 0) {
              memmove (component->rfc4571_buffer,
                  component->rfc4571_buffer + component->rfc4571_frame_offset,
                  headroom);
              local_buf.buffer = (guint8 *) local_buf.buffer + headroom;
              local_buf.size -= headroom;
            }

            component->rfc4571_buffer_offset = headroom;
            component->rfc4571_frame_offset = 0;

            sockret = nice_socket_recv_messages (nicesock, &local_message, 1);
            if (sockret == 1) {
              component->rfc4571_buffer_offset += local_message.length;
              headroom += local_message.length;
            }
          }

          if (component->rfc4571_frame_size == 0 &&
              headroom >= sizeof (guint16)) {
            component->rfc4571_frame_size = sizeof (guint16) + ntohs (
                *((guint16 *) (component->rfc4571_buffer +
                    component->rfc4571_frame_offset)));
          }
        }

        have_whole_frame = component->rfc4571_frame_size != 0 &&
            headroom >= component->rfc4571_frame_size;
        if (have_whole_frame) {
          rfc4571_buf.buffer = component->rfc4571_buffer +
              component->rfc4571_frame_offset + sizeof (guint16);
          rfc4571_buf.size = component->rfc4571_frame_size - sizeof (guint16);

          rfc4571_message.buffers = &rfc4571_buf;
          rfc4571_message.n_buffers = 1;
          rfc4571_message.from = provided_message->from;
          rfc4571_message.length = rfc4571_buf.size;

          message = &rfc4571_message;
          *message->from = component->rfc4571_remote_addr;

          sockret = 1;
        } else {
          if (sockret == 1)
            sockret = 0;
        }
      }
    }
  } else {
    sockret = nice_socket_recv_messages (nicesock, message, 1);
  }

  if (sockret == 0) {
    retval = RECV_WOULD_BLOCK;  /* EWOULDBLOCK */
    nice_debug_verbose ("%s: Agent %p: no message available on read attempt",
        G_STRFUNC, agent);
    goto done;
  } else if (sockret < 0) {
    nice_debug ("Agent %p: %s returned %d, errno (%d) : %s",
        agent, G_STRFUNC, sockret, errno, g_strerror (errno));

    retval = RECV_ERROR;
    goto done;
  } else {
    retval = sockret;
  }

  g_assert (retval != RECV_OOB);
  if (message->length == 0) {
    retval = RECV_OOB;
    nice_debug_verbose ("%s: Agent %p: message handled out-of-band", G_STRFUNC,
        agent);
    goto done;
  }

  if (nice_debug_is_verbose ()) {
    gchar tmpbuf[INET6_ADDRSTRLEN];
    nice_address_to_string (message->from, tmpbuf);
    nice_debug_verbose ("%s: Agent %p : Packet received on local socket %p "
        "(fd %d) from [%s]:%u (%" G_GSSIZE_FORMAT " octets).", G_STRFUNC, agent,
        nicesock, nicesock->fileno ? g_socket_get_fd (nicesock->fileno) : -1, tmpbuf,
        nice_address_get_port (message->from), message->length);
  }

  is_turn = _agent_recv_turn_message_unlocked (agent, stream, component, &nicesock,
      message, &retval);

  if (agent->force_relay && !is_turn) {
    /* Ignore messages not from TURN if TURN is required */
    retval = RECV_WOULD_BLOCK;  /* EWOULDBLOCK */
    goto done;
  }

  if (retval == RECV_OOB)
    goto done;

  /* If the message’s stated length is equal to its actual length, it’s probably
   * a STUN message; otherwise it’s probably data. */
  if (stun_message_validate_buffer_length_fast (
      (StunInputVector *) message->buffers, message->n_buffers, message->length,
      (agent->compatibility != NICE_COMPATIBILITY_OC2007 &&
       agent->compatibility != NICE_COMPATIBILITY_OC2007R2)) == (ssize_t) message->length) {
    /* Slow path: If this message isn’t obviously *not* a STUN packet, compact
     * its buffers
     * into a single monolithic one and parse the packet properly. */
    guint8 *big_buf;
    gsize big_buf_len;
    int validated_len;

    big_buf = compact_input_message (message, &big_buf_len);

    validated_len = stun_message_validate_buffer_length (big_buf, big_buf_len,
        (agent->compatibility != NICE_COMPATIBILITY_OC2007 &&
         agent->compatibility != NICE_COMPATIBILITY_OC2007R2));

    if (validated_len == (gint) big_buf_len) {
      gboolean handled;

      handled =
        conn_check_handle_inbound_stun (agent, stream, component, nicesock,
            message->from, (gchar *) big_buf, big_buf_len);

      if (handled) {
        /* Handled STUN message. */
        nice_debug ("%s: Valid STUN packet received.", G_STRFUNC);
        retval = RECV_OOB;
        g_free (big_buf);
        goto done;
      }
    }

    nice_debug ("%s: Packet passed fast STUN validation but failed "
        "slow validation.", G_STRFUNC);

    g_free (big_buf);
  }

  if (!nice_component_verify_remote_candidate (component,
      message->from, nicesock)) {
    if (nice_debug_is_verbose ()) {
      gchar str[INET6_ADDRSTRLEN];

      nice_address_to_string (message->from, str);
      nice_debug_verbose ("Agent %p : %d:%d DROPPING packet from unknown source"
          " %s:%d sock-type: %d", agent, stream->id, component->id, str,
          nice_address_get_port (message->from), nicesock->type);
    }

    retval = RECV_OOB;
    goto done;
  }

  agent->media_after_tick = TRUE;

  /* Unhandled STUN; try handling TCP data, then pass to the client. */
  if (message->length > 0  && agent->reliable) {
    if (!nice_socket_is_reliable (nicesock) &&
        !pseudo_tcp_socket_is_closed (component->tcp)) {
      /* If we don’t yet have an underlying selected socket, queue up the
       * incoming data to handle later. This is because we can’t send ACKs (or,
       * more importantly for the first few packets, SYNACKs) without an
       * underlying socket. We’d rather wait a little longer for a pair to be
       * selected, then process the incoming packets and send out ACKs, than try
       * to process them now, fail to send the ACKs, and incur a timeout in our
       * pseudo-TCP state machine. */
      if (component->selected_pair.local == NULL) {
        GOutputVector *vec = g_slice_new (GOutputVector);
        vec->buffer = compact_input_message (message, &vec->size);
        g_queue_push_tail (&component->queued_tcp_packets, vec);
        nice_debug ("%s: Queued %" G_GSSIZE_FORMAT " bytes for agent %p.",
            G_STRFUNC, vec->size, agent);

        return RECV_OOB;
      } else {
        process_queued_tcp_packets (agent, stream, component);
      }

      /* Received data on a reliable connection. */

      nice_debug_verbose ("%s: notifying pseudo-TCP of packet, length %" G_GSIZE_FORMAT,
          G_STRFUNC, message->length);
      pseudo_tcp_socket_notify_message (component->tcp, message);

      adjust_tcp_clock (agent, stream, component);

      /* Success! Handled out-of-band. */
      retval = RECV_OOB;
      goto done;
    } else if (pseudo_tcp_socket_is_closed (component->tcp)) {
      nice_debug ("Received data on a pseudo tcp FAILED component. Ignoring.");

      retval = RECV_OOB;
      goto done;
    }
  }

done:
  if (message == &rfc4571_message) {
    if (retval == RECV_SUCCESS) {
      NiceInputMessageIter iter = { 0, 0, 0 };
      agent_consume_next_rfc4571_chunk (agent, component, provided_message, 1,
          &iter);
    } else {
      agent_consume_next_rfc4571_chunk (agent, component, NULL, 0, NULL);
    }
  }

  /* Clear local modifications. */
  if (provided_message->from == &from) {
    provided_message->from = NULL;
  }

  return retval;
}

static void
agent_consume_next_rfc4571_chunk (NiceAgent *agent, NiceComponent *component,
    NiceInputMessage *messages, guint n_messages, NiceInputMessageIter *iter)
{
  gboolean fully_consumed;

  if (messages != NULL) {
    gsize bytes_unconsumed, bytes_copied;

    bytes_unconsumed = component->rfc4571_frame_size - sizeof (guint16) -
        component->rfc4571_consumed_size;

    bytes_copied = append_buffer_to_input_messages (agent->bytestream_tcp,
        messages, n_messages, iter, component->rfc4571_buffer +
            component->rfc4571_frame_offset + component->rfc4571_frame_size -
            bytes_unconsumed,
        bytes_unconsumed);

    component->rfc4571_consumed_size += bytes_copied;

    fully_consumed = bytes_copied == bytes_unconsumed || !agent->bytestream_tcp;
  } else {
    fully_consumed = TRUE;
  }

  if (fully_consumed) {
    guint headroom;
    gboolean have_whole_next_frame;

    component->rfc4571_frame_offset += component->rfc4571_frame_size;
    component->rfc4571_frame_size = 0;
    component->rfc4571_consumed_size = 0;

    headroom = nice_component_compute_rfc4571_headroom (component);
    if (headroom >= sizeof (guint16)) {
      component->rfc4571_frame_size = sizeof (guint16) + ntohs (
          *((guint16 *) (component->rfc4571_buffer +
              component->rfc4571_frame_offset)));
      have_whole_next_frame = headroom >= component->rfc4571_frame_size;
    } else {
      have_whole_next_frame = FALSE;
    }

    component->rfc4571_wakeup_needed = have_whole_next_frame;
  } else {
    component->rfc4571_wakeup_needed = TRUE;
  }
}

static gboolean
agent_try_consume_next_rfc4571_chunk (NiceAgent *agent,
    NiceComponent *component, NiceInputMessage *messages, guint n_messages,
    NiceInputMessageIter *iter)
{
  guint headroom;

  if (component->rfc4571_frame_size == 0)
    return FALSE;

  headroom = nice_component_compute_rfc4571_headroom (component);
  if (headroom < component->rfc4571_frame_size)
    return FALSE;

  agent_consume_next_rfc4571_chunk (agent, component, messages, n_messages,
      iter);

  return TRUE;
}

/* Print the composition of an array of messages. No-op if debugging is
 * disabled. */
static void
nice_debug_input_message_composition (const NiceInputMessage *messages,
    guint n_messages)
{
  guint i;

  if (!nice_debug_is_verbose ())
    return;

  for (i = 0; i < n_messages; i++) {
    const NiceInputMessage *message = &messages[i];
    guint j;

    nice_debug_verbose ("Message %p (from: %p, length: %" G_GSIZE_FORMAT ")", message,
        message->from, message->length);

    for (j = 0;
         (message->n_buffers >= 0 && j < (guint) message->n_buffers) ||
         (message->n_buffers < 0 && message->buffers[j].buffer != NULL);
         j++) {
      GInputVector *buffer = &message->buffers[j];

      nice_debug_verbose ("\tBuffer %p (length: %" G_GSIZE_FORMAT ")", buffer->buffer,
          buffer->size);
    }
  }
}

static guint8 *
compact_message (const NiceOutputMessage *message, gsize buffer_length)
{
  guint8 *buffer;
  gsize offset = 0;
  guint i;

  buffer = g_malloc (buffer_length);

  for (i = 0;
       (message->n_buffers >= 0 && i < (guint) message->n_buffers) ||
       (message->n_buffers < 0 && message->buffers[i].buffer != NULL);
       i++) {
    gsize len = MIN (buffer_length - offset, message->buffers[i].size);
    memcpy (buffer + offset, message->buffers[i].buffer, len);
    offset += len;
  }

  return buffer;
}

/* Concatenate all the buffers in the given @recv_message into a single, newly
 * allocated, monolithic buffer which is returned. The length of the new buffer
 * is returned in @buffer_length, and should be equal to the length field of
 * @recv_message.
 *
 * The return value must be freed with g_free(). */
guint8 *
compact_input_message (const NiceInputMessage *message, gsize *buffer_length)
{
  nice_debug_verbose ("%s: **WARNING: SLOW PATH**", G_STRFUNC);
  nice_debug_input_message_composition (message, 1);

  /* This works as long as NiceInputMessage is a subset of eNiceOutputMessage */

  *buffer_length = message->length;

  return compact_message ((NiceOutputMessage *) message, *buffer_length);
}

/* Returns the number of bytes copied. Silently drops any data from @buffer
 * which doesn’t fit in @message. */
gsize
memcpy_buffer_to_input_message (NiceInputMessage *message,
    const guint8 *buffer, gsize buffer_length)
{
  guint i;

  nice_debug_verbose ("%s: **WARNING: SLOW PATH**", G_STRFUNC);

  message->length = 0;

  for (i = 0;
       buffer_length > 0 &&
       ((message->n_buffers >= 0 && i < (guint) message->n_buffers) ||
        (message->n_buffers < 0 && message->buffers[i].buffer != NULL));
       i++) {
    gsize len;

    len = MIN (message->buffers[i].size, buffer_length);
    memcpy (message->buffers[i].buffer, buffer, len);

    buffer += len;
    buffer_length -= len;

    message->length += len;
  }

  nice_debug_input_message_composition (message, 1);

  if (buffer_length > 0) {
    g_warning ("Dropped %" G_GSIZE_FORMAT " bytes of data from the end of "
        "buffer %p (length: %" G_GSIZE_FORMAT ") due to not fitting in "
        "message %p", buffer_length, buffer - message->length,
        message->length + buffer_length, message);
  }

  return message->length;
}

static gsize
append_buffer_to_input_messages (gboolean bytestream_tcp,
    NiceInputMessage *messages, guint n_messages, NiceInputMessageIter *iter,
    const guint8 *buffer, gsize buffer_length)
{
  NiceInputMessage *message = &messages[iter->message];
  gsize buffer_offset;

  if (iter->buffer == 0 && iter->offset == 0) {
    message->length = 0;
  }

  for (buffer_offset = 0;
       (message->n_buffers >= 0 && iter->buffer < (guint) message->n_buffers) ||
       (message->n_buffers < 0 && message->buffers[iter->buffer].buffer != NULL);
       iter->buffer++) {
    GInputVector *v = &message->buffers[iter->buffer];
    gsize len;

    len = MIN (buffer_length - buffer_offset, v->size - iter->offset);
    memcpy ((guint8 *) v->buffer + iter->offset, buffer + buffer_offset, len);

    message->length += len;
    iter->offset += len;
    buffer_offset += len;

    if (buffer_offset == buffer_length)
      break;

    iter->offset = 0;
  }

  if (!bytestream_tcp || nice_input_message_iter_get_message_capacity (iter,
        messages, n_messages) == 0) {
    iter->offset = 0;
    iter->buffer = 0;
    iter->message++;
  }

  return buffer_offset;
}

/* Concatenate all the buffers in the given @message into a single, newly
 * allocated, monolithic buffer which is returned. The length of the new buffer
 * is returned in @buffer_length, and should be equal to the length field of
 * @recv_message.
 *
 * The return value must be freed with g_free(). */
guint8 *
compact_output_message (const NiceOutputMessage *message, gsize *buffer_length)
{
  nice_debug ("%s: **WARNING: SLOW PATH**", G_STRFUNC);

  *buffer_length = output_message_get_size (message);

  return compact_message (message, *buffer_length);
}

gsize
output_message_get_size (const NiceOutputMessage *message)
{
  guint i;
  gsize message_len = 0;

  /* Find the total size of the message */
  for (i = 0;
       (message->n_buffers >= 0 && i < (guint) message->n_buffers) ||
           (message->n_buffers < 0 && message->buffers[i].buffer != NULL);
       i++)
    message_len += message->buffers[i].size;

  return message_len;
}

gsize
input_message_get_size (const NiceInputMessage *message)
{
  guint i;
  gsize message_len = 0;

  /* Find the total size of the message */
  for (i = 0;
       (message->n_buffers >= 0 && i < (guint) message->n_buffers) ||
           (message->n_buffers < 0 && message->buffers[i].buffer != NULL);
       i++)
    message_len += message->buffers[i].size;

  return message_len;
}

/*
 * nice_input_message_iter_reset:
 * @iter: a #NiceInputMessageIter
 *
 * Reset the given @iter to point to the beginning of the array of messages.
 * This may be used both to initialise it and to reset it after use.
 *
 * Since: 0.1.5
 */
void
nice_input_message_iter_reset (NiceInputMessageIter *iter)
{
  iter->message = 0;
  iter->buffer = 0;
  iter->offset = 0;
}

/*
 * nice_input_message_iter_is_at_end:
 * @iter: a #NiceInputMessageIter
 * @messages: (array length=n_messages): an array of #NiceInputMessages
 * @n_messages: number of entries in @messages
 *
 * Determine whether @iter points to the end of the given @messages array. If it
 * does, the array is full: every buffer in every message is full of valid
 * bytes.
 *
 * Returns: %TRUE if the messages’ buffers are full, %FALSE otherwise
 *
 * Since: 0.1.5
 */
gboolean
nice_input_message_iter_is_at_end (NiceInputMessageIter *iter,
    NiceInputMessage *messages, guint n_messages)
{
  return (iter->message == n_messages &&
      iter->buffer == 0 && iter->offset == 0);
}

/*
 * nice_input_message_iter_get_n_valid_messages:
 * @iter: a #NiceInputMessageIter
 *
 * Calculate the number of valid messages in the messages array. A valid message
 * is one which contains at least one valid byte of data in its buffers.
 *
 * Returns: number of valid messages (may be zero)
 *
 * Since: 0.1.5
 */
guint
nice_input_message_iter_get_n_valid_messages (NiceInputMessageIter *iter)
{
  if (iter->buffer == 0 && iter->offset == 0)
    return iter->message;
  else
    return iter->message + 1;
}

static gsize
nice_input_message_iter_get_message_capacity (NiceInputMessageIter *iter,
    NiceInputMessage *messages, guint n_messages)
{
  NiceInputMessage *message = &messages[iter->message];
  guint i;
  gsize total;

  if (iter->message == n_messages)
    return 0;

  total = 0;
  for (i = iter->buffer;
       (message->n_buffers >= 0 && i < (guint) message->n_buffers) ||
       (message->n_buffers < 0 && message->buffers[i].buffer != NULL);
       i++) {
    total += message->buffers[i].size;
  }

  return total - iter->offset;
}

/**
 * nice_input_message_iter_compare:
 * @a: a #NiceInputMessageIter
 * @b: another #NiceInputMessageIter
 *
 * Compare two #NiceInputMessageIters for equality, returning %TRUE if they
 * point to the same place in the receive message array.
 *
 * Returns: %TRUE if the iters match, %FALSE otherwise
 *
 * Since: 0.1.5
 */
gboolean
nice_input_message_iter_compare (const NiceInputMessageIter *a,
    const NiceInputMessageIter *b)
{
  return (a->message == b->message && a->buffer == b->buffer && a->offset == b->offset);
}

/* Will fill up @messages from the first free byte onwards (as determined using
 * @iter). This may be used in bytestream or packetized mode; in packetized mode
 * it will always increment the message index after each buffer is consumed.
 *
 * Updates @iter in place. No errors can occur.
 *
 * Returns the number of valid messages in @messages on success (which may be
 * zero if reading into the first buffer of the message would have blocked).
 *
 * Must be called with the io_mutex held. */
static gint
pending_io_messages_recv_messages (NiceComponent *component,
    gboolean bytestream_tcp, NiceInputMessage *messages, guint n_messages,
    NiceInputMessageIter *iter)
{
  IOCallbackData *data;
  gsize bytes_copied;

  g_assert (component->io_callback_id == 0);

  data = g_queue_peek_head (&component->pending_io_messages);
  if (data == NULL)
    goto done;

  bytes_copied = append_buffer_to_input_messages (bytestream_tcp, messages,
      n_messages, iter, data->buf + data->offset, data->buf_len - data->offset);
  data->offset += bytes_copied;

  if (!bytestream_tcp || data->offset == data->buf_len) {
    g_queue_pop_head (&component->pending_io_messages);
    io_callback_data_free (data);
  }

done:
  return nice_input_message_iter_get_n_valid_messages (iter);
}

static gboolean
nice_agent_recv_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
  GError **error = user_data;

  if (error && !*error)
    g_cancellable_set_error_if_cancelled (cancellable, error);
  return G_SOURCE_REMOVE;
}

static gint
nice_agent_recv_messages_blocking_or_nonblocking (NiceAgent *agent,
  guint stream_id, guint component_id, gboolean blocking,
  NiceInputMessage *messages, guint n_messages,
  GCancellable *cancellable, GError **error)
{
  GMainContext *context;
  NiceStream *stream;
  NiceComponent *component;
  gint n_valid_messages = -1;
  GSource *cancellable_source = NULL;
  gboolean received_enough = FALSE, error_reported = FALSE;
  gboolean all_sockets_would_block = FALSE;
  gboolean reached_eos = FALSE;
  GError *child_error = NULL;
  NiceInputMessage *messages_orig = NULL;
  guint i;

  g_return_val_if_fail (NICE_IS_AGENT (agent), -1);
  g_return_val_if_fail (stream_id >= 1, -1);
  g_return_val_if_fail (component_id >= 1, -1);
  g_return_val_if_fail (n_messages == 0 || messages != NULL, -1);
  g_return_val_if_fail (
      cancellable == NULL || G_IS_CANCELLABLE (cancellable), -1);
  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  if (n_messages == 0)
    return 0;

  if (n_messages > G_MAXINT) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "The number of messages can't exceed G_MAXINT: %d", G_MAXINT);
    return -1;
  }

  /* Receive buffer size must be at least 1280 for STUN */
  if (!agent->reliable) {
    for (i = 0; i < n_messages; i++) {
      if (input_message_get_size (&messages[i]) < 1280) {
        GInputVector *vec;

        if (messages_orig == NULL)
          messages_orig = g_memdup (messages,
              sizeof (NiceInputMessage) * n_messages);
        vec = g_slice_new (GInputVector);
        vec->buffer = g_slice_alloc (1280);
        vec->size = 1280;
        messages[i].buffers = vec;
        messages[i].n_buffers = 1;
      }
    }
  }

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component)) {
    g_set_error (&child_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                 "Invalid stream/component.");
    goto done;
  }

  nice_debug_verbose ("%s: %p: (%s):", G_STRFUNC, agent,
      blocking ? "blocking" : "non-blocking");
  nice_debug_input_message_composition (messages, n_messages);

  /* Disallow re-entrant reads. */
  g_assert (component->n_recv_messages == 0 &&
      component->recv_messages == NULL);

  /* Set the component’s receive buffer. */
  context = nice_component_dup_io_context (component);
  nice_component_set_io_callback (component, NULL, NULL, messages, n_messages,
      &child_error);

  /* Add the cancellable as a source. */
  if (cancellable != NULL) {
    cancellable_source = g_cancellable_source_new (cancellable);
    g_source_set_callback (cancellable_source,
        (GSourceFunc) G_CALLBACK (nice_agent_recv_cancelled_cb), &child_error,
        NULL);
    g_source_attach (cancellable_source, context);
  }

  /* Is there already pending data left over from having an I/O callback
   * attached and switching to using nice_agent_recv()? This is a horrifically
   * specific use case which I hope nobody ever tries. And yet, it still must be
   * supported. */
  g_mutex_lock (&component->io_mutex);

  while (!received_enough &&
         !g_queue_is_empty (&component->pending_io_messages)) {
    pending_io_messages_recv_messages (component, agent->bytestream_tcp,
        component->recv_messages, component->n_recv_messages,
        &component->recv_messages_iter);

    nice_debug_verbose ("%s: %p: Received %d valid messages from pending I/O buffer.",
        G_STRFUNC, agent,
        nice_input_message_iter_get_n_valid_messages (
            &component->recv_messages_iter));

    received_enough =
        nice_input_message_iter_is_at_end (&component->recv_messages_iter,
            component->recv_messages, component->n_recv_messages);
  }

  g_mutex_unlock (&component->io_mutex);

  if (!received_enough && agent_try_consume_next_rfc4571_chunk (agent,
          component, component->recv_messages, component->n_recv_messages,
          &component->recv_messages_iter)) {
    n_valid_messages = nice_input_message_iter_get_n_valid_messages (
        &component->recv_messages_iter);
    nice_component_set_io_callback (component, NULL, NULL, NULL, 0, NULL);
    goto done;
  }

  /* For a reliable stream, grab any data from the pseudo-TCP input buffer
   * before trying the sockets. */
  if (agent->reliable &&
      pseudo_tcp_socket_get_available_bytes (component->tcp) > 0) {
    pseudo_tcp_socket_recv_messages (component->tcp,
        component->recv_messages, component->n_recv_messages,
        &component->recv_messages_iter, &child_error);
    adjust_tcp_clock (agent, stream, component);

    nice_debug_verbose ("%s: %p: Received %d valid messages from pseudo-TCP read "
        "buffer.", G_STRFUNC, agent,
        nice_input_message_iter_get_n_valid_messages (
            &component->recv_messages_iter));

    received_enough =
        nice_input_message_iter_is_at_end (&component->recv_messages_iter,
            component->recv_messages, component->n_recv_messages);
    error_reported = (child_error != NULL);
  }

  /* Each iteration of the main context will either receive some data, a
   * cancellation error or a socket error. In non-reliable mode, the iter’s
   * @message counter will be incremented after each read.
   *
   * In blocking, reliable mode, iterate the loop enough to fill exactly
   * @n_messages messages. In blocking, non-reliable mode, iterate the loop to
   * receive @n_messages messages (which may not fill all the buffers). In
   * non-blocking mode, stop iterating the loop if all sockets would block (i.e.
   * if no data was received for an iteration; in which case @child_error will
   * be set to %G_IO_ERROR_WOULD_BLOCK).
   */
  while (!received_enough && !error_reported && !all_sockets_would_block &&
      !reached_eos) {
    NiceInputMessageIter prev_recv_messages_iter;

    g_clear_error (&child_error);
    memcpy (&prev_recv_messages_iter, &component->recv_messages_iter,
        sizeof (NiceInputMessageIter));

    agent_unlock (agent);
    g_main_context_iteration (context, blocking);
    agent_lock (agent);

    if (!agent_find_component (agent, stream_id, component_id,
            &stream, &component)) {
      g_clear_error (&child_error);
      g_set_error (&child_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
          "Component removed during call.");

      component = NULL;

      goto recv_error;
    }

    received_enough =
        nice_input_message_iter_is_at_end (&component->recv_messages_iter,
            component->recv_messages, component->n_recv_messages);
    error_reported = (child_error != NULL &&
        !g_error_matches (child_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK));
    reached_eos = (agent->reliable &&
        pseudo_tcp_socket_is_closed_remotely (component->tcp) &&
        nice_input_message_iter_compare (&prev_recv_messages_iter,
            &component->recv_messages_iter));
    all_sockets_would_block = (!blocking && !reached_eos &&
        nice_input_message_iter_compare (&prev_recv_messages_iter,
            &component->recv_messages_iter));
  }

  n_valid_messages =
      nice_input_message_iter_get_n_valid_messages (
          &component->recv_messages_iter);  /* grab before resetting the iter */

  nice_component_set_io_callback (component, NULL, NULL, NULL, 0, NULL);

recv_error:
  /* Tidy up. Below this point, @component may be %NULL. */
  if (cancellable_source != NULL) {
    g_source_destroy (cancellable_source);
    g_source_unref (cancellable_source);
  }

  g_main_context_unref (context);

  /* Handle errors and cancellations. */
  if (child_error != NULL) {
    n_valid_messages = -1;
  } else if (n_valid_messages == 0 && all_sockets_would_block) {
    g_set_error_literal (&child_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
        g_strerror (EAGAIN));
    n_valid_messages = -1;
  }

  nice_debug_verbose ("%s: %p: n_valid_messages: %d, n_messages: %u", G_STRFUNC, agent,
      n_valid_messages, n_messages);

done:
  g_assert ((child_error != NULL) == (n_valid_messages == -1));
  g_assert (n_valid_messages < 0 || (guint) n_valid_messages <= n_messages);
  g_assert (n_valid_messages != 0 || reached_eos);

  if (child_error != NULL)
    g_propagate_error (error, child_error);

  agent_unlock_and_emit (agent);

  if (messages_orig) {
    for (i = 0; i < n_messages; i++) {
      if (messages[i].buffers != messages_orig[i].buffers) {
        g_assert (messages[i].n_buffers == 1);

        memcpy_buffer_to_input_message (&messages_orig[i],
            messages[i].buffers[0].buffer, messages[i].length);

        g_slice_free1 (1280, messages[i].buffers[0].buffer);
        g_slice_free (GInputVector, messages[i].buffers);

        messages[i].buffers = messages_orig[i].buffers;
        messages[i].n_buffers = messages_orig[i].n_buffers;
        messages[i].length = messages_orig[i].length;
      }
    }
    g_free (messages_orig);
  }

  return n_valid_messages;
}

NICEAPI_EXPORT gint
nice_agent_recv_messages (NiceAgent *agent, guint stream_id, guint component_id,
  NiceInputMessage *messages, guint n_messages, GCancellable *cancellable,
  GError **error)
{
  return nice_agent_recv_messages_blocking_or_nonblocking (agent, stream_id,
      component_id, TRUE, messages, n_messages, cancellable, error);
}

NICEAPI_EXPORT gssize
nice_agent_recv (NiceAgent *agent, guint stream_id, guint component_id,
  guint8 *buf, gsize buf_len, GCancellable *cancellable, GError **error)
{
  gint n_valid_messages;
  GInputVector local_bufs = { buf, buf_len };
  NiceInputMessage local_messages = { &local_bufs, 1, NULL, 0 };

  g_return_val_if_fail (NICE_IS_AGENT (agent), -1);
  g_return_val_if_fail (stream_id >= 1, -1);
  g_return_val_if_fail (component_id >= 1, -1);
  g_return_val_if_fail (buf != NULL || buf_len == 0, -1);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), -1);
  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  if (buf_len > G_MAXSSIZE) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "The buffer length can't exceed G_MAXSSIZE: %" G_GSSIZE_FORMAT,
        G_MAXSSIZE);
    return -1;
  }

  n_valid_messages = nice_agent_recv_messages (agent, stream_id, component_id,
      &local_messages, 1, cancellable, error);

  if (n_valid_messages <= 0)
    return n_valid_messages;

  return local_messages.length;
}

NICEAPI_EXPORT gint
nice_agent_recv_messages_nonblocking (NiceAgent *agent, guint stream_id,
    guint component_id, NiceInputMessage *messages, guint n_messages,
    GCancellable *cancellable, GError **error)
{
  return nice_agent_recv_messages_blocking_or_nonblocking (agent, stream_id,
      component_id, FALSE, messages, n_messages, cancellable, error);
}

NICEAPI_EXPORT gssize
nice_agent_recv_nonblocking (NiceAgent *agent, guint stream_id,
    guint component_id, guint8 *buf, gsize buf_len, GCancellable *cancellable,
    GError **error)
{
  gint n_valid_messages;
  GInputVector local_bufs = { buf, buf_len };
  NiceInputMessage local_messages = { &local_bufs, 1, NULL, 0 };

  g_return_val_if_fail (NICE_IS_AGENT (agent), -1);
  g_return_val_if_fail (stream_id >= 1, -1);
  g_return_val_if_fail (component_id >= 1, -1);
  g_return_val_if_fail (buf != NULL || buf_len == 0, -1);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), -1);
  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  if (buf_len > G_MAXSSIZE) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "The buffer length can't exceed G_MAXSSIZE: %" G_GSSIZE_FORMAT,
        G_MAXSSIZE);
    return -1;
  }

  n_valid_messages = nice_agent_recv_messages_nonblocking (agent, stream_id,
      component_id, &local_messages, 1, cancellable, error);

  if (n_valid_messages <= 0)
    return n_valid_messages;

  return local_messages.length;
}

/* nice_agent_send_messages_nonblocking_internal:
 *
 * Returns: number of bytes sent if allow_partial is %TRUE, the number
 * of messages otherwise.
 */

static gint
nice_agent_send_messages_nonblocking_internal (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  const NiceOutputMessage *messages,
  guint n_messages,
  gboolean allow_partial,
  GError **error)
{
  NiceStream *stream;
  NiceComponent *component;
  gint n_sent = -1; /* is in bytes if allow_partial is TRUE,
                       otherwise in messages */
  GError *child_error = NULL;

  g_assert (n_messages == 1 || !allow_partial);

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component)) {
    g_set_error (&child_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                 "Invalid stream/component.");
    goto done;
  }

  if (component->selected_pair.local != NULL &&
        !component->selected_pair.remote_consent.have) {
    g_set_error (&child_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 "Consent to send has been revoked by the peer");
    goto done;
  }

  /* FIXME: Cancellation isn’t yet supported, but it doesn’t matter because
   * we only deal with non-blocking writes. */
  if (component->selected_pair.local != NULL) {
    if (nice_debug_is_enabled ()) {
      gchar tmpbuf[INET6_ADDRSTRLEN];
      nice_address_to_string (&component->selected_pair.remote->c.addr, tmpbuf);

      nice_debug_verbose ("Agent %p : s%d:%d: sending %u messages to "
          "[%s]:%d", agent, stream_id, component_id, n_messages, tmpbuf,
          nice_address_get_port (&component->selected_pair.remote->c.addr));
    }

    if(agent->reliable &&
        !nice_socket_is_reliable (component->selected_pair.local->sockptr)) {
      if (!pseudo_tcp_socket_is_closed (component->tcp)) {
        /* Send on the pseudo-TCP socket. */
        n_sent = pseudo_tcp_socket_send_messages (component->tcp, messages,
            n_messages, allow_partial, &child_error);
        adjust_tcp_clock (agent, stream, component);

        if (!pseudo_tcp_socket_can_send (component->tcp))
          g_cancellable_reset (component->tcp_writable_cancellable);
        if (n_sent < 0 && !g_error_matches (child_error, G_IO_ERROR,
                G_IO_ERROR_WOULD_BLOCK)) {
          /* Signal errors */
          priv_pseudo_tcp_error (agent, component);
        }
      } else {
        g_set_error (&child_error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Pseudo-TCP socket not connected.");
      }
    } else {
      NiceSocket *sock;
      NiceAddress *addr;

      sock = component->selected_pair.local->sockptr;
      addr = &component->selected_pair.remote->c.addr;

      if (nice_socket_is_reliable (sock)) {
        guint i;

        /* ICE-TCP requires that all packets be framed with RFC4571 */
        n_sent = 0;
        for (i = 0; i < n_messages; i++) {
          const NiceOutputMessage *message = &messages[i];
          gsize message_len = output_message_get_size (message);
          gsize offset = 0;
          gsize current_offset = 0;
          gsize offset_in_buffer = 0;
          gint n_sent_framed;
          GOutputVector *local_bufs;
          NiceOutputMessage local_message;
          guint j;
          guint n_bufs = 0;

          /* Count the number of buffers. */
          if (message->n_buffers == -1) {
            for (j = 0; message->buffers[j].buffer != NULL; j++)
              n_bufs++;
          } else {
            n_bufs = message->n_buffers;
          }

          local_bufs = g_malloc_n (n_bufs + 1, sizeof (GOutputVector));
          local_message.buffers = local_bufs;

          while (message_len > 0) {
            guint16 packet_len;
            guint16 rfc4571_frame;

            /* Split long messages into 62KB packets, leaving enough space
             * for TURN overhead as well */
            if (message_len > 0xF800)
              packet_len = 0xF800;
            else
              packet_len = (guint16) message_len;
            message_len -= packet_len;
            rfc4571_frame = htons (packet_len);

            local_bufs[0].buffer = &rfc4571_frame;
            local_bufs[0].size = sizeof (guint16);

            local_message.n_buffers = 1;
            /* If we had to split the message, we need to find which buffer
             * to start copying from and our offset within that buffer */
            offset_in_buffer = 0;
            current_offset = 0;
            for (j = 0; j < n_bufs; j++) {
              if (message->buffers[j].size < offset - current_offset) {
                current_offset += message->buffers[j].size;
                continue;
              } else {
                offset_in_buffer = offset - current_offset;
                current_offset = offset;
                break;
              }
            }

            /* Keep j position in array and start copying from there */
            for (; j < n_bufs; j++) {
              local_bufs[local_message.n_buffers].buffer =
                  ((guint8 *) message->buffers[j].buffer) + offset_in_buffer;
              local_bufs[local_message.n_buffers].size =
                  MIN (message->buffers[j].size, packet_len);
              packet_len -= local_bufs[local_message.n_buffers].size;
              offset += local_bufs[local_message.n_buffers++].size;
              offset_in_buffer = 0;
            }

            /* If we sent part of the message already, then send the rest
             * reliably so the message is sent as a whole even if it's split */
            if (current_offset == 0 && !agent->reliable)
              n_sent_framed = nice_socket_send_messages (sock, addr,
                  &local_message, 1);
            else
              n_sent_framed = nice_socket_send_messages_reliable (sock, addr,
                  &local_message, 1);

            if (component->tcp_writable_cancellable &&
                !nice_socket_can_send (sock, addr))
              g_cancellable_reset (component->tcp_writable_cancellable);

            if (n_sent_framed < 0 && n_sent == 0)
              n_sent = n_sent_framed;
            if (n_sent_framed != 1)
              break;
            /* This is the last split frame, increment n_sent */
            if (message_len == 0)
              n_sent ++;
          }
          g_free (local_bufs);
        }

      } else {
        n_sent = nice_socket_send_messages (sock, addr, messages, n_messages);
      }

      if (n_sent < 0) {
        g_set_error (&child_error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Error writing data to socket.");
      } else if (n_sent > 0 && allow_partial) {
        g_assert (n_messages == 1);
        n_sent = output_message_get_size (messages);
      }
    }
  } else {
    /* Socket isn’t properly open yet. */
    n_sent = 0;  /* EWOULDBLOCK */
  }

  /* Handle errors and cancellations. */
  if (n_sent == 0) {
    g_set_error_literal (&child_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
        g_strerror (EAGAIN));
    n_sent = -1;
  }

  nice_debug_verbose ("%s: n_sent: %d, n_messages: %u", G_STRFUNC,
      n_sent, n_messages);

done:
  g_assert ((child_error != NULL) == (n_sent == -1));
  g_assert (n_sent != 0);
  g_assert (n_sent < 0 ||
      (!allow_partial && (guint) n_sent <= n_messages) ||
      (allow_partial && n_messages == 1 &&
          (gsize) n_sent <= output_message_get_size (&messages[0])));

  if (child_error != NULL)
    g_propagate_error (error, child_error);

  agent_unlock_and_emit (agent);

  return n_sent;
}

NICEAPI_EXPORT gint
nice_agent_send_messages_nonblocking (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  const NiceOutputMessage *messages,
  guint n_messages,
  GCancellable *cancellable,
  GError **error)
{
  g_return_val_if_fail (NICE_IS_AGENT (agent), -1);
  g_return_val_if_fail (stream_id >= 1, -1);
  g_return_val_if_fail (component_id >= 1, -1);
  g_return_val_if_fail (n_messages == 0 || messages != NULL, -1);
  g_return_val_if_fail (
      cancellable == NULL || G_IS_CANCELLABLE (cancellable), -1);
  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

  return nice_agent_send_messages_nonblocking_internal (agent, stream_id,
      component_id, messages, n_messages, FALSE, error);
}

NICEAPI_EXPORT gint
nice_agent_send (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  guint len,
  const gchar *buf)
{
  GOutputVector local_buf = { buf, len };
  NiceOutputMessage local_message = { &local_buf, 1 };
  gint n_sent_bytes;

  g_return_val_if_fail (NICE_IS_AGENT (agent), -1);
  g_return_val_if_fail (stream_id >= 1, -1);
  g_return_val_if_fail (component_id >= 1, -1);
  g_return_val_if_fail (buf != NULL, -1);

  n_sent_bytes = nice_agent_send_messages_nonblocking_internal (agent,
      stream_id, component_id, &local_message, 1, TRUE, NULL);

  return n_sent_bytes;
}

NICEAPI_EXPORT GSList *
nice_agent_get_local_candidates (
  NiceAgent *agent,
  guint stream_id,
  guint component_id)
{
  NiceComponent *component;
  GSList * ret = NULL;
  GSList * item = NULL;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (component_id >= 1, NULL);

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id, NULL, &component)) {
    goto done;
  }

  for (item = component->local_candidates; item; item = item->next) {
    NiceCandidate *cand = item->data;

    if (agent->force_relay && cand->type != NICE_CANDIDATE_TYPE_RELAYED)
      continue;

    ret = g_slist_append (ret, nice_candidate_copy (cand));
  }

 done:
  agent_unlock_and_emit (agent);
  return ret;
}


NICEAPI_EXPORT GSList *
nice_agent_get_remote_candidates (
  NiceAgent *agent,
  guint stream_id,
  guint component_id)
{
  NiceComponent *component;
  GSList *ret = NULL, *item = NULL;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (component_id >= 1, NULL);

  agent_lock (agent);
  if (!agent_find_component (agent, stream_id, component_id, NULL, &component))
    {
      goto done;
    }

  for (item = component->remote_candidates; item; item = item->next)
    ret = g_slist_append (ret, nice_candidate_copy (item->data));

 done:
  agent_unlock_and_emit (agent);
  return ret;
}

gboolean
nice_agent_restart (
  NiceAgent *agent)
{
  GSList *i;

  agent_lock (agent);

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;

    /* step: reset local credentials for the stream and
     * clean up the list of remote candidates */
    nice_stream_restart (stream, agent);
  }

  agent_unlock_and_emit (agent);
  return TRUE;
}

gboolean
nice_agent_restart_stream (
    NiceAgent *agent,
    guint stream_id)
{
  gboolean res = FALSE;
  NiceStream *stream;

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (!stream) {
    g_warning ("Could not find  stream %u", stream_id);
    goto done;
  }

  /* step: reset local credentials for the stream,
   * clean up the list of candidates, and the conncheck list */
  nice_stream_restart (stream, agent);

  res = TRUE;
 done:
  agent_unlock_and_emit (agent);
  return res;
}


static void
nice_agent_dispose (GObject *object)
{
  GSList *i;
  QueuedSignal *sig;
  NiceAgent *agent = NICE_AGENT (object);

  agent_lock (agent);

  /* step: free resources for the binding discovery timers */
  discovery_free (agent);
  g_assert (agent->discovery_list == NULL);

  /* step: free resources for the connectivity check timers */
  conn_check_free (agent);

  g_cancellable_cancel (agent->stun_resolving_cancellable);
  g_clear_object (&agent->stun_resolving_cancellable);
  g_slist_free (agent->stun_resolving_list);
  agent->stun_resolving_list = NULL;

  priv_remove_keepalive_timer (agent);

  for (i = agent->local_addresses; i; i = i->next)
    {
      NiceAddress *a = i->data;

      nice_address_free (a);
    }

  g_slist_free (agent->local_addresses);
  agent->local_addresses = NULL;

  if (agent->refresh_list)
    g_warning ("Agent %p : We still have alive TURN refreshes. Consider "
        "using nice_agent_close_async() to prune them before releasing the "
        "agent.", agent);

  /* We must free refreshes before closing streams because a refresh
   * callback data may contain a pointer to a stream to be freed, when
   * previously called in the context of a stream removal, by
   * refresh_prune_stream_async()
   */
  for (i = agent->refresh_list; i;) {
    GSList *next = i->next;
    CandidateRefresh *refresh = i->data;

    refresh_free (agent, refresh);
    i = next;
  }

  while (agent->streams) {
    NiceStream *s = agent->streams->data;

    priv_stop_upnp (agent, s);
    nice_stream_close (agent, s);
    g_object_unref (s);

    agent->streams = g_slist_delete_link(agent->streams, agent->streams);
  }

  while (agent->pruning_streams) {
    NiceStream *s = agent->pruning_streams->data;

    nice_stream_close (agent, s);
    g_object_unref (s);

    agent->pruning_streams = g_slist_delete_link(agent->pruning_streams,
        agent->pruning_streams);
  }

  while ((sig = g_queue_pop_head (&agent->pending_signals))) {
    free_queued_signal (sig);
  }

  g_free (agent->stun_server_ip);
  agent->stun_server_ip = NULL;

  g_free (agent->proxy_ip);
  agent->proxy_ip = NULL;
  g_free (agent->proxy_username);
  agent->proxy_username = NULL;
  g_free (agent->proxy_password);
  agent->proxy_password = NULL;
  if (agent->proxy_extra_headers != NULL) {
    g_hash_table_unref (agent->proxy_extra_headers);
    agent->proxy_extra_headers = NULL;
  }
  nice_rng_free (agent->rng);
  agent->rng = NULL;

#ifdef HAVE_GUPNP
  if (agent->upnp) {
    g_object_unref (agent->upnp);
    agent->upnp = NULL;
  }
#endif

  g_free (agent->software_attribute);
  agent->software_attribute = NULL;

  if (agent->main_context != NULL)
    g_main_context_unref (agent->main_context);
  agent->main_context = NULL;

  agent_unlock (agent);

  g_mutex_clear (&agent->agent_mutex);

  if (G_OBJECT_CLASS (nice_agent_parent_class)->dispose)
    G_OBJECT_CLASS (nice_agent_parent_class)->dispose (object);
}

gboolean
component_io_cb (GSocket *gsocket, GIOCondition condition, gpointer user_data)
{
  SocketSource *socket_source = user_data;
  NiceComponent *component;
  NiceAgent *agent;
  NiceStream *stream;
  gboolean has_io_callback;
  gboolean remove_source = FALSE;

  component = socket_source->component;

  if (g_source_is_destroyed (g_main_current_source ())) {
    /* Silently return FALSE. */
    nice_debug ("%s: source %p destroyed", G_STRFUNC, g_main_current_source ());
    return G_SOURCE_REMOVE;
  }

  agent = g_weak_ref_get (&component->agent_ref);
  if (agent == NULL)
    return G_SOURCE_REMOVE;

  agent_lock (agent);

  if (g_source_is_destroyed (g_main_current_source ())) {
    /* Silently return FALSE. */
    nice_debug ("%s: source %p destroyed", G_STRFUNC, g_main_current_source ());

    agent_unlock (agent);
    g_object_unref (agent);
    return G_SOURCE_REMOVE;
  }

  stream = agent_find_stream (agent, component->stream_id);

  if (stream == NULL) {
    nice_debug ("%s: stream %d destroyed", G_STRFUNC, component->stream_id);

    agent_unlock (agent);
    g_object_unref (agent);
    return G_SOURCE_REMOVE;
  }

  /* Remove disconnected sockets when we get a HUP and there's no more data to
   * be read. */
  if (condition & G_IO_HUP && !(condition & G_IO_IN)) {
    nice_debug ("Agent %p: NiceSocket %p has received HUP", agent,
        socket_source->socket);
    if (component->selected_pair.local &&
        component->selected_pair.local->sockptr == socket_source->socket &&
        component->state == NICE_COMPONENT_STATE_READY) {
      nice_debug ("Agent %p: Selected pair socket %p has HUP, declaring failed",
          agent, socket_source->socket);
      agent_signal_component_state_change (agent,
          stream->id, component->id, NICE_COMPONENT_STATE_FAILED);
    }

    nice_component_remove_socket (agent, component, socket_source->socket);
    agent_unlock (agent);
    g_object_unref (agent);
    return G_SOURCE_REMOVE;
  }

  has_io_callback = nice_component_has_io_callback (component);

  /* Choose which receive buffer to use. If we’re reading for
   * nice_agent_attach_recv(), use a local static buffer. If we’re reading for
   * nice_agent_recv_messages(), use the buffer provided by the client.
   *
   * has_io_callback cannot change throughout this function, as we operate
   * entirely with the agent lock held, and nice_component_set_io_callback()
   * would need to take the agent lock to change the Component’s
   * io_callback. */
  g_assert (!has_io_callback || component->recv_messages == NULL);

  if (agent->reliable && !nice_socket_is_reliable (socket_source->socket)) {
#define TCP_HEADER_SIZE 24 /* bytes */
    guint8 local_header_buf[TCP_HEADER_SIZE];
    /* FIXME: Currently, the critical path for reliable packet delivery has two
     * memcpy()s: one into the pseudo-TCP receive buffer, and one out of it.
     * This could moderately easily be reduced to one memcpy() in the common
     * case of in-order packet delivery, by replacing local_body_buf with a
     * pointer into the pseudo-TCP receive buffer. If it turns out the packet
     * is out-of-order (which we can only know after parsing its header), the
     * data will need to be moved in the buffer. If the packet *is* in order,
     * however, the only memcpy() then needed is from the pseudo-TCP receive
     * buffer to the client’s message buffers.
     *
     * In fact, in the case of a reliable agent with I/O callbacks, zero
     * memcpy()s can be achieved (for in-order packet delivery) by emittin the
     * I/O callback directly from the pseudo-TCP receive buffer. */
    GInputVector local_bufs[] = {
      { local_header_buf, sizeof (local_header_buf) },
      { component->recv_buffer, component->recv_buffer_size },
    };
    NiceInputMessage local_message = {
      local_bufs, G_N_ELEMENTS (local_bufs), NULL, 0
    };
    RecvStatus retval = 0;

    if (pseudo_tcp_socket_is_closed (component->tcp)) {
      nice_debug ("Agent %p: not handling incoming packet for s%d:%d "
          "because pseudo-TCP socket does not exist in reliable mode.", agent,
          stream->id, component->id);
      remove_source = TRUE;
      goto done;
    }

    while (has_io_callback ||
        (component->recv_messages != NULL &&
            !nice_input_message_iter_is_at_end (&component->recv_messages_iter,
                component->recv_messages, component->n_recv_messages))) {
      /* Receive a single message. This will receive it into the given
       * @local_bufs then, for pseudo-TCP, emit I/O callbacks or copy it into
       * component->recv_messages in pseudo_tcp_socket_readable(). STUN packets
       * will be parsed in-place. */
      retval = agent_recv_message_unlocked (agent, stream, component,
          socket_source->socket, &local_message);

      nice_debug_verbose ("%s: %p: received %d valid messages with %" G_GSSIZE_FORMAT
          " bytes", G_STRFUNC, agent, retval, local_message.length);

      /* Don’t expect any valid messages to escape pseudo_tcp_socket_readable()
       * when in reliable mode. */
      g_assert (retval != RECV_SUCCESS);

      if (retval == RECV_WOULD_BLOCK) {
        /* EWOULDBLOCK. */
        break;
      } else if (retval == RECV_ERROR) {
        /* Other error. */
        nice_debug ("%s: error receiving message", G_STRFUNC);
        remove_source = TRUE;
        break;
      }

      has_io_callback = nice_component_has_io_callback (component);
    }
  } else if (agent->reliable &&
      nice_socket_is_reliable (socket_source->socket)) {
    NiceInputMessageIter *iter = &component->recv_messages_iter;
    gsize total_bytes_received = 0;

    while (has_io_callback ||
        (component->recv_messages != NULL &&
            !nice_input_message_iter_is_at_end (iter,
                component->recv_messages, component->n_recv_messages))) {
      GInputVector internal_buf = {
        component->recv_buffer, component->recv_buffer_size
      };
      NiceInputMessage internal_message = {
        &internal_buf, 1, NULL, 0
      };
      NiceInputMessage *msg;
      guint n_bufs, i;
      GInputVector *bufs;
      RecvStatus retval = 0;

      msg = has_io_callback
          ? &internal_message
          : &component->recv_messages[iter->message];

      if (msg->n_buffers == -1) {
        n_bufs = 0;
        for (i = 0; msg->buffers[i].buffer != NULL; i++)
          n_bufs++;
      } else {
        n_bufs = msg->n_buffers;
      }

      bufs = g_newa (GInputVector, n_bufs);
      memcpy (bufs, msg->buffers, n_bufs * sizeof (GInputVector));

      msg->length = 0;

      do {
        NiceInputMessage m = { bufs, n_bufs, msg->from, 0 };
        gsize off;

        retval = agent_recv_message_unlocked (agent, stream, component,
            socket_source->socket, &m);
        if (retval == RECV_WOULD_BLOCK || retval == RECV_ERROR)
          break;
        if (retval == RECV_OOB)
          continue;

        msg->length += m.length;
        total_bytes_received += m.length;

        if (!agent->bytestream_tcp)
          break;

        off = 0;
        for (i = 0; i < n_bufs; i++) {
          GInputVector *buf = &bufs[i];
          const gsize start = off;
          const gsize end = start + buf->size;

          if (m.length > start) {
            const gsize consumed = MIN (m.length - start, buf->size);
            buf->buffer = (guint8 *) buf->buffer + consumed;
            buf->size -= consumed;
            if (buf->size > 0)
              break;
          } else {
            break;
          }

          off = end;
        }
        bufs += i;
        n_bufs -= i;
      } while (n_bufs > 0);

      if (msg->length > 0) {
        nice_debug_verbose ("%s: %p: received a valid message with %"
            G_GSIZE_FORMAT " bytes", G_STRFUNC, agent, msg->length);
        if (has_io_callback) {
          nice_component_emit_io_callback (agent, component, msg->length);
        } else {
          iter->message++;
        }
      }

      if (retval == RECV_WOULD_BLOCK) {
        /* EWOULDBLOCK. */
        break;
      } else if (retval == RECV_ERROR) {
        /* Other error. */
        nice_debug ("%s: error receiving message", G_STRFUNC);
        remove_source = TRUE;
        break;
      }

      if (has_io_callback && g_source_is_destroyed (g_main_current_source ())) {
        nice_debug ("Component IO source disappeared during the callback");
        goto out;
      }
      has_io_callback = nice_component_has_io_callback (component);
    }
  } else if (has_io_callback) {
    while (has_io_callback) {
      GInputVector local_bufs = {
        component->recv_buffer, component->recv_buffer_size
      };
      NiceInputMessage local_message = { &local_bufs, 1, NULL, 0 };
      RecvStatus retval;

      /* Receive a single message. */
      retval = agent_recv_message_unlocked (agent, stream, component,
          socket_source->socket, &local_message);

      if (retval == RECV_WOULD_BLOCK) {
        /* EWOULDBLOCK. */
        nice_debug_verbose ("%s: %p: no message available on read attempt",
            G_STRFUNC, agent);
        break;
      } else if (retval == RECV_ERROR) {
        /* Other error. */
        nice_debug ("%s: %p: error receiving message", G_STRFUNC, agent);
        remove_source = TRUE;
        break;
      }

      if (retval == RECV_SUCCESS) {
        nice_debug_verbose ("%s: %p: received a valid message with %" G_GSSIZE_FORMAT
            " bytes", G_STRFUNC, agent, local_message.length);

        if (local_message.length > 0) {
          nice_component_emit_io_callback (agent, component,
              local_message.length);
        }
      }

      if (g_source_is_destroyed (g_main_current_source ())) {
        nice_debug ("Component IO source disappeared during the callback");
        goto out;
      }
      has_io_callback = nice_component_has_io_callback (component);
    }
  } else if (component->recv_messages != NULL) {
    RecvStatus retval;

    /* Don’t want to trample over partially-valid buffers. */
    g_assert (component->recv_messages_iter.buffer == 0);
    g_assert (component->recv_messages_iter.offset == 0);

    while (!nice_input_message_iter_is_at_end (&component->recv_messages_iter,
        component->recv_messages, component->n_recv_messages)) {
      /* Receive a single message. This will receive it into the given
       * user-provided #NiceInputMessage, which it’s the user’s responsibility
       * to ensure is big enough to avoid data loss (since we’re in non-reliable
       * mode). Iterate to receive as many messages as possible.
       *
       * STUN packets will be parsed in-place. */
      retval = agent_recv_message_unlocked (agent, stream, component,
          socket_source->socket,
          &component->recv_messages[component->recv_messages_iter.message]);

      nice_debug_verbose ("%s: %p: received %d valid messages", G_STRFUNC, agent,
          retval);

      if (retval == RECV_SUCCESS) {
        /* Successfully received a single message. */
        component->recv_messages_iter.message++;
        g_clear_error (component->recv_buf_error);
      } else if (retval == RECV_WOULD_BLOCK) {
        /* EWOULDBLOCK. */
        if (component->recv_messages_iter.message == 0 &&
            component->recv_buf_error != NULL &&
            *component->recv_buf_error == NULL) {
          g_set_error_literal (component->recv_buf_error, G_IO_ERROR,
              G_IO_ERROR_WOULD_BLOCK, g_strerror (EAGAIN));
        }
        break;
      } else if (retval == RECV_ERROR) {
        /* Other error. */
        remove_source = TRUE;
        break;
      } /* else if (retval == RECV_OOB) { ignore me and continue; } */
    }
  }

done:

  if (remove_source)
    nice_component_remove_socket (agent, component, socket_source->socket);

  /* If we’re in the middle of a read, don’t emit any signals, or we could cause
   * re-entrancy by (e.g.) emitting component-state-changed and having the
   * client perform a read. */
  if (component->n_recv_messages == 0 && component->recv_messages == NULL) {
    agent_unlock_and_emit (agent);
  } else {
    agent_unlock (agent);
  }

  g_object_unref (agent);

  return !remove_source;

out:
  agent_unlock_and_emit (agent);

  g_object_unref (agent);

  return G_SOURCE_REMOVE;
}

NICEAPI_EXPORT gboolean
nice_agent_attach_recv (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  GMainContext *ctx,
  NiceAgentRecvFunc func,
  gpointer data)
{
  NiceComponent *component = NULL;
  NiceStream *stream = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);
  g_return_val_if_fail (component_id >= 1, FALSE);

  agent_lock (agent);

  /* attach candidates */

  /* step: check that params specify an existing pair */
  if (!agent_find_component (agent, stream_id, component_id, &stream, &component)) {
    g_warning ("Could not find component %u in stream %u", component_id,
        stream_id);
    goto done;
  }

  if (ctx == NULL)
    ctx = g_main_context_default ();

  /* Set the component’s I/O context. */
  nice_component_set_io_context (component, ctx);
  nice_component_set_io_callback (component, func, data, NULL, 0, NULL);
  ret = TRUE;

  if (func) {
    /* If we got detached, maybe our readable callback didn't finish reading
     * all available data in the pseudotcp, so we need to make sure we free
     * our recv window, so the readable callback can be triggered again on the
     * next incoming data.
     * but only do this if we know we're already readable, otherwise we might
     * trigger an error in the initial, pre-connection attach. */
    if (agent->reliable && !pseudo_tcp_socket_is_closed (component->tcp) &&
        component->tcp_readable)
      pseudo_tcp_socket_readable (component->tcp, component);
  }

 done:
  agent_unlock_and_emit (agent);
  return ret;
}

NICEAPI_EXPORT gboolean
nice_agent_set_selected_pair (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  const gchar *lfoundation,
  const gchar *rfoundation)
{
  NiceComponent *component;
  NiceStream *stream;
  CandidatePair pair;
  gboolean ret = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);
  g_return_val_if_fail (component_id >= 1, FALSE);
  g_return_val_if_fail (lfoundation, FALSE);
  g_return_val_if_fail (rfoundation, FALSE);

  agent_lock (agent);

  /* step: check that params specify an existing pair */
  if (!agent_find_component (agent, stream_id, component_id, &stream, &component)) {
    goto done;
  }

  if (!nice_component_find_pair (component, agent, lfoundation, rfoundation, &pair)){
    goto done;
  }

  /* step: stop connectivity checks (note: for the whole stream) */
  conn_check_prune_stream (agent, stream);

  if (agent->reliable && !nice_socket_is_reliable (pair.local->sockptr) &&
      pseudo_tcp_socket_is_closed (component->tcp)) {
    nice_debug ("Agent %p: not setting selected pair for s%d:%d because "
        "pseudo tcp socket does not exist in reliable mode", agent,
        stream->id, component->id);
    goto done;
  }

  /* step: change component state; we could be in STATE_DISCONNECTED; skip
   * STATE_GATHERING and continue through the states to give client code a nice
   * logical progression. See http://phabricator.freedesktop.org/D218 for
   * discussion. */
  if (component->state < NICE_COMPONENT_STATE_CONNECTING ||
      component->state == NICE_COMPONENT_STATE_FAILED)
    agent_signal_component_state_change (agent, stream_id, component_id,
        NICE_COMPONENT_STATE_CONNECTING);
  if (component->state < NICE_COMPONENT_STATE_CONNECTED)
    agent_signal_component_state_change (agent, stream_id, component_id,
        NICE_COMPONENT_STATE_CONNECTED);
  agent_signal_component_state_change (agent, stream_id, component_id,
      NICE_COMPONENT_STATE_READY);

  /* step: set the selected pair */
  /* XXX: assume we have consent to send to this selected remote address */
  pair.remote_consent.have = TRUE;
  nice_component_update_selected_pair (agent, component, &pair);
  agent_signal_new_selected_pair (agent, stream_id, component_id,
      (NiceCandidate *) pair.local, (NiceCandidate *) pair.remote);

  ret = TRUE;

 done:
  agent_unlock_and_emit (agent);
  return ret;
}

NICEAPI_EXPORT gboolean
nice_agent_get_selected_pair (NiceAgent *agent, guint stream_id,
    guint component_id, NiceCandidate **local, NiceCandidate **remote)
{
  NiceComponent *component;
  NiceStream *stream;
  gboolean ret = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);
  g_return_val_if_fail (component_id >= 1, FALSE);
  g_return_val_if_fail (local != NULL, FALSE);
  g_return_val_if_fail (remote != NULL, FALSE);

  agent_lock (agent);

  /* step: check that params specify an existing pair */
  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component))
    goto done;

  if (component->selected_pair.local && component->selected_pair.remote) {
    *local = (NiceCandidate *) component->selected_pair.local;
    *remote = (NiceCandidate *) component->selected_pair.remote;
    ret = TRUE;
  }

 done:
  agent_unlock_and_emit (agent);

  return ret;
}

NICEAPI_EXPORT GSocket *
nice_agent_get_selected_socket (NiceAgent *agent, guint stream_id,
    guint component_id)
{
  NiceComponent *component;
  NiceStream *stream;
  NiceSocket *nice_socket;
  GSocket *g_socket = NULL;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (component_id >= 1, NULL);

  agent_lock (agent);

  /* Reliable streams are pseudotcp or MUST use RFC 4571 framing */
  if (agent->reliable)
    goto done;

  /* step: check that params specify an existing pair */
  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component))
    goto done;

  if (!component->selected_pair.local || !component->selected_pair.remote)
    goto done;

  if (component->selected_pair.local->c.type == NICE_CANDIDATE_TYPE_RELAYED)
    goto done;

  /* ICE-TCP requires RFC4571 framing, even if unreliable */
  if (component->selected_pair.local->c.transport != NICE_CANDIDATE_TRANSPORT_UDP)
    goto done;

  nice_socket = (NiceSocket *)component->selected_pair.local->sockptr;
  if (nice_socket->fileno)
    g_socket = g_object_ref (nice_socket->fileno);

 done:
  agent_unlock_and_emit (agent);

  return g_socket;
}

typedef struct _TimeoutData
{
  GWeakRef/*<NiceAgent>*/ agent_ref;
  NiceTimeoutLockedCallback function;
  gpointer user_data;
} TimeoutData;

static void
timeout_data_destroy (TimeoutData *data)
{
  g_weak_ref_clear (&data->agent_ref);
  g_slice_free (TimeoutData, data);
}

static TimeoutData *
timeout_data_new (NiceAgent *agent, NiceTimeoutLockedCallback function,
    gpointer user_data)
{
  TimeoutData *data = g_slice_new0 (TimeoutData);

  g_weak_ref_init (&data->agent_ref, agent);
  data->function = function;
  data->user_data = user_data;

  return data;
}

static gboolean
timeout_cb (gpointer user_data)
{
  TimeoutData *data = user_data;
  NiceAgent *agent;
  gboolean ret = G_SOURCE_REMOVE;

  agent = g_weak_ref_get (&data->agent_ref);
  if (agent == NULL) {
    return G_SOURCE_REMOVE;
  }

  agent_lock (agent);

  /* A race condition might happen where the mutex above waits for the lock
   * and in the meantime another thread destroys the source.
   * In that case, we don't need to run the function since it should
   * have been cancelled */
  if (g_source_is_destroyed (g_main_current_source ())) {
    nice_debug ("Source was destroyed. Avoided race condition in timeout_cb");

    agent_unlock (agent);
    goto end;
  }

  ret = data->function (agent, data->user_data);

  agent_unlock_and_emit (agent);

 end:
  g_object_unref (agent);

  return ret;
}

/* Create a new timer GSource with the given @name, @interval, callback
 * @function and @data, and assign it to @out, destroying and freeing any
 * existing #GSource in @out first.
 *
 * This guarantees that a timer won’t be overwritten without being destroyed.
 *
 * @interval is given in milliseconds.
 */
static void agent_timeout_add_with_context_internal (NiceAgent *agent,
    GSource **out, const gchar *name, guint interval, gboolean seconds,
    NiceTimeoutLockedCallback function, gpointer user_data)
{
  GSource *source;
  TimeoutData *data;

  g_return_if_fail (function != NULL);
  g_return_if_fail (out != NULL);

  /* Destroy any existing source. */
  if (*out != NULL) {
    g_source_destroy (*out);
    g_source_unref (*out);
    *out = NULL;
  }

  /* Create the new source. */
  if (seconds)
    source = g_timeout_source_new_seconds (interval);
  else
    source = g_timeout_source_new (interval);

  g_source_set_name (source, name);
  data = timeout_data_new (agent, function, user_data);
  g_source_set_callback (source, timeout_cb, data,
      (GDestroyNotify)timeout_data_destroy);
  g_source_attach (source, agent->main_context);

  /* Return it! */
  *out = source;
}

void agent_timeout_add_with_context (NiceAgent *agent,
    GSource **out, const gchar *name, guint interval,
    NiceTimeoutLockedCallback function, gpointer user_data)
{
  agent_timeout_add_with_context_internal (agent, out, name, interval, FALSE,
      function, user_data);
}

void agent_timeout_add_seconds_with_context (NiceAgent *agent,
    GSource **out, const gchar *name, guint interval,
    NiceTimeoutLockedCallback function, gpointer user_data)
{
  agent_timeout_add_with_context_internal (agent, out, name, interval, TRUE,
      function, user_data);
}

NICEAPI_EXPORT gboolean
nice_agent_set_selected_remote_candidate (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  NiceCandidate *candidate)
{
  NiceComponent *component;
  NiceStream *stream;
  NiceCandidateImpl *lcandidate = NULL;
  gboolean ret = FALSE;
  NiceCandidateImpl *local = NULL, *remote = NULL;
  guint64 priority;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id != 0, FALSE);
  g_return_val_if_fail (component_id != 0, FALSE);
  g_return_val_if_fail (candidate != NULL, FALSE);

  agent_lock (agent);

  /* step: check if the component exists*/
  if (!agent_find_component (agent, stream_id, component_id, &stream, &component)) {
    goto done;
  }

  /* step: stop connectivity checks (note: for the whole stream) */
  conn_check_prune_stream (agent, stream);

  /* Store previous selected pair */
  local = component->selected_pair.local;
  remote = component->selected_pair.remote;
  priority = component->selected_pair.priority;

  /* step: set the selected pair */
  lcandidate = nice_component_set_selected_remote_candidate (component, agent,
      candidate);
  if (!lcandidate)
    goto done;

  if (agent->reliable && !nice_socket_is_reliable (lcandidate->sockptr) &&
      pseudo_tcp_socket_is_closed (component->tcp)) {
    nice_debug ("Agent %p: not setting selected remote candidate s%d:%d because"
        " pseudo tcp socket does not exist in reliable mode", agent,
        stream->id, component->id);
    /* Revert back to previous selected pair */
    /* FIXME: by doing this, we lose the keepalive tick */
    component->selected_pair.local = (NiceCandidateImpl *) local;
    component->selected_pair.remote = remote;
    component->selected_pair.priority = priority;
    goto done;
  }

  /* step: change component state; we could be in STATE_DISCONNECTED; skip
   * STATE_GATHERING and continue through the states to give client code a nice
   * logical progression. See http://phabricator.freedesktop.org/D218 for
   * discussion. */
  if (component->state < NICE_COMPONENT_STATE_CONNECTING ||
      component->state == NICE_COMPONENT_STATE_FAILED)
    agent_signal_component_state_change (agent, stream_id, component_id,
        NICE_COMPONENT_STATE_CONNECTING);
  if (component->state < NICE_COMPONENT_STATE_CONNECTED)
    agent_signal_component_state_change (agent, stream_id, component_id,
        NICE_COMPONENT_STATE_CONNECTED);
  agent_signal_component_state_change (agent, stream_id, component_id,
      NICE_COMPONENT_STATE_READY);

  agent_signal_new_selected_pair (agent, stream_id, component_id,
      (NiceCandidate *) lcandidate, candidate);

  ret = TRUE;

 done:
  agent_unlock_and_emit (agent);
  return ret;
}

void
_priv_set_socket_tos (NiceAgent *agent, NiceSocket *sock, gint tos)
{
  if (sock->fileno == NULL)
    return;

  if (setsockopt (g_socket_get_fd (sock->fileno), IPPROTO_IP,
          IP_TOS, (const char *) &tos, sizeof (tos)) < 0) {
    nice_debug ("Agent %p: Could not set socket ToS: %s", agent,
        g_strerror (errno));
  }
#ifdef IPV6_TCLASS
  if (setsockopt (g_socket_get_fd (sock->fileno), IPPROTO_IPV6,
          IPV6_TCLASS, (const char *) &tos, sizeof (tos)) < 0) {
    nice_debug ("Agent %p: Could not set IPV6 socket ToS: %s", agent,
        g_strerror (errno));
  }
#endif
}


NICEAPI_EXPORT void
nice_agent_set_stream_tos (NiceAgent *agent,
  guint stream_id, gint tos)
{
  GSList *i, *j;
  NiceStream *stream;

  g_return_if_fail (NICE_IS_AGENT (agent));
  g_return_if_fail (stream_id >= 1);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream == NULL)
    goto done;

  stream->tos = tos;
  for (i = stream->components; i; i = i->next) {
    NiceComponent *component = i->data;

    for (j = component->local_candidates; j; j = j->next) {
      NiceCandidateImpl *local_candidate = j->data;

      _priv_set_socket_tos (agent, local_candidate->sockptr, tos);
    }
  }

 done:
  agent_unlock_and_emit (agent);
}

NICEAPI_EXPORT void
nice_agent_set_software (NiceAgent *agent, const gchar *software)
{
  g_return_if_fail (NICE_IS_AGENT (agent));

  agent_lock (agent);

  g_free (agent->software_attribute);
  if (software)
    agent->software_attribute = g_strdup_printf ("%s/%s",
        software, PACKAGE_STRING);
  else
    agent->software_attribute = NULL;

  nice_agent_reset_all_stun_agents (agent, TRUE);

  agent_unlock_and_emit (agent);
}

NICEAPI_EXPORT gboolean
nice_agent_set_stream_name (NiceAgent *agent, guint stream_id,
    const gchar *name)
{
  NiceStream *stream_to_name = NULL;
  GSList *i;
  gboolean ret = FALSE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);
  g_return_val_if_fail (name, FALSE);

  if (strcmp (name, "audio") &&
      strcmp (name, "video") &&
      strcmp (name, "text") &&
      strcmp (name, "application") &&
      strcmp (name, "message") &&
      strcmp (name, "image")) {
    g_critical ("Stream name %s will produce invalid SDP, only \"audio\","
        " \"video\", \"text\", \"application\", \"image\" and \"message\""
        " are valid", name);
  }

  agent_lock (agent);

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;

    if (stream->id != stream_id &&
        g_strcmp0 (stream->name, name) == 0)
      goto done;
    else if (stream->id == stream_id)
      stream_to_name = stream;
  }

  if (stream_to_name == NULL)
    goto done;

  if (stream_to_name->name)
    g_free (stream_to_name->name);
  stream_to_name->name = g_strdup (name);
  ret = TRUE;

 done:
  agent_unlock_and_emit (agent);

  return ret;
}

NICEAPI_EXPORT const gchar *
nice_agent_get_stream_name (NiceAgent *agent, guint stream_id)
{
  NiceStream *stream;
  gchar *name = NULL;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream == NULL)
    goto done;

  name = stream->name;

 done:
  agent_unlock_and_emit (agent);
  return name;
}

static NiceCandidate *
_get_default_local_candidate_locked (NiceAgent *agent,
    NiceStream *stream,  NiceComponent *component)
{
  GSList *i;
  NiceCandidate *default_candidate = NULL;
  NiceCandidate *default_rtp_candidate = NULL;

  if (component->id != NICE_COMPONENT_TYPE_RTP) {
    NiceComponent *rtp_component;

    if (!agent_find_component (agent, stream->id, NICE_COMPONENT_TYPE_RTP,
            NULL, &rtp_component))
      goto done;

    default_rtp_candidate = _get_default_local_candidate_locked (agent, stream,
        rtp_component);
    if (default_rtp_candidate == NULL)
      goto done;
  }


  for (i = component->local_candidates; i; i = i->next) {
    NiceCandidate *local_candidate = i->data;

    if (agent->force_relay &&
        local_candidate->type != NICE_CANDIDATE_TYPE_RELAYED)
      continue;

    /* Only check for ipv4 candidates */
    if (nice_address_ip_version (&local_candidate->addr) != 4)
      continue;
    if (component->id == NICE_COMPONENT_TYPE_RTP) {
      if (default_candidate == NULL ||
          local_candidate->priority < default_candidate->priority) {
        default_candidate = local_candidate;
      }
    } else if (strncmp (local_candidate->foundation,
            default_rtp_candidate->foundation,
            NICE_CANDIDATE_MAX_FOUNDATION) == 0) {
      default_candidate = local_candidate;
      break;
    }
  }

 done:
  return default_candidate;
}

NICEAPI_EXPORT NiceCandidate *
nice_agent_get_default_local_candidate (NiceAgent *agent,
    guint stream_id,  guint component_id)
{
  NiceStream *stream = NULL;
  NiceComponent *component = NULL;
  NiceCandidate *default_candidate = NULL;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (component_id >= 1, NULL);

  agent_lock (agent);

  /* step: check if the component exists*/
  if (!agent_find_component (agent, stream_id, component_id,
          &stream, &component))
    goto done;

  default_candidate = _get_default_local_candidate_locked (agent, stream,
      component);
  if (default_candidate)
    default_candidate = nice_candidate_copy (default_candidate);

 done:
  agent_unlock_and_emit (agent);

  return default_candidate;
}

static const gchar *
_cand_type_to_sdp (NiceCandidateType type) {
  switch(type) {
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      return "srflx";
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      return "prflx";
    case NICE_CANDIDATE_TYPE_RELAYED:
      return "relay";
    case NICE_CANDIDATE_TYPE_HOST:
    default:
      return "host";
  }
}

static const gchar *
_transport_to_sdp (NiceCandidateTransport type) {
  switch(type) {
    case NICE_CANDIDATE_TRANSPORT_UDP:
      return "UDP";
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
      return "TCP";
    default:
      return "???";
  }
}

static const gchar *
_transport_to_sdp_tcptype (NiceCandidateTransport type) {
  switch(type) {
    case NICE_CANDIDATE_TRANSPORT_UDP:
      return "";
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      return "active";
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      return "passive";
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
      return "so";
    default:
      return "";
  }
}

static void
_generate_candidate_sdp (NiceAgent *agent,
    NiceCandidate *candidate, GString *sdp)
{
  gchar ip4[INET6_ADDRSTRLEN];
  guint16 port;

  nice_address_to_string (&candidate->addr, ip4);
  port = nice_address_get_port (&candidate->addr);
  g_string_append_printf (sdp, "a=candidate:%.*s %d %s %d %s %d",
      NICE_CANDIDATE_MAX_FOUNDATION, candidate->foundation,
      candidate->component_id,
      _transport_to_sdp (candidate->transport),
      candidate->priority, ip4, port == 0 ? 9 : port);
  g_string_append_printf (sdp, " typ %s", _cand_type_to_sdp (candidate->type));
  if (nice_address_is_valid (&candidate->base_addr) &&
      !nice_address_equal (&candidate->addr, &candidate->base_addr)) {
    port = nice_address_get_port (&candidate->base_addr);
    nice_address_to_string (&candidate->base_addr, ip4);
    g_string_append_printf (sdp, " raddr %s rport %d", ip4,
        port == 0 ? 9 : port);
  }
  if (candidate->transport != NICE_CANDIDATE_TRANSPORT_UDP) {
    g_string_append_printf (sdp, " tcptype %s",
        _transport_to_sdp_tcptype (candidate->transport));
  }
}

static void
_generate_stream_sdp (NiceAgent *agent, NiceStream *stream,
    GString *sdp, gboolean include_non_ice)
{
  GSList *i, *j;

  if (include_non_ice) {
    NiceAddress rtp, rtcp;
    gchar ip4[INET6_ADDRSTRLEN] = "";

    nice_address_init (&rtp);
    nice_address_set_ipv4 (&rtp, 0);
    nice_address_init (&rtcp);
    nice_address_set_ipv4 (&rtcp, 0);

    /* Find default candidates */
    for (i = stream->components; i; i = i->next) {
      NiceComponent *component = i->data;
      NiceCandidate *default_candidate;

      if (component->id == NICE_COMPONENT_TYPE_RTP) {
        default_candidate = _get_default_local_candidate_locked (agent, stream,
            component);
        if (default_candidate)
          rtp = default_candidate->addr;
      } else if (component->id == NICE_COMPONENT_TYPE_RTCP) {
        default_candidate = _get_default_local_candidate_locked (agent, stream,
            component);
        if (default_candidate)
          rtcp = default_candidate->addr;
      }
    }

    nice_address_to_string (&rtp, ip4);
    g_string_append_printf (sdp, "m=%s %d ICE/SDP\n",
        stream->name ? stream->name : "-", nice_address_get_port (&rtp));
    g_string_append_printf (sdp, "c=IN IP4 %s\n", ip4);
    if (nice_address_get_port (&rtcp) != 0)
      g_string_append_printf (sdp, "a=rtcp:%d\n",
          nice_address_get_port (&rtcp));
  }

  g_string_append_printf (sdp, "a=ice-ufrag:%s\n", stream->local_ufrag);
  g_string_append_printf (sdp, "a=ice-pwd:%s\n", stream->local_password);

  for (i = stream->components; i; i = i->next) {
    NiceComponent *component = i->data;

    for (j = component->local_candidates; j; j = j->next) {
      NiceCandidate *candidate = j->data;

      if (agent->force_relay && candidate->type != NICE_CANDIDATE_TYPE_RELAYED)
        continue;

      _generate_candidate_sdp (agent, candidate, sdp);
      g_string_append (sdp, "\n");
    }
  }
}

NICEAPI_EXPORT gchar *
nice_agent_generate_local_sdp (NiceAgent *agent)
{
  GString *sdp;
  GSList *i;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);

  sdp = g_string_new (NULL);

  agent_lock (agent);

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;

    _generate_stream_sdp (agent, stream, sdp, TRUE);
  }

  agent_unlock_and_emit (agent);

  return g_string_free (sdp, FALSE);
}

NICEAPI_EXPORT gchar *
nice_agent_generate_local_stream_sdp (NiceAgent *agent, guint stream_id,
    gboolean include_non_ice)
{
  GString *sdp = NULL;
  gchar *ret = NULL;
  NiceStream *stream;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream == NULL)
    goto done;

  sdp = g_string_new (NULL);
  _generate_stream_sdp (agent, stream, sdp, include_non_ice);
  ret = g_string_free (sdp, FALSE);

 done:
  agent_unlock_and_emit (agent);

  return ret;
}

NICEAPI_EXPORT gchar *
nice_agent_generate_local_candidate_sdp (NiceAgent *agent,
    NiceCandidate *candidate)
{
  GString *sdp = NULL;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (candidate != NULL, NULL);

  agent_lock (agent);

  sdp = g_string_new (NULL);
  _generate_candidate_sdp (agent, candidate, sdp);

  agent_unlock_and_emit (agent);

  return g_string_free (sdp, FALSE);
}

NICEAPI_EXPORT gint
nice_agent_parse_remote_sdp (NiceAgent *agent, const gchar *sdp)
{
  NiceStream *current_stream = NULL;
  gchar **sdp_lines = NULL;
  GSList *stream_item = NULL;
  gint i;
  gint ret = 0;

  g_return_val_if_fail (NICE_IS_AGENT (agent), -1);
  g_return_val_if_fail (sdp != NULL, -1);

  agent_lock (agent);

  sdp_lines = g_strsplit (sdp, "\n", 0);
  for (i = 0; sdp_lines && sdp_lines[i]; i++) {
    if (g_str_has_prefix (sdp_lines[i], "m=")) {
      if (stream_item == NULL)
        stream_item = agent->streams;
      else
        stream_item = stream_item->next;
      if (!stream_item) {
        g_critical("More streams in SDP than in agent");
        ret = -1;
        goto done;
      }
      current_stream = stream_item->data;
   } else if (g_str_has_prefix (sdp_lines[i], "a=ice-ufrag:")) {
      if (current_stream == NULL) {
        ret = -1;
        goto done;
      }
      g_strlcpy (current_stream->remote_ufrag, sdp_lines[i] + 12,
          NICE_STREAM_MAX_UFRAG);
    } else if (g_str_has_prefix (sdp_lines[i], "a=ice-pwd:")) {
      if (current_stream == NULL) {
        ret = -1;
        goto done;
      }
      g_strlcpy (current_stream->remote_password, sdp_lines[i] + 10,
          NICE_STREAM_MAX_PWD);
    } else if (g_str_has_prefix (sdp_lines[i], "a=candidate:")) {
      NiceCandidate *candidate = NULL;
      NiceComponent *component = NULL;
      GSList *cands = NULL;
      gint added;

      if (current_stream == NULL) {
        ret = -1;
        goto done;
      }
      candidate = nice_agent_parse_remote_candidate_sdp (agent,
          current_stream->id, sdp_lines[i]);
      if (candidate == NULL) {
        ret = -1;
        goto done;
      }

      if (!agent_find_component (agent, candidate->stream_id,
              candidate->component_id, NULL, &component)) {
        nice_candidate_free (candidate);
        ret = -1;
        goto done;
      }
      cands = g_slist_prepend (cands, candidate);
      added = _set_remote_candidates_locked (agent, current_stream,
          component, cands);
      g_slist_free_full(cands, (GDestroyNotify)&nice_candidate_free);
      if (added > 0)
        ret++;
    }
  }

 done:
  if (sdp_lines)
    g_strfreev(sdp_lines);

  agent_unlock_and_emit (agent);

  return ret;
}

NICEAPI_EXPORT GSList *
nice_agent_parse_remote_stream_sdp (NiceAgent *agent, guint stream_id,
    const gchar *sdp, gchar **ufrag, gchar **pwd)
{
  NiceStream *stream = NULL;
  gchar **sdp_lines = NULL;
  GSList *candidates = NULL;
  gint i;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (sdp != NULL, NULL);

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream == NULL) {
    goto done;
  }

  sdp_lines = g_strsplit (sdp, "\n", 0);
  for (i = 0; sdp_lines && sdp_lines[i]; i++) {
    if (ufrag && g_str_has_prefix (sdp_lines[i], "a=ice-ufrag:")) {
      *ufrag = g_strdup (sdp_lines[i] + 12);
    } else if (pwd && g_str_has_prefix (sdp_lines[i], "a=ice-pwd:")) {
      *pwd = g_strdup (sdp_lines[i] + 10);
    } else if (g_str_has_prefix (sdp_lines[i], "a=candidate:")) {
      NiceCandidate *candidate = NULL;

      candidate = nice_agent_parse_remote_candidate_sdp (agent, stream->id,
          sdp_lines[i]);
      if (candidate == NULL) {
        g_slist_free_full(candidates, (GDestroyNotify)&nice_candidate_free);
        candidates = NULL;
        break;
      }
      candidates = g_slist_prepend (candidates, candidate);
    }
  }

 done:
  if (sdp_lines)
    g_strfreev(sdp_lines);

  agent_unlock_and_emit (agent);

  return candidates;
}

NICEAPI_EXPORT NiceCandidate *
nice_agent_parse_remote_candidate_sdp (NiceAgent *agent, guint stream_id,
    const gchar *sdp)
{
  NiceCandidate *candidate = NULL;
  int ntype = -1;
  gchar **tokens = NULL;
  const gchar *foundation = NULL;
  guint component_id = 0;
  const gchar *transport = NULL;
  guint32 priority = 0;
  const gchar *addr = NULL;
  guint16 port = 0;
  const gchar *type = NULL;
  const gchar *tcptype = NULL;
  const gchar *raddr = NULL;
  guint16 rport = 0;
  static const gchar *type_names[] = {"host", "srflx", "prflx", "relay"};
  NiceCandidateTransport ctransport;
  guint i;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (sdp != NULL, NULL);

  if (!g_str_has_prefix (sdp, "a=candidate:"))
    goto done;

  tokens = g_strsplit (sdp + 12, " ", 0);
  for (i = 0; tokens && tokens[i]; i++) {
    switch (i) {
      case 0:
        foundation = tokens[i];
        break;
      case 1:
        component_id = (guint) g_ascii_strtoull (tokens[i], NULL, 10);
        break;
      case 2:
        transport = tokens[i];
        break;
      case 3:
        priority = (guint32) g_ascii_strtoull (tokens[i], NULL, 10);
        break;
      case 4:
        addr = tokens[i];
        break;
      case 5:
        port = (guint16) g_ascii_strtoull (tokens[i], NULL, 10);
        break;
      default:
        if (tokens[i + 1] == NULL)
          goto done;

        if (g_strcmp0 (tokens[i], "typ") == 0) {
          type = tokens[i + 1];
        } else if (g_strcmp0 (tokens[i], "raddr") == 0) {
          raddr = tokens[i + 1];
        } else if (g_strcmp0 (tokens[i], "rport") == 0) {
          rport = (guint16) g_ascii_strtoull (tokens[i + 1], NULL, 10);
        } else if (g_strcmp0 (tokens[i], "tcptype") == 0) {
          tcptype = tokens[i + 1];
        }
        i++;
        break;
    }
  }
  if (type == NULL)
    goto done;

  ntype = -1;
  for (i = 0; i < G_N_ELEMENTS (type_names); i++) {
    if (g_strcmp0 (type, type_names[i]) == 0) {
      ntype = i;
      break;
    }
  }
  if (ntype == -1)
    goto done;

  if (g_ascii_strcasecmp (transport, "UDP") == 0)
    ctransport = NICE_CANDIDATE_TRANSPORT_UDP;
  else if (g_ascii_strcasecmp (transport, "TCP-SO") == 0)
    ctransport = NICE_CANDIDATE_TRANSPORT_TCP_SO;
  else if (g_ascii_strcasecmp (transport, "TCP-ACT") == 0)
    ctransport = NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
  else if (g_ascii_strcasecmp (transport, "TCP-PASS") == 0)
    ctransport = NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
  else if (g_ascii_strcasecmp (transport, "TCP") == 0) {
    if (g_ascii_strcasecmp (tcptype, "so") == 0)
      ctransport = NICE_CANDIDATE_TRANSPORT_TCP_SO;
    else if (g_ascii_strcasecmp (tcptype, "active") == 0)
      ctransport = NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
    else if (g_ascii_strcasecmp (tcptype, "passive") == 0)
      ctransport = NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
    else
      goto done;
  } else
    goto done;

  candidate = nice_candidate_new(ntype);
  candidate->component_id = component_id;
  candidate->stream_id = stream_id;
  candidate->transport = ctransport;
  g_strlcpy(candidate->foundation, foundation, NICE_CANDIDATE_MAX_FOUNDATION);
  candidate->priority = priority;

  if (!nice_address_set_from_string (&candidate->addr, addr)) {
    nice_candidate_free (candidate);
    candidate = NULL;
    goto done;
  }
  nice_address_set_port (&candidate->addr, port);

  if (raddr && rport) {
    if (!nice_address_set_from_string (&candidate->base_addr, raddr)) {
      nice_candidate_free (candidate);
      candidate = NULL;
      goto done;
    }
    nice_address_set_port (&candidate->base_addr, rport);
  }

 done:
  if (tokens)
    g_strfreev(tokens);

  return candidate;
}


NICEAPI_EXPORT GIOStream *
nice_agent_get_io_stream (NiceAgent *agent, guint stream_id,
    guint component_id)
{
  GIOStream *iostream = NULL;
  NiceComponent *component;

  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (component_id >= 1, NULL);

  g_return_val_if_fail (agent->reliable, NULL);

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id, NULL, &component))
    goto done;

  if (component->iostream == NULL)
    component->iostream = nice_io_stream_new (agent, stream_id, component_id);

  iostream = g_object_ref (component->iostream);

 done:
  agent_unlock_and_emit (agent);

  return iostream;
}

NICEAPI_EXPORT gboolean
nice_agent_forget_relays (NiceAgent *agent, guint stream_id, guint component_id)
{
  NiceComponent *component;
  gboolean ret = TRUE;

  g_return_val_if_fail (NICE_IS_AGENT (agent), FALSE);
  g_return_val_if_fail (stream_id >= 1, FALSE);
  g_return_val_if_fail (component_id >= 1, FALSE);

  agent_lock (agent);

  if (!agent_find_component (agent, stream_id, component_id, NULL, &component)) {
    ret = FALSE;
    goto done;
  }

  nice_component_clean_turn_servers (agent, component);

 done:
  agent_unlock_and_emit (agent);

  return ret;
}

/* Helper function to allow us to send connchecks reliably.
 * If the transport is reliable, then we request a reliable send, which will
 * either send the data, or queue it in the case of unestablished http/socks5
 * proxies or tcp-turn. If the transport is not reliable, then it could be an
 * unreliable tcp-bsd, so we still try a reliable send to see if it can succeed
 * meaning the message was queued, or if it failed, then it was either udp-bsd
 * or turn and so we retry with a non reliable send and let the retransmissions
 * take care of the rest.
 * This is in order to avoid having to retransmit something if the underlying
 * socket layer can queue the message and send it once a connection is
 * established.
 */
gssize
agent_socket_send (NiceSocket *sock, const NiceAddress *addr, gsize len,
    const gchar *buf)
{
  if (nice_socket_is_reliable (sock)) {
    guint16 rfc4571_frame = htons (len);
    GOutputVector local_buf[2] = {{&rfc4571_frame, 2}, { buf, len }};
    NiceOutputMessage local_message = { local_buf, 2};
    gint ret;

    /* ICE-TCP requires that all packets be framed with RFC4571 */
    ret = nice_socket_send_messages_reliable (sock, addr, &local_message, 1);
    if (ret == 1)
      return len;
    return ret;
  } else {
    gssize ret = nice_socket_send_reliable (sock, addr, len, buf);
    if (ret < 0)
      ret = nice_socket_send (sock, addr, len, buf);
    return ret;
  }
}

NiceComponentState
nice_agent_get_component_state (NiceAgent *agent,
    guint stream_id, guint component_id)
{
  NiceComponentState state = NICE_COMPONENT_STATE_FAILED;
  NiceComponent *component;

  agent_lock (agent);

  if (agent_find_component (agent, stream_id, component_id, NULL, &component))
    state = component->state;

  agent_unlock (agent);

  return state;
}

gboolean
nice_agent_peer_candidate_gathering_done (NiceAgent *agent, guint stream_id)
{
  NiceStream *stream;
  gboolean result = FALSE;

  agent_lock (agent);

  stream = agent_find_stream (agent, stream_id);
  if (stream) {
    stream->peer_gathering_done = TRUE;
    result = TRUE;
  }

  agent_unlock (agent);

  return result;
}

static gboolean
on_agent_refreshes_pruned (NiceAgent *agent, gpointer user_data)
{
  GTask *task = user_data;

  if (agent->refresh_list) {
    GSource *timeout_source = NULL;
    agent_timeout_add_with_context (agent, &timeout_source,
        "Async refresh prune", agent->stun_initial_timeout,
        on_agent_refreshes_pruned, user_data);
    g_source_unref (timeout_source);
    return G_SOURCE_REMOVE;
  }

  /* This is called from a timeout cb with agent lock held */

  agent_unlock (agent);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);

  agent_lock (agent);

  return G_SOURCE_REMOVE;
}

void
nice_agent_close_async (NiceAgent *agent, GAsyncReadyCallback callback,
    gpointer callback_data)
{
  GTask *task;

  task = g_task_new (agent, NULL, callback, callback_data);
  g_task_set_source_tag (task, nice_agent_close_async);

  /* Hold an extra ref here in case the application releases the last ref
   * during the callback.
   */
  g_object_ref (agent);
  agent_lock (agent);

  g_cancellable_cancel (agent->stun_resolving_cancellable);
  refresh_prune_agent_async (agent, on_agent_refreshes_pruned, task);

  agent_unlock (agent);
  g_object_unref (agent);
}


NICEAPI_EXPORT GPtrArray *
nice_agent_get_sockets (NiceAgent *agent, guint stream_id, guint component_id)
{
  GPtrArray *array = NULL;
  NiceComponent *component;

  agent_lock (agent);
  if (agent_find_component (agent, stream_id, component_id, NULL, &component))
    array = nice_component_get_sockets (component);
  agent_unlock (agent);

  return array;
}

NICEAPI_EXPORT gboolean
nice_agent_consent_lost (
    NiceAgent *agent,
    guint stream_id,
    guint component_id)
{
  gboolean result = FALSE;
  NiceComponent *component;

  agent_lock (agent);
  if (!agent->consent_freshness) {
    g_warning ("Agent %p: Attempt made to signal consent lost for "
        "stream/component %u/%u but RFC7675/consent-freshness is not enabled "
        "for this agent. Ignoring request", agent, stream_id, component_id);
  } else if (agent_find_component (agent, stream_id, component_id, NULL, &component)) {
    nice_debug ("Agent %p: local consent lost for stream/component %u/%u", agent,
        component->stream_id, component->id);
    component->have_local_consent = FALSE;
    result = TRUE;
  }
  agent_unlock_and_emit (agent);

  return result;
}
