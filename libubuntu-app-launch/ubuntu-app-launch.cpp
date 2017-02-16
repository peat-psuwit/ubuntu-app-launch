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

extern "C" {
#include "ubuntu-app-launch.h"
#include <upstart.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <zeitgeist.h>
#include <libwhoopsie/recoverable-problem.h>

#include "ubuntu-app-launch-trace.h"
#include "helpers.h"
#include "ual-tracepoint.h"
#include "proxy-socket-demangler.h"
}

/* C++ Interface */
#include "application.h"
#include "appid.h"
#include "helper-impl.h"
#include "registry.h"
#include "registry-impl.h"
#include <algorithm>

/* Make a typed string vector out of a GStrv */
template <typename T>
static std::vector<T> uriVector (const gchar * const * uris)
{
	std::vector<T> urivect;

	for (auto i = 0; uris != nullptr && uris[i] != nullptr; i++) {
		urivect.emplace_back(T::from_raw(uris[i]));
	}

	return urivect;
}

gboolean
ubuntu_app_launch_start_application (const gchar * appid, const gchar * const * uris)
{
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);

		auto uriv = uriVector<ubuntu::app_launch::Application::URL>(uris);

		auto instance = app->launch(uriv);

		if (instance) {
			return TRUE;
		} else {
			return FALSE;
		}
	} catch (std::runtime_error &e) {
		g_warning("Unable to start app '%s': %s", appid, e.what());
		return FALSE;
	}
}

gboolean
ubuntu_app_launch_start_application_test (const gchar * appid, const gchar * const * uris)
{
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);

		auto uriv = uriVector<ubuntu::app_launch::Application::URL>(uris);

		auto instance = app->launchTest(uriv);

		if (instance) {
			return TRUE;
		} else {
			return FALSE;
		}
	} catch (std::runtime_error &e) {
		g_warning("Unable to start app '%s': %s", appid, e.what());
		return FALSE;
	}
}

gboolean
ubuntu_app_launch_stop_application (const gchar * appid)
{
	g_return_val_if_fail(appid != NULL, FALSE);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);

		auto instances = app->instances();
		for (auto instance : instances) {
			instance->stop();
		}

		return TRUE;
	} catch (std::runtime_error &e) {
		g_warning("Unable to stop app '%s': %s", appid, e.what());
		return FALSE;
	}
}

gboolean
ubuntu_app_launch_pause_application (const gchar * appid)
{
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);
		app->instances().at(0)->pause();
		return TRUE;
	} catch (...) {
		return FALSE;
	}
}

gboolean
ubuntu_app_launch_resume_application (const gchar * appid)
{
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);
		app->instances().at(0)->resume();
		return TRUE;
	} catch (...) {
		return FALSE;
	}
}

gchar *
ubuntu_app_launch_application_log_path (const gchar * appid)
{
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);
		auto log = app->instances()[0]->logPath();
		return g_strdup(log.c_str());
	} catch (...) {
		return nullptr;
	}
}

static GDBusConnection *
gdbus_upstart_ref (void) {
	static GDBusConnection * gdbus_upstart = NULL;

	if (gdbus_upstart != NULL) {
		return G_DBUS_CONNECTION(g_object_ref(gdbus_upstart));
	}

	GError * error = NULL;
	gdbus_upstart = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

	if (error != NULL) {
		g_warning("Unable to connect to Upstart bus: %s", error->message);
		g_error_free(error);
		return NULL;
	}

	g_object_add_weak_pointer(G_OBJECT(gdbus_upstart), (void **)(&gdbus_upstart));

	return gdbus_upstart;
}

/* The data we keep for each observer */
typedef struct _observer_t observer_t;
struct _observer_t {
	GDBusConnection * conn;
	guint sighandle;
	UbuntuAppLaunchAppObserver func;
	gpointer user_data;
};

/* The data we keep for each failed observer */
typedef struct _failed_observer_t failed_observer_t;
struct _failed_observer_t {
	GDBusConnection * conn;
	guint sighandle;
	UbuntuAppLaunchAppFailedObserver func;
	gpointer user_data;
};

/* Function to take a work function and have it execute on a given
   GMainContext */
static void executeOnContext (const std::shared_ptr<GMainContext>& context, std::function<void()> work)
{
	if (!context) {
		work();
		return;
	}

    auto heapWork = new std::function<void()>(work);

    auto source = std::shared_ptr<GSource>(g_idle_source_new(), [](GSource* src) { g_clear_pointer(&src, g_source_unref); });
    g_source_set_callback(source.get(),
                          [](gpointer data) {
                              auto heapWork = static_cast<std::function<void()>*>(data);
                              (*heapWork)();
                              return G_SOURCE_REMOVE;
                          },
                          heapWork,
                          [](gpointer data) {
                              auto heapWork = static_cast<std::function<void()>*>(data);
                              delete heapWork;
                          });

    g_source_attach(source.get(), context.get());
}

/** A handy helper function that is based of a function to get
    a signal and put it into a map. */
template <core::Signal<const std::shared_ptr<ubuntu::app_launch::Application>&, const std::shared_ptr<ubuntu::app_launch::Application::Instance>&>& (*getSignal)(const std::shared_ptr<ubuntu::app_launch::Registry>&)>
static gboolean
observer_add (UbuntuAppLaunchAppObserver observer, gpointer user_data, std::map<std::pair<UbuntuAppLaunchAppObserver, gpointer>, core::ScopedConnection> &observers)
{
	auto context = std::shared_ptr<GMainContext>(g_main_context_ref_thread_default(), [](GMainContext * context) { g_clear_pointer(&context, g_main_context_unref); });

	observers.emplace(std::make_pair(
		std::make_pair(observer, user_data),
		core::ScopedConnection(
			getSignal(ubuntu::app_launch::Registry::getDefault())
				.connect([context, observer, user_data](std::shared_ptr<ubuntu::app_launch::Application> app, std::shared_ptr<ubuntu::app_launch::Application::Instance> instance) {
					std::string appid = app->appId();
					executeOnContext(context, [appid, observer, user_data]() {
						observer(appid.c_str(), user_data);
					});
			})
		)
		));

	return TRUE;
}

/** A handy helper to delete items from an observer map */
template<typename observertype>
static gboolean
observer_delete (observertype observer, gpointer user_data, std::map<std::pair<observertype, gpointer>, core::ScopedConnection> &observers)
{
	auto iter = observers.find(std::make_pair(observer, user_data));

	if (iter == observers.end()) {
		return FALSE;
	}

	observers.erase(iter);
	return TRUE;
}

/** Map of all the observers listening for app started */
static std::map<std::pair<UbuntuAppLaunchAppObserver, gpointer>, core::ScopedConnection> appStartedObservers;

gboolean
ubuntu_app_launch_observer_add_app_started (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	return observer_add<&ubuntu::app_launch::Registry::appStarted>(observer, user_data, appStartedObservers);
}

gboolean
ubuntu_app_launch_observer_delete_app_started (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	return observer_delete<UbuntuAppLaunchAppObserver>(observer, user_data, appStartedObservers);
}

/* Map of all the observers listening for app stopped */
static std::map<std::pair<UbuntuAppLaunchAppObserver, gpointer>, core::ScopedConnection> appStoppedObservers;

gboolean
ubuntu_app_launch_observer_add_app_stop (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	return observer_add<&ubuntu::app_launch::Registry::appStopped>(observer, user_data, appStoppedObservers);
}

gboolean
ubuntu_app_launch_observer_delete_app_stop (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	return observer_delete<UbuntuAppLaunchAppObserver>(observer, user_data, appStoppedObservers);
}

/** Class to implement the Registry::Manager interface for the C code
    using a GLib mainloop. */
class CManager : public ubuntu::app_launch::Registry::Manager
{
public:
	CManager () {
		g_debug("Creating the CManager object");
	}
	virtual ~CManager() {
		g_debug("Removing the shared the CManager object");
	}

	void startingRequest(const std::shared_ptr<ubuntu::app_launch::Application>& app,
                                     const std::shared_ptr<ubuntu::app_launch::Application::Instance>& instance,
                                     std::function<void(bool)> reply) override {
		requestImpl(app, instance, reply, "starting", startingList);
	}

	void focusRequest(const std::shared_ptr<ubuntu::app_launch::Application>& app,
                                  const std::shared_ptr<ubuntu::app_launch::Application::Instance>& instance,
                                  std::function<void(bool)> reply) override {
		requestImpl(app, instance, reply, "focus", focusList);
	}

	void resumeRequest(const std::shared_ptr<ubuntu::app_launch::Application> &app,
                                   const std::shared_ptr<ubuntu::app_launch::Application::Instance> &instance,
                                   std::function<void(bool)> reply) override {
		requestImpl(app, instance, reply, "resume", resumeList);
	}

private:
	/** The Data that we track on an observer. It is the functions to
	    call, the user data and the context to call it on. */
	struct ObserverData {
		UbuntuAppLaunchAppObserver observer;
		gpointer user_data;
		std::shared_ptr<GMainContext> context;

		/** Handy constructor to get the context in one place */
		ObserverData(UbuntuAppLaunchAppObserver obs, gpointer ud)
			: observer(obs)
			, user_data(ud) {
			context = std::shared_ptr<GMainContext>(g_main_context_ref_thread_default(), [](GMainContext * context) { g_clear_pointer(&context, g_main_context_unref); });
		}
	};

	std::list<ObserverData> focusList;    /**< List of observers on the focus signal */
	std::list<ObserverData> resumeList;   /**< List of observers on the resume signal */
	std::list<ObserverData> startingList; /**< List of observers on the starting signal */

	/** Removes an observer from a specified list */
	bool removeObserver (std::list<ObserverData> &list, UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		auto iter = std::find_if(list.begin(), list.end(), [observer, user_data](const ObserverData &data) {
			return data.observer == observer && data.user_data == user_data;
		});

		if (iter == list.end()) {
			return false;
		}

		list.erase(iter);
		return true;
	}

	/** Implements a request for a specified list by calling each observer and then the reply */
	inline void requestImpl ( const std::shared_ptr<ubuntu::app_launch::Application> &app,
                                   const std::shared_ptr<ubuntu::app_launch::Application::Instance> &instance,
                                   std::function<void(bool)> reply,
				   const std::string& name,
				   std::list<ObserverData>& list) {
		std::string sappid = app->appId();
		g_debug("CManager %s: %s", name.c_str(), sappid.c_str());

		for (const auto &data : list) {
			executeOnContext(data.context, [data, sappid]() {
				data.observer(sappid.c_str(), data.user_data);
			});
		}

		reply(true);
	}

public:
	void addFocus (UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		focusList.emplace_back(ObserverData(observer, user_data));
	}
	void addResume (UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		resumeList.emplace_back(ObserverData(observer, user_data));
	}
	void addStarting (UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		startingList.emplace_back(ObserverData(observer, user_data));
	}
	bool deleteFocus (UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		return removeObserver(focusList, observer, user_data);
	}
	bool deleteResume (UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		return removeObserver(resumeList, observer, user_data);
	}
	bool deleteStarting (UbuntuAppLaunchAppObserver observer, gpointer user_data) {
		return removeObserver(startingList, observer, user_data);
	}
};

/** Weak pointer to the CManager if it is still in use. If it gets free'd by
    the registry we're okay with that. */
static std::weak_ptr<CManager> cmanager;

/** Function to create the CManager if it doesn't currently exist. Otherwise
    just return a lock to it */
static std::shared_ptr<CManager>
ensure_cmanager ()
{
	auto retval = cmanager.lock();

	if (!retval) {
		retval = std::make_shared<CManager>();
		ubuntu::app_launch::Registry::setManager(retval, ubuntu::app_launch::Registry::getDefault());
		cmanager = retval;
	}

	return retval;
}

gboolean
ubuntu_app_launch_observer_add_app_focus (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	auto manager = ensure_cmanager();
	manager->addFocus(observer, user_data);
	return TRUE;
}

gboolean
ubuntu_app_launch_observer_delete_app_focus (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	auto manager = ensure_cmanager();
	return manager->deleteFocus(observer, user_data) ? TRUE : FALSE;
}

gboolean
ubuntu_app_launch_observer_add_app_resume (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	auto manager = ensure_cmanager();
	manager->addResume(observer, user_data);
	return TRUE;
}

gboolean
ubuntu_app_launch_observer_delete_app_resume (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	auto manager = ensure_cmanager();
	return manager->deleteResume(observer, user_data) ? TRUE : FALSE;
}

gboolean
ubuntu_app_launch_observer_add_app_starting (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	auto manager = ensure_cmanager();
	ubuntu::app_launch::Registry::Impl::watchingAppStarting(true);
	manager->addStarting(observer, user_data);
	return TRUE;
}

gboolean
ubuntu_app_launch_observer_delete_app_starting (UbuntuAppLaunchAppObserver observer, gpointer user_data)
{
	auto manager = ensure_cmanager();
	ubuntu::app_launch::Registry::Impl::watchingAppStarting(false);
	return manager->deleteStarting(observer, user_data) ? TRUE : FALSE;
}

/* Map of all the observers listening for app stopped */
static std::map<std::pair<UbuntuAppLaunchAppFailedObserver, gpointer>, core::ScopedConnection> appFailedObservers;

gboolean
ubuntu_app_launch_observer_add_app_failed (UbuntuAppLaunchAppFailedObserver observer, gpointer user_data)
{
	auto context = std::shared_ptr<GMainContext>(g_main_context_ref_thread_default(), [](GMainContext * context) { g_clear_pointer(&context, g_main_context_unref); });

	appFailedObservers.emplace(std::make_pair(
		std::make_pair(observer, user_data),
			core::ScopedConnection(
				ubuntu::app_launch::Registry::appFailed().connect([context, observer, user_data](std::shared_ptr<ubuntu::app_launch::Application> app, std::shared_ptr<ubuntu::app_launch::Application::Instance> instance, ubuntu::app_launch::Registry::FailureType type) {
					std::string appid = app->appId();
					executeOnContext(context, [appid, type, observer, user_data]() {
						UbuntuAppLaunchAppFailed ctype{UBUNTU_APP_LAUNCH_APP_FAILED_CRASH};

						switch (type) {
						case ubuntu::app_launch::Registry::FailureType::CRASH:
							ctype = UBUNTU_APP_LAUNCH_APP_FAILED_CRASH;
							break;
						case ubuntu::app_launch::Registry::FailureType::START_FAILURE:
							ctype = UBUNTU_APP_LAUNCH_APP_FAILED_START_FAILURE;
							break;
						}

						observer(appid.c_str(), ctype, user_data);
					});
				})
			)
		));

	return TRUE;
}

gboolean
ubuntu_app_launch_observer_delete_app_failed (UbuntuAppLaunchAppFailedObserver observer, gpointer user_data)
{
	return observer_delete<UbuntuAppLaunchAppFailedObserver>(observer, user_data, appFailedObservers);
}

/** Handy helper for pause and resume here */
template <core::Signal<const std::shared_ptr<ubuntu::app_launch::Application>&, const std::shared_ptr<ubuntu::app_launch::Application::Instance>&, const std::vector<pid_t>&>& (*getSignal)(const std::shared_ptr<ubuntu::app_launch::Registry>&)>
static gboolean
observer_add_pause (UbuntuAppLaunchAppPausedResumedObserver observer, gpointer user_data, std::map<std::pair<UbuntuAppLaunchAppPausedResumedObserver, gpointer>, core::ScopedConnection> &observers)
{
	auto context = std::shared_ptr<GMainContext>(g_main_context_ref_thread_default(), [](GMainContext * context) { g_clear_pointer(&context, g_main_context_unref); });

	observers.emplace(std::make_pair(
		std::make_pair(observer, user_data),
		core::ScopedConnection(
			getSignal(ubuntu::app_launch::Registry::getDefault())
				.connect([context, observer, user_data](std::shared_ptr<ubuntu::app_launch::Application> app, std::shared_ptr<ubuntu::app_launch::Application::Instance> instance, const std::vector<pid_t> &pids) {
					std::vector<pid_t> lpids = pids;
					lpids.emplace_back(0);

					std::string appid = app->appId();

					executeOnContext(context, [appid, observer, user_data, lpids]() {
						observer(appid.c_str(), (int *)(lpids.data()), user_data);
					});
			})
		)
		));

	return TRUE;
}

static std::map<std::pair<UbuntuAppLaunchAppPausedResumedObserver, gpointer>, core::ScopedConnection> appPausedObservers;

gboolean
ubuntu_app_launch_observer_add_app_paused (UbuntuAppLaunchAppPausedResumedObserver observer, gpointer user_data)
{
	return observer_add_pause<&ubuntu::app_launch::Registry::appPaused>(observer, user_data, appPausedObservers);
}

gboolean
ubuntu_app_launch_observer_delete_app_paused (UbuntuAppLaunchAppPausedResumedObserver observer, gpointer user_data)
{
	return observer_delete<UbuntuAppLaunchAppPausedResumedObserver>(observer, user_data, appPausedObservers);
}

static std::map<std::pair<UbuntuAppLaunchAppPausedResumedObserver, gpointer>, core::ScopedConnection> appResumedObservers;

gboolean
ubuntu_app_launch_observer_add_app_resumed (UbuntuAppLaunchAppPausedResumedObserver observer, gpointer user_data)
{
	return observer_add_pause<&ubuntu::app_launch::Registry::appResumed>(observer, user_data, appResumedObservers);
}

gboolean
ubuntu_app_launch_observer_delete_app_resumed (UbuntuAppLaunchAppPausedResumedObserver observer, gpointer user_data)
{
	return observer_delete<UbuntuAppLaunchAppPausedResumedObserver>(observer, user_data, appResumedObservers);
}

gchar **
ubuntu_app_launch_list_running_apps (void)
{
	try {
		GArray * apps = g_array_new(TRUE, TRUE, sizeof(gchar *));
		for (auto app : ubuntu::app_launch::Registry::runningApps()) {
			std::string appid = app->appId();
			g_debug("Adding AppID to list: %s", appid.c_str());
			gchar * gappid = g_strdup(appid.c_str());
			g_array_append_val(apps, gappid);
		}

		return (gchar **)g_array_free(apps, FALSE);
	} catch (...) {
		return nullptr;
	}
}

GPid
ubuntu_app_launch_get_primary_pid (const gchar * appid)
{
	g_return_val_if_fail(appid != NULL, 0);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);
		return app->instances().at(0)->primaryPid();
	} catch (...) {
		return 0;
	}
}

/* Get the PIDs for an AppID. If it's click or legacy single instance that's
   a simple call to the helper. But if it's not, we have to make a call for
   each instance of the app that we have running. */
GList *
ubuntu_app_launch_get_pids (const gchar * appid)
{
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);
		auto pids = app->instances().at(0)->pids();

		GList * retval = nullptr;
		for (auto pid : pids) {
			retval = g_list_prepend(retval, GINT_TO_POINTER(pid));
		}

		return retval;
	} catch (...) {
		return nullptr;
	}
}

gboolean
ubuntu_app_launch_pid_in_app_id (GPid pid, const gchar * appid)
{
	g_return_val_if_fail(appid != NULL, FALSE);
	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::find(appid);
		auto app = ubuntu::app_launch::Application::create(appId, registry);

		if (app->instances().at(0)->hasPid(pid)) {
			return TRUE;
		} else {
			return FALSE;
		}
	} catch (...) {
		return FALSE;
	}
}

gboolean
ubuntu_app_launch_app_id_parse (const gchar * appid, gchar ** package, gchar ** application, gchar ** version)
{
	g_return_val_if_fail(appid != NULL, FALSE);

	try {
		auto appId = ubuntu::app_launch::AppID::parse(appid);

		if (appId.empty()) {
			return FALSE;
		}

		if (package != nullptr) {
			*package = g_strdup(appId.package.value().c_str());
		}

		if (application != nullptr) {
			*application = g_strdup(appId.appname.value().c_str());
		}

		if (version != nullptr) {
			*version = g_strdup(appId.version.value().c_str());
		}
	} catch (...) {
		return FALSE;
	}

	return TRUE;
}

/* Figure out whether we're a libertine container app or a click and then
   choose which function to use */
gchar *
ubuntu_app_launch_triplet_to_app_id (const gchar * pkg, const gchar * app, const gchar * ver)
{
	g_return_val_if_fail(pkg != NULL, NULL);

	std::string package{pkg};
	std::string appname;
	std::string version;

	if (app != nullptr) {
		appname = app;
	}

	if (ver != nullptr) {
		version = ver;
	}

	auto appid = ubuntu::app_launch::AppID::discover(package, appname, version);
	if (appid.empty()) {
		return nullptr;
	}

	return g_strdup(std::string(appid).c_str());
}

gboolean
ubuntu_app_launch_start_helper (const gchar * type, const gchar * appid, const gchar * const * uris)
{
	g_return_val_if_fail(type != NULL, FALSE);
	g_return_val_if_fail(appid != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, FALSE);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::parse(appid);
		auto helper = ubuntu::app_launch::Helper::create(ubuntu::app_launch::Helper::Type::from_raw(type), appId, registry);

		if (!helper->instances().empty()) {
			throw std::runtime_error{"Instance already exits"};
		}

		auto uriv = uriVector<ubuntu::app_launch::Helper::URL>(uris);
		auto inst = helper->launch(uriv);

		if (!inst) {
			throw std::runtime_error{"Empty instance"};
		}

		return true;
	} catch (std::runtime_error &e) {
		g_warning("Unable to launch helper of type '%s' id '%s': %s", type, appid, e.what());
		return false;
	}
}

gchar *
ubuntu_app_launch_start_multiple_helper (const gchar * type, const gchar * appid, const gchar * const * uris)
{
	g_return_val_if_fail(type != NULL, NULL);
	g_return_val_if_fail(appid != NULL, NULL);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, NULL);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::parse(appid);
		auto helper = ubuntu::app_launch::Helper::create(ubuntu::app_launch::Helper::Type::from_raw(type), appId, registry);

		auto uriv = uriVector<ubuntu::app_launch::Helper::URL>(uris);
		auto inst = helper->launch(uriv);

		if (!inst) {
			throw std::runtime_error{"Empty instance"};
		}

		return g_strdup(std::dynamic_pointer_cast<ubuntu::app_launch::helper_impls::BaseInstance>(inst)->getInstanceId().c_str());
	} catch (std::runtime_error &e) {
		g_warning("Unable to launch helper of type '%s' id '%s': %s", type, appid, e.what());
		return nullptr;
	}
}

gchar *
ubuntu_app_launch_start_session_helper (const gchar * type, MirPromptSession * session, const gchar * appid, const gchar * const * uris)
{
	g_return_val_if_fail(type != NULL, NULL);
	g_return_val_if_fail(session != NULL, NULL);
	g_return_val_if_fail(appid != NULL, NULL);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, NULL);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::parse(appid);
		auto helper = ubuntu::app_launch::Helper::create(ubuntu::app_launch::Helper::Type::from_raw(type), appId, registry);

		auto uriv = uriVector<ubuntu::app_launch::Helper::URL>(uris);
		auto inst = helper->launch(session, uriv);

		if (!inst) {
			throw std::runtime_error{"Empty instance"};
		}

		return g_strdup(std::dynamic_pointer_cast<ubuntu::app_launch::helper_impls::BaseInstance>(inst)->getInstanceId().c_str());
	} catch (std::runtime_error &e) {
		g_warning("Unable to launch helper of type '%s' id '%s': %s", type, appid, e.what());
		return nullptr;
	}
}

gboolean
ubuntu_app_launch_stop_helper (const gchar * type, const gchar * appid)
{
	g_return_val_if_fail(type != NULL, FALSE);
	g_return_val_if_fail(appid != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, FALSE);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::parse(appid);
		auto helper = ubuntu::app_launch::Helper::create(ubuntu::app_launch::Helper::Type::from_raw(type), appId, registry);

		auto insts = helper->instances();
		if (insts.empty()) {
			throw std::runtime_error{"No instances"};
		}
		if (insts.size() > 1u) {
			throw std::runtime_error{"Expecting single instance but has multiple instances"};
		}

		(*insts.begin())->stop();

		return TRUE;
	} catch (std::runtime_error &e) {
		g_warning("Unable to stop helper of type '%s' id '%s': %s", type, appid, e.what());
		return FALSE;
	}
}

gboolean
ubuntu_app_launch_stop_multiple_helper (const gchar * type, const gchar * appid, const gchar * instanceid)
{
	g_return_val_if_fail(type != NULL, FALSE);
	g_return_val_if_fail(appid != NULL, FALSE);
	g_return_val_if_fail(instanceid != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, FALSE);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::parse(appid);
		auto helper = ubuntu::app_launch::Helper::create(ubuntu::app_launch::Helper::Type::from_raw(type), appId, registry);

		auto inst = std::dynamic_pointer_cast<ubuntu::app_launch::helper_impls::Base>(helper)->existingInstance(instanceid);
		if (!inst) {
			throw std::runtime_error{"No instances"};
		}

		inst->stop();

		return TRUE;
	} catch (std::runtime_error &e) {
		g_warning("Unable to stop helper of type '%s' id '%s' instance '%s': %s", type, appid, instanceid, e.what());
		return FALSE;
	}
}

gchar **
ubuntu_app_launch_list_helpers (const gchar * type)
{
	g_return_val_if_fail(type != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, FALSE);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto helpers = registry->runningHelpers(ubuntu::app_launch::Helper::Type::from_raw(type));

		auto array = g_array_new(TRUE, TRUE, sizeof(gchar *));

		for (const auto &helper : helpers) {
			gchar * appid = g_strdup(std::string(helper->appId()).c_str());
			g_array_append_val(array, appid);
		}

		return (gchar **)g_array_free(array, FALSE);
	} catch (std::runtime_error &e) {
		g_warning("Unable to get helpers for type '%s': %s", type, e.what());
		return NULL;
	}
}

gchar **
ubuntu_app_launch_list_helper_instances (const gchar * type, const gchar * appid)
{
	g_return_val_if_fail(type != NULL, NULL);
	g_return_val_if_fail(g_strstr_len(type, -1, ":") == NULL, NULL);
	g_return_val_if_fail(appid != NULL, NULL);

	try {
		auto registry = ubuntu::app_launch::Registry::getDefault();
		auto appId = ubuntu::app_launch::AppID::parse(appid);
		auto helper = ubuntu::app_launch::Helper::create(ubuntu::app_launch::Helper::Type::from_raw(type), appId, registry);

		auto insts = helper->instances();

		auto array = g_array_new(TRUE, TRUE, sizeof(gchar *));

		for (const auto &inst : insts) {
			/* TODO: Copy IDS */
			if (inst->isRunning()) {
				g_debug("Running");
			}
		}

		return (gchar **)g_array_free(array, FALSE);
	} catch (std::runtime_error &e) {
		g_warning("Unable to get helper instances for '%s' of type '%s': %s", appid, type, e.what());
		return NULL;
	}
}

/* The data we keep for each observer */
typedef struct _helper_observer_t helper_observer_t;
struct _helper_observer_t {
	GDBusConnection * conn;
	guint sighandle;
	gchar * type;
	UbuntuAppLaunchHelperObserver func;
	gpointer user_data;
};

/* The lists of helper observers */
static GList * helper_started_obs = NULL;
static GList * helper_stopped_obs = NULL;

static void
helper_observer_cb (GDBusConnection * conn, const gchar * sender, const gchar * object, const gchar * interface, const gchar * signal, GVariant * params, gpointer user_data)
{
	helper_observer_t * observer = (helper_observer_t *)user_data;

	gchar * env = NULL;
	GVariant * envs = g_variant_get_child_value(params, 1);
	GVariantIter iter;
	g_variant_iter_init(&iter, envs);

	gboolean job_found = FALSE;
	gchar * instance = NULL;

	while (g_variant_iter_loop(&iter, "s", &env)) {
		if (g_strcmp0(env, "JOB=untrusted-helper") == 0) {
			job_found = TRUE;
		} else if (g_str_has_prefix(env, "INSTANCE=")) {
			instance = g_strdup(env + strlen("INSTANCE="));
		}
	}

	g_variant_unref(envs);

	if (instance != NULL && !g_str_has_prefix(instance, observer->type)) {
		g_free(instance);
		instance = NULL;
	}

	gchar * appid = NULL;
	gchar * instanceid = NULL;
	gchar * type = NULL;

	if (instance != NULL) {
		gchar ** split = g_strsplit(instance, ":", 3);
		type = split[0];
		instanceid = split[1];
		appid = split[2];
		g_free(split);
	}
	g_free(instance);

	if (instanceid != NULL && instanceid[0] == '\0') {
		g_free(instanceid);
		instanceid = NULL;
	}

	if (job_found && appid != NULL) {
		observer->func(appid, instanceid, type, observer->user_data);
	}

	g_free(appid);
	g_free(instanceid);
	g_free(type);
}

/* Creates the observer structure and registers for the signal with
   GDBus so that we can get a callback */
static gboolean
add_helper_generic (UbuntuAppLaunchHelperObserver observer, const gchar * helper_type, gpointer user_data, const gchar * signal, GList ** list)
{
	GDBusConnection * conn = gdbus_upstart_ref();

	if (conn == NULL) {
		return FALSE;
	}

	helper_observer_t * observert = g_new0(helper_observer_t, 1);

	observert->conn = conn;
	observert->func = observer;
	observert->user_data = user_data;
	observert->type = g_strdup_printf("%s:", helper_type);

	*list = g_list_prepend(*list, observert);

	observert->sighandle = g_dbus_connection_signal_subscribe(conn,
		NULL, /* sender */
		DBUS_INTERFACE_UPSTART, /* interface */
		"EventEmitted", /* signal */
		DBUS_PATH_UPSTART, /* path */
		signal, /* arg0 */
		G_DBUS_SIGNAL_FLAGS_NONE,
		helper_observer_cb,
		observert,
		NULL); /* user data destroy */

	return TRUE;
}

static gboolean
delete_helper_generic (UbuntuAppLaunchHelperObserver observer, const gchar * type, gpointer user_data, GList ** list)
{
	helper_observer_t * observert = NULL;
	GList * look;

	for (look = *list; look != NULL; look = g_list_next(look)) {
		observert = (helper_observer_t *)look->data;

		if (observert->func == observer && observert->user_data == user_data && g_str_has_prefix(observert->type, type)) {
			break;
		}
	}

	if (look == NULL) {
		return FALSE;
	}

	g_dbus_connection_signal_unsubscribe(observert->conn, observert->sighandle);
	g_object_unref(observert->conn);

	g_free(observert->type);
	g_free(observert);
	*list = g_list_delete_link(*list, look);

	return TRUE;
}

gboolean
ubuntu_app_launch_observer_add_helper_started (UbuntuAppLaunchHelperObserver observer, const gchar * helper_type, gpointer user_data)
{
	g_return_val_if_fail(observer != NULL, FALSE);
	g_return_val_if_fail(helper_type != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(helper_type, -1, ":") == NULL, FALSE);

	return add_helper_generic(observer, helper_type, user_data, "started", &helper_started_obs);
}

gboolean
ubuntu_app_launch_observer_add_helper_stop (UbuntuAppLaunchHelperObserver observer, const gchar * helper_type, gpointer user_data)
{
	g_return_val_if_fail(observer != NULL, FALSE);
	g_return_val_if_fail(helper_type != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(helper_type, -1, ":") == NULL, FALSE);

	return add_helper_generic(observer, helper_type, user_data, "stopped", &helper_stopped_obs);
}

gboolean
ubuntu_app_launch_observer_delete_helper_started (UbuntuAppLaunchHelperObserver observer, const gchar * helper_type, gpointer user_data)
{
	g_return_val_if_fail(observer != NULL, FALSE);
	g_return_val_if_fail(helper_type != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(helper_type, -1, ":") == NULL, FALSE);

	return delete_helper_generic(observer, helper_type, user_data, &helper_started_obs);
}

gboolean
ubuntu_app_launch_observer_delete_helper_stop (UbuntuAppLaunchHelperObserver observer, const gchar * helper_type, gpointer user_data)
{
	g_return_val_if_fail(observer != NULL, FALSE);
	g_return_val_if_fail(helper_type != NULL, FALSE);
	g_return_val_if_fail(g_strstr_len(helper_type, -1, ":") == NULL, FALSE);

	return delete_helper_generic(observer, helper_type, user_data, &helper_stopped_obs);
}

/* Sets an environment variable in Upstart */
static void
set_var (GDBusConnection * bus, const gchar * job_name, const gchar * instance_name, const gchar * envvar)
{
	GVariantBuilder builder; /* Target: (assb) */
	g_variant_builder_init(&builder, G_VARIANT_TYPE_TUPLE);

	/* Setup the job properties */
	g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);
	g_variant_builder_add_value(&builder, g_variant_new_string(job_name));
	if (instance_name != NULL)
		g_variant_builder_add_value(&builder, g_variant_new_string(instance_name));
	g_variant_builder_close(&builder);

	g_variant_builder_add_value(&builder, g_variant_new_string(envvar));

	/* Do we want to replace?  Yes, we do! */
	g_variant_builder_add_value(&builder, g_variant_new_boolean(TRUE));

	g_dbus_connection_call(bus,
		"com.ubuntu.Upstart",
		"/com/ubuntu/Upstart",
		"com.ubuntu.Upstart0_6",
		"SetEnv",
		g_variant_builder_end(&builder),
		NULL, /* reply */
		G_DBUS_CALL_FLAGS_NONE,
		-1, /* timeout */
		NULL, /* cancellable */
		NULL, NULL); /* callback */
}

gboolean
ubuntu_app_launch_helper_set_exec (const gchar * execline, const gchar * directory)
{
	g_return_val_if_fail(execline != NULL, FALSE);
	g_return_val_if_fail(execline[0] != '\0', FALSE);

	/* Check to see if we can get the job environment */
	const gchar * job_name = g_getenv("UPSTART_JOB");
	const gchar * instance_name = g_getenv("UPSTART_INSTANCE");
	const gchar * demangler = g_getenv("UBUNTU_APP_LAUNCH_DEMANGLE_NAME");
	g_return_val_if_fail(job_name != NULL, FALSE);

	GError * error = NULL;
	GDBusConnection * bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

	if (error != NULL) {
		g_warning("Unable to get session bus: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	/* The exec value */
	gchar * envstr = NULL;
	if (demangler) {
		const gchar * demangler_path = g_getenv("UBUNTU_APP_LAUNCH_DEMANGLER");
		if (demangler_path == nullptr) {
			demangler_path = DEMANGLER_PATH;
		}
		envstr = g_strdup_printf("APP_EXEC=%s %s", demangler_path, execline);
	} else {
		envstr = g_strdup_printf("APP_EXEC=%s", execline);
	}

	set_var(bus, job_name, instance_name, envstr);
	g_free(envstr);

	/* The directory value */
	if (directory != NULL) {
		gchar * direnv = g_strdup_printf("APP_DIR=%s", directory);
		set_var(bus, job_name, instance_name, direnv);
		g_free(direnv);
	}

	g_object_unref(bus);

	return TRUE;
}

