/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */

/*
 *  Caja
 *
 *  Copyright (C) 1999, 2000 Red Hat, Inc.
 *  Copyright (C) 2000, 2001 Eazel, Inc.
 *
 *  Caja is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  Caja is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Authors: Elliot Lee <sopwith@redhat.com>,
 *           Darin Adler <darin@bentspoon.com>,
 *          Cosimo Cecchi <cosimoc@gnome.org> (some caja commits post 2.32)
 *
 */

#include <config.h>

#include "caja-application.h"

#include "file-manager/fm-desktop-icon-view.h"
#include "file-manager/fm-icon-view.h"
#include "file-manager/fm-list-view.h"
#include "file-manager/fm-tree-view.h"
#if ENABLE_EMPTY_VIEW
#include "file-manager/fm-empty-view.h"
#endif /* ENABLE_EMPTY_VIEW */

#include "caja-application-smclient.h"
#include "caja-desktop-window.h"
#include "caja-history-sidebar.h"
#include "caja-image-properties-page.h"
#include "caja-navigation-window.h"
#include "caja-navigation-window-slot.h"
#include "caja-notes-viewer.h"
#include "caja-places-sidebar.h"
#include "caja-self-check-functions.h"
#include "caja-spatial-window.h"
#include "caja-window-bookmarks.h"
#include "caja-window-manage-views.h"
#include "caja-window-private.h"
#include "caja-window-slot.h"

#include <libcaja-private/caja-autorun.h>
#include <libcaja-private/caja-debug-log.h>
#include <libcaja-private/caja-desktop-link-monitor.h>
#include <libcaja-private/caja-directory-private.h>
#include <libcaja-private/caja-file-utilities.h>
#include <libcaja-private/caja-file-operations.h>
#include <libcaja-private/caja-global-preferences.h>
#include <libcaja-private/caja-lib-self-check-functions.h>
#include <libcaja-private/caja-module.h>
#include <libcaja-private/caja-signaller.h>
#include <libcaja-extension/caja-menu-provider.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-gtk-macros.h>
#include <eel/eel-stock-dialogs.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

/* Keep window from shrinking down ridiculously small; numbers are somewhat arbitrary */
#define APPLICATION_WINDOW_MIN_WIDTH	300
#define APPLICATION_WINDOW_MIN_HEIGHT	100

#define START_STATE_CONFIG "start-state"

#define CAJA_ACCEL_MAP_SAVE_DELAY 30

static CajaApplication *singleton = NULL;

/* Keeps track of all the desktop windows. */
static GList *caja_application_desktop_windows;

/* Keeps track of all the object windows */
static GList *caja_application_spatial_window_list;

/* The saving of the accelerator map was requested  */
static gboolean save_of_accel_map_requested = FALSE;

static void     desktop_changed_callback          (gpointer                  user_data);
static void     mount_removed_callback            (GVolumeMonitor            *monitor,
						   GMount                    *mount,
						   CajaApplication       *application);
static void     mount_added_callback              (GVolumeMonitor            *monitor,
						   GMount                    *mount,
						   CajaApplication       *application);
static void     volume_added_callback              (GVolumeMonitor           *monitor,
						    GVolume                  *volume,
						    CajaApplication      *application);
static void     drive_connected_callback           (GVolumeMonitor           *monitor,
						    GDrive                   *drive,
						    CajaApplication      *application);
static void     drive_listen_for_eject_button      (GDrive *drive, 
						    CajaApplication *application);

G_DEFINE_TYPE (CajaApplication, caja_application, GTK_TYPE_APPLICATION);

GList *
caja_application_get_window_list (void)
{
    return caja_application_spatial_window_list;
}


static GList *
caja_application_get_spatial_window_list (void)
{
	return caja_application_spatial_window_list;
}

static void
startup_volume_mount_cb (GObject *source_object,
			 GAsyncResult *res,
			 gpointer user_data)
{
	g_volume_mount_finish (G_VOLUME (source_object), res, NULL);
}

static void
automount_all_volumes (CajaApplication *application)
{
	GList *volumes, *l;
	GMount *mount;
	GVolume *volume;

	if (g_settings_get_boolean (caja_media_preferences, CAJA_PREFERENCES_MEDIA_AUTOMOUNT)) {
		/* automount all mountable volumes at start-up */
		volumes = g_volume_monitor_get_volumes (application->volume_monitor);
		for (l = volumes; l != NULL; l = l->next) {
			volume = l->data;
			
			if (!g_volume_should_automount (volume) ||
			    !g_volume_can_mount (volume)) {
				continue;
			}
			
			mount = g_volume_get_mount (volume);
			if (mount != NULL) {
				g_object_unref (mount);
				continue;
			}

			/* pass NULL as GMountOperation to avoid user interaction */
			g_volume_mount (volume, 0, NULL, NULL, startup_volume_mount_cb, NULL);
		}
		g_list_free (volumes);
	}
	
}

static gboolean
check_required_directories (CajaApplication *application)
{
	char *user_directory;
	char *desktop_directory;
	GSList *directories;
	gboolean ret;

	g_assert (CAJA_IS_APPLICATION (application));

	ret = TRUE;

	user_directory = caja_get_user_directory ();
	desktop_directory = caja_get_desktop_directory ();

	directories = NULL;

	if (!g_file_test (user_directory, G_FILE_TEST_IS_DIR)) {
		directories = g_slist_prepend (directories, user_directory);
	}

	if (!g_file_test (desktop_directory, G_FILE_TEST_IS_DIR)) {
		directories = g_slist_prepend (directories, desktop_directory);
	}

	if (directories != NULL) {
		int failed_count;
		GString *directories_as_string;
		GSList *l;
		char *error_string;
		const char *detail_string;
		GtkDialog *dialog;

		ret = FALSE;

		failed_count = g_slist_length (directories);

		directories_as_string = g_string_new ((const char *)directories->data);
		for (l = directories->next; l != NULL; l = l->next) {
			g_string_append_printf (directories_as_string, ", %s", (const char *)l->data);
		}

		if (failed_count == 1) {
			error_string = g_strdup_printf (_("Caja could not create the required folder \"%s\"."),
							directories_as_string->str);
			detail_string = _("Before running Caja, please create the following folder, or "
					  "set permissions such that Caja can create it.");
		} else {
			error_string = g_strdup_printf (_("Caja could not create the following required folders: "
							  "%s."), directories_as_string->str);
			detail_string = _("Before running Caja, please create these folders, or "
					  "set permissions such that Caja can create them.");
		}

		dialog = eel_show_error_dialog (error_string, detail_string, NULL);
		/* We need the main event loop so the user has a chance to see the dialog. */
		gtk_application_add_window (GTK_APPLICATION (application),
					    GTK_WINDOW (dialog));

		g_string_free (directories_as_string, TRUE);
		g_free (error_string);
	}

	g_slist_free (directories);
	g_free (user_directory);
	g_free (desktop_directory);

	return ret;
}

static void
menu_provider_items_updated_handler (CajaMenuProvider *provider, GtkWidget* parent_window, gpointer data)
{

	g_signal_emit_by_name (caja_signaller_get_current (),
			       "popup_menu_changed");
}

static void
menu_provider_init_callback (void)
{
        GList *items;
        GList *providers;
        GList *l;

        providers = caja_module_get_extensions_for_type (CAJA_TYPE_MENU_PROVIDER);
        items = NULL;

        for (l = providers; l != NULL; l = l->next) {
                CajaMenuProvider *provider = CAJA_MENU_PROVIDER (l->data);

		g_signal_connect_after (G_OBJECT (provider), "items_updated",
                           (GCallback)menu_provider_items_updated_handler,
                           NULL);
        }

        caja_module_extension_list_free (providers);
}

static gboolean
automount_all_volumes_idle_cb (gpointer data)
{
	CajaApplication *application = CAJA_APPLICATION (data);

	automount_all_volumes (application);

	application->automount_idle_id = 0;
	return FALSE;
}

static void
mark_desktop_files_trusted (void)
{
	char *do_once_file;
	GFile *f, *c;
	GFileEnumerator *e;
	GFileInfo *info;
	const char *name;
	int fd;
	
	do_once_file = g_build_filename (g_get_user_data_dir (),
					 ".converted-launchers", NULL);

	if (g_file_test (do_once_file, G_FILE_TEST_EXISTS)) {
		goto out;
	}

	f = caja_get_desktop_location ();
	e = g_file_enumerate_children (f,
				       G_FILE_ATTRIBUTE_STANDARD_TYPE ","
				       G_FILE_ATTRIBUTE_STANDARD_NAME ","
				       G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
				       ,
				       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				       NULL, NULL);
	if (e == NULL) {
		goto out2;
	}
	
	while ((info = g_file_enumerator_next_file (e, NULL, NULL)) != NULL) {
		name = g_file_info_get_name (info);
		
		if (g_str_has_suffix (name, ".desktop") &&
		    !g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)) {
			c = g_file_get_child (f, name);
			caja_file_mark_desktop_file_trusted (c,
								 NULL, FALSE,
								 NULL, NULL);
			g_object_unref (c);
		}
		g_object_unref (info);
	}
	
	g_object_unref (e);
 out2:
	fd = g_creat (do_once_file, 0666);
	close (fd);
	
	g_object_unref (f);
 out:	
	g_free (do_once_file);
}

#define CK_NAME       "org.freedesktop.ConsoleKit"
#define CK_PATH       "/org/freedesktop/ConsoleKit"
#define CK_INTERFACE  "org.freedesktop.ConsoleKit"

static void
ck_session_proxy_signal_cb (GDBusProxy *proxy,
			    const char *sender_name,
			    const char *signal_name,
			    GVariant   *parameters,
			    gpointer    user_data)
{
	CajaApplication *application = user_data;

	if (g_strcmp0 (signal_name, "ActiveChanged") == 0) {
		g_variant_get (parameters, "(b)", &application->session_is_active);
	}
}

static void
ck_call_is_active_cb (GDBusProxy   *proxy,
		      GAsyncResult *result,
		      gpointer      user_data)
{
	CajaApplication *application = user_data;
	GVariant *variant;
	GError *error = NULL;

	variant = g_dbus_proxy_call_finish (proxy, result, &error);

	if (variant == NULL) {
		g_warning ("Error when calling IsActive(): %s\n", error->message);
		application->session_is_active = TRUE;

		g_error_free (error);
		return;
	}

	g_variant_get (variant, "(b)", &application->session_is_active);

	g_variant_unref (variant);
}

static void
session_proxy_appeared (GObject       *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
	CajaApplication *application = user_data;
        GDBusProxy *proxy;
	GError *error = NULL;

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);

	if (error != NULL) {
		g_warning ("Failed to get the current CK session: %s", error->message);
		g_error_free (error);

		application->session_is_active = TRUE;
		return;
	}

	g_signal_connect (proxy, "g-signal",
			  G_CALLBACK (ck_session_proxy_signal_cb),
			  application);

	g_dbus_proxy_call (proxy,
			   "IsActive",
			   g_variant_new ("()"),
			   G_DBUS_CALL_FLAGS_NONE,
			   -1,
			   NULL,
			   (GAsyncReadyCallback) ck_call_is_active_cb,
			   application);

        application->proxy = proxy;
}

static void
ck_get_current_session_cb (GDBusConnection *connection,
			   GAsyncResult    *result,
			   gpointer         user_data)
{
	CajaApplication *application = user_data;
	GVariant *variant;
	const char *session_path = NULL;
	GError *error = NULL;

	variant = g_dbus_connection_call_finish (connection, result, &error);

	if (variant == NULL) {
		g_warning ("Failed to get the current CK session: %s", error->message);
		g_error_free (error);

		application->session_is_active = TRUE;
		return;
	}

	g_variant_get (variant, "(&o)", &session_path);

	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
				  G_DBUS_PROXY_FLAGS_NONE,
				  NULL,
				  CK_NAME,
				  session_path,
				  CK_INTERFACE ".Session",
				  NULL,
				  session_proxy_appeared,
				  application);

	g_variant_unref (variant);
}

static void
do_initialize_consolekit (CajaApplication *application)
{
	GDBusConnection *connection;

	connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);

	if (connection == NULL) {
		application->session_is_active = TRUE;
		return;
	}

	g_dbus_connection_call (connection,
				CK_NAME,
				CK_PATH "/Manager",
				CK_INTERFACE ".Manager",
				"GetCurrentSession",
				g_variant_new ("()"),
				G_VARIANT_TYPE ("(o)"),
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				NULL,
				(GAsyncReadyCallback) ck_get_current_session_cb,
				application);

	g_object_unref (connection);
}

static void
do_upgrades_once (CajaApplication *application,
		  gboolean no_desktop)
{
	char *metafile_dir, *updated, *caja_dir, *xdg_dir;
	const gchar *message;
	int fd, res;

	if (!no_desktop) {
		mark_desktop_files_trusted ();
	}

	metafile_dir = g_build_filename (g_get_home_dir (),
					 ".caja/metafiles", NULL);
	if (g_file_test (metafile_dir, G_FILE_TEST_IS_DIR)) {
		updated = g_build_filename (metafile_dir, "migrated-to-gvfs", NULL);
		if (!g_file_test (updated, G_FILE_TEST_EXISTS)) {
			g_spawn_command_line_async (LIBEXECDIR"/caja-convert-metadata --quiet", NULL);
			fd = g_creat (updated, 0600);
			if (fd != -1) {
				close (fd);
			}
		}
		g_free (updated);
	}
	g_free (metafile_dir);

	caja_dir = g_build_filename (g_get_home_dir (),
					 ".caja", NULL);
	xdg_dir = caja_get_user_directory ();
	if (g_file_test (caja_dir, G_FILE_TEST_IS_DIR)) {
		/* test if we already attempted to migrate first */
		updated = g_build_filename (caja_dir, "DEPRECATED-DIRECTORY", NULL);
		message = _("Caja 3.0 deprecated this directory and tried migrating "
			    "this configuration to ~/.config/caja");
		if (!g_file_test (updated, G_FILE_TEST_EXISTS)) {
			/* rename() works fine if the destination directory is
			 * empty.
			 */
			res = g_rename (caja_dir, xdg_dir);

			if (res == -1) {
				fd = g_creat (updated, 0600);
				if (fd != -1) {
					res = write (fd, message, strlen (message));
					close (fd);
				}
			}
		}

		g_free (updated);
	}

	g_free (caja_dir);
	g_free (xdg_dir);
}

static void
finish_startup (CajaApplication *application,
		gboolean no_desktop)
{
	GList *drives;

	do_upgrades_once (application, no_desktop);
	
	/* initialize caja modules */
	caja_module_setup ();

	/* attach menu-provider module callback */
	menu_provider_init_callback ();
	
	/* Initialize the desktop link monitor singleton */
	caja_desktop_link_monitor_get ();

	/* Initialize the ConsoleKit listener for active session */
	do_initialize_consolekit (application);

	/* Watch for mounts so we can restore open windows This used
	 * to be for showing new window on mount, but is not used
	 * anymore */

	/* Watch for unmounts so we can close open windows */
	/* TODO-gio: This should be using the UNMOUNTED feature of GFileMonitor instead */
	application->volume_monitor = g_volume_monitor_get ();
	g_signal_connect_object (application->volume_monitor, "mount_removed",
				 G_CALLBACK (mount_removed_callback), application, 0);
	g_signal_connect_object (application->volume_monitor, "mount_pre_unmount",
				 G_CALLBACK (mount_removed_callback), application, 0);
	g_signal_connect_object (application->volume_monitor, "mount_added",
				 G_CALLBACK (mount_added_callback), application, 0);
	g_signal_connect_object (application->volume_monitor, "volume_added",
				 G_CALLBACK (volume_added_callback), application, 0);
	g_signal_connect_object (application->volume_monitor, "drive_connected",
				 G_CALLBACK (drive_connected_callback), application, 0);

	/* listen for eject button presses */
	drives = g_volume_monitor_get_connected_drives (application->volume_monitor);
	g_list_foreach (drives, (GFunc) drive_listen_for_eject_button, application);
	g_list_foreach (drives, (GFunc) g_object_unref, NULL);
	g_list_free (drives);

	application->automount_idle_id = 
		g_idle_add_full (G_PRIORITY_LOW,
				 automount_all_volumes_idle_cb,
				 application, NULL);
}

static void
open_window (CajaApplication *application,
	     const char *startup_id,
	     const char *uri, GdkScreen *screen, const char *geometry,
	     gboolean browser_window)
{
	GFile *location;
	CajaWindow *window;
	gboolean existing;

	if (uri == NULL) {
		location = g_file_new_for_path (g_get_home_dir ());
	} else {
		location = g_file_new_for_uri (uri);
	}

	existing = FALSE;

	if (browser_window ||
	    g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER)) {
		window = caja_application_create_navigation_window (application,
									startup_id,
									screen);
	} else {
		window = caja_application_get_spatial_window (application,
								  NULL,
								  startup_id,
								  location,
								  screen,
								  NULL);
	}

	caja_window_go_to (window, location);

	g_object_unref (location);

	if (geometry != NULL && !gtk_widget_get_visible (GTK_WIDGET (window))) {
		/* never maximize windows opened from shell if a
		 * custom geometry has been requested.
		 */
		gtk_window_unmaximize (GTK_WINDOW (window));
		eel_gtk_window_set_initial_geometry_from_string (GTK_WINDOW (window),
								 geometry,
								 APPLICATION_WINDOW_MIN_WIDTH,
								 APPLICATION_WINDOW_MIN_HEIGHT,
								 FALSE);
	}
}

static void
open_windows (CajaApplication *application,
	      const char *startup_id,
	      char **uris,
	      GdkScreen *screen,
	      const char *geometry,
	      gboolean browser_window)
{
	guint i;

	if (uris == NULL || uris[0] == NULL) {
		/* Open a window pointing at the default location. */
		open_window (application, startup_id, NULL, screen, geometry, browser_window);
	} else {
		/* Open windows at each requested location. */
		for (i = 0; uris[i] != NULL; i++) {
			open_window (application, startup_id, uris[i], screen, geometry, browser_window);
		}
	}
}

void
caja_application_open_location (CajaApplication *application,
                                GFile *location,
                                GFile *selection,
                                const char *startup_id)
{
    CajaWindow *window;
    GList *sel_list = NULL;

    window = caja_application_create_navigation_window (application, startup_id, gdk_screen_get_default ());

    if (selection != NULL) {
        sel_list = g_list_prepend (NULL, g_object_ref (selection));
    }

    caja_window_slot_open_location_full (caja_window_get_active_slot (window), location,
                                         0, CAJA_WINDOW_OPEN_FLAG_NEW_WINDOW, sel_list, NULL, NULL);

    if (sel_list != NULL) {
        caja_file_list_free (sel_list);
    }
}


static gboolean 
caja_application_save_accel_map (gpointer data)
{
	if (save_of_accel_map_requested) {
		char *accel_map_filename;
	 	accel_map_filename = caja_get_accel_map_file ();
	 	if (accel_map_filename) {
	 		gtk_accel_map_save (accel_map_filename);
	 		g_free (accel_map_filename);
	 	}
		save_of_accel_map_requested = FALSE;
	}

	return FALSE;
}


static void 
queue_accel_map_save_callback (GtkAccelMap *object, gchar *accel_path,
		guint accel_key, GdkModifierType accel_mods,
		gpointer user_data)
{
	if (!save_of_accel_map_requested) {
		save_of_accel_map_requested = TRUE;
		g_timeout_add_seconds (CAJA_ACCEL_MAP_SAVE_DELAY, 
				caja_application_save_accel_map, NULL);
	}
}

static void 
selection_get_cb (GtkWidget          *widget,
		  GtkSelectionData   *selection_data,
		  guint               info,
		  guint               time)
{
	/* No extra targets atm */
}

static GtkWidget *
get_desktop_manager_selection (GdkDisplay *display, int screen)
{
	char selection_name[32];
	GdkAtom selection_atom;
	Window selection_owner;
	GtkWidget *selection_widget;

	g_snprintf (selection_name, sizeof (selection_name), "_NET_DESKTOP_MANAGER_S%d", screen);
	selection_atom = gdk_atom_intern (selection_name, FALSE);

	selection_owner = XGetSelectionOwner (GDK_DISPLAY_XDISPLAY (display),
					      gdk_x11_atom_to_xatom_for_display (display, 
										 selection_atom));
	if (selection_owner != None) {
		return NULL;
	}
	
	selection_widget = gtk_invisible_new_for_screen (gdk_display_get_screen (display, screen));
	/* We need this for gdk_x11_get_server_time() */
	gtk_widget_add_events (selection_widget, GDK_PROPERTY_CHANGE_MASK);

	if (gtk_selection_owner_set_for_display (display,
						 selection_widget,
						 selection_atom,
						 gdk_x11_get_server_time (gtk_widget_get_window (selection_widget)))) {
		
		g_signal_connect (selection_widget, "selection_get",
				  G_CALLBACK (selection_get_cb), NULL);
		return selection_widget;
	}

	gtk_widget_destroy (selection_widget);
	
	return NULL;
}

static void
desktop_unrealize_cb (GtkWidget        *widget,
		      GtkWidget        *selection_widget)
{
	gtk_widget_destroy (selection_widget);
}

static gboolean
selection_clear_event_cb (GtkWidget	        *widget,
			  GdkEventSelection     *event,
			  CajaDesktopWindow *window)
{
	gtk_widget_destroy (GTK_WIDGET (window));
	
	caja_application_desktop_windows =
		g_list_remove (caja_application_desktop_windows, window);

	return TRUE;
}

static void
caja_application_create_desktop_windows (CajaApplication *application)
{
	GdkDisplay *display;
	CajaDesktopWindow *window;
	GtkWidget *selection_widget;
	int screens, i;
	gboolean exit_with_last_window;


	display = gdk_display_get_default ();
	screens = gdk_display_get_n_screens (display);

	exit_with_last_window =
		g_settings_get_boolean (caja_preferences,
					CAJA_PREFERENCES_EXIT_WITH_LAST_WINDOW);

	for (i = 0; i < screens; i++) {
		selection_widget = get_desktop_manager_selection (display, i);
		if (selection_widget != NULL) {
			window = caja_desktop_window_new (application,
							      gdk_display_get_screen (display, i));
			
			g_signal_connect (selection_widget, "selection_clear_event",
					  G_CALLBACK (selection_clear_event_cb), window);
			
			g_signal_connect (window, "unrealize",
					  G_CALLBACK (desktop_unrealize_cb), selection_widget);
			
			/* We realize it immediately so that the CAJA_DESKTOP_WINDOW_ID
			   property is set so mate-settings-daemon doesn't try to set the
			   background. And we do a gdk_flush() to be sure X gets it. */
			gtk_widget_realize (GTK_WIDGET (window));
			gdk_flush ();

			
			caja_application_desktop_windows =
				g_list_prepend (caja_application_desktop_windows, window);

			gtk_application_add_window (GTK_APPLICATION (application),
										GTK_WINDOW (window));
			/* don't add the desktop windows to the GtkApplication hold toplevels
			 * if we should exit when the last window is closed.
			 */
			if (!exit_with_last_window) {
				gtk_application_add_window (GTK_APPLICATION (application),
							    GTK_WINDOW (window));
			}
		}
	}
}

static void
caja_application_open_desktop (CajaApplication *application)
{
	if (caja_application_desktop_windows == NULL) {
		caja_application_create_desktop_windows (application);
	}
}

static void
caja_application_close_desktop (void)
{
	if (caja_application_desktop_windows != NULL) {
		g_list_foreach (caja_application_desktop_windows,
				(GFunc) gtk_widget_destroy, NULL);
		g_list_free (caja_application_desktop_windows);
		caja_application_desktop_windows = NULL;
	}
}

void
caja_application_close_all_navigation_windows (CajaApplication *self)
{
	GList *list_copy;
	GList *l;
	
	list_copy = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (self)));
	/* First hide all window to get the feeling of quick response */
	for (l = list_copy; l != NULL; l = l->next) {
		CajaWindow *window;
		
		window = CAJA_WINDOW (l->data);

		if (CAJA_IS_NAVIGATION_WINDOW (window)) {
			gtk_widget_hide (GTK_WIDGET (window));
		}
	}

	for (l = list_copy; l != NULL; l = l->next) {
		CajaWindow *window;
		
		window = CAJA_WINDOW (l->data);
		
		if (CAJA_IS_NAVIGATION_WINDOW (window)) {
			caja_window_close (window);
		}
	}
	g_list_free (list_copy);
}

static CajaSpatialWindow *
caja_application_get_existing_spatial_window (GFile *location)
{
	GList *l;
	CajaWindowSlot *slot;
	GFile *window_location;

	for (l = caja_application_get_spatial_window_list ();
	     l != NULL; l = l->next) {
		slot = CAJA_WINDOW (l->data)->details->active_pane->active_slot;

		window_location = slot->pending_location;
		
		if (window_location == NULL) {
			window_location = slot->location;
		}

		if (window_location != NULL) {
			if (g_file_equal (location, window_location)) {
				return CAJA_SPATIAL_WINDOW (l->data);
			}
		}
	}

	return NULL;
}

static CajaSpatialWindow *
find_parent_spatial_window (CajaSpatialWindow *window)
{
	CajaFile *file;
	CajaFile *parent_file;
	CajaWindowSlot *slot;
	GFile *location;

	slot = CAJA_WINDOW (window)->details->active_pane->active_slot;

	location = slot->location;
	if (location == NULL) {
		return NULL;
	}
	file = caja_file_get (location);

	if (!file) {
		return NULL;
	}

	parent_file = caja_file_get_parent (file);
	caja_file_unref (file);
	while (parent_file) {
		CajaSpatialWindow *parent_window;

		location = caja_file_get_location (parent_file);
		parent_window = caja_application_get_existing_spatial_window (location);
		g_object_unref (location);

		/* Stop at the desktop directory if it's not explicitely opened
		 * in a spatial window of its own.
		 */
		if (caja_file_is_desktop_directory (parent_file) && !parent_window) {
			caja_file_unref (parent_file);
			return NULL;
		}

		if (parent_window) {
			caja_file_unref (parent_file);
			return parent_window;
		}
		file = parent_file;
		parent_file = caja_file_get_parent (file);
		caja_file_unref (file);
	}

	return NULL;
}

void
caja_application_close_parent_windows (CajaSpatialWindow *window)
{
	CajaSpatialWindow *parent_window;
	CajaSpatialWindow *new_parent_window;

	g_return_if_fail (CAJA_IS_SPATIAL_WINDOW (window));

	parent_window = find_parent_spatial_window (window);
	
	while (parent_window) {
		
		new_parent_window = find_parent_spatial_window (parent_window);
		caja_window_close (CAJA_WINDOW (parent_window));
		parent_window = new_parent_window;
	}
}

void
caja_application_close_all_spatial_windows (void)
{
	GList *list_copy;
	GList *l;
	
	list_copy = g_list_copy (caja_application_spatial_window_list);
	/* First hide all window to get the feeling of quick response */
	for (l = list_copy; l != NULL; l = l->next) {
		CajaWindow *window;
		
		window = CAJA_WINDOW (l->data);
		
		if (CAJA_IS_SPATIAL_WINDOW (window)) {
			gtk_widget_hide (GTK_WIDGET (window));
		}
	}

	for (l = list_copy; l != NULL; l = l->next) {
		CajaWindow *window;
		
		window = CAJA_WINDOW (l->data);
		
		if (CAJA_IS_SPATIAL_WINDOW (window)) {
			caja_window_close (window);
		}
	}
	g_list_free (list_copy);
}

static gboolean
caja_window_delete_event_callback (GtkWidget *widget,
				       GdkEvent *event,
				       gpointer user_data)
{
	CajaWindow *window;

	window = CAJA_WINDOW (widget);
	caja_window_close (window);

	return TRUE;
}				       


static CajaWindow *
create_window (CajaApplication *application,
	       GType window_type,
	       const char *startup_id,
	       GdkScreen *screen)
{
	CajaWindow *window;
	
	g_return_val_if_fail (CAJA_IS_APPLICATION (application), NULL);
	
	window = CAJA_WINDOW (gtk_widget_new (window_type,
						  "app", application,
						  "screen", screen,
						  NULL));

	if (startup_id) {
		gtk_window_set_startup_id (GTK_WINDOW (window), startup_id);
	}
	
	g_signal_connect_data (window, "delete_event",
			       G_CALLBACK (caja_window_delete_event_callback), NULL, NULL,
			       G_CONNECT_AFTER);

	gtk_application_add_window (GTK_APPLICATION (application),
				    GTK_WINDOW (window));

	/* Do not yet show the window. It will be shown later on if it can
	 * successfully display its initial URI. Otherwise it will be destroyed
	 * without ever having seen the light of day.
	 */

	return window;
}

static void
spatial_window_destroyed_callback (void *user_data, GObject *window)
{
	caja_application_spatial_window_list = g_list_remove (caja_application_spatial_window_list, window);
		
}

CajaWindow *
caja_application_get_spatial_window (CajaApplication *application,
					 CajaWindow      *requesting_window,
					 const char          *startup_id,
					 GFile               *location,
					 GdkScreen           *screen,
					 gboolean            *existing)
{
	CajaWindow *window;
	gchar *uri;

	g_return_val_if_fail (CAJA_IS_APPLICATION (application), NULL);

	window = CAJA_WINDOW
		(caja_application_get_existing_spatial_window (location));

	if (window != NULL) {
		if (existing != NULL) {
			*existing = TRUE;
		}

		return window;
	}

	if (existing != NULL) {
		*existing = FALSE;
	}

	window = create_window (application, CAJA_TYPE_SPATIAL_WINDOW, startup_id, screen);
	if (requesting_window) {
		/* Center the window over the requesting window by default */
		int orig_x, orig_y, orig_width, orig_height;
		int new_x, new_y, new_width, new_height;
		
		gtk_window_get_position (GTK_WINDOW (requesting_window), 
					 &orig_x, &orig_y);
		gtk_window_get_size (GTK_WINDOW (requesting_window), 
				     &orig_width, &orig_height);
		gtk_window_get_default_size (GTK_WINDOW (window),
					     &new_width, &new_height);
		
		new_x = orig_x + (orig_width - new_width) / 2;
		new_y = orig_y + (orig_height - new_height) / 2;
		
		if (orig_width - new_width < 10) {
			new_x += 10;
			new_y += 10;
		}

		gtk_window_move (GTK_WINDOW (window), new_x, new_y);
	}

	caja_application_spatial_window_list = g_list_prepend (caja_application_spatial_window_list, window);
	g_object_weak_ref (G_OBJECT (window), 
			   spatial_window_destroyed_callback, NULL);

	uri = g_file_get_uri (location);
	caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
			    "present NEW spatial window=%p: %s",
			    window, uri);
	g_free (uri);
	
	return window;
}

static gboolean
another_navigation_window_already_showing (CajaApplication *application,
					   CajaWindow *the_window)
{
	GList *list, *item;
	
	list = gtk_application_get_windows (GTK_APPLICATION (application));
	for (item = list; item != NULL; item = item->next) {
		if (item->data != the_window &&
		    CAJA_IS_NAVIGATION_WINDOW (item->data)) {
			return TRUE;
		}
	}
	
	return FALSE;
}

CajaWindow *
caja_application_create_navigation_window (CajaApplication *application,
					       const char          *startup_id,
					       GdkScreen           *screen)
{
	CajaWindow *window;
	char *geometry_string;
	gboolean maximized;

	g_return_val_if_fail (CAJA_IS_APPLICATION (application), NULL);

	window = create_window (application, CAJA_TYPE_NAVIGATION_WINDOW, startup_id, screen);

	maximized = g_settings_get_boolean
		(caja_window_state, CAJA_WINDOW_STATE_MAXIMIZED);
	if (maximized) {
		gtk_window_maximize (GTK_WINDOW (window));
	} else {
		gtk_window_unmaximize (GTK_WINDOW (window));
	}

	geometry_string = g_settings_get_string
		(caja_window_state, CAJA_WINDOW_STATE_GEOMETRY);
	if (geometry_string != NULL &&
	    geometry_string[0] != 0) {
		/* Ignore saved window position if a window with the same
		 * location is already showing. That way the two windows
		 * wont appear at the exact same location on the screen.
		 */
		eel_gtk_window_set_initial_geometry_from_string 
			(GTK_WINDOW (window), 
			 geometry_string,
			 CAJA_NAVIGATION_WINDOW_MIN_WIDTH,
			 CAJA_NAVIGATION_WINDOW_MIN_HEIGHT,
			 another_navigation_window_already_showing (application, window));
	}
	g_free (geometry_string);

	caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
			    "create new navigation window=%p",
			    window);

	return window;
}

/* callback for showing or hiding the desktop based on the user's preference */
static void
desktop_changed_callback (gpointer user_data)
{
	CajaApplication *application;

	application = CAJA_APPLICATION (user_data);
	if (g_settings_get_boolean (mate_background_preferences, MATE_BG_KEY_SHOW_DESKTOP)) {
		caja_application_open_desktop (application);
	} else {
		caja_application_close_desktop ();
	}
}

static gboolean
window_can_be_closed (CajaWindow *window)
{
	if (!CAJA_IS_DESKTOP_WINDOW (window)) {
		return TRUE;
	}
	
	return FALSE;
}

static void
volume_added_callback (GVolumeMonitor *monitor,
		       GVolume *volume,
		       CajaApplication *application)
{
	if (g_settings_get_boolean (caja_media_preferences, CAJA_PREFERENCES_MEDIA_AUTOMOUNT) &&
	    g_volume_should_automount (volume) &&
	    g_volume_can_mount (volume)) {
		caja_file_operations_mount_volume (NULL, volume, TRUE);
	} else {
		/* Allow caja_autorun() to run. When the mount is later
		 * added programmatically (i.e. for a blank CD),
		 * caja_autorun() will be called by mount_added_callback(). */
		caja_allow_autorun_for_volume (volume);
		caja_allow_autorun_for_volume_finish (volume);
	}
}

static void
drive_eject_cb (GObject *source_object,
		GAsyncResult *res,
		gpointer user_data)
{
	GError *error;
	char *primary;
	char *name;
	error = NULL;
	if (!g_drive_eject_with_operation_finish (G_DRIVE (source_object), res, &error)) {
		if (error->code != G_IO_ERROR_FAILED_HANDLED) {
			name = g_drive_get_name (G_DRIVE (source_object));
			primary = g_strdup_printf (_("Unable to eject %s"), name);
			g_free (name);
			eel_show_error_dialog (primary,
					       error->message,
				       NULL);
			g_free (primary);
		}
		g_error_free (error);
	}
}

static void
drive_eject_button_pressed (GDrive *drive,
			    CajaApplication *application)
{
	GMountOperation *mount_op;

	mount_op = gtk_mount_operation_new (NULL);
	g_drive_eject_with_operation (drive, 0, mount_op, NULL, drive_eject_cb, NULL);
	g_object_unref (mount_op);
}

static void
drive_listen_for_eject_button (GDrive *drive, CajaApplication *application)
{
	g_signal_connect (drive,
			  "eject-button",
			  G_CALLBACK (drive_eject_button_pressed),
			  application);
}

static void
drive_connected_callback (GVolumeMonitor *monitor,
			  GDrive *drive,
			  CajaApplication *application)
{
	drive_listen_for_eject_button (drive, application);
}

static void
autorun_show_window (GMount *mount, gpointer user_data)
{
	GFile *location;
	CajaApplication *application = user_data;
	CajaWindow *window;
	gboolean existing;

	location = g_mount_get_root (mount);
	existing = FALSE;

	/* There should probably be an easier way to do this */
	if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER)) {
		window = caja_application_create_navigation_window (application, 
									NULL, 
									gdk_screen_get_default ());
	} else {
		window = caja_application_get_spatial_window (application,
								  NULL,
								  NULL,
								  location,
								  gdk_screen_get_default (),
								  NULL);
	}

	caja_window_go_to (window, location);

	g_object_unref (location);
}

static void
mount_added_callback (GVolumeMonitor *monitor,
		      GMount *mount,
		      CajaApplication *application)
{
	CajaDirectory *directory;
	GFile *root;

	if (!application->session_is_active) {
		return;
	}
		
	root = g_mount_get_root (mount);
	directory = caja_directory_get_existing (root);
	g_object_unref (root);
	if (directory != NULL) {
		caja_directory_force_reload (directory);
		caja_directory_unref (directory);
	}

	caja_autorun (mount, autorun_show_window, application);
}

static CajaWindowSlot *
get_first_navigation_slot (GList *slot_list)
{
	GList *l;

	for (l = slot_list; l != NULL; l = l->next) {
		if (CAJA_IS_NAVIGATION_WINDOW_SLOT (l->data)) {
			return l->data;
		}
	}

	return NULL;
}

/* We redirect some slots and close others */
static gboolean
should_close_slot_with_mount (CajaWindow *window,
			      CajaWindowSlot *slot,
			      GMount *mount)
{
	if (CAJA_IS_SPATIAL_WINDOW (window)) {
		return TRUE;
	}
	return caja_navigation_window_slot_should_close_with_mount (CAJA_NAVIGATION_WINDOW_SLOT (slot),
									mount);
}

/* Called whenever a mount is unmounted. Check and see if there are
 * any windows open displaying contents on the mount. If there are,
 * close them.  It would also be cool to save open window and position
 * info.
 *
 * This is also called on pre_unmount.
 */
static void
mount_removed_callback (GVolumeMonitor *monitor,
			GMount *mount,
			CajaApplication *application)
{
	GList *window_list, *node, *close_list;
	CajaWindow *window;
	CajaWindowSlot *slot;
	CajaWindowSlot *force_no_close_slot;
	GFile *root, *computer;
	gboolean unclosed_slot;

	close_list = NULL;
	force_no_close_slot = NULL;
	unclosed_slot = FALSE;

	/* Check and see if any of the open windows are displaying contents from the unmounted mount */
	window_list = gtk_application_get_windows (GTK_APPLICATION (application));

	root = g_mount_get_root (mount);
	/* Construct a list of windows to be closed. Do not add the non-closable windows to the list. */
	for (node = window_list; node != NULL; node = node->next) {
		window = CAJA_WINDOW (node->data);
		if (window != NULL && window_can_be_closed (window)) {
			GList *l;
			GList *lp;
			GFile *location;

			for (lp = window->details->panes; lp != NULL; lp = lp->next) {
				CajaWindowPane *pane;
				pane = (CajaWindowPane*) lp->data;
				for (l = pane->slots; l != NULL; l = l->next) {
					slot = l->data;
					location = slot->location;
					if (g_file_has_prefix (location, root) ||
					    g_file_equal (location, root)) {
						close_list = g_list_prepend (close_list, slot);

						if (!should_close_slot_with_mount (window, slot, mount)) {
							/* We'll be redirecting this, not closing */
							unclosed_slot = TRUE;
						}
					} else {
						unclosed_slot = TRUE;
					}
				} /* for all slots */
			} /* for all panes */
		}
	}

	if (caja_application_desktop_windows == NULL &&
	    !unclosed_slot) {
		/* We are trying to close all open slots. Keep one navigation slot open. */
		force_no_close_slot = get_first_navigation_slot (close_list);
	}

	/* Handle the windows in the close list. */
	for (node = close_list; node != NULL; node = node->next) {
		slot = node->data;
		window = slot->pane->window;

		if (should_close_slot_with_mount (window, slot, mount) &&
		    slot != force_no_close_slot) {
			caja_window_slot_close (slot);
		} else {
			computer = g_file_new_for_uri ("computer:///");
			caja_window_slot_go_to (slot, computer, FALSE);
			g_object_unref(computer);
		}
	}

	g_list_free (close_list);
}

static GObject *
caja_application_constructor (GType type,
				  guint n_construct_params,
				  GObjectConstructParam *construct_params)
{
        GObject *retval;

        if (singleton != NULL) {
                return g_object_ref (singleton);
        }

        retval = G_OBJECT_CLASS (caja_application_parent_class)->constructor
                (type, n_construct_params, construct_params);

        singleton = CAJA_APPLICATION (retval);
        g_object_add_weak_pointer (retval, (gpointer) &singleton);

        return retval;
}

static void
caja_application_init (CajaApplication *application)
{
	/* do nothing */
}

static void
caja_application_finalize (GObject *object)
{
	CajaApplication *application;

	application = CAJA_APPLICATION (object);

	caja_bookmarks_exiting ();

	if (application->volume_monitor) {
		g_object_unref (application->volume_monitor);
		application->volume_monitor = NULL;
	}

	if (application->automount_idle_id != 0) {
		g_source_remove (application->automount_idle_id);
		application->automount_idle_id = 0;
	}

	if (application->proxy != NULL) {
		g_object_unref (application->proxy);
		application->proxy = NULL;
	}

        G_OBJECT_CLASS (caja_application_parent_class)->finalize (object);
}

static gint
caja_application_command_line (GApplication *app,
				   GApplicationCommandLine *command_line)
{
	gboolean perform_self_check = FALSE;
	gboolean version = FALSE;
	gboolean no_default_window = FALSE;
	gboolean no_desktop = FALSE;
	gboolean browser_window = FALSE;
	gboolean kill_shell = FALSE;
	gboolean autostart_mode = FALSE;
	const gchar *autostart_id;
	gchar *geometry = NULL;
	gchar **remaining = NULL;
	char *accel_map_filename;
	const GOptionEntry options[] = {
#ifndef CAJA_OMIT_SELF_CHECK
		{ "check", 'c', 0, G_OPTION_ARG_NONE, &perform_self_check, 
		  N_("Perform a quick set of self-check tests."), NULL },
#endif
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &version,
		  N_("Show the version of the program."), NULL },
		{ "geometry", 'g', 0, G_OPTION_ARG_STRING, &geometry,
		  N_("Create the initial window with the given geometry."), N_("GEOMETRY") },
		{ "no-default-window", 'n', 0, G_OPTION_ARG_NONE, &no_default_window,
		  N_("Only create windows for explicitly specified URIs."), NULL },
		{ "no-desktop", '\0', 0, G_OPTION_ARG_NONE, &no_desktop,
		  N_("Do not manage the desktop (ignore the preference set in the preferences dialog)."), NULL },
		{ "browser", '\0', 0, G_OPTION_ARG_NONE, &browser_window, 
		  N_("Open a browser window."), NULL },
		{ "quit", 'q', 0, G_OPTION_ARG_NONE, &kill_shell, 
		  N_("Quit Caja."), NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &remaining, NULL,  N_("[URI...]") },

		{ NULL }
	};
	GOptionContext *context;
	GError *error = NULL;
	CajaApplication *self = CAJA_APPLICATION (app);
	gint argc = 0;
	gchar **argv = NULL, **uris = NULL;
	gint retval = EXIT_SUCCESS;
	gboolean exit_with_last_window;

	context = g_option_context_new (_("\n\nBrowse the file system with the file manager"));
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_add_group (context, gtk_get_option_group (TRUE));

	if (!self->initialized) {
		g_option_context_add_group (context, egg_sm_client_get_option_group ());
	}

	argv = g_application_command_line_get_arguments (command_line, &argc);

	/* we need to do this here, as parsing the EggSMClient option context,
	 * unsets this variable.
	 */
	autostart_id = g_getenv ("DESKTOP_AUTOSTART_ID");
	if (autostart_id != NULL && *autostart_id != '\0') {
		autostart_mode = TRUE;
        }

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_printerr ("Could not parse arguments: %s\n", error->message);
		g_error_free (error);

		retval = EXIT_FAILURE;
		goto out;
	}

	if (version) {
		g_application_command_line_print (command_line, "MATE caja " PACKAGE_VERSION "\n");
		goto out;
	}
	if (perform_self_check && (remaining != NULL || kill_shell)) {
		g_application_command_line_printerr (command_line, "%s\n",
						     _("--check cannot be used with other options."));
		retval = EXIT_FAILURE;
		goto out;
	}
	if (kill_shell && remaining != NULL) {
		g_application_command_line_printerr (command_line, "%s\n",
						     _("-- quit cannot be used with URIs."));
		retval = EXIT_FAILURE;
		goto out;
	}
	if (geometry != NULL &&
	    remaining != NULL && remaining[0] != NULL && remaining[1] != NULL) {
		g_application_command_line_printerr (command_line, "%s\n",
						     _("--geometry cannot be used with more than one URI."));
		retval = EXIT_FAILURE;
		goto out;
	}

	/* Do either the self-check or the real work. */
	if (perform_self_check) {
#ifndef CAJA_OMIT_SELF_CHECK
		/* Run the checks (each twice) for caja and libcaja-private. */

		caja_run_self_checks ();
		caja_run_lib_self_checks ();
		eel_exit_if_self_checks_failed ();

		caja_run_self_checks ();
		caja_run_lib_self_checks ();
		eel_exit_if_self_checks_failed ();

		retval = EXIT_SUCCESS;
		goto out;
#endif
	}

	/* Check the user's ~/.caja directories and post warnings
	 * if there are problems.
	 */
	if (!kill_shell && !check_required_directories (self)) {
		retval = EXIT_FAILURE;
		goto out;
	}

	/* If in autostart mode (aka started by mate-session), we need to ensure 
         * caja starts with the correct options.
         */
	if (autostart_mode) {
		no_default_window = TRUE;
		no_desktop = FALSE;
	}

	exit_with_last_window =
		g_settings_get_boolean (caja_preferences,
					CAJA_PREFERENCES_EXIT_WITH_LAST_WINDOW);


	if (kill_shell) {
		caja_application_close_desktop ();
		g_application_release (app);

		if (!exit_with_last_window) {
			g_application_release (app);
			}	
	} else {
		if (!self->initialized) {
			char *accel_map_filename;
			caja_application_smclient_init (self);

			if (egg_sm_client_is_resumed (self->smclient)) {
				no_default_window = TRUE;
			}
		}

			if (!no_desktop &&
			    !g_settings_get_boolean (mate_background_preferences,
						     MATE_BG_KEY_SHOW_DESKTOP)) {
				no_desktop = TRUE;
			}

			if (!no_desktop) {
				caja_application_open_desktop (self);
			}		

			if (!exit_with_last_window) {
				g_application_hold (app);
			}

			finish_startup (self, no_desktop);

			/* Monitor the preference to show or hide the desktop */
			g_signal_connect_swapped (mate_background_preferences, "changed::" MATE_BG_KEY_SHOW_DESKTOP,
						  G_CALLBACK (desktop_changed_callback),
						  self);

			/* load accelerator map, and register save callback */
			accel_map_filename = caja_get_accel_map_file ();
			if (accel_map_filename) {
				gtk_accel_map_load (accel_map_filename);
				g_free (accel_map_filename);
	
		finish_startup (self, no_desktop);

			/* Load session info if availible */
			caja_application_smclient_load (self);

			self->initialized = TRUE;
		}

		/* Convert args to URIs */
		if (remaining != NULL) {
			GFile *file;
			GPtrArray *uris_array;
			gint i;
			gchar *uri;

			uris_array = g_ptr_array_new ();

			for (i = 0; remaining[i] != NULL; i++) {
				file = g_file_new_for_commandline_arg (remaining[i]);
				if (file != NULL) {
					uri = g_file_get_uri (file);
					g_object_unref (file);
					if (uri) {
						g_ptr_array_add (uris_array, uri);
					}
				}
			}

			g_ptr_array_add (uris_array, NULL);
			uris = (char **) g_ptr_array_free (uris_array, FALSE);
			g_strfreev (remaining);
		}

		/* Create the other windows. */
		if (uris != NULL || !no_default_window) {
			open_windows (self, NULL,
				      uris,
				      gdk_screen_get_default (),
				      geometry,
				      browser_window);
		}
	}

 out:
	g_option_context_free (context);
	g_strfreev (argv);

	return retval;
}

static void
caja_application_startup (GApplication *app)
{
	CajaApplication *self = CAJA_APPLICATION (app);

	/* chain up to the GTK+ implementation early, so gtk_init()
	 * is called for us.
	 */
	G_APPLICATION_CLASS (caja_application_parent_class)->startup (app);

	/* initialize the session manager client */
	egg_sm_client_set_mode (EGG_SM_CLIENT_MODE_DISABLED);

	/* Initialize preferences. This is needed to create the
	 * global GSettings objects.
	 */
	caja_global_preferences_init ();

	/* register views */
	fm_icon_view_register ();
	fm_desktop_icon_view_register ();
	fm_list_view_register ();
	fm_compact_view_register ();
#if ENABLE_EMPTY_VIEW
	fm_empty_view_register ();
#endif /* ENABLE_EMPTY_VIEW */

	/* register sidebars */
	caja_places_sidebar_register ();
	caja_information_panel_register ();
	fm_tree_view_register ();
	caja_history_sidebar_register ();
	caja_notes_viewer_register (); /* also property page */
	caja_emblem_sidebar_register ();

	/* register property pages */
	caja_image_properties_page_register ();

	/* initialize search path for custom icons */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   CAJA_DATADIR G_DIR_SEPARATOR_S "icons");
}

 static void
caja_application_quit_mainloop (GApplication *app)
{
	caja_icon_info_clear_caches ();
 	caja_application_save_accel_map (NULL);

	G_APPLICATION_CLASS (caja_application_parent_class)->quit_mainloop (app);
}


static void
caja_application_class_init (CajaApplicationClass *class)
{
        GObjectClass *object_class;
	GApplicationClass *application_class;

        object_class = G_OBJECT_CLASS (class);
	object_class->constructor = caja_application_constructor;
        object_class->finalize = caja_application_finalize;

	application_class = G_APPLICATION_CLASS (class);
	application_class->startup = caja_application_startup;
	application_class->command_line = caja_application_command_line;
	application_class->quit_mainloop = caja_application_quit_mainloop;
}

CajaApplication *
caja_application_dup_singleton (void)
{
	return g_object_new (CAJA_TYPE_APPLICATION,
			     "application-id", "org.mate.caja",
			     "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
			     NULL);
}
