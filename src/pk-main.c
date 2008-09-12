/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2008 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "egg-debug.h"
#include "pk-conf.h"
#include "pk-engine.h"
#include "pk-transaction.h"
#include "pk-backend-internal.h"
#include "pk-interface.h"

static guint exit_idle_time;
static PkEngine *engine = NULL;
static PkBackend *backend = NULL;

/**
 * pk_object_register:
 * @connection: What we want to register to
 * @object: The GObject we want to register
 *
 * Register org.freedesktop.PackageKit on the system bus.
 * This function MUST be called before DBUS service will work.
 *
 * Return value: success
 **/
G_GNUC_WARN_UNUSED_RESULT static gboolean
pk_object_register (DBusGConnection *connection, GObject *object, GError **error)
{
	DBusGProxy *bus_proxy = NULL;
	guint request_name_result;
	gboolean ret;
	gchar *message;

	bus_proxy = dbus_g_proxy_new_for_name (connection,
					       DBUS_SERVICE_DBUS,
					       DBUS_PATH_DBUS,
					       DBUS_INTERFACE_DBUS);

	ret = dbus_g_proxy_call (bus_proxy, "RequestName", error,
				 G_TYPE_STRING, PK_DBUS_SERVICE,
				 G_TYPE_UINT, 0,
				 G_TYPE_INVALID,
				 G_TYPE_UINT, &request_name_result,
				 G_TYPE_INVALID);

	/* free the bus_proxy */
	g_object_unref (G_OBJECT (bus_proxy));

	/* abort as the DBUS method failed */
	if (!ret) {
		egg_warning ("RequestName failed!");
		g_clear_error (error);
		message = g_strdup_printf ("%s\n%s\n* %s\n* %s\n",
					   _("Startup failed due to security policies on this machine."),
					   _("This can happen for two reasons:"),
					   _("The correct user is not launching the executable (usually root)"),
					   _("The org.freedesktop.PackageKit.conf file is not "
					     "installed in the system /etc/dbus-1/system.d directory"));
		g_set_error (error, PK_ENGINE_ERROR, PK_ENGINE_ERROR_DENIED, "%s", message);
		g_free (message);
		return FALSE;
	}

	/* already running */
 	if (request_name_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		g_set_error (error, PK_ENGINE_ERROR, PK_ENGINE_ERROR_DENIED,
			     "Already running on this machine");
		return FALSE;
	}

	dbus_g_object_type_install_info (PK_TYPE_ENGINE, &dbus_glib_pk_engine_object_info);
	dbus_g_error_domain_register (PK_ENGINE_ERROR, NULL, PK_ENGINE_TYPE_ERROR);
	dbus_g_error_domain_register (PK_TRANSACTION_ERROR, NULL, PK_TRANSACTION_TYPE_ERROR);
	dbus_g_connection_register_g_object (connection, PK_DBUS_PATH, object);

	return TRUE;
}

/**
 * timed_exit_cb:
 * @loop: The main loop
 *
 * Exits the main loop, which is helpful for valgrinding g-p-m.
 *
 * Return value: FALSE, as we don't want to repeat this action.
 **/
static gboolean
timed_exit_cb (GMainLoop *loop)
{
	g_main_loop_quit (loop);
	return FALSE;
}

/**
 * pk_main_timeout_check_cb:
 **/
static gboolean
pk_main_timeout_check_cb (PkEngine *engine)
{
	guint idle;
	idle = pk_engine_get_seconds_idle (engine);
	egg_debug ("idle is %i", idle);
	if (idle > exit_idle_time) {
		egg_warning ("exit!!");
		exit (0);
	}
	return TRUE;
}

/**
 * pk_main_sigint_handler:
 **/
static void
pk_main_sigint_handler (int sig)
{
	egg_debug ("Handling SIGINT");

	/* restore default ASAP, as the finalisers might hang */
	signal (SIGINT, SIG_DFL);

	/* cleanup */
	g_object_unref (backend);
	g_object_unref (engine);

	/* give the backend a sporting chance */
	g_usleep (500*1000);

	/* kill ourselves */
	egg_debug ("Retrying SIGINT");
	kill (getpid (), SIGINT);
}

/**
 * main:
 **/
int
main (int argc, char *argv[])
{
	GMainLoop *loop;
	DBusGConnection *system_connection;
	gboolean ret;
	gboolean verbose = FALSE;
	gboolean disable_timer = FALSE;
	gboolean version = FALSE;
	gboolean use_daemon = FALSE;
	gboolean timed_exit = FALSE;
	gboolean immediate_exit = FALSE;
	gboolean do_logging = FALSE;
	gchar *backend_name = NULL;
	PkConf *conf = NULL;
	GError *error = NULL;
	GOptionContext *context;

	const GOptionEntry options[] = {
		{ "backend", '\0', 0, G_OPTION_ARG_STRING, &backend_name,
		  _("Packaging backend to use, e.g. dummy"), NULL },
		{ "daemonize", '\0', 0, G_OPTION_ARG_NONE, &use_daemon,
		  _("Daemonize and detach from the terminal"), NULL },
		{ "verbose", '\0', 0, G_OPTION_ARG_NONE, &verbose,
		  _("Show extra debugging information"), NULL },
		{ "disable-timer", '\0', 0, G_OPTION_ARG_NONE, &disable_timer,
		  _("Disable the idle timer"), NULL },
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &version,
		  _("Show version and exit"), NULL },
		{ "timed-exit", '\0', 0, G_OPTION_ARG_NONE, &timed_exit,
		  _("Exit after a small delay"), NULL },
		{ "immediate-exit", '\0', 0, G_OPTION_ARG_NONE, &immediate_exit,
		  _("Exit after the engine has loaded"), NULL },
		{ NULL}
	};

	if (! g_thread_supported ()) {
		g_thread_init (NULL);
	}
	dbus_g_thread_init ();
	g_type_init ();

	context = g_option_context_new (_("PackageKit service"));
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);
	egg_debug_init (verbose);

	if (version) {
		g_print ("Version %s\n", VERSION);
		goto exit_program;
	}

	/* do stuff on ctrl-c */
	signal (SIGINT, pk_main_sigint_handler);

	/* we need to daemonize before we get a system connection */
	if (use_daemon && daemon (0, 0)) {
		g_print ("Could not daemonize: %s\n", g_strerror (errno));
		goto exit_program;
	}

	/* don't let GIO start it's own session bus: http://bugzilla.gnome.org/show_bug.cgi?id=526454 */
	setenv ("GIO_USE_VFS", "local", 1);

	/* check dbus connections, exit if not valid */
	system_connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error) {
		g_print ("%s: %s\n", _("Cannot connect to the system bus"), error->message);
		g_error_free (error);
		goto exit_program;
	}

	/* we don't actually need to do this, except it rules out the
	 * 'it works from the command line but not service activation' bugs */
	clearenv ();

	/* get values from the config file */
	conf = pk_conf_new ();

	/* do we log? */
	do_logging = pk_conf_get_bool (conf, "TransactionLogging");
	egg_debug ("Log all transactions: %i", do_logging);
	egg_debug_set_logging (do_logging);

	/* after how long do we timeout? */
	exit_idle_time = pk_conf_get_int (conf, "ShutdownTimeout");
	egg_debug ("daemon shutdown set to %i seconds", exit_idle_time);

	if (backend_name == NULL) {
		backend_name = pk_conf_get_string (conf, "DefaultBackend");
		egg_debug ("using default backend %s", backend_name);
	}

	/* load our chosen backend */
	backend = pk_backend_new ();
	ret = pk_backend_set_name (backend, backend_name);
	g_free (backend_name);

	/* all okay? */
	if (!ret) {
		egg_error ("cannot continue, backend invalid");
	}

	/* create a new engine object */
	engine = pk_engine_new ();

	if (!pk_object_register (system_connection, G_OBJECT (engine), &error)) {
		g_print (_("Error trying to start: %s\n"), error->message);
		g_error_free (error);
		goto out;
	}

	loop = g_main_loop_new (NULL, FALSE);

	/* Only timeout and close the mainloop if we have specified it
	 * on the command line */
	if (timed_exit)
		g_timeout_add_seconds (20, (GSourceFunc) timed_exit_cb, loop);

	/* only poll every 10 seconds when we are alive */
	if (exit_idle_time != 0 && !disable_timer)
		g_timeout_add_seconds (5, (GSourceFunc) pk_main_timeout_check_cb, engine);

	/* immediatly exit */
	if (immediate_exit)
		g_timeout_add (50, (GSourceFunc) timed_exit_cb, loop);

	g_main_loop_run (loop);
	g_main_loop_unref (loop);

out:
	g_object_unref (conf);
	g_object_unref (engine);
	g_object_unref (backend);

exit_program:
	return 0;
}
