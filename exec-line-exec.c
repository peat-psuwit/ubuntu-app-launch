/*
 * Copyright 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 */

#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "exec-line-exec-trace.h"
#include "helpers.h"
#include "ual-tracepoint.h"

int
main (int argc, char * argv[])
{
	/* Make sure we have work to do */
	/* This string is quoted using desktop file quoting:
	   http://standards.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#exec-variables */
	const gchar * app_exec = g_getenv("APP_EXEC");
	if (app_exec == NULL) {
		/* There should be no reason for this, a g_error() so that it gets
		   picked up by Apport and we can track it */
		g_error("No exec line given, nothing to do except fail");
		return 1;
	}

	/* For the tracepoints */
	const gchar * app_id = g_getenv("APP_ID");

	ual_tracepoint(exec_start, app_id);

	/* URIs */
	const gchar * app_uris = g_getenv("APP_URIS");

	/* Look to see if we have a directory defined that we
	   should be using for everything.  If so, change to it
	   and add it to the path */
	const gchar * appdir = g_getenv("APP_DIR");

	if (appdir != NULL) {
		if (g_chdir(appdir) != 0) {
			g_warning("Unable to change directory to '%s'", appdir);
		}
	}

	/* Protect against app directories that have ':' in them */
	if (appdir != NULL && strchr(appdir, ':') == NULL) {
		const gchar * path_path = g_getenv("PATH");
		if (path_path != NULL && path_path[0] == '\0')
			path_path = NULL;
		gchar * path_libpath = NULL;
		const gchar * path_joinable[4] = { 0 };

		const gchar * lib_path = g_getenv("LD_LIBRARY_PATH");
		if (lib_path != NULL && lib_path[0] == '\0')
			lib_path = NULL;
		gchar * lib_libpath = g_build_filename(appdir, "lib", NULL);
		const gchar * lib_joinable[4] = { 0 };

		const gchar * import_path = g_getenv("QML2_IMPORT_PATH");
		if (import_path != NULL && import_path[0] == '\0')
			import_path = NULL;
		gchar * import_libpath = NULL;
		const gchar * import_joinable[4] = { 0 };

		/* If we've got an architecture set insert that into the
		   path before everything else */
		const gchar * archdir = g_getenv("UBUNTU_APP_LAUNCH_ARCH");
		if (archdir != NULL && strchr(archdir, ':') == NULL) {
			path_libpath = g_build_filename(appdir, "lib", archdir, "bin", NULL);
			import_libpath = g_build_filename(appdir, "lib", archdir, NULL);

			path_joinable[0] = path_libpath;
			path_joinable[1] = appdir;
			path_joinable[2] = path_path;

			lib_joinable[0] = import_libpath;
			lib_joinable[1] = lib_libpath;
			lib_joinable[2] = lib_path;

			/* Need to check whether the original is NULL because we're
			   appending instead of prepending */
			if (import_path == NULL) {
				import_joinable[0] = import_libpath;
			} else {
				import_joinable[0] = import_path;
				import_joinable[1] = import_libpath;
			}
		} else {
			path_joinable[0] = appdir;
			path_joinable[1] = path_path;

			lib_joinable[0] = lib_libpath;
			lib_joinable[1] = lib_path;

			import_joinable[0] = import_path;
		}

		gchar * newpath = g_strjoinv(":", (gchar**)path_joinable);
		g_setenv("PATH", newpath, TRUE);
		g_free(path_libpath);
		g_free(newpath);

		gchar * newlib = g_strjoinv(":", (gchar**)lib_joinable);
		g_setenv("LD_LIBRARY_PATH", newlib, TRUE);
		g_free(lib_libpath);
		g_free(newlib);

		if (import_joinable[0] != NULL) {
			gchar * newimport = g_strjoinv(":", (gchar**)import_joinable);
			g_setenv("QML2_IMPORT_PATH", newimport, TRUE);
			g_free(newimport);
		}
		g_free(import_libpath);
	}

	/* Parse the execiness of it all */
	GArray * newargv = desktop_exec_parse(app_exec, app_uris);
	if (newargv == NULL) {
		g_warning("Unable to parse exec line '%s'", app_exec);
		return 1;
	}

	ual_tracepoint(exec_parse_complete, app_id);

	if (g_getenv("MIR_SOCKET") != NULL && g_strcmp0(g_getenv("APP_XMIR_ENABLE"), "1") == 0) {
		g_debug("XMir Helper being used");

		/* xmir-helper $(APP_ID) $(COMMAND) */
		const gchar * appid = g_getenv("APP_ID");
		g_array_prepend_val(newargv, appid);

		/* Pulling into the heap instead of the code page */
		char * xmir_helper = g_strdup(XMIR_HELPER);
		g_array_prepend_val(newargv, xmir_helper);
	}

	/* Now exec */
	gchar ** nargv = (gchar**)g_array_free(newargv, FALSE);

	ual_tracepoint(exec_pre_exec, app_id);

	int execret = execvp(nargv[0], nargv);

	if (execret != 0) {
		gchar * execprint = g_strjoinv(" ", nargv);
		g_warning("Unable to exec '%s' in '%s': %s", execprint, appdir, strerror(errno));
		g_free(execprint);
	}

	return execret;
}
