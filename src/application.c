/*
 * application: Single-instance managing application and single-instance
 *              objects like window manager and so on.
 * 
 * Copyright 2012-2013 Stephan Haller <nomad@froevel.de>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "application.h"

#include <glib/gi18n-lib.h>
#include <clutter/x11/clutter-x11.h>
#include <xfconf/xfconf.h>

#include "stage.h"
#include "types.h"
#include "view-manager.h"
#include "applications-view.h"
#include "windows-view.h"

/* Define this class in GObject system */
G_DEFINE_TYPE(XfdashboardApplication,
				xfdashboard_application,
				G_TYPE_APPLICATION)

/* Private structure - access only by public API if needed */
#define XFDASHBOARD_APPLICATION_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), XFDASHBOARD_TYPE_APPLICATION, XfdashboardApplicationPrivate))

struct _XfdashboardApplicationPrivate
{
	/* Properties related */
	gboolean					isDaemon;

	/* Instance related */
	gboolean					inited;
	gboolean					shouldInit;
	XfconfChannel				*xfconfChannel;
	XfdashboardViewManager		*viewManager;
};

/* Properties */
enum
{
	PROP_0,

	PROP_DAEMONIZED,

	PROP_LAST
};

GParamSpec* XfdashboardApplicationProperties[PROP_LAST]={ 0, };

/* Signals */
enum
{
	SIGNAL_QUIT,

	SIGNAL_LAST
};

guint XfdashboardApplicationSignals[SIGNAL_LAST]={ 0, };

/* IMPLEMENTATION: Private variables and methods */

#define XFDASHBOARD_APP_ID				"de.froevel.nomad.xfdashboard"
#define XFDASHBOARD_XFCONF_CHANNEL		"xfdashboard"

/* Application options */
struct applicationOptions
{
	gboolean	doDaemonize;
	gboolean	doReplace;
	gboolean	doQuit;
} applicationOptions;

const GOptionEntry XfdashboardApplicationOptions[]=
	{
		{"daemonize", 'd', 0, G_OPTION_ARG_NONE, &applicationOptions.doDaemonize, N_("Fork to background"), NULL},
		{"restart", 'r', 0, G_OPTION_ARG_NONE, &applicationOptions.doReplace, N_("Replace existing instance"), NULL},
		{"quit", 'q', 0, G_OPTION_ARG_NONE, &applicationOptions.doQuit, N_("Quit existing instance"), NULL},
		{NULL}
	};

/* Single instance of application */
static XfdashboardApplication*		application=NULL;

/* Quit application depending on daemon mode and force parameter */
void _xfdashboard_application_quit(XfdashboardApplication *self, gboolean inForceQuit)
{
	g_return_if_fail(XFDASHBOARD_IS_APPLICATION(self));

	XfdashboardApplicationPrivate	*priv=self->priv;
	gboolean						shouldQuit=FALSE;
	GSList							*stages, *entry;

	/* Check if we should really quit this instance */
	if(inForceQuit==TRUE || priv->isDaemon==FALSE) shouldQuit=TRUE;

	/* If application is not in daemon mode or if forced is set to TRUE
	 * destroy all stage windows. Otherwise just hide them.
	 */
	stages=clutter_stage_manager_list_stages(clutter_stage_manager_get_default());
	for(entry=stages; entry!=NULL; entry=g_slist_next(entry))
	{
		if(shouldQuit==TRUE) clutter_actor_destroy(CLUTTER_ACTOR(entry->data));
			else clutter_actor_hide(CLUTTER_ACTOR(entry->data));
	}
	g_slist_free(stages);

	/* Quit main loop if we should */
	if(shouldQuit==TRUE)
	{
		/* Emit "quit" signal */
		g_signal_emit(self, XfdashboardApplicationSignals[SIGNAL_QUIT], 0);

		/* Really quit application here and now */
		if(priv->inited) clutter_main_quit();
	}
}

/* A stage window should be destroyed */
gboolean _xfdashboard_application_on_delete_stage(XfdashboardApplication *self,
													ClutterEvent *inEvent,
													gpointer inUserData)
{
	g_return_val_if_fail(XFDASHBOARD_IS_APPLICATION(self), FALSE);

	/* Quit application */
	_xfdashboard_application_quit(self, FALSE);

	/* Prevent the default handler being called */
	return(CLUTTER_EVENT_STOP);
}

/* A stage window was unfullscreened */
void _xfdashboard_application_on_unfullscreen_stage(XfdashboardApplication *self, gpointer inUserData)
{
	g_return_if_fail(XFDASHBOARD_IS_APPLICATION(self));
	g_return_if_fail(XFDASHBOARD_IS_STAGE(inUserData));

	XfdashboardStage				*stage=XFDASHBOARD_STAGE(inUserData);

	/* Set window fullscreen again just in case the application will not quit */
	clutter_stage_set_fullscreen(CLUTTER_STAGE(stage), TRUE);

	/* Quit application */
	_xfdashboard_application_quit(self, FALSE);
}

/* Perform full initialization of this application instance */
gboolean _xfdashboard_application_initialize_full(XfdashboardApplication *self)
{
	g_return_val_if_fail(XFDASHBOARD_IS_APPLICATION(self), FALSE);

	XfdashboardApplicationPrivate	*priv=self->priv;
	GError							*error=NULL;
	ClutterActor					*stage;

	/* Initialize xfconf */
	if(!xfconf_init(&error))
	{
		g_critical(_("Could not initialize xfconf: %s"),
					(error && error->message) ? error->message : _("unknown error"));
		if(error!=NULL) g_error_free(error);
		return(FALSE);
	}

	priv->xfconfChannel=xfconf_channel_get(XFDASHBOARD_XFCONF_CHANNEL);

	/* Register views (order of registration is important) */
	priv->viewManager=xfdashboard_view_manager_get_default();

	// TODO: Reorder!
	xfdashboard_view_manager_register(priv->viewManager, XFDASHBOARD_TYPE_APPLICATIONS_VIEW);
	xfdashboard_view_manager_register(priv->viewManager, XFDASHBOARD_TYPE_WINDOWS_VIEW);

	/* Create primary stage on first monitor */
	// TODO: Create stage for each monitor connected
	//       but only primary monitor gets its stage
	//       setup for primary display
	stage=xfdashboard_stage_new();

	clutter_actor_show(stage);
	g_signal_connect_swapped(stage, "delete-event", G_CALLBACK(_xfdashboard_application_on_delete_stage), self);

	/* Initialization was successful so return TRUE */
	return(TRUE);
}

/* IMPLEMENTATION: GApplication */

/* Received "activate" signal on primary instance */
void _xfdashboard_application_activate(GApplication *inApplication)
{
	g_return_if_fail(XFDASHBOARD_IS_APPLICATION(inApplication));

	GSList							*stages, *entry;

	/* Show all stages again */
	stages=clutter_stage_manager_list_stages(clutter_stage_manager_get_default());
	for(entry=stages; entry!=NULL; entry=g_slist_next(entry))
	{
		clutter_actor_show(CLUTTER_ACTOR(stages->data));
	}
	g_slist_free(stages);
}

/* Primary instance is starting up */
void _xfdashboard_application_startup(GApplication *inApplication)
{
	g_return_if_fail(XFDASHBOARD_IS_APPLICATION(inApplication));

	XfdashboardApplication			*self=XFDASHBOARD_APPLICATION(inApplication);
	XfdashboardApplicationPrivate	*priv=self->priv;

	/* Call parent's class startup method */
	G_APPLICATION_CLASS(xfdashboard_application_parent_class)->startup(inApplication);

	/* Set flag indicating that command-line handler
	 * should initialize this instance as it is the primary one
	 */
	priv->shouldInit=TRUE;
}

/* Handle command-line on primary instance */
int _xfdashboard_application_command_line(GApplication *inApplication, GApplicationCommandLine *inCommandLine)
{
	g_return_val_if_fail(XFDASHBOARD_IS_APPLICATION(inApplication), 1);

	XfdashboardApplication			*self=XFDASHBOARD_APPLICATION(inApplication);
	XfdashboardApplicationPrivate	*priv=self->priv;
	GOptionContext					*context;
	gboolean						result;
	gint							argc;
	gchar							**argv;
	GError							*error=NULL;

	/* Set up options */
	context=g_option_context_new(N_("- A Gnome Shell like dashboard for Xfce4"));
	g_option_context_add_group(context, gtk_get_option_group(TRUE));
	g_option_context_add_group(context, clutter_get_option_group_without_init());
	g_option_context_add_main_entries(context, XfdashboardApplicationOptions, GETTEXT_PACKAGE);

	/* Parse command-line arguments */
	applicationOptions.doDaemonize=FALSE;
	applicationOptions.doReplace=FALSE;
	applicationOptions.doQuit=FALSE;

	argv=g_application_command_line_get_arguments(inCommandLine, &argc);
	result=g_option_context_parse(context, &argc, &argv, &error);
	g_strfreev(argv);
	g_option_context_free(context);
	if(result==FALSE)
	{
		g_print(N_("%s\n"), (error && error->message) ? error->message : _("unknown error"));
		if(error) g_error_free(error);
		return(XFDASHBOARD_APPLICATION_ERROR_FAILED);
	}

	/* Handle options: restart, quit */
	if(applicationOptions.doReplace==TRUE || applicationOptions.doQuit==TRUE)
	{
		/* Quit existing instance */
		g_debug(_("Quitting running instance!"));
		_xfdashboard_application_quit(self, TRUE);

		/* If we should just quit the running instance return here */
		if(applicationOptions.doQuit==TRUE) return(XFDASHBOARD_APPLICATION_ERROR_QUIT);

		/* If we get here we are going to replace the just quitted instance,
		 * so force full initialization of this instance
		 */
		g_debug(_("Replacing running instance - force full initialization"));
		priv->shouldInit=TRUE;
	}

	/* Handle options: daemonized */
	priv->isDaemon=applicationOptions.doDaemonize;
	g_object_notify_by_pspec(G_OBJECT(self), XfdashboardApplicationProperties[PROP_DAEMONIZED]);

	/* Check if this instance needs to be initialized fully */
	if(priv->shouldInit==TRUE)
	{
		/* Perform full initialization of this application instance */
		result=_xfdashboard_application_initialize_full(self);
		if(result==FALSE) return(XFDASHBOARD_APPLICATION_ERROR_FAILED);

		/* Prevent further accident initialization on this instance */
		priv->shouldInit=FALSE;
	}

	/* All done successfully so return status code 0 for success */
	priv->inited=TRUE;
	return(applicationOptions.doReplace==FALSE ? XFDASHBOARD_APPLICATION_ERROR_NONE : XFDASHBOARD_APPLICATION_ERROR_RESTART);
}

/* IMPLEMENTATION: GObject */

/* Dispose this object */
void _xfdashboard_application_dispose(GObject *inObject)
{
	XfdashboardApplication			*self=XFDASHBOARD_APPLICATION(inObject);
	XfdashboardApplicationPrivate	*priv=self->priv;

	/* Release allocated resources */
	if(priv->viewManager)
	{
		/* Unregisters all remaining registered views - no need to unregister them here */
		g_object_unref(priv->viewManager);
		priv->viewManager=NULL;
	}

	/* Shutdown xfconf */
	xfconf_shutdown();
	priv->xfconfChannel=NULL;

	/* Unset singleton */
	if(G_LIKELY(G_OBJECT(application)==inObject)) application=NULL;

	/* Call parent's class dispose method */
	G_OBJECT_CLASS(xfdashboard_application_parent_class)->dispose(inObject);
}

/* Set/get properties */
void _xfdashboard_application_set_property(GObject *inObject,
														guint inPropID,
														const GValue *inValue,
														GParamSpec *inSpec)
{
	switch(inPropID)
	{
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(inObject, inPropID, inSpec);
			break;
	}
}

void _xfdashboard_application_get_property(GObject *inObject,
														guint inPropID,
														GValue *outValue,
														GParamSpec *inSpec)
{
	XfdashboardApplication	*self=XFDASHBOARD_APPLICATION(inObject);

	switch(inPropID)
	{
		case PROP_DAEMONIZED:
			g_value_set_boolean(outValue, self->priv->isDaemon);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(inObject, inPropID, inSpec);
			break;
	}
}

/* Class initialization
 * Override functions in parent classes and define properties
 * and signals
 */
void xfdashboard_application_class_init(XfdashboardApplicationClass *klass)
{
	GApplicationClass	*appClass=G_APPLICATION_CLASS(klass);
	GObjectClass		*gobjectClass=G_OBJECT_CLASS(klass);

	/* Override functions */
	appClass->activate=_xfdashboard_application_activate;
	appClass->startup=_xfdashboard_application_startup;
	appClass->command_line=_xfdashboard_application_command_line;

	gobjectClass->dispose=_xfdashboard_application_dispose;
	gobjectClass->set_property=_xfdashboard_application_set_property;
	gobjectClass->get_property=_xfdashboard_application_get_property;

	/* Set up private structure */
	g_type_class_add_private(klass, sizeof(XfdashboardApplicationPrivate));

	/* Define properties */
	XfdashboardApplicationProperties[PROP_DAEMONIZED]=
		g_param_spec_boolean("daemonized",
								_("Daemonized"),
								_("Flag indicating if application is daemonized"),
								FALSE,
								G_PARAM_READABLE);

	g_object_class_install_properties(gobjectClass, PROP_LAST, XfdashboardApplicationProperties);

	/* Define signals */
	XfdashboardApplicationSignals[SIGNAL_QUIT]=
		g_signal_new("quit",
						G_TYPE_FROM_CLASS(klass),
						G_SIGNAL_RUN_LAST,
						G_STRUCT_OFFSET(XfdashboardApplicationClass, quit),
						NULL,
						NULL,
						g_cclosure_marshal_VOID__VOID,
						G_TYPE_NONE,
						0);
}

/* Object initialization
 * Create private structure and set up default values
 */
void xfdashboard_application_init(XfdashboardApplication *self)
{
	XfdashboardApplicationPrivate	*priv;

	priv=self->priv=XFDASHBOARD_APPLICATION_GET_PRIVATE(self);

	/* Set default values */
	priv->inited=FALSE;
	priv->shouldInit=FALSE;
	priv->isDaemon=FALSE;
	priv->xfconfChannel=NULL;
	priv->viewManager=NULL;
}

/* Implementation: Public API */

/* Get single instance of application */
XfdashboardApplication* xfdashboard_application_get_default(void)
{
	if(G_UNLIKELY(application==NULL))
	{
		application=g_object_new(XFDASHBOARD_TYPE_APPLICATION,
									"application-id", XFDASHBOARD_APP_ID,
									"flags", G_APPLICATION_HANDLES_COMMAND_LINE,
									NULL);
	}

	return(application);
}

/* Quit application */
void xfdashboard_application_quit(void)
{
	if(G_LIKELY(application!=NULL))
	{
		_xfdashboard_application_quit(application, FALSE);
	}
}

void xfdashboard_application_quit_forced(void)
{
	if(G_LIKELY(application!=NULL))
	{
		_xfdashboard_application_quit(application, TRUE);
	}
		else clutter_main_quit();
}