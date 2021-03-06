/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * Copyright (C) 2013 Jiri Pirko <jiri@resnulli.us>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <teamdctl.h>
#include <stdlib.h>

#include "nm-device-team.h"
#include "nm-logging.h"
#include "NetworkManagerUtils.h"
#include "nm-device-private.h"
#include "nm-platform.h"
#include "nm-dbus-glib-types.h"
#include "nm-dbus-manager.h"
#include "nm-enum-types.h"
#include "nm-team-enum-types.h"
#include "nm-posix-signals.h"
#include "nm-core-internal.h"

#include "nm-device-team-glue.h"

#include "nm-device-logging.h"
_LOG_DECLARE_SELF(NMDeviceTeam);

G_DEFINE_TYPE (NMDeviceTeam, nm_device_team, NM_TYPE_DEVICE)

#define NM_DEVICE_TEAM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_TEAM, NMDeviceTeamPrivate))

static gboolean teamd_start (NMDevice *device, NMSettingTeam *s_team);

typedef struct {
	struct teamdctl *tdc;
	GPid teamd_pid;
	guint teamd_process_watch;
	guint teamd_timeout;
	guint teamd_dbus_watch;
} NMDeviceTeamPrivate;

enum {
	PROP_0,
	PROP_SLAVES,

	LAST_PROP
};

/******************************************************************/

static guint32
get_generic_capabilities (NMDevice *device)
{
	return NM_DEVICE_CAP_CARRIER_DETECT;
}

static gboolean
is_available (NMDevice *device)
{
	if (NM_DEVICE_GET_CLASS (device)->is_up)
		return NM_DEVICE_GET_CLASS (device)->is_up (device);
	return FALSE;
}

static gboolean
check_connection_available (NMDevice *device,
                            NMConnection *connection,
                            const char *specific_object)
{
	/* Connections are always available because the carrier state is determined
	 * by the team port carrier states, not the team's state.
	 */
	return TRUE;
}

static gboolean
check_connection_compatible (NMDevice *device, NMConnection *connection)
{
	const char *iface;
	NMSettingTeam *s_team;

	if (!NM_DEVICE_CLASS (nm_device_team_parent_class)->check_connection_compatible (device, connection))
		return FALSE;

	s_team = nm_connection_get_setting_team (connection);
	if (!s_team || !nm_connection_is_type (connection, NM_SETTING_TEAM_SETTING_NAME))
		return FALSE;

	/* Team connections must specify the virtual interface name */
	iface = nm_connection_get_interface_name (connection);
	if (!iface || strcmp (nm_device_get_iface (device), iface))
		return FALSE;

	/* FIXME: match team properties like mode, etc? */

	return TRUE;
}

static gboolean
complete_connection (NMDevice *device,
                     NMConnection *connection,
                     const char *specific_object,
                     const GSList *existing_connections,
                     GError **error)
{
	NMSettingTeam *s_team;

	nm_utils_complete_generic (connection,
	                           NM_SETTING_TEAM_SETTING_NAME,
	                           existing_connections,
	                           NULL,
	                           _("Team connection"),
	                           "team",
	                           TRUE);

	s_team = nm_connection_get_setting_team (connection);
	if (!s_team) {
		s_team = (NMSettingTeam *) nm_setting_team_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_team));
	}

	return TRUE;
}

static gboolean
ensure_teamd_connection (NMDevice *device)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (device);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (self);
	int err;

	if (priv->tdc)
		return TRUE;

	priv->tdc = teamdctl_alloc ();
	g_assert (priv->tdc);
	err = teamdctl_connect (priv->tdc, nm_device_get_iface (device), NULL, NULL);
	if (err != 0) {
		_LOGE (LOGD_TEAM, "failed to connect to teamd (err=%d)", err);
		teamdctl_free (priv->tdc);
		priv->tdc = NULL;
	}

	return !!priv->tdc;
}

static void
update_connection (NMDevice *device, NMConnection *connection)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (device);
	NMSettingTeam *s_team = nm_connection_get_setting_team (connection);

	if (!s_team) {
		s_team = (NMSettingTeam *) nm_setting_team_new ();
		nm_connection_add_setting (connection, (NMSetting *) s_team);
	}
	g_object_set (G_OBJECT (s_team), NM_SETTING_TEAM_CONFIG, NULL, NULL);

	teamd_start (device, s_team);
	if (NM_DEVICE_TEAM_GET_PRIVATE (device)->teamd_pid > 0 && ensure_teamd_connection (device)) {
		const char *config = NULL;
		int err;

		err = teamdctl_config_get_raw_direct (NM_DEVICE_TEAM_GET_PRIVATE (device)->tdc,
		                                      (char **)&config);
		if (err == 0)
			g_object_set (G_OBJECT (s_team), NM_SETTING_TEAM_CONFIG, config, NULL);
		else
			_LOGE (LOGD_TEAM, "failed to read teamd config (err=%d)", err);
	}
}

/******************************************************************/

static gboolean
master_update_slave_connection (NMDevice *self,
                                   NMDevice *slave,
                                   NMConnection *connection,
                                   GError **error)
{
	NMSettingTeamPort *s_port;
	char *port_config = NULL;
	int err = 0;
	struct teamdctl *tdc;
	const char *team_port_config = NULL;
	const char *iface = nm_device_get_iface (self);
	const char *iface_slave = nm_device_get_iface (slave);

	tdc = teamdctl_alloc ();
	if (!tdc) {
		g_set_error (error,
		             NM_DEVICE_ERROR,
		             NM_DEVICE_ERROR_FAILED,
		             "update slave connection for slave '%s' failed to connect to teamd for master %s (out of memory?)",
		             iface_slave, iface);
		g_return_val_if_reached (FALSE);
	}

	err = teamdctl_connect (tdc, iface, NULL, NULL);
	if (err) {
		teamdctl_free (tdc);
		g_set_error (error,
		             NM_DEVICE_ERROR,
		             NM_DEVICE_ERROR_FAILED,
		             "update slave connection for slave '%s' failed to connect to teamd for master %s (err=%d)",
		             iface_slave, iface, err);
		return FALSE;
	}

	err = teamdctl_port_config_get_raw_direct (tdc, iface_slave, (char **)&team_port_config);
	port_config = g_strdup (team_port_config);
	teamdctl_free (tdc);
	if (err) {
		g_set_error (error,
		             NM_DEVICE_ERROR,
		             NM_DEVICE_ERROR_FAILED,
		             "update slave connection for slave '%s' failed to get configuration from teamd master %s (err=%d)",
		             iface_slave, iface, err);
		g_free (port_config);
		return FALSE;
	}

	s_port = nm_connection_get_setting_team_port (connection);
	if (!s_port) {
		s_port = (NMSettingTeamPort *) nm_setting_team_port_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_port));
	}

	g_object_set (G_OBJECT (s_port), NM_SETTING_TEAM_PORT_CONFIG, port_config, NULL);
	g_free (port_config);

	g_object_set (nm_connection_get_setting_connection (connection),
	              NM_SETTING_CONNECTION_MASTER, iface,
	              NM_SETTING_CONNECTION_SLAVE_TYPE, NM_SETTING_TEAM_SETTING_NAME,
	              NULL);
	return TRUE;
}

/******************************************************************/

static void
teamd_timeout_remove (NMDevice *device)
{
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (device);

	if (priv->teamd_timeout) {
		g_source_remove (priv->teamd_timeout);
		priv->teamd_timeout = 0;
	}
}

static void
teamd_cleanup (NMDevice *device, gboolean device_state_failed)
{
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (device);

	if (priv->teamd_dbus_watch) {
		g_bus_unwatch_name (priv->teamd_dbus_watch);
		priv->teamd_dbus_watch = 0;
	}

	if (priv->teamd_process_watch) {
		g_source_remove (priv->teamd_process_watch);
		priv->teamd_process_watch = 0;
	}

	if (priv->teamd_pid > 0) {
		nm_utils_kill_child_async (priv->teamd_pid, SIGTERM, LOGD_TEAM, "teamd", 2000, NULL, NULL);
		priv->teamd_pid = 0;
	}

	if (priv->tdc) {
		teamdctl_disconnect (priv->tdc);
		teamdctl_free (priv->tdc);
		priv->tdc = NULL;
	}

	teamd_timeout_remove (device);

	if (device_state_failed) {
		if (nm_device_is_activating (device) ||
		    (nm_device_get_state (device) == NM_DEVICE_STATE_ACTIVATED))
			nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_TEAMD_CONTROL_FAILED);
	}
}

static gboolean
teamd_timeout_cb (gpointer user_data)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (user_data);
	NMDevice *device = NM_DEVICE (self);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (device);

	g_return_val_if_fail (priv->teamd_timeout, FALSE);

	_LOGI (LOGD_TEAM, "teamd timed out.");
	teamd_cleanup (device, TRUE);

	return FALSE;
}

static void
teamd_dbus_appeared (GDBusConnection *connection,
                     const gchar *name,
                     const gchar *name_owner,
                     gpointer user_data)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (user_data);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (self);
	NMDevice *device = NM_DEVICE (self);

	g_return_if_fail (priv->teamd_dbus_watch);

	_LOGI (LOGD_TEAM, "teamd appeared on D-Bus");
	teamd_timeout_remove (device);
	if (!ensure_teamd_connection (device)) {
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_TEAMD_CONTROL_FAILED);
		return;
	}
	nm_device_activate_schedule_stage2_device_config (device);
}

static void
teamd_dbus_vanished (GDBusConnection *connection,
                     const gchar *name,
                     gpointer user_data)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (user_data);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (self);
	NMDevice *device = NM_DEVICE (self);

	g_return_if_fail (priv->teamd_dbus_watch);

	if (priv->teamd_timeout) {
		/* g_bus_watch_name will always raise an initial signal, to indicate whether the
		 * name exists/not exists initially. Do not take this as a failure, until the
		 * startup timeout is over.
		 *
		 * Note that g_bus_watch_name is guaranteed to alternate vanished/appeared signals,
		 * so we won't hit this condition again (because the next signal is either 'appeared'
		 * or 'timeout'). */
		_LOGD (LOGD_TEAM, "teamd vanished from D-Bus (ignored)");
		return;
	}

	_LOGI (LOGD_TEAM, "teamd vanished from D-Bus");
	teamd_cleanup (device, TRUE);
}

static void
teamd_process_watch_cb (GPid pid, gint status, gpointer user_data)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (user_data);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (self);
	NMDevice *device = NM_DEVICE (self);

	g_return_if_fail (priv->teamd_process_watch);

	_LOGI (LOGD_TEAM, "teamd died with status %d", status);
	priv->teamd_process_watch = 0;
	priv->teamd_pid = 0;
	teamd_cleanup (device, TRUE);
}

static void
teamd_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point.
	 * Give child it's own program group for signal
	 * separation.
	 */
	pid_t pid = getpid ();
	setpgid (pid, pid);

	/*
	 * We blocked signals in main(). We need to restore original signal
	 * mask for avahi-autoipd here so that it can receive signals.
	 */
	nm_unblock_posix_signals (NULL);
}

static gboolean
teamd_start (NMDevice *device, NMSettingTeam *s_team)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (device);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (self);
	const char *iface = nm_device_get_ip_iface (device);
	char *tmp_str = NULL;
	const char *config;
	const char *teamd_binary;
	GPtrArray *argv;
	GError *error = NULL;
	gboolean ret;
	int status;

	if (priv->teamd_dbus_watch ||
	    priv->teamd_process_watch ||
	    priv->teamd_pid > 0 ||
	    priv->tdc ||
	    priv->teamd_timeout)
	{
		/* Just return if teamd_start() was already called */
		return TRUE;
	}

	teamd_binary = nm_utils_find_helper ("teamd", NULL, NULL);
	if (!teamd_binary) {
		_LOGW (LOGD_TEAM, "Activation: (team) failed to start teamd: teamd binary not found");
		return FALSE;
	}

	/* Kill teamd for same named device first if it is there */
	argv = g_ptr_array_new ();
	g_ptr_array_add (argv, (gpointer) teamd_binary);
	g_ptr_array_add (argv, (gpointer) "-k");
	g_ptr_array_add (argv, (gpointer) "-t");
	g_ptr_array_add (argv, (gpointer) iface);
	g_ptr_array_add (argv, NULL);

	_LOGD (LOGD_TEAM, "running: %s",
	       (tmp_str = g_strjoinv (" ", (gchar **) argv->pdata)));
	g_clear_pointer (&tmp_str, g_free);

	ret = g_spawn_sync ("/", (char **) argv->pdata, NULL, 0, nm_unblock_posix_signals, NULL, NULL, NULL, &status, &error);
	g_ptr_array_free (argv, TRUE);

	/* Start teamd now */
	argv = g_ptr_array_new ();
	g_ptr_array_add (argv, (gpointer) teamd_binary);
	g_ptr_array_add (argv, (gpointer) "-o");
	g_ptr_array_add (argv, (gpointer) "-n");
	g_ptr_array_add (argv, (gpointer) "-U");
	g_ptr_array_add (argv, (gpointer) "-D");
	g_ptr_array_add (argv, (gpointer) "-N");
	g_ptr_array_add (argv, (gpointer) "-t");
	g_ptr_array_add (argv, (gpointer) iface);

	config = nm_setting_team_get_config(s_team);
	if (config) {
		g_ptr_array_add (argv, (gpointer) "-c");
		g_ptr_array_add (argv, (gpointer) config);
	}

	if (nm_logging_enabled (LOGL_DEBUG, LOGD_TEAM))
		g_ptr_array_add (argv, (gpointer) "-gg");
	g_ptr_array_add (argv, NULL);

	_LOGD (LOGD_TEAM, "running: %s",
	       (tmp_str = g_strjoinv (" ", (gchar **) argv->pdata)));
	g_clear_pointer (&tmp_str, g_free);

	/* Start a timeout for teamd to appear at D-Bus */
	priv->teamd_timeout = g_timeout_add_seconds (5, teamd_timeout_cb, device);

	/* Register D-Bus name watcher */
	tmp_str = g_strdup_printf ("org.libteam.teamd.%s", iface);
	priv->teamd_dbus_watch = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
	                                           tmp_str,
	                                           G_BUS_NAME_WATCHER_FLAGS_NONE,
	                                           teamd_dbus_appeared,
	                                           teamd_dbus_vanished,
	                                           device,
	                                           NULL);
	g_free (tmp_str);

	ret = g_spawn_async ("/", (char **) argv->pdata, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                    &teamd_child_setup, NULL, &priv->teamd_pid, &error);
	g_ptr_array_free (argv, TRUE);
	if (!ret) {
		_LOGW (LOGD_TEAM, "Activation: (team) failed to start teamd: %s", error->message);
		g_clear_error (&error);
		teamd_cleanup (device, FALSE);
		return FALSE;
	}

	/* Monitor the child process so we know when it dies */
	priv->teamd_process_watch = g_child_watch_add (priv->teamd_pid,
	                                               teamd_process_watch_cb,
	                                               device);

	_LOGI (LOGD_TEAM, "Activation: (team) started teamd...");
	return TRUE;
}

static void
teamd_stop (NMDevice *device)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (device);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (self);

	if (priv->teamd_pid > 0)
		_LOGI (LOGD_TEAM, "Deactivation: stopping teamd...");
	else
		_LOGD (LOGD_TEAM, "Deactivation: stopping teamd (not started)...");
	teamd_cleanup (device, FALSE);
}

static NMActStageReturn
act_stage1_prepare (NMDevice *device, NMDeviceStateReason *reason)
{
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_SUCCESS;
	NMConnection *connection;
	NMSettingTeam *s_team;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	ret = NM_DEVICE_CLASS (nm_device_team_parent_class)->act_stage1_prepare (device, reason);
	if (ret == NM_ACT_STAGE_RETURN_SUCCESS) {
		connection = nm_device_get_connection (device);
		g_assert (connection);
		s_team = nm_connection_get_setting_team (connection);
		g_assert (s_team);
		if (teamd_start (device, s_team))
			ret = NM_ACT_STAGE_RETURN_POSTPONE;
		else
			ret = NM_ACT_STAGE_RETURN_FAILURE;
	}
	return ret;
}

static void
deactivate (NMDevice *device)
{
	teamd_stop (device);
}

static gboolean
enslave_slave (NMDevice *device,
               NMDevice *slave,
               NMConnection *connection,
               gboolean configure)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (device);
	NMDeviceTeamPrivate *priv = NM_DEVICE_TEAM_GET_PRIVATE (device);
	gboolean success = TRUE, no_firmware = FALSE;
	const char *slave_iface = nm_device_get_ip_iface (slave);
	NMSettingTeamPort *s_team_port;

	nm_device_master_check_slave_physical_port (device, slave, LOGD_TEAM);

	if (configure) {
		nm_device_take_down (slave, TRUE);

		s_team_port = nm_connection_get_setting_team_port (connection);
		if (s_team_port) {
			const char *config = nm_setting_team_port_get_config (s_team_port);

			if (config) {
				if (!priv->tdc) {
					_LOGW (LOGD_TEAM, "enslaved team port %s config not changed, not connected to teamd",
					       slave_iface);
				} else {
					int err;
					char *sanitized_config;

					sanitized_config = g_strdelimit (g_strdup (config), "\r\n", ' ');
					err = teamdctl_port_config_update_raw (priv->tdc, slave_iface, sanitized_config);
					g_free (sanitized_config);
					if (err != 0) {
						_LOGE (LOGD_TEAM, "failed to update config for port %s (err=%d)",
						       slave_iface, err);
						return FALSE;
					}
				}
			}
		}
		success = nm_platform_link_enslave (nm_device_get_ip_ifindex (device),
		                                    nm_device_get_ip_ifindex (slave));
		nm_device_bring_up (slave, TRUE, &no_firmware);

		if (!success)
			return FALSE;

		_LOGI (LOGD_TEAM, "enslaved team port %s", slave_iface);
	} else
		_LOGI (LOGD_TEAM, "team port %s was enslaved", slave_iface);

	g_object_notify (G_OBJECT (device), NM_DEVICE_TEAM_SLAVES);

	return TRUE;
}

static gboolean
release_slave (NMDevice *device,
               NMDevice *slave,
               gboolean configure)
{
	NMDeviceTeam *self = NM_DEVICE_TEAM (device);
	gboolean success = TRUE, no_firmware = FALSE;

	if (configure) {
		success = nm_platform_link_release (nm_device_get_ip_ifindex (device),
		                                    nm_device_get_ip_ifindex (slave));

		if (success)
			_LOGI (LOGD_TEAM, "released team port %s", nm_device_get_ip_iface (slave));
		else
			_LOGW (LOGD_TEAM, "failed to release team port %s", nm_device_get_ip_iface (slave));
	} else
		_LOGI (LOGD_TEAM, "team port %s was released", nm_device_get_ip_iface (slave));

	if (success)
		g_object_notify (G_OBJECT (device), NM_DEVICE_TEAM_SLAVES);

	if (configure) {
		/* Kernel team code "closes" the port when releasing it, (which clears
		 * IFF_UP), so we must bring it back up here to ensure carrier changes and
		 * other state is noticed by the now-released port.
		 */
		if (!nm_device_bring_up (slave, TRUE, &no_firmware))
			_LOGW (LOGD_TEAM, "released team port %s could not be brought up",
			       nm_device_get_ip_iface (slave));
	}

	return success;
}

/******************************************************************/

NMDevice *
nm_device_team_new (NMPlatformLink *platform_device)
{
	g_return_val_if_fail (platform_device != NULL, NULL);

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_TEAM,
	                                  NM_DEVICE_PLATFORM_DEVICE, platform_device,
	                                  NM_DEVICE_DRIVER, "team",
	                                  NM_DEVICE_TYPE_DESC, "Team",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_TEAM,
	                                  NM_DEVICE_IS_MASTER, TRUE,
	                                  NULL);
}

NMDevice *
nm_device_team_new_for_connection (NMConnection *connection, GError **error)
{
	const char *iface;

	g_return_val_if_fail (connection != NULL, NULL);

	iface = nm_connection_get_interface_name (connection);
	g_return_val_if_fail (iface != NULL, NULL);

	if (   !nm_platform_team_add (iface)
	    && nm_platform_get_error () != NM_PLATFORM_ERROR_EXISTS) {
		g_set_error (error,
		             NM_DEVICE_ERROR,
		             NM_DEVICE_ERROR_CREATION_FAILED,
		             "failed to create team master interface '%s' for connection '%s': %s",
		             iface, nm_connection_get_id (connection),
		             nm_platform_get_error_msg ());
		return NULL;
	}

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_TEAM,
	                                  NM_DEVICE_IFACE, iface,
	                                  NM_DEVICE_DRIVER, "team",
	                                  NM_DEVICE_TYPE_DESC, "Team",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_TEAM,
	                                  NM_DEVICE_IS_MASTER, TRUE,
	                                  NULL);
}

static void
nm_device_team_init (NMDeviceTeam * self)
{
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	GPtrArray *slaves;
	GSList *list, *iter;

	switch (prop_id) {
		break;
	case PROP_SLAVES:
		slaves = g_ptr_array_new ();
		list = nm_device_master_get_slaves (NM_DEVICE (object));
		for (iter = list; iter; iter = iter->next)
			g_ptr_array_add (slaves, g_strdup (nm_device_get_path (NM_DEVICE (iter->data))));
		g_slist_free (list);
		g_value_take_boxed (value, slaves);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
dispose (GObject *object)
{
	teamd_cleanup (NM_DEVICE (object), FALSE);

	G_OBJECT_CLASS (nm_device_team_parent_class)->dispose (object);
}

static void
nm_device_team_class_init (NMDeviceTeamClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *parent_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDeviceTeamPrivate));

	parent_class->connection_type = NM_SETTING_TEAM_SETTING_NAME;

	/* virtual methods */
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->dispose = dispose;

	parent_class->get_generic_capabilities = get_generic_capabilities;
	parent_class->is_available = is_available;
	parent_class->check_connection_compatible = check_connection_compatible;
	parent_class->check_connection_available = check_connection_available;
	parent_class->complete_connection = complete_connection;
	parent_class->update_connection = update_connection;
	parent_class->master_update_slave_connection = master_update_slave_connection;

	parent_class->act_stage1_prepare = act_stage1_prepare;
	parent_class->deactivate = deactivate;
	parent_class->enslave_slave = enslave_slave;
	parent_class->release_slave = release_slave;

	/* properties */
	g_object_class_install_property
		(object_class, PROP_SLAVES,
		 g_param_spec_boxed (NM_DEVICE_TEAM_SLAVES, "", "",
		                     DBUS_TYPE_G_ARRAY_OF_OBJECT_PATH,
		                     G_PARAM_READABLE |
		                     G_PARAM_STATIC_STRINGS));

	nm_dbus_manager_register_exported_type (nm_dbus_manager_get (),
	                                        G_TYPE_FROM_CLASS (klass),
	                                        &dbus_glib_nm_device_team_object_info);
}
