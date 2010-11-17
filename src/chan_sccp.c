/*!
 * \file 	chan_sccp.c
 * \brief 	An implementation of Skinny Client Control Protocol (SCCP)
 * \author 	Sergio Chersovani <mlists [at] c-net.it>
 * \brief 	Main chan_sccp Class
 * \note	Reworked, but based on chan_sccp code.
 *        	The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *        	Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 * \remarks	Purpose: 	This source file should be used only for asterisk module related content.
 * 		When to use:	Methods communicating to asterisk about module initialization, status, destruction
 *   		Relationships: 	Main hub for all other sourcefiles.
 *
 * $Date$
 * $Revision$
 */

#define AST_MODULE "chan_sccp"

#include "config.h"
#if ASTERISK_VERSION_NUM >= 10400
#    include <asterisk.h>
#endif
#include "chan_sccp.h"

SCCP_FILE_VERSION(__FILE__, "$Revision$")
#include "sccp_hint.h"
#include "sccp_lock.h"
#include "sccp_actions.h"
#include "sccp_utils.h"
#include "sccp_device.h"
#include "sccp_channel.h"
#include "sccp_cli.h"
#include "sccp_line.h"
#include "sccp_socket.h"
#include "sccp_pbx.h"
#include "sccp_indicate.h"
#include "sccp_config.h"
#include "sccp_management.h"
#include "sccp_mwi.h"
#include "sccp_conference.h"
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <asterisk/pbx.h>
#ifdef CS_AST_HAS_APP_SEPARATE_ARGS
#    include <asterisk/app.h>
#endif
#ifndef CS_AST_HAS_TECH_PVT
#    include <asterisk/channel_pvt.h>
#endif
#include <asterisk/callerid.h>
#include <asterisk/utils.h>
#include <asterisk/causes.h>
#ifdef CS_AST_HAS_NEW_DEVICESTATE
#    include <asterisk/devicestate.h>
#endif
#include <asterisk/translate.h>
#ifdef CS_AST_HAS_AST_STRING_FIELD
#    include <asterisk/stringfields.h>
#endif
#include <asterisk/astdb.h>
#include <asterisk/rtp.h>
#include <asterisk/frame.h>
#include <asterisk/channel.h>
#if ASTERISK_VERSION_NUM >= 10400
#    ifdef CS_AST_HAS_TECH_PVT
#        define SET_CAUSE(x)	*cause = x;
#    else
#        define SET_CAUSE(x)
#    endif
void *sccp_create_hotline(void);

/*!
 * \brief	Buffer for Jitterbuffer use
 */
static struct ast_jb_conf default_jbconf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
#    ifdef CS_AST_JB_TARGET_EXTRA
	.impl = "",
	.target_extra = -1
#    else
	.impl = ""
#    endif
};
#endif

/*!
 * \brief	Global null frame
 */
struct ast_frame sccp_null_frame;						/*!< Asterisk Structure */

/*!
 * \brief	Global variables
 */
struct sccp_global_vars *sccp_globals = 0;

/*!
 * \brief	Global scheduler and IO context
 */
struct sched_context *sched = 0;
struct io_context *io = 0;

#ifdef CS_AST_HAS_TECH_PVT
/*!
 * \brief	handle request coming from asterisk
 * \param	type	type of data as char
 * \param	format	format of data as int
 * \param	data	actual data
 * \param 	cause	Cause of the request
 * \return	Asterisk Channel
 */
struct ast_channel *sccp_request(const char *type, int format, void *data, int *cause)
{
#else
/*!
 * \brief	handle request coming from asterisk
 * \param	type	type of data as char
 * \param	format	format of data as int
 * \param	data	actual data
 * \return	Asterisk Channel
 * 
 * \warning
 * 	- line->devices is not always locked
 */
struct ast_channel *sccp_request(char *type, int format, void *data)
{
#endif

	struct composedId lineSubscriptionId;
	sccp_line_t *l = NULL;
	sccp_channel_t *c = NULL;
	char *options = NULL, *lineName = NULL;
	int optc = 0;
	char *optv[2];
	int opti = 0;
	int oldformat = format;

	memset(&lineSubscriptionId, 0, sizeof(struct composedId));
	SET_CAUSE(AST_CAUSE_NOTDEFINED);

	if (!type) {
		ast_log(LOG_NOTICE, "Attempt to call the wrong type of channel\n");
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		goto OUT;
	}

	if (!data) {
		ast_log(LOG_NOTICE, "Attempt to call SCCP/ failed\n");
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		goto OUT;
	}

	/* we leave the data unchanged */
	lineName = strdup(data);

	if ((options = strchr(lineName, '/'))) {
		*options = '\0';
		options++;
	}

	lineSubscriptionId = sccp_parseComposedId(lineName, 80);

	sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: Asterisk asked to create a channel type=%s, format=%d, line=%s, subscriptionId.number=%s, options=%s\n", type, format, lineSubscriptionId.mainId, lineSubscriptionId.subscriptionId.number, (options) ? options : "");

	l = sccp_line_find_byname(lineSubscriptionId.mainId);

	if (!l) {
		sccp_log(1) (VERBOSE_PREFIX_3 "SCCP/%s does not exist!\n", lineSubscriptionId.mainId);
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		goto OUT;
	}

	sccp_log((DEBUGCAT_SCCP + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_1 "[SCCP] in file %s, line %d (%s)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
	if (SCCP_LIST_FIRST(&l->devices) == NULL) {
		sccp_log((DEBUGCAT_DEVICE | DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "SCCP/%s isn't currently registered anywhere.\n", l->name);
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		goto OUT;
	}

	sccp_log((DEBUGCAT_SCCP + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_1 "[SCCP] in file %s, line %d (%s)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
	/* call forward check */

	// Allocate a new SCCP channel.
	/* on multiline phone we set the line when answering or switching lines */
	c = sccp_channel_allocate(l, NULL);
	if (!c) {
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		goto OUT;
	}

	/* set subscriberId for individual device addressing */
	if (!ast_strlen_zero(lineSubscriptionId.subscriptionId.number)) {
		sccp_copy_string(c->subscriptionId.number, lineSubscriptionId.subscriptionId.number, sizeof(c->subscriptionId.number));
		if (!ast_strlen_zero(lineSubscriptionId.subscriptionId.name)) {
			sccp_copy_string(c->subscriptionId.name, lineSubscriptionId.subscriptionId.name, sizeof(c->subscriptionId.name));
			//ast_log(LOG_NOTICE, "%s: calling subscriber id=%s\n, name=%s", l->id, c->subscriptionId.number,c->subscriptionId.name);
		} else {
			//ast_log(LOG_NOTICE, "%s: calling subscriber id=%s\n", l->id, c->subscriptionId.number);
		}
	} else {
		sccp_copy_string(c->subscriptionId.number, l->defaultSubscriptionId.number, sizeof(c->subscriptionId.number));
		sccp_copy_string(c->subscriptionId.name, l->defaultSubscriptionId.name, sizeof(c->subscriptionId.name));
		//ast_log(LOG_NOTICE, "%s: calling all subscribers\n", l->id);
	}

	if (!sccp_pbx_channel_allocate(c)) {
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		sccp_channel_delete(c);
		c = NULL;
		goto OUT;
	}

#if 0
	sccp_log((DEBUGCAT_DEVICE | DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "Line %s has %d device%s\n", l->name, l->devices.size, (l->devices.size > 1) ? "s" : "");
	if (l->devices.size < 2) {
		if (!c->owner) {
			sccp_log((DEBUGCAT_DEVICE | DEBUGCAT_LINE | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: channel has no owner\n", l->name);
			SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
			sccp_channel_delete(c);
			c = NULL;
			goto OUT;
		}

		sccp_log(1) (VERBOSE_PREFIX_3 "%s: Call forward type: %d\n", l->name, l->cfwd_type);
		if (l->cfwd_type == SCCP_CFWD_ALL) {
			sccp_log(1) (VERBOSE_PREFIX_3 "%s: Call forward (all) to %s\n", l->name, l->cfwd_num);
#    ifdef CS_AST_HAS_AST_STRING_FIELD
			ast_string_field_set(c->owner, call_forward, l->cfwd_num);
#    else
			sccp_copy_string(c->owner->call_forward, l->cfwd_num, sizeof(c->owner->call_forward));
#    endif
		} else if (l->cfwd_type == SCCP_CFWD_BUSY && l->channelCount > 1) {
			sccp_log(1) (VERBOSE_PREFIX_3 "%s: Call forward (busy) to %s\n", l->name, l->cfwd_num);
#    ifdef CS_AST_HAS_AST_STRING_FIELD
			ast_string_field_set(c->owner, call_forward, l->cfwd_num);
#    else
			sccp_copy_string(c->owner->call_forward, l->cfwd_num, sizeof(c->owner->call_forward));
#    endif
		}
	}
#endif

	sccp_log(1) (VERBOSE_PREFIX_1 "[SCCP] in file %s, line %d (%s)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
	/* we have a single device given */

	/*
	   if(c->device){
	   if(c->device->session){
	   hasSession = TRUE;
	   format &= c->device->capability;
	   }
	   }else{
	   sccp_linedevices_t *linedevice;

	   SCCP_LIST_LOCK(&l->devices);
	   SCCP_LIST_TRAVERSE(&l->devices, linedevice, list){
	   if(!linedevice->device)
	   continue;

	   device = linedevice->device;
	   // \todo TODO check capability on shared lines
	   format &= device->capability;
	   if(device->session)
	   hasSession = TRUE;
	   }
	   SCCP_LIST_UNLOCK(&l->devices);
	   } */

	if (l->devices.size == 0) {
		sccp_log(1) (VERBOSE_PREFIX_3 "SCCP/%s we have no registered devices for this line.\n", l->name);
		SET_CAUSE(AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
		goto OUT;
	}
	/*
	   if (!format) {
	   format = oldformat;
	   res = ast_translator_best_choice(&format, &GLOB(global_capability));
	   if (res < 0) {
	   ast_log(LOG_NOTICE, "Asked to get a channel of unsupported format '%d'\n", oldformat);
	   SET_CAUSE(AST_CAUSE_CHANNEL_UNACCEPTABLE);
	   goto OUT;
	   }
	   } */

	c->format = oldformat;
	c->isCodecFix = TRUE;
	sccp_channel_updateChannelCapability(c);

	/* we don't need to parse any options when we have a call forward status */
//      if (c->owner && !ast_strlen_zero(c->owner->call_forward))
//              goto OUT;

	/* check for the channel params */
	if (options && (optc = sccp_app_separate_args(options, '/', optv, sizeof(optv) / sizeof(optv[0])))) {

		for (opti = 0; opti < optc; opti++) {
			if (!strncasecmp(optv[opti], "aa", 2)) {
				/* let's use the old style auto answer aa1w and aa2w */
				if (!strncasecmp(optv[opti], "aa1w", 4)) {
					c->autoanswer_type = SCCP_AUTOANSWER_1W;
					optv[opti] += 4;
				} else if (!strncasecmp(optv[opti], "aa2w", 4)) {
					c->autoanswer_type = SCCP_AUTOANSWER_2W;
					optv[opti] += 4;
				} else if (!strncasecmp(optv[opti], "aa=", 3)) {
					optv[opti] += 3;
					if (!strncasecmp(optv[opti], "1w", 2)) {
						c->autoanswer_type = SCCP_AUTOANSWER_1W;
						optv[opti] += 2;
					} else if (!strncasecmp(optv[opti], "2w", 2)) {
						c->autoanswer_type = SCCP_AUTOANSWER_2W;
						optv[opti] += 2;
					}
				}

				/* since the pbx ignores autoanswer_cause unless channelCount > 1, it is safe to set it if provided */
				if (!ast_strlen_zero(optv[opti]) && (c->autoanswer_type)) {
					if (!strcasecmp(optv[opti], "b"))
						c->autoanswer_cause = AST_CAUSE_BUSY;
					else if (!strcasecmp(optv[opti], "u"))
						c->autoanswer_cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
					else if (!strcasecmp(optv[opti], "c"))
						c->autoanswer_cause = AST_CAUSE_CONGESTION;
				}
#ifdef CS_AST_HAS_TECH_PVT
				if (c->autoanswer_cause)
					*cause = c->autoanswer_cause;
#endif
				/* check for ringer options */
			} else if (!strncasecmp(optv[opti], "ringer=", 7)) {
				optv[opti] += 7;
				if (!strcasecmp(optv[opti], "inside"))
					c->ringermode = SKINNY_STATION_INSIDERING;
				else if (!strcasecmp(optv[opti], "outside"))
					c->ringermode = SKINNY_STATION_OUTSIDERING;
				else if (!strcasecmp(optv[opti], "feature"))
					c->ringermode = SKINNY_STATION_FEATURERING;
				else if (!strcasecmp(optv[opti], "silent"))
					c->ringermode = SKINNY_STATION_SILENTRING;
				else if (!strcasecmp(optv[opti], "urgent"))
					c->ringermode = SKINNY_STATION_URGENTRING;
				else
					c->ringermode = SKINNY_STATION_OUTSIDERING;

			} else {
				ast_log(LOG_WARNING, "%s: Wrong option %s\n", l->id, optv[opti]);
			}
		}

	}

 OUT:
	if (lineName)
		sccp_free(lineName);

	sccp_restart_monitor();
	return (c && c->owner ? c->owner : NULL);
}

/*!
 * \brief returns the state of device
 * \param data name of device
 * \return devicestate of AST_DEVICE_*
 *
 * \warning
 * 	- line->devices is not always locked
 */
int sccp_devicestate(void *data)
{
	sccp_line_t *l = NULL;
	int res = AST_DEVICE_UNKNOWN;
	char *lineName = (char *)data, *options = NULL;

	/* exclude options */
	if ((options = strchr(lineName, '/'))) {
		*options = '\0';
		options++;
	}

	l = sccp_line_find_byname(lineName);
	if (!l)
		res = AST_DEVICE_INVALID;
	else if (SCCP_LIST_FIRST(&l->devices) == NULL)
		res = AST_DEVICE_UNAVAILABLE;

	// \todo TODO handle dnd on device
//      else if ((l->device->dnd && l->device->dndmode == SCCP_DNDMODE_REJECT)
//                      || (l->dnd && (l->dndmode == SCCP_DNDMODE_REJECT
//                                      || (l->dndmode == SCCP_DNDMODE_USERDEFINED && l->dnd == SCCP_DNDMODE_REJECT) )) )
//              res = AST_DEVICE_BUSY;
	else if (l->incominglimit && l->channelCount == l->incominglimit)
		res = AST_DEVICE_BUSY;
	else if (!l->channelCount)
		res = AST_DEVICE_NOT_INUSE;
#ifdef CS_AST_DEVICE_RINGING
	else if (sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_RINGING))
#    ifdef CS_AST_DEVICE_RINGINUSE
		if (sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_CONNECTED))
			res = AST_DEVICE_RINGINUSE;
		else
#    endif
			res = AST_DEVICE_RINGING;
#endif
#ifdef CS_AST_DEVICE_ONHOLD
	else if (sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_HOLD))
		res = AST_DEVICE_ONHOLD;
#endif
	else
		res = AST_DEVICE_INUSE;

	sccp_log((DEBUGCAT_DEVICE | DEBUGCAT_LINE | DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "SCCP: Asterisk asked for the state (%d) of the line %s\n", res, (char *)data);

	return res;
}

/*!
 * \brief 	Controller function to handle Received Messages
 * \param 	r Message as sccp_moo_t
 * \param 	s Session as sccp_session_t
 */
uint8_t sccp_handle_message(sccp_moo_t * r, sccp_session_t * s)
{
	if (!s) {
		ast_log(LOG_ERROR, "%s: (sccp_handle_message) Client does not have a sessions, Required !\n", s->device->id ? s->device->id : "SCCP");
		ast_free(r);
		return -1;
	}

	if (!r) {
		ast_log(LOG_ERROR, "%s: (sccp_handle_message) No Message Specified.\n, Required !", s->device->id ? s->device->id : "SCCP");
		ast_free(r);
		return 0;
	}
	uint32_t mid = letohl(r->lel_messageId);
	//sccp_log(1)(VERBOSE_PREFIX_3 "%s: last keepAlive within %d (%d)\n", (s->device)?s->device->id:"null", (uint32_t)(time(0) - s->lastKeepAlive), (s->device)?s->device->keepalive:0 );

	s->lastKeepAlive = time(0);						/* always update keepalive */

	/* Check if all necessary information is available */
	if ((!s->device) && (mid != RegisterMessage && mid != UnregisterMessage && mid != RegisterTokenReq && mid != AlarmMessage && mid != KeepAliveMessage && mid != IpPortMessage)) {
		ast_log(LOG_WARNING, "SCCP: Client sent %s without first registering. Attempting reconnect.\n", message2str(mid));
	} else if (s->device) {
		if (s->device != sccp_device_find_byipaddress(s->sin.sin_addr.s_addr)) {
			// IP Address has changed mid session
			if (s->device->nat == 1) {
				// We are natted, what should we do, Not doing anything for now, just sending warning -- DdG
				ast_log(LOG_WARNING, "%s: Device (%s) attempted to send messages via a different ip-address (%s).\n", DEV_ID_LOG(s->device), sccp_inet_ntoa(s->sin.sin_addr), sccp_inet_ntoa(s->device->session->sin.sin_addr));
				// \todo write auto recover ip-address change during session with natted device should be be implemented
				/*
				   s->device->session->sin.sin_addr.s_addr = s->sin.sin_addr.s_addr;
				   s->device->nat = 1;
				   sccp_device_reset(s->device, r);
				   sccp_session_unlock(s);
				   sccp_session_close(s,5);
				   sccp_session_close(s->device->session,5);
				   destroy_session(s,5);
				   destroy_session(s->device->session,5);
				   sccp_session_lock(s);
				 */
			} else {
				// We are not natted, but the ip-address has changed
				ast_log(LOG_ERROR, "(sccp_handle_message): SCCP: Device is attempting to send message via a different ip-address.\nIf this is behind a firewall please set it up in sccp.conf with nat=1.\n");
				ast_free(r);
				return 0;
			}
		} else if (s->device && (!s->device->session || s->device->session != s)) {
			sccp_log(1) (VERBOSE_PREFIX_3 "%s: cross device session (Removing Old Session)\n", DEV_ID_LOG(s->device));
			sccp_session_close(s->device->session);
			destroy_session(s->device->session, 2);
			ast_free(r);
			return 0;
		}
	}

	if (mid != KeepAliveMessage) {
		if (s && s->device) {
			sccp_log((DEBUGCAT_MESSAGE)) (VERBOSE_PREFIX_3 "%s: >> Got message %s\n", s->device->id, message2str(mid));
		} else {
			sccp_log((DEBUGCAT_MESSAGE)) (VERBOSE_PREFIX_3 "SCCP: >> Got message %s\n", message2str(mid));
		}
	}

	switch (mid) {
	case AlarmMessage:
		sccp_handle_alarm(s, r);
		break;
	case RegisterMessage:
	case RegisterTokenReq:
		sccp_handle_register(s, r);
		break;
	case UnregisterMessage:
		sccp_handle_unregister(s, r);
		break;
	case KeepAliveMessage:
		sccp_session_sendmsg(s->device, KeepAliveAckMessage);
		break;
	case IpPortMessage:
		/* obsolete message */
		s->rtpPort = letohs(r->msg.IpPortMessage.les_rtpMediaPort);
		break;
	case VersionReqMessage:
		sccp_handle_version(s, r);
		break;
	case CapabilitiesResMessage:
		sccp_handle_capabilities_res(s, r);
		break;
	case ButtonTemplateReqMessage:
		sccp_handle_button_template_req(s, r);
		break;
	case SoftKeyTemplateReqMessage:
		sccp_handle_soft_key_template_req(s, r);
		break;
	case SoftKeySetReqMessage:
		sccp_handle_soft_key_set_req(s, r);
		break;
	case LineStatReqMessage:
		sccp_handle_line_number(s, r);
		break;
	case SpeedDialStatReqMessage:
		sccp_handle_speed_dial_stat_req(s, r);
		break;
	case StimulusMessage:
		sccp_handle_stimulus(s, r);
		break;
	case OffHookMessage:
		sccp_handle_offhook(s, r);
		break;
	case OnHookMessage:
		sccp_handle_onhook(s, r);
		break;
	case HeadsetStatusMessage:
		sccp_handle_headset(s, r);
		break;
	case TimeDateReqMessage:
		sccp_handle_time_date_req(s, r);
		break;
	case KeypadButtonMessage:
		sccp_handle_keypad_button(s, r);
		break;
	case SoftKeyEventMessage:
		sccp_handle_soft_key_event(s, r);
		break;
	case OpenReceiveChannelAck:
		sccp_handle_open_receive_channel_ack(s, r);
		break;
	case OpenMultiMediaReceiveChannelAckMessage:
		sccp_handle_OpenMultiMediaReceiveAck(s, r);
		break;
	case ConnectionStatisticsRes:
		sccp_handle_ConnectionStatistics(s, r);
		break;
	case ServerReqMessage:
		sccp_handle_ServerResMessage(s, r);
		break;
	case ConfigStatReqMessage:
		sccp_handle_ConfigStatMessage(s, r);
		break;
	case EnblocCallMessage:
		sccp_handle_EnblocCallMessage(s, r);
		break;
	case RegisterAvailableLinesMessage:
		sccp_handle_AvailableLines(s->device);
		break;
	case ForwardStatReqMessage:
		sccp_handle_forward_stat_req(s, r);
		break;
	case FeatureStatReqMessage:
		sccp_handle_feature_stat_req(s, r);
		break;
	case ServiceURLStatReqMessage:
		sccp_handle_services_stat_req(s, r);
		break;
	case AccessoryStatusMessage:
		sccp_handle_accessorystatus_message(s, r);
		break;
	case DialedPhoneBookMessage:
		sccp_handle_dialedphonebook_message(s, r);
		break;
	case UpdateCapabilitiesMessage:
		sccp_handle_updatecapabilities_message(s, r);
		break;
	case StartMediaTransmissionAck:
		sccp_handle_startmediatransmission_ack(s, r);
		break;
	case Unknown_0x004A_Message:
		if ((GLOB(debug) & DEBUGCAT_MESSAGE) == DEBUGCAT_MESSAGE) {
			sccp_handle_unknown_message(s, r);
		}
		break;
	case Unknown_0x0143_Message:
		if ((GLOB(debug) & DEBUGCAT_MESSAGE) == DEBUGCAT_MESSAGE) {
			sccp_handle_unknown_message(s, r);
		}
		break;
	case Unknown_0x0144_Message:
		if ((GLOB(debug) & DEBUGCAT_MESSAGE) == DEBUGCAT_MESSAGE) {
			sccp_handle_unknown_message(s, r);
		}
		break;
	case SpeedDialStatDynamicMessage:
		sccp_handle_speed_dial_stat_req(s, r);
		break;
	case ExtensionDeviceCaps:
		if ((GLOB(debug) & DEBUGCAT_MESSAGE) == DEBUGCAT_MESSAGE) {
			sccp_handle_unknown_message(s, r);
		}
		break;
	default:
		sccp_handle_unknown_message(s, r);
	}

	ast_free(r);
	return 1;
}

/**
 * \brief load the configuration from sccp.conf
 *
 */
static int load_config(void)
{
	int oldport = ntohs(GLOB(bindaddr.sin_port));
	int on = 1;
#if ASTERISK_VERSION_NUM < 10400
	char iabuf[INET_ADDRSTRLEN];
#endif

#if ASTERISK_VERSION_NUM >= 10400
	/* Copy the default jb config over global_jbconf */
	memcpy(&GLOB(global_jbconf), &default_jbconf, sizeof(struct ast_jb_conf));

	/* Setup the monitor thread default */
	GLOB(monitor_thread) = AST_PTHREADT_NULL;				// ADDED IN SVN 414 -FS
	GLOB(mwiMonitorThread) = AST_PTHREADT_NULL;
#endif

	memset(&GLOB(global_codecs), 0, sizeof(GLOB(global_codecs)));
	memset(&GLOB(bindaddr), 0, sizeof(GLOB(bindaddr)));
	GLOB(allowAnonymus) = TRUE;

#ifdef CS_SCCP_REALTIME
	sccp_copy_string(GLOB(realtimedevicetable), "sccpdevice", sizeof(GLOB(realtimedevicetable)));
	sccp_copy_string(GLOB(realtimelinetable), "sccpline", sizeof(GLOB(realtimelinetable)));
#endif

#if SCCP_PLATFORM_BYTE_ORDER == SCCP_LITTLE_ENDIAN
	sccp_log(0) (VERBOSE_PREFIX_2 "Platform byte order   : LITTLE ENDIAN\n");
#else
	sccp_log(0) (VERBOSE_PREFIX_2 "Platform byte order   : BIG ENDIAN\n");
#endif

	if (!sccp_config_general(SCCP_CONFIG_READINITIAL)) {
		return 0;
	}
	sccp_config_readDevicesLines(SCCP_CONFIG_READINITIAL);
	/* ok the config parse is done */
	if ((GLOB(descriptor) > -1) && (ntohs(GLOB(bindaddr.sin_port)) != oldport)) {
		close(GLOB(descriptor));
		GLOB(descriptor) = -1;
	}

	if (GLOB(descriptor) < 0) {
		GLOB(descriptor) = socket(AF_INET, SOCK_STREAM, 0);

		on = 1;
		if (setsockopt(GLOB(descriptor), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
			ast_log(LOG_WARNING, "Failed to set SCCP socket to SO_REUSEADDR mode: %s\n", strerror(errno));
		if (setsockopt(GLOB(descriptor), IPPROTO_IP, IP_TOS, &GLOB(sccp_tos), sizeof(GLOB(sccp_tos))) < 0)
			ast_log(LOG_WARNING, "Failed to set SCCP socket TOS to %d: %s\n", GLOB(sccp_tos), strerror(errno));
		else if (GLOB(sccp_tos))
			sccp_log(DEBUGCAT_SOCKET) (VERBOSE_PREFIX_1 "Using SCCP Socket ToS mark %d\n", GLOB(sccp_tos));
		if (setsockopt(GLOB(descriptor), IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
			ast_log(LOG_WARNING, "Failed to set SCCP socket to TCP_NODELAY: %s\n", strerror(errno));
#if defined(linux)
		if (setsockopt(GLOB(descriptor), SOL_SOCKET, SO_PRIORITY, &GLOB(sccp_cos), sizeof(GLOB(sccp_cos))) < 0)
			ast_log(LOG_WARNING, "Failed to set SCCP socket COS to %d: %s\n", GLOB(sccp_cos), strerror(errno));
		else if (GLOB(sccp_cos))
			sccp_log(DEBUGCAT_SOCKET) (VERBOSE_PREFIX_1 "Using SCCP Socket CoS mark %d\n", GLOB(sccp_cos));
#endif

		if (GLOB(descriptor) < 0) {
			ast_log(LOG_WARNING, "Unable to create SCCP socket: %s\n", strerror(errno));
		} else {
			if (bind(GLOB(descriptor), (struct sockaddr *)&GLOB(bindaddr), sizeof(GLOB(bindaddr))) < 0) {
#if ASTERISK_VERSION_NUM < 10400
				ast_log(LOG_WARNING, "Failed to bind to %s:%d: %s!\n", ast_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)), strerror(errno));
#else
				ast_log(LOG_WARNING, "Failed to bind to %s:%d: %s!\n", ast_inet_ntoa(GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)), strerror(errno));
#endif
				close(GLOB(descriptor));
				GLOB(descriptor) = -1;
				return 0;
			}
#if ASTERISK_VERSION_NUM < 10400
			ast_verbose(VERBOSE_PREFIX_3 "SCCP channel driver up and running on %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));
#else
			ast_verbose(VERBOSE_PREFIX_3 "SCCP channel driver up and running on %s:%d\n", ast_inet_ntoa(GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));
#endif

			if (listen(GLOB(descriptor), DEFAULT_SCCP_BACKLOG)) {
#if ASTERISK_VERSION_NUM < 10400
				ast_log(LOG_WARNING, "Failed to start listening to %s:%d: %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)), strerror(errno));
#else
				ast_log(LOG_WARNING, "Failed to start listening to %s:%d: %s\n", ast_inet_ntoa(GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)), strerror(errno));
#endif
				close(GLOB(descriptor));
				GLOB(descriptor) = -1;
				return 0;
			}
#if ASTERISK_VERSION_NUM < 10400
			sccp_log(0) (VERBOSE_PREFIX_3 "SCCP listening on %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));
#else
			sccp_log(0) (VERBOSE_PREFIX_3 "SCCP listening on %s:%d\n", ast_inet_ntoa(GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));
#endif
			GLOB(reload_in_progress) = FALSE;
			ast_pthread_create(&GLOB(socket_thread), NULL, sccp_socket_thread, NULL);

		}
	}

	sccp_restart_monitor();

	return 0;
}

/*!
 * \brief 	create a hotline
 * 
 * \lock
 * 	- lines
 */
void *sccp_create_hotline(void)
{
	sccp_line_t *hotline;

	hotline = sccp_line_create();
#ifdef CS_SCCP_REALTIME
	hotline->realtime = TRUE;
#endif
	sccp_copy_string(hotline->name, "Hotline", sizeof(hotline->name));
	sccp_copy_string(hotline->cid_name, "hotline", sizeof(hotline->cid_name));
	sccp_copy_string(hotline->cid_num, "hotline", sizeof(hotline->cid_name));
	sccp_copy_string(hotline->context, "default", sizeof(hotline->context));
	sccp_copy_string(hotline->label, "hotline", sizeof(hotline->label));
	sccp_copy_string(hotline->adhocNumber, "111", sizeof(hotline->adhocNumber));

	//sccp_copy_string(hotline->mailbox, "hotline", sizeof(hotline->mailbox));

	SCCP_LIST_LOCK(&GLOB(lines));
	SCCP_LIST_INSERT_HEAD(&GLOB(lines), hotline, list);
	SCCP_LIST_UNLOCK(&GLOB(lines));

	GLOB(hotline)->line = hotline;
	sccp_copy_string(GLOB(hotline)->exten, "111", sizeof(GLOB(hotline)->exten));

	return NULL;
}

/*!
 * \brief 	start monitoring thread of chan_sccp
 * \param 	data
 * 
 * \lock
 * 	- monitor_lock
 */
void *sccp_do_monitor(void *data)
{
	int res;

	/* This thread monitors all the interfaces which are not yet in use
	   (and thus do not have a separate thread) indefinitely */
	/* From here on out, we die whenever asked */
	for (;;) {
		pthread_testcancel();
		/* Wait for sched or io */
		res = ast_sched_wait(sched);
		if ((res < 0) || (res > 1000)) {
			res = 1000;
		}
		res = ast_io_wait(io, res);
		ast_mutex_lock(&GLOB(monitor_lock));
		if (res >= 0) {
			ast_sched_runq(sched);
		}
		ast_mutex_unlock(&GLOB(monitor_lock));
	}
	/* Never reached */
	return NULL;

}

/*!
 * \brief 	restart the monitoring thread of chan_sccp
 * \return	Success as int
 * 
 * \lock
 * 	- monitor_lock
 */
int sccp_restart_monitor(void)
{
	/* If we're supposed to be stopped -- stay stopped */
	if (GLOB(monitor_thread) == AST_PTHREADT_STOP)
		return 0;

	ast_mutex_lock(&GLOB(monitor_lock));
	if (GLOB(monitor_thread) == pthread_self()) {
		ast_mutex_unlock(&GLOB(monitor_lock));
		sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_restart_monitor) Cannot kill myself\n");
		return -1;
	}
	if (GLOB(monitor_thread) != AST_PTHREADT_NULL) {
		/* Wake up the thread */
		pthread_kill(GLOB(monitor_thread), SIGURG);
	} else {
		/* Start a new monitor */
		if (ast_pthread_create_background(&GLOB(monitor_thread), NULL, sccp_do_monitor, NULL) < 0) {
			ast_mutex_unlock(&GLOB(monitor_lock));
			sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_restart_monitor) Unable to start monitor thread.\n");
			return -1;
		}
	}
	ast_mutex_unlock(&GLOB(monitor_lock));
	return 0;
}

/*!
 * \brief  ${SCCPDEVICE()} Dialplan function - reads device data 
 * \param chan Asterisk Channel
 * \param cmd Command as char
 * \param data Extra data as char
 * \param buf Buffer as chan*
 * \param len Lenght as size_t
 * \return Status as int
 *
 * \author Diederik de Groot <ddegroot@users.sourceforce.net>
 * \ref nf_sccp_dialplan_sccpdevice
 * 
 * \lock
 * 	- device
 * 	  - device->buttonconfig
 */
#if ASTERISK_VERSION_NUM >= 10600
static int sccp_func_sccpdevice(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
#else
static int sccp_func_sccpdevice(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
#endif
{
	sccp_device_t *d;
	char *colname;
	char tmp[1024] = "";
	char lbuf[1024] = "";
	int first = 0;

	if ((colname = strchr(data, ':'))) {					/*! \todo Will be deprecated after 1.4 */
		static int deprecation_warning = 0;
		*colname++ = '\0';
		if (deprecation_warning++ % 10 == 0)
			ast_log(LOG_WARNING, "SCCPDEVICE(): usage of ':' to separate arguments is deprecated.  Please use ',' instead.\n");
	} else if ((colname = strchr(data, ',')))
		*colname++ = '\0';
	else
		colname = "ip";

	if (!strncasecmp(data, "current", 7)) {
		sccp_channel_t *c;
		if (!(c = get_sccp_channel_from_ast_channel(chan))) {
/*			ast_log(LOG_WARNING, "SCCPDEVICE(): Not an SCCP channel\n");*/
			return -1;
		}

		if (!c || !c->device) {
			ast_log(LOG_WARNING, "SCCPDEVICE(): SCCP Device not available\n");
			return -1;
		}

		d = c->device;

	} else {
		if (!(d = sccp_device_find_byid(data, TRUE))) {
			ast_log(LOG_WARNING, "SCCPDEVICE(): SCCP Device not available\n");
			return -1;
		}
	}

	sccp_device_lock(d);
	if (!strcasecmp(colname, "ip")) {
		sccp_session_t *s = d->session;
		if (s) {
#if ASTERISK_VERSION_NUM < 10400
			ast_copy_string(buf, s->sin.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr) : "", len);
#else
			ast_copy_string(buf, s->sin.sin_addr.s_addr ? ast_inet_ntoa(s->sin.sin_addr) : "", len);
#endif
		}
	} else if (!strcasecmp(colname, "id")) {
		ast_copy_string(buf, d->id, len);
	} else if (!strcasecmp(colname, "status")) {
		ast_copy_string(buf, devicestatus2str(d->state), len);
	} else if (!strcasecmp(colname, "description")) {
		ast_copy_string(buf, d->description, len);
	} else if (!strcasecmp(colname, "config_type")) {
		ast_copy_string(buf, d->config_type, len);
	} else if (!strcasecmp(colname, "skinny_type")) {
		ast_copy_string(buf, devicetype2str(d->skinny_type), len);
	} else if (!strcasecmp(colname, "tz_offset")) {
		snprintf(buf, len, "%d", d->tz_offset);
	} else if (!strcasecmp(colname, "image_version")) {
		ast_copy_string(buf, d->imageversion, len);
	} else if (!strcasecmp(colname, "accessory_status")) {
		ast_copy_string(buf, accessorystatus2str(d->accessorystatus), len);
	} else if (!strcasecmp(colname, "registration_state")) {
		ast_copy_string(buf, deviceregistrationstatus2str(d->registrationState), len);
	} else if (!strcasecmp(colname, "codecs")) {
		ast_codec_pref_string(&d->codecs, buf, sizeof(buf) - 1);
	} else if (!strcasecmp(colname, "capability")) {
		ast_getformatname_multiple(buf, len - 1, d->capability);
	} else if (!strcasecmp(colname, "state")) {
		ast_copy_string(buf, accessorystatus2str(d->accessorystatus), len);
	} else if (!strcasecmp(colname, "lines_registered")) {
		ast_copy_string(buf, d->linesRegistered ? "yes" : "no", len);
	} else if (!strcasecmp(colname, "lines_count")) {
		snprintf(buf, len, "%d", d->linesCount);
	} else if (!strcasecmp(colname, "last_number")) {
		ast_copy_string(buf, d->lastNumber, len);
	} else if (!strcasecmp(colname, "capability")) {
		snprintf(buf, len, "%d", d->capability);
	} else if (!strcasecmp(colname, "early_rtp")) {
		snprintf(buf, len, "%d", d->earlyrtp);
	} else if (!strcasecmp(colname, "channel_count")) {
		snprintf(buf, len, "%d", d->channelCount);
	} else if (!strcasecmp(colname, "supported_protocol_version")) {
		snprintf(buf, len, "%d", d->protocolversion);
	} else if (!strcasecmp(colname, "used_protocol_version")) {
		snprintf(buf, len, "%d", d->inuseprotocolversion);
	} else if (!strcasecmp(colname, "mwi_light")) {
		ast_copy_string(buf, d->mwilight ? "ON" : "OFF", len);
	} else if (!strcasecmp(colname, "dynamic") || !strcasecmp(colname, "realtime")) {
#ifdef CS_SCCP_REALTIME
		ast_copy_string(buf, d->realtime ? "yes" : "no", len);
#else
		ast_copy_string(buf, "not supported", len);
#endif
	} else if (!strcasecmp(colname, "active_channel")) {
		snprintf(buf, len, "%d", d->active_channel->callid);
	} else if (!strcasecmp(colname, "transfer_channel")) {
		snprintf(buf, len, "%d", d->transfer_channel->callid);
	} else if (!strcasecmp(colname, "conference_channel")) {
		snprintf(buf, len, "%d", d->conference_channel->callid);
	} else if (!strcasecmp(colname, "current_line")) {
		ast_copy_string(buf, d->currentLine->id, len);
	} else if (!strcasecmp(colname, "button_config")) {
		sccp_buttonconfig_t *config;
		SCCP_LIST_LOCK(&d->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			switch (config->type) {
			case LINE:
				snprintf(tmp, sizeof(tmp), "[%d,%s,%s]", config->instance, sccp_buttontype2str(config->type), config->button.line.name);
				break;
			case SPEEDDIAL:
				snprintf(tmp, sizeof(tmp), "[%d,%s,%s,%s]", config->instance, sccp_buttontype2str(config->type), config->button.speeddial.label, config->button.speeddial.ext);
				break;
			case SERVICE:
				snprintf(tmp, sizeof(tmp), "[%d,%s,%s,%s]", config->instance, sccp_buttontype2str(config->type), config->button.service.label, config->button.service.url);
				break;
			case FEATURE:
				snprintf(tmp, sizeof(tmp), "[%d,%s,%s,%s]", config->instance, sccp_buttontype2str(config->type), config->button.feature.label, config->button.feature.options);
				break;
			case EMPTY:
				snprintf(tmp, sizeof(tmp), "[%d,%s]", config->instance, sccp_buttontype2str(config->type));
				break;
			}
			if (first == 0) {
				first = 1;
				strcat(lbuf, tmp);
			} else {
				strcat(lbuf, ",");
				strcat(lbuf, tmp);
			}
		}
		SCCP_LIST_UNLOCK(&d->buttonconfig);
		snprintf(buf, len, "[ %s ]", lbuf);
	} else if (!strcasecmp(colname, "pending_delete")) {
#ifdef CS_DYNAMIC_CONFIG
		ast_copy_string(buf, d->pendingDelete ? "yes" : "no", len);
#else
		ast_copy_string(buf, "not supported", len);
#endif
	} else if (!strcasecmp(colname, "pending_update")) {
#ifdef CS_DYNAMIC_CONFIG
		ast_copy_string(buf, d->pendingUpdate ? "yes" : "no", len);
#else
		ast_copy_string(buf, "not supported", len);
#endif
	} else if (!strncasecmp(colname, "chanvar[", 8)) {
		char *chanvar = colname + 8;
		struct ast_variable *v;

		chanvar = strsep(&chanvar, "]");
		for (v = d->variables; v; v = v->next) {
			if (!strcasecmp(v->name, chanvar)) {
				ast_copy_string(buf, v->value, len);
			}
		}
	} else if (!strncasecmp(colname, "codec[", 6)) {
		char *codecnum;
		int codec = 0;

		codecnum = colname + 6;						// move past the '[' 
		codecnum = strsep(&codecnum, "]");				// trim trailing ']' if any 
		if ((codec = ast_codec_pref_index(&d->codecs, atoi(codecnum)))) {
			ast_copy_string(buf, ast_getformatname(codec), len);
		} else {
			buf[0] = '\0';
		}
	} else {
		ast_log(LOG_WARNING, "SCCPDEVICE(): unknown function option: %s", data);
		buf[0] = '\0';
	}
	sccp_device_unlock(d);
	return 0;
}

/*! \brief Stucture to declare a dialplan function: SCCPDEVICE */
static struct ast_custom_function sccpdevice_function = {
	.name = "SCCPDEVICE",
	.synopsis = "Retrieves information about an SCCP Device",
	.syntax = "Usage: SCCPDEVICE(deviceId,<option>)",
	.read = sccp_func_sccpdevice,
};

/*!
 * \brief  ${SCCPLINE()} Dialplan function - reads sccp line data 
 * \param chan Asterisk Channel
 * \param cmd Command as char
 * \param data Extra data as char
 * \param buf Buffer as chan*
 * \param len Lenght as size_t
 * \return Status as int
 *
 * \author Diederik de Groot <ddegroot@users.sourceforce.net>
 * \ref nf_sccp_dialplan_sccpline
 * 
 * \lock
 * 	- line
 * 	  - line->devices
 */
#if ASTERISK_VERSION_NUM >= 10600
static int sccp_func_sccpline(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
#else
static int sccp_func_sccpline(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
#endif
{
	sccp_line_t *l;
	sccp_channel_t *c;
	char *colname;
	char tmp[1024] = "";
	char lbuf[1024] = "";
	int first = 0;

	if ((colname = strchr(data, ':'))) {					/*! \todo Will be deprecated after 1.4 */
		static int deprecation_warning = 0;
		*colname++ = '\0';
		if (deprecation_warning++ % 10 == 0)
			ast_log(LOG_WARNING, "SCCPLINE(): usage of ':' to separate arguments is deprecated.  Please use ',' instead.\n");
	} else if ((colname = strchr(data, ',')))
		*colname++ = '\0';
	else
		colname = "id";

	if (!strncasecmp(data, "current", 7)) {
		if (!(c = get_sccp_channel_from_ast_channel(chan))) {
/*			ast_log(LOG_WARNING, "SCCPLINE(): Not an SCCP Channel\n");*/
			return -1;
		}

		if (!c || !c->line) {
			ast_log(LOG_WARNING, "SCCPLINE(): SCCP Line not available\n");
			return -1;
		}
		l = c->line;
	} else if (!strncasecmp(data, "parent", 7)) {
		if (!(c = get_sccp_channel_from_ast_channel(chan))) {
/*			ast_log(LOG_WARNING, "SCCPLINE(): Not an SCCP Channel\n");*/
			return -1;
		}

		if (!c || !c->parentChannel || !c->parentChannel->line) {
			ast_log(LOG_WARNING, "SCCPLINE(): SCCP Line not available\n");
			return -1;
		}
		l = c->parentChannel->line;
	} else {
		if (!(l = sccp_line_find_byname_wo(data, TRUE))) {
			ast_log(LOG_WARNING, "sccp_func_sccpdevice: SCCP Line not available\n");
			return -1;
		}
	}
	sccp_line_lock(l);

	if (!strcasecmp(colname, "id")) {
		ast_copy_string(buf, l->id, len);
	} else if (!strcasecmp(colname, "name")) {
		ast_copy_string(buf, l->name, len);
	} else if (!strcasecmp(colname, "description")) {
		ast_copy_string(buf, l->description, len);
	} else if (!strcasecmp(colname, "label")) {
		ast_copy_string(buf, l->label, len);
	} else if (!strcasecmp(colname, "vmnum")) {
		ast_copy_string(buf, l->vmnum, len);
	} else if (!strcasecmp(colname, "trnsfvm")) {
		ast_copy_string(buf, l->trnsfvm, len);
	} else if (!strcasecmp(colname, "meetme")) {
		ast_copy_string(buf, l->meetme ? "on" : "off", len);
	} else if (!strcasecmp(colname, "meetmenum")) {
		ast_copy_string(buf, l->meetmenum, len);
	} else if (!strcasecmp(colname, "meetmeopts")) {
		ast_copy_string(buf, l->meetmeopts, len);
	} else if (!strcasecmp(colname, "context")) {
		ast_copy_string(buf, l->context, len);
	} else if (!strcasecmp(colname, "language")) {
		ast_copy_string(buf, l->language, len);
	} else if (!strcasecmp(colname, "accountcode")) {
		ast_copy_string(buf, l->accountcode, len);
	} else if (!strcasecmp(colname, "musicclass")) {
		ast_copy_string(buf, l->musicclass, len);
	} else if (!strcasecmp(colname, "amaflags")) {
		ast_copy_string(buf, l->amaflags ? "yes" : "no", len);
	} else if (!strcasecmp(colname, "callgroup")) {
		ast_print_group(buf, len, l->callgroup);
	} else if (!strcasecmp(colname, "pickupgroup")) {
#ifdef CS_SCCP_PICKUP
		ast_print_group(buf, len, l->pickupgroup);
#else
		ast_copy_string(buf, "not supported", len);
#endif
	} else if (!strcasecmp(colname, "cid_name")) {
		ast_copy_string(buf, l->cid_name ? l->cid_name : "<not set>", len);
	} else if (!strcasecmp(colname, "cid_num")) {
		ast_copy_string(buf, l->cid_num ? l->cid_num : "<not set>", len);
	} else if (!strcasecmp(colname, "incoming_limit")) {
		snprintf(buf, len, "%d", l->incominglimit);
	} else if (!strcasecmp(colname, "channel_count")) {
		snprintf(buf, len, "%d", l->channelCount);
	} else if (!strcasecmp(colname, "dynamic") || !strcasecmp(colname, "realtime")) {
#ifdef CS_SCCP_REALTIME
		ast_copy_string(buf, l->realtime ? "Yes" : "No", len);
#else
		ast_copy_string(buf, "not supported", len);
#endif
	} else if (!strcasecmp(colname, "pending_delete")) {

#ifdef CS_DYNAMIC_CONFIG
		ast_copy_string(buf, l->pendingDelete ? "yes" : "no", len);
#else
		ast_copy_string(buf, "not supported", len);
#endif
	} else if (!strcasecmp(colname, "pending_update")) {

#ifdef CS_DYNAMIC_CONFIG
		ast_copy_string(buf, l->pendingUpdate ? "yes" : "no", len);
#else
		ast_copy_string(buf, "not supported", len);
#endif

		/* regexten feature -- */
	} else if (!strcasecmp(colname, "regexten")) {
		ast_copy_string(buf, l->regexten ? l->regexten : "Unset", len);
	} else if (!strcasecmp(colname, "regcontext")) {
		ast_copy_string(buf, l->regcontext ? l->regcontext : "Unset", len);
		/* -- regexten feature */

	} else if (!strcasecmp(colname, "adhoc_number")) {
		ast_copy_string(buf, l->adhocNumber ? l->adhocNumber : "No", len);
	} else if (!strcasecmp(colname, "newmsgs")) {
		snprintf(buf, len, "%d", l->voicemailStatistic.newmsgs);
	} else if (!strcasecmp(colname, "oldmsgs")) {
		snprintf(buf, len, "%d", l->voicemailStatistic.oldmsgs);
	} else if (!strcasecmp(colname, "num_lines")) {
		snprintf(buf, len, "%d", l->devices.size);
	} else if (!strcasecmp(colname, "cfwd")) {
		sccp_linedevices_t *linedevice;
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			if (linedevice)
				snprintf(tmp, sizeof(tmp), "[id:%s,cfwdAll:%s,num:%s,cfwdBusy:%s,num:%s]", linedevice->device->id, linedevice->cfwdAll.enabled ? "on" : "off", linedevice->cfwdAll.number ? linedevice->cfwdAll.number : "<not set>", linedevice->cfwdBusy.enabled ? "on" : "off", linedevice->cfwdBusy.number ? linedevice->cfwdBusy.number : "<not set>");
			if (first == 0) {
				first = 1;
				strcat(lbuf, tmp);
			} else {
				strcat(lbuf, ",");
				strcat(lbuf, tmp);
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
		snprintf(buf, len, "%s", lbuf);
	} else if (!strcasecmp(colname, "devices")) {
		sccp_linedevices_t *linedevice;
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			if (linedevice)
				snprintf(tmp, sizeof(tmp), "%s", linedevice->device->id);
			if (first == 0) {
				first = 1;
				strcat(lbuf, tmp);
			} else {
				strcat(lbuf, ",");
				strcat(lbuf, tmp);
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
		snprintf(buf, len, "%s", lbuf);
	} else if (!strncasecmp(colname, "chanvar[", 8)) {
		char *chanvar = colname + 8;
		struct ast_variable *v;

		chanvar = strsep(&chanvar, "]");
		for (v = l->variables; v; v = v->next) {
			if (!strcasecmp(v->name, chanvar)) {
				ast_copy_string(buf, v->value, len);
			}
		}
	} else {
		ast_log(LOG_WARNING, "SCCPLINE(): unknown function option: %s", data);
		buf[0] = '\0';
	}
	sccp_line_unlock(l);
	return 0;
}

/*! \brief Stucture to declare a dialplan function: SCCPLINE */
static struct ast_custom_function sccpline_function = {
	.name = "SCCPLINE",
	.synopsis = "Retrieves information about an SCCP Line",
	.syntax = "Usage: SCCPLINE(lineName,<option>)",
	.read = sccp_func_sccpline,
};

/*!
 * \brief  ${SCCPCHANNEL()} Dialplan function - reads sccp line data 
 * \param chan Asterisk Channel
 * \param cmd Command as char
 * \param data Extra data as char
 * \param buf Buffer as chan*
 * \param len Lenght as size_t
 * \return Status as int
 *
 * \author Diederik de Groot <ddegroot@users.sourceforce.net>
 * \ref nf_sccp_dialplan_sccpchannel
 * 
 * \lock
 * 	- channel
 */
#if ASTERISK_VERSION_NUM >= 10600
static int sccp_func_sccpchannel(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
#else
static int sccp_func_sccpchannel(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
#endif
{
	sccp_channel_t *c;
	char *colname;

	if ((colname = strchr(data, ':'))) {					/*! \todo Will be deprecated after 1.4 */
		static int deprecation_warning = 0;
		*colname++ = '\0';
		if (deprecation_warning++ % 10 == 0)
			ast_log(LOG_WARNING, "SCCPCHANNEL(): usage of ':' to separate arguments is deprecated.  Please use ',' instead.\n");
	} else if ((colname = strchr(data, ',')))
		*colname++ = '\0';
	else
		colname = "callid";

	if (!strncasecmp(data, "current", 7)) {
		if (!(c = get_sccp_channel_from_ast_channel(chan))) {
/*			ast_log(LOG_WARNING, "SCCPCHANNEL(): Not an SCCP channel\n");*/
			return -1;
		}

		if (!c) {
			ast_log(LOG_WARNING, "SCCPCHANNEL(): SCCP Channel not available\n");
			return -1;
		}
	} else {
		uint32_t callid = atoi(data);
		if (!(c = sccp_channel_find_byid(callid))) {
			ast_log(LOG_WARNING, "SCCPCHANNEL(): SCCP Channel not available\n");
			return -1;
		}
	}
	sccp_channel_lock(c);

	if (!strcasecmp(colname, "callid") || !strcasecmp(colname, "id")) {
		snprintf(buf, len, "%d", c->callid);
	} else if (!strcasecmp(colname, "format")) {
		snprintf(buf, len, "%d", c->format);
	} else if (!strcasecmp(colname, "isCodecFix")) {
		ast_copy_string(buf, c->isCodecFix ? "yes" : "no", len);
	} else if (!strcasecmp(colname, "codecs")) {
		ast_codec_pref_string(&c->codecs, buf, sizeof(buf) - 1);
	} else if (!strcasecmp(colname, "capability")) {
		ast_getformatname_multiple(buf, len - 1, c->capability);
	} else if (!strcasecmp(colname, "calledPartyName")) {
		ast_copy_string(buf, c->callInfo.calledPartyName, len);
	} else if (!strcasecmp(colname, "calledPartyNumber")) {
		ast_copy_string(buf, c->callInfo.calledPartyNumber, len);
	} else if (!strcasecmp(colname, "callingPartyName")) {
		ast_copy_string(buf, c->callInfo.callingPartyName, len);
	} else if (!strcasecmp(colname, "callingPartyNumber")) {
		ast_copy_string(buf, c->callInfo.callingPartyNumber, len);
	} else if (!strcasecmp(colname, "originalCallingPartyName")) {
		ast_copy_string(buf, c->callInfo.originalCallingPartyName, len);
	} else if (!strcasecmp(colname, "originalCallingPartyNumber")) {
		ast_copy_string(buf, c->callInfo.originalCallingPartyNumber, len);
	} else if (!strcasecmp(colname, "originalCalledPartyName")) {
		ast_copy_string(buf, c->callInfo.originalCalledPartyName, len);
	} else if (!strcasecmp(colname, "originalCalledPartyNumber")) {
		ast_copy_string(buf, c->callInfo.originalCalledPartyNumber, len);
	} else if (!strcasecmp(colname, "lastRedirectingPartyName")) {
		ast_copy_string(buf, c->callInfo.lastRedirectingPartyName, len);
	} else if (!strcasecmp(colname, "lastRedirectingPartyNumber")) {
		ast_copy_string(buf, c->callInfo.lastRedirectingPartyNumber, len);
	} else if (!strcasecmp(colname, "cgpnVoiceMailbox")) {
		ast_copy_string(buf, c->callInfo.cgpnVoiceMailbox, len);
	} else if (!strcasecmp(colname, "cdpnVoiceMailbox")) {
		ast_copy_string(buf, c->callInfo.cdpnVoiceMailbox, len);
	} else if (!strcasecmp(colname, "originalCdpnVoiceMailbox")) {
		ast_copy_string(buf, c->callInfo.originalCdpnVoiceMailbox, len);
	} else if (!strcasecmp(colname, "lastRedirectingVoiceMailbox")) {
		ast_copy_string(buf, c->callInfo.lastRedirectingVoiceMailbox, len);
	} else if (!strcasecmp(colname, "passthrupartyid")) {
		snprintf(buf, len, "%d", c->passthrupartyid);
	} else if (!strcasecmp(colname, "state")) {
		ast_copy_string(buf, channelstate2str(c->state), len);
	} else if (!strcasecmp(colname, "previous_state")) {
		ast_copy_string(buf, channelstate2str(c->previousChannelState), len);
	} else if (!strcasecmp(colname, "calltype")) {
		ast_copy_string(buf, calltype2str(c->calltype), len);
	} else if (!strcasecmp(colname, "dialed_number")) {
		ast_copy_string(buf, c->dialedNumber, len);
	} else if (!strcasecmp(colname, "device")) {
		ast_copy_string(buf, c->device->id, len);
	} else if (!strcasecmp(colname, "line")) {
		ast_copy_string(buf, c->line->name, len);
	} else if (!strcasecmp(colname, "answered_elsewhere")) {
		ast_copy_string(buf, c->answered_elsewhere ? "yes" : "no", len);
	} else if (!strcasecmp(colname, "privacy")) {
		ast_copy_string(buf, c->privacy ? "yes" : "no", len);
	} else if (!strcasecmp(colname, "ss_action")) {
		snprintf(buf, len, "%d", c->ss_action);
	} else if (!strcasecmp(colname, "monitorEnabled")) {
		ast_copy_string(buf, c->monitorEnabled ? "yes" : "no", len);
	} else if (!strcasecmp(colname, "conference")) {
		/*! \todo needs to be implemented */
	} else if (!strcasecmp(colname, "parent")) {
		snprintf(buf, len, "%d", c->parentChannel->callid);
	} else if (!strcasecmp(colname, "peer")) {
		/*! \todo needs to be implemented */
	} else if (!strncasecmp(colname, "codec[", 6)) {
		char *codecnum;
		int codec = 0;

		codecnum = colname + 6;						// move past the '[' 
		codecnum = strsep(&codecnum, "]");				// trim trailing ']' if any 
		if ((codec = ast_codec_pref_index(&c->codecs, atoi(codecnum)))) {
			ast_copy_string(buf, ast_getformatname(codec), len);
		} else {
			buf[0] = '\0';
		}
	} else {
		ast_log(LOG_WARNING, "SCCPCHANNEL(): unknown function option: %s", data);
		buf[0] = '\0';
	}
	sccp_channel_unlock(c);
	return 0;
}

/*! \brief Stucture to declare a dialplan function: SCCPCHANNEL */
static struct ast_custom_function sccpchannel_function = {
	.name = "SCCPCHANNEL",
	.synopsis = "Retrieves information about an SCCP Line",
	.syntax = "Usage: SCCPCHANNEL(deviceId,<option>)",
	.read = sccp_func_sccpchannel,
};

/*!
 * \brief 	Set the Name and Number of the Called Party to the Calling Phone
 * \param 	chan Asterisk Channel
 * \param 	data CallerId in format "Name" \<number\>
 * \return	Success as int
 */
static int sccp_app_calledparty(struct ast_channel *chan, void *data)
{
	char *text = data;
	char *num, *name;
	sccp_channel_t *c = NULL;

	if (!(c = get_sccp_channel_from_ast_channel(chan))) {
		ast_log(LOG_WARNING, "SCCPDEVICE(): Not an SCCP channel\n");
		return 0;
	}

	if (!text || !c)
		return 0;

	ast_callerid_parse(text, &name, &num);
	sccp_channel_set_calledparty(c, name, num);

	return 0;
}

/*! \brief Stucture to declare a dialplan function: SETCALLEDPARTY */
static char *calledparty_name = "SetCalledParty";
static char *calledparty_synopsis = "Sets the callerid of the called party";
static char *calledparty_descr = "Usage: SetCalledParty(\"Name\" <ext>)" "Sets the name and number of the called party for use with chan_sccp\n";

/*!
 * \brief	It allows you to send a message to the calling device.
 * \author	Frank Segtrop <fs@matflow.net>
 * \param	chan asterisk channel
 * \param	data message to sent - if empty clear display
 * \version	20071112_1944
 * 
 * \lock
 * 	- device
 */
static int sccp_app_setmessage(struct ast_channel *chan, void *data)
{
	char *text = data;
	sccp_channel_t *c = NULL;
	sccp_device_t *d;

	if (!(c = get_sccp_channel_from_ast_channel(chan))) {
		ast_log(LOG_WARNING, "SCCPDEVICE(): Not an SCCP channel\n");
		return 0;
	}

	if (!text || !c || !c->device)
		return 0;

	d = c->device;
	sccp_device_lock(d);
	ast_free(d->phonemessage);
	if (text[0] != '\0') {
		sccp_dev_displayprinotify(d, text, 5, 0);
		sccp_dev_displayprompt(d, 0, 0, text, 0);
		d->phonemessage = ast_strdup(text);
		ast_db_put("SCCPM", d->id, text);
	} else {
		sccp_dev_displayprinotify(d, "Message off", 5, 1);
		sccp_dev_displayprompt(d, 0, 0, "Message off", 1);
		d->phonemessage = NULL;
		ast_db_del("SCCPM", d->id);
	}
	sccp_device_unlock(d);

	return 0;
}

static char *setmessage_name = "SetMessage";
static char *setmessage_synopsis = "Send a Message to the current Phone";
static char *setmessage_descr = "Usage: SetMessage(\"Message\")\n" "       Send a Message to the Calling Device\n";

static int sccp_register_dialplan_functions(void)
{
	int result;

	/* Register application functions */
	result = ast_register_application(calledparty_name, sccp_app_calledparty, calledparty_synopsis, calledparty_descr);
	result |= ast_register_application(setmessage_name, sccp_app_setmessage, setmessage_synopsis, setmessage_descr);

	/* Register dialplan functions */
	result |= ast_custom_function_register(&sccpdevice_function);
	result |= ast_custom_function_register(&sccpline_function);
	result |= ast_custom_function_register(&sccpchannel_function);

	return result;
}

static int sccp_unregister_dialplan_functions(void)
{
	int result;

	/* Unregister applications functions */
	result = ast_unregister_application(calledparty_name);
	result |= ast_unregister_application(setmessage_name);

	/* Unregister dial plan functions */
	result |= ast_custom_function_unregister(&sccpdevice_function);
	result |= ast_custom_function_unregister(&sccpline_function);
	result |= ast_custom_function_unregister(&sccpchannel_function);

	return result;
}

#if ASTERISK_VERSION_NUM >= 10400
/*!
 * \brief	Initialize and Astersik RTP Bridge
 * \param	c0	Asterisk Channel 0
 * \param	c1	Asterisk Channel 1
 * \param	flags	Flags as int
 * \param	fo	Asterisk Frame
 * \param	rc	Asterisk Channel
 * \param	timeoutms Time Out in Millisecs as int
 * \return 	Asterisk Bridge Result as enum
 * 
 * \lock
 * 	- asterisk channel0
 * 	- asterisk channel1
 */
enum ast_bridge_result sccp_rtp_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms)
{
	struct ast_rtp *p0 = NULL, *p1 = NULL;					/* Audio RTP Channels */
	enum ast_rtp_get_result audio_p0_res = AST_RTP_GET_FAILED;
	enum ast_rtp_get_result audio_p1_res = AST_RTP_GET_FAILED;
	enum ast_bridge_result res = AST_BRIDGE_FAILED;

	sccp_channel_t *pvt0 = NULL, *pvt1 = NULL;

	/* Lock channels */
	sccp_ast_channel_lock(c0);
	while (ast_channel_trylock(c1)) {
		sccp_ast_channel_unlock(c0);
		usleep(1);
		sccp_ast_channel_lock(c0);
	}

	/* Ensure neither channel got hungup during lock avoidance */
	if (ast_check_hangup(c0) || ast_check_hangup(c1)) {
		sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_rtp_bridge) Got hangup while attempting to bridge '%s' and '%s'\n", c0->name, c1->name);
		ast_channel_unlock(c1);
		ast_channel_unlock(c0);
		return AST_BRIDGE_FAILED;
	}

	/* Get channel specific interface structures */
	pvt0 = c0->tech_pvt;
	pvt1 = c1->tech_pvt;

	/* Get audio and video interface (if native bridge is possible) */
	audio_p0_res = sccp_channel_get_rtp_peer(c0, &p0);
	audio_p1_res = sccp_channel_get_rtp_peer(c1, &p1);

	/* Check if a bridge is possible (partial/native) */
	if (audio_p0_res == AST_RTP_GET_FAILED || audio_p1_res == AST_RTP_GET_FAILED) {
		/* Somebody doesn't want to play... */
		sccp_ast_channel_unlock(c1);
		sccp_ast_channel_unlock(c0);
		return AST_BRIDGE_FAILED_NOWARN;
	}

	/* If either side can only do a partial bridge, then don't try for a true native bridge */
	if (audio_p0_res == AST_RTP_TRY_PARTIAL || audio_p1_res == AST_RTP_TRY_PARTIAL) {
		struct ast_format_list fmt0, fmt1;

		/* In order to do Packet2Packet bridging both sides must be in the same rawread/rawwrite */
		if (c0->rawreadformat != c1->rawwriteformat || c1->rawreadformat != c0->rawwriteformat) {
			sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_rtp_bridge) Cannot packet2packet bridge - raw formats are incompatible\n");
			sccp_ast_channel_unlock(c1);
			sccp_ast_channel_unlock(c0);
			return AST_BRIDGE_FAILED_NOWARN;
		}

		/* They must also be using the same packetization */
		fmt0 = ast_codec_pref_getsize(&pvt0->device->codecs, c0->rawreadformat);
		fmt1 = ast_codec_pref_getsize(&pvt1->device->codecs, c1->rawreadformat);
		if (fmt0.cur_ms != fmt1.cur_ms) {
			sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_rtp_bridge) Cannot packet2packet bridge - packetization settings prevent it\n");
			sccp_ast_channel_unlock(c1);
			sccp_ast_channel_unlock(c0);
			return AST_BRIDGE_FAILED_NOWARN;
		}

		sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_rtp_bridge) Packet2Packet bridging '%s' and '%s'\n", c0->name, c1->name);
		res = AST_BRIDGE_FAILED_NOWARN;
	} else {
		sccp_log(1) (VERBOSE_PREFIX_3 "SCCP: (sccp_rtp_bridge) Native bridging '%s' and '%s'\n", c0->name, c1->name);

		// \todo TODO The purpose of this code is obscure and must be clarified (-DD)
		//sccp_pbx_set_rtp_peer(c0, pvt1->rtp, NULL, NULL, pvt1->device->codecs, 0);
		//sccp_pbx_set_rtp_peer(c1, pvt0->rtp, NULL, NULL, pvt0->device->codecs, 0);

		res = AST_BRIDGE_FAILED;					//_NOWARN;
	}

	sccp_ast_channel_unlock(c1);
	sccp_ast_channel_unlock(c0);

	return res;
}
#endif

#if ASTERISK_VERSION_NUM < 10400
/*!
 * \brief 	Load the actual chan_sccp module
 * \return	Success as int
 */
int load_module()
{
#else
static int load_module(void)
{
#endif

#ifdef HAVE_LIBGC
	GC_INIT();
	(void)GC_set_warn_proc(gc_warn_handler);
#    if DEBUG > 0
	GC_find_leak = 1;
#    endif
#endif
	/* make globals */
	sccp_globals = ast_malloc(sizeof(struct sccp_global_vars));
	sccp_event_listeners = ast_malloc(sizeof(struct sccp_event_subscriptions));
	if (!sccp_globals || !sccp_event_listeners) {
		ast_log(LOG_ERROR, "No free memory for SCCP global vars. SCCP channel type disabled\n");
#if ASTERISK_VERSION_NUM < 10400
		return -1;
#else
		return AST_MODULE_LOAD_FAILURE;
#endif
	}

	/* Initialize memory */
	memset(&sccp_null_frame, 0, sizeof(sccp_null_frame));

	sched = sched_context_create();
	if (!sched) {
		ast_log(LOG_WARNING, "Unable to create schedule context. SCCP channel type disabled\n");
		return AST_MODULE_LOAD_FAILURE;
	}
	io = io_context_create();
	if (!io) {
		ast_log(LOG_WARNING, "Unable to create I/O context. SCCP channel type disabled\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	memset(sccp_globals, 0, sizeof(struct sccp_global_vars));
	memset(sccp_event_listeners, 0, sizeof(struct sccp_event_subscriptions));
	ast_mutex_init(&GLOB(lock));
	ast_mutex_init(&GLOB(usecnt_lock));
	ast_mutex_init(&GLOB(monitor_lock));
	SCCP_LIST_HEAD_INIT(&GLOB(sessions));
	SCCP_LIST_HEAD_INIT(&GLOB(devices));
	SCCP_LIST_HEAD_INIT(&GLOB(lines));

	SCCP_LIST_HEAD_INIT(&sccp_event_listeners->subscriber);

	sccp_mwi_module_start();
	sccp_hint_module_start();
#ifdef CS_SCCP_CONFERENCE
	sccp_conference_module_start();
#endif
	sccp_event_subscribe(SCCP_EVENT_FEATURECHANGED, sccp_util_handleFeatureChangeEvent);

	/* GLOB() is a macro for sccp_globals-> */
	GLOB(descriptor) = -1;
	GLOB(ourport) = 2000;
	GLOB(externrefresh) = 60;
	GLOB(keepalive) = SCCP_KEEPALIVE;
	sccp_copy_string(GLOB(date_format), "D/M/YA", sizeof(GLOB(date_format)));
	sccp_copy_string(GLOB(context), "default", sizeof(GLOB(context)));
	sccp_copy_string(GLOB(servername), "Asterisk", sizeof(GLOB(servername)));

	/* Wait up to 16 seconds for first digit */
	GLOB(firstdigittimeout) = 16;
	/* How long to wait for following digits */
	GLOB(digittimeout) = 8;
	/* Yes, these are all that the phone supports (except it's own 'Wideband 256k') */
	GLOB(global_capability) = AST_FORMAT_ALAW | AST_FORMAT_ULAW | AST_FORMAT_G729A | AST_FORMAT_H263;

	GLOB(debug) = 1;
	GLOB(sccp_tos) = (0x68 & 0xff);						// AF31
	GLOB(audio_tos) = (0xB8 & 0xff);					// EF
	GLOB(video_tos) = (0x88 & 0xff);					// AF41
	GLOB(sccp_cos) = 4;
	GLOB(audio_cos) = 6;
	GLOB(video_cos) = 5;
	GLOB(echocancel) = 1;
	GLOB(silencesuppression) = 0;
	GLOB(dndmode) = SCCP_DNDMODE_REJECT;
	GLOB(autoanswer_tone) = SKINNY_TONE_ZIP;
	GLOB(remotehangup_tone) = SKINNY_TONE_ZIP;
	GLOB(callwaiting_tone) = SKINNY_TONE_CALLWAITINGTONE;
	GLOB(privacy) = 1;							/* permit private function */
	GLOB(mwilamp) = SKINNY_LAMP_ON;
	GLOB(protocolversion) = SCCP_DRIVER_SUPPORTED_PROTOCOL_HIGH;
	GLOB(amaflags) = ast_cdr_amaflags2int("documentation");
	GLOB(callAnswerOrder) = ANSWER_OLDEST_FIRST;
	GLOB(socket_thread) = AST_PTHREADT_NULL;
	GLOB(hotline) = ast_malloc(sizeof(sccp_hotline_t));
	memset(GLOB(hotline), 0, sizeof(sccp_hotline_t));

	sccp_create_hotline();

	if (!load_config()) {
#ifdef CS_AST_HAS_TECH_PVT
		if (ast_channel_register(&sccp_tech)) {
#else
		if (ast_channel_register_ex("SCCP", "SCCP", GLOB(global_capability), sccp_request, sccp_devicestate)) {
#endif
			ast_log(LOG_ERROR, "Unable to register channel class SCCP\n");
			return -1;
		}
	}
#if ASTERISK_VERSION_NUM >= 10400
#    ifndef CS_AST_HAS_RTP_ENGINE
	ast_rtp_proto_register(&sccp_rtp);
#    else
	ast_rtp_glue_register(&sccp_rtp);
#    endif
#endif

#ifdef CS_SCCP_MANAGER
	sccp_register_management();
#endif

	sccp_register_cli();
	sccp_register_dialplan_functions();

	/* And start the monitor for the first time */
	sccp_restart_monitor();

	return 0;
}

#if ASTERISK_VERSION_NUM >= 10400
/*!
 * \brief Schedule free memory
 * \param ptr pointer
 * \return Success as int
 */
int sccp_sched_free(void *ptr)
{
	if (!ptr)
		return -1;

	ast_free(ptr);
	return 0;

}
#endif

#if ASTERISK_VERSION_NUM < 10400
/*!
 * \brief 	Unload the chan_sccp module
 * \return	Success as int
 * 
 * \warning
 * 	- lines is not always locked
 * 	- line->channels is not always locked
 *
 * \lock
 * 	- lines
 * 	- monitor_lock
 * 	- devices
 * 	- lines
 * 	- sessions
 * 	- socket_lock
 */
int unload_module()
{
	char iabuf[INET_ADDRSTRLEN];
#else
static int unload_module(void)
{
#endif
	sccp_device_t *d;
	sccp_line_t *l;
	sccp_channel_t *c;
	sccp_session_t *s;
	int openchannels = 0;

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: Unloading Module\n");

	/* temporary fix to close open channels */
	/* \todo Temporary fix to unload Module. Needs to be looked at */
	struct ast_channel *astChannel = NULL;

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Hangup open channels\n");
	while ((astChannel = ast_channel_walk_locked(astChannel)) != NULL) {
		if (!ast_check_hangup(astChannel)) {
			if ((c = get_sccp_channel_from_ast_channel(astChannel))) {
				astChannel->hangupcause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
				astChannel->_softhangup = AST_SOFTHANGUP_APPUNLOAD;
				sccp_channel_endcall(c);
				ast_safe_sleep(astChannel, 100);
				openchannels++;
			}
		}
		ast_channel_unlock(astChannel);
	}
	sccp_safe_sleep(openchannels * 1000);					// wait for everything to settle

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Unregister SCCP RTP protocol\n");
#if ASTERISK_VERSION_NUM >= 10400
	ast_rtp_proto_unregister(&sccp_rtp);
#endif
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Unregister SCCP Channel Tech\n");
#ifdef CS_AST_HAS_TECH_PVT
	ast_channel_unregister(&sccp_tech);
#else
	ast_channel_unregister("SCCP");
#endif
	sccp_unregister_dialplan_functions();
	sccp_unregister_cli();

	sccp_mwi_module_stop();
	sccp_hint_module_stop();

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Removing monitor thread\n");
	sccp_globals_lock(monitor_lock);
	if ((GLOB(monitor_thread) != AST_PTHREADT_NULL) && (GLOB(monitor_thread) != AST_PTHREADT_STOP)) {
		pthread_cancel(GLOB(monitor_thread));
		pthread_kill(GLOB(monitor_thread), SIGURG);
#ifndef HAVE_LIBGC
		pthread_join(GLOB(monitor_thread), NULL);
#endif
	}
	GLOB(monitor_thread) = AST_PTHREADT_STOP;
	sccp_globals_unlock(monitor_lock);
	sccp_mutex_destroy(&GLOB(monitor_lock));

#ifdef CS_SCCP_MANAGER
	sccp_unregister_management();
#endif

	/* removing devices */
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Removing Devices\n");
	SCCP_LIST_LOCK(&GLOB(devices));
	while ((d = SCCP_LIST_REMOVE_HEAD(&GLOB(devices), list))) {
		sccp_log((DEBUGCAT_CORE | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: Removing device %s\n", d->id);
		sccp_dev_clean(d, TRUE, 0);
	}
	SCCP_LIST_UNLOCK(&GLOB(devices));
	if (SCCP_LIST_EMPTY(&GLOB(devices)))
		SCCP_LIST_HEAD_DESTROY(&GLOB(devices));

	/* hotline will be removed by line removing function */
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Removing Hotline\n");
	GLOB(hotline)->line = NULL;
	ast_free(GLOB(hotline));

	/* removing lines */
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Removing Lines\n");
	SCCP_LIST_LOCK(&GLOB(lines));
	while ((l = SCCP_LIST_REMOVE_HEAD(&GLOB(lines), list))) {
		sccp_log((DEBUGCAT_CORE | DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "SCCP: Removing line %s\n", l->name);
		sccp_line_clean(l, FALSE);
	}
	SCCP_LIST_UNLOCK(&GLOB(lines));
	if (SCCP_LIST_EMPTY(&GLOB(lines)))
		SCCP_LIST_HEAD_DESTROY(&GLOB(lines));

	/* removing sessions */
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Removing Sessions\n");
	SCCP_LIST_LOCK(&GLOB(sessions));
	while ((s = SCCP_LIST_REMOVE_HEAD(&GLOB(sessions), list))) {
#if ASTERISK_VERSION_NUM < 10400
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Removing session %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
#else
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Removing session %s\n", ast_inet_ntoa(s->sin.sin_addr));
#endif
		if (s->fd > -1)
			close(s->fd);
		ast_mutex_destroy(&s->lock);
		ast_free(s);
	}
	SCCP_LIST_UNLOCK(&GLOB(sessions));

	if (SCCP_LIST_EMPTY(&GLOB(sessions)))
		SCCP_LIST_HEAD_DESTROY(&GLOB(sessions));

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Removing Descriptor\n");
	close(GLOB(descriptor));
	GLOB(descriptor) = -1;

	sccp_log((DEBUGCAT_CORE | DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_2 "SCCP: Killing the socket thread\n");
	sccp_globals_lock(socket_lock);
	if ((GLOB(socket_thread) != AST_PTHREADT_NULL) && (GLOB(socket_thread) != AST_PTHREADT_STOP)) {
		pthread_cancel(GLOB(socket_thread));
		pthread_kill(GLOB(socket_thread), SIGURG);
#ifndef HAVE_LIBGC
		pthread_join(GLOB(socket_thread), NULL);
#endif
	}
	GLOB(socket_thread) = AST_PTHREADT_STOP;
	sccp_globals_unlock(socket_lock);
	sccp_mutex_destroy(&GLOB(socket_lock));
	sccp_log((DEBUGCAT_CORE | DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_2 "SCCP: Killed the socket thread\n");

	sccp_log((DEBUGCAT_CORE | DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_2 "SCCP: Removing bind\n");
	if (GLOB(ha))
		ast_free_ha(GLOB(ha));

	if (GLOB(localaddr))
		ast_free_ha(GLOB(localaddr));

	sccp_log((DEBUGCAT_CORE | DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_2 "SCCP: Removing io/sched\n");
	if (io)
		io_context_destroy(io);
	if (sched)
		sched_context_destroy(sched);

	ast_mutex_destroy(&GLOB(usecnt_lock));
	ast_mutex_destroy(&GLOB(lock));
	ast_free(sccp_globals);

	ast_log(LOG_NOTICE, "Running Cleanup\n");
#ifdef HAVE_LIBGC
	CHECK_LEAKS();
//      GC_gcollect();
#endif
	ast_log(LOG_NOTICE, "Module chan_sccp unloaded\n");
	return 0;
}

#if ASTERISK_VERSION_NUM >= 10400
AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Skinny Client Control Protocol (SCCP). Release: " SCCP_VERSION " " SCCP_BRANCH " (built by '" BUILD_USER "' on '" BUILD_DATE "')");
#else
/*!
 * \brief 	number of instances of chan_sccp
 * \return	res number of instances
 */
int usecount()
{
	int res;
	sccp_globals_lock(usecnt_lock);
	res = GLOB(usecnt);
	sccp_globals_unlock(usecnt_lock);
	return res;
}

/*!
 * \brief 	Asterisk GPL Key
 * \return 	the asterisk key
 */
char *key()
{
	return ASTERISK_GPL_KEY;
}

/*!
 * \brief 	echo the module description
 * \return	chan_sccp description
 */
char *description()
{
	return ("Skinny Client Control Protocol (SCCP). Release: " SCCP_VERSION " " SCCP_BRANCH " - " SCCP_REVISION " (built by '" BUILD_USER "' on '" BUILD_DATE "')");
}
#endif

#ifdef CS_DEVSTATE_FEATURE
const char devstate_astdb_family[] = "CustomDevstate";
#endif
