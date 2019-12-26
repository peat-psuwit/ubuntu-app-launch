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

#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <future>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>
#include <libdbustest/dbus-test.h>
#include <numeric>
#include <thread>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <zeitgeist.h>
#pragma GCC diagnostic pop

#include "application.h"
#include "glib-thread.h"
#include "helper-impl.h"
#include "helper.h"
#include "jobs-base.h"
#include "registry.h"
#include "ubuntu-app-launch.h"

#include "eventually-fixture.h"
#ifdef HAVE_LIBERTINE
#include "libertine-service.h"
#endif
#include "mir-mock.h"
#include "registry-mock.h"
#include "snapd-mock.h"
#include "spew-master.h"
#include "systemd-mock.h"
#include "test-directory.h"
#include "zg-mock.h"

#define LOCAL_SNAPD_TEST_SOCKET (SNAPD_TEST_SOCKET "-libual-cpp-test")

#define CGROUP_DIR (CMAKE_BINARY_DIR "/systemd-libual-cpp-cgroups")

class LibUAL : public EventuallyFixture
{
protected:
    DbusTestService* service = NULL;
    DbusTestDbusMock* mock = NULL;
    DbusTestDbusMock* cgmock = NULL;
#ifdef HAVE_LIBERTINE
    std::shared_ptr<LibertineService> libertine;
#endif
    std::shared_ptr<SystemdMock> systemd;
    GDBusConnection* bus = NULL;
    guint resume_timeout = 0;
    std::shared_ptr<ubuntu::app_launch::Registry> registry;

    class ManagerMock : public ubuntu::app_launch::Registry::Manager
    {
        GLib::ContextThread thread;

    public:
        ManagerMock()
        {
            g_debug("Building a Manager Mock");
        }

        ~ManagerMock()
        {
            g_debug("Freeing a Manager Mock");
        }

        void quit()
        {
            thread.quit();
        }

        ubuntu::app_launch::AppID lastStartedApp;
        ubuntu::app_launch::AppID lastFocusedApp;
        ubuntu::app_launch::AppID lastResumedApp;

        bool startingResponse{true};
        bool focusResponse{true};
        bool resumeResponse{true};

        std::chrono::milliseconds startingTimeout{0};
        std::chrono::milliseconds focusTimeout{0};
        std::chrono::milliseconds resumeTimeout{0};

        void startingRequest(const std::shared_ptr<ubuntu::app_launch::Application>& app,
                             const std::shared_ptr<ubuntu::app_launch::Application::Instance>& instance,
                             std::function<void(bool)> reply) override
        {
            g_debug("Manager Mock: Starting Request: %s", std::string(app->appId()).c_str());
            thread.timeout(startingTimeout, [this, app, instance, reply]() {
                lastStartedApp = app->appId();
                reply(startingResponse);
            });
        }

        void focusRequest(const std::shared_ptr<ubuntu::app_launch::Application>& app,
                          const std::shared_ptr<ubuntu::app_launch::Application::Instance>& instance,
                          std::function<void(bool)> reply) override
        {
            g_debug("Manager Mock: Focus Request: %s", std::string(app->appId()).c_str());
            thread.timeout(focusTimeout, [this, app, instance, reply]() {
                lastFocusedApp = app->appId();
                reply(focusResponse);
            });
        }

        void resumeRequest(const std::shared_ptr<ubuntu::app_launch::Application>& app,
                           const std::shared_ptr<ubuntu::app_launch::Application::Instance>& instance,
                           std::function<void(bool)> reply) override
        {
            g_debug("Manager Mock: Resume Request: %s", std::string(app->appId()).c_str());
            thread.timeout(resumeTimeout, [this, app, instance, reply]() {
                lastResumedApp = app->appId();
                reply(resumeResponse);
            });
        }
    };
    std::shared_ptr<ManagerMock> manager;

    /* Useful debugging stuff, but not on by default.  You really want to
       not get all this noise typically */
    void debugConnection()
    {
        if (true)
        {
            return;
        }

        DbusTestBustle* bustle = dbus_test_bustle_new("test.bustle");
        dbus_test_service_add_task(service, DBUS_TEST_TASK(bustle));
        g_object_unref(bustle);

        DbusTestProcess* monitor = dbus_test_process_new("dbus-monitor");
        dbus_test_service_add_task(service, DBUS_TEST_TASK(monitor));
        g_object_unref(monitor);
    }

    virtual void SetUp()
    {
        g_setenv("XDG_DATA_DIRS", CMAKE_SOURCE_DIR, TRUE);
        g_setenv("XDG_CACHE_HOME", CMAKE_SOURCE_DIR "/libertine-data", TRUE);
        g_setenv("XDG_DATA_HOME", CMAKE_SOURCE_DIR "/libertine-home", TRUE);

        g_setenv("UBUNTU_APP_LAUNCH_SNAPD_SOCKET", LOCAL_SNAPD_TEST_SOCKET, TRUE);
        g_setenv("UBUNTU_APP_LAUNCH_SNAP_BASEDIR", SNAP_BASEDIR, TRUE);
        g_setenv("UBUNTU_APP_LAUNCH_DISABLE_SNAPD_TIMEOUT", "You betcha!", TRUE);
        g_unlink(LOCAL_SNAPD_TEST_SOCKET);

        g_setenv("UBUNTU_APP_LAUNCH_SYSTEMD_PATH", "/this/should/not/exist", TRUE);
        /* Setting the cgroup temp directory */
        g_setenv("UBUNTU_APP_LAUNCH_SYSTEMD_CGROUP_ROOT", CGROUP_DIR, TRUE);

        service = dbus_test_service_new(NULL);

        debugConnection();

        systemd = std::make_shared<SystemdMock>(
            std::list<SystemdMock::Instance>{
                {"application-snap", "unity8-package_foo_x123", {}, getpid(), {100, 200, 300}},
                {"application-legacy", "multiple", "2342345", 5678, {100, 200, 300}},
                {"application-legacy", "single", {}, getpid(), {getpid()}},
                {"untrusted-helper", "com.foo_bar_43.23.12", {}, 1, {100, 200, 300}},
                {"untrusted-helper", "com.bar_foo_8432.13.1", "24034582324132", 1, {100, 200, 300}}},
            CGROUP_DIR);

        /* Put it together */
        dbus_test_service_add_task(service, *systemd);

#ifdef HAVE_LIBERTINE
        /* Add in Libertine */
        libertine = std::make_shared<LibertineService>();
        dbus_test_service_add_task(service, *libertine);
#endif

        dbus_test_service_start_tasks(service);

        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        g_dbus_connection_set_exit_on_close(bus, FALSE);
        g_object_add_weak_pointer(G_OBJECT(bus), (gpointer*)&bus);

#ifdef HAVE_LIBERTINE
        ASSERT_EVENTUALLY_FUNC_EQ(false, std::function<bool()>{[&] { return libertine->getUniqueName().empty(); }});
#endif

        registry = std::make_shared<ubuntu::app_launch::Registry>();

        manager = std::make_shared<ManagerMock>();
        ubuntu::app_launch::Registry::setManager(manager, registry);
    }

    virtual void TearDown()
    {
        manager->quit();
        registry.reset();
        manager.reset();

        // NOTE: This should generally always be commented out, but
        // it is useful for debugging common errors, so leaving it
        // as a comment to make debugging those eaiser.
        //
        // ubuntu::app_launch::Registry::clearDefault();

        systemd.reset();
#ifdef HAVE_LIBERTINE
        libertine.reset();
#endif
        g_clear_object(&service);

        g_object_unref(bus);

        ASSERT_EVENTUALLY_EQ(nullptr, bus);

        g_unlink(LOCAL_SNAPD_TEST_SOCKET);
    }

    static std::string find_env(std::set<std::string>& envs, std::string var)
    {
        auto iter =
            std::find_if(envs.begin(), envs.end(), [var](std::string value) { return split_env(value).first == var; });

        if (iter == envs.end())
        {
            return {};
        }
        else
        {
            return *iter;
        }
    }

    static std::pair<std::string, std::string> split_env(const std::string& env)
    {
        auto eq = std::find(env.begin(), env.end(), '=');
        if (eq == env.end())
        {
            throw std::runtime_error{"Environment value is invalid: " + env};
        }

        return std::make_pair(std::string(env.begin(), eq), std::string(eq + 1, env.end()));
    }

    static bool check_env(std::set<std::string>& envs, const std::string& key, const std::string& value)
    {
        auto val = find_env(envs, key);
        if (val.empty())
        {
            return false;
        }
        return split_env(val).second == value;
    }

    void storeForApp(const ubuntu::app_launch::AppID& appid, const std::string& jobtype, const std::string& instanceid)
    {
        auto store = storeForHelper(appid);

        ON_CALL(*store, list())
            .WillByDefault(testing::Return(std::list<std::shared_ptr<ubuntu::app_launch::Application>>{}));

        auto app = std::make_shared<MockApp>(appid, registry->impl);
        ON_CALL(*store, create(appid)).WillByDefault(testing::Return(app));

        if (!instanceid.empty())
        {
            std::vector<ubuntu::app_launch::Application::URL> urls;
            auto inst = std::make_shared<MockInst>(appid, jobtype, instanceid, urls, registry->impl);
            ON_CALL(*app, findInstance(instanceid)).WillByDefault(testing::Return(inst));
            ON_CALL(*app, launch(testing::_)).WillByDefault(testing::Return(inst));
            ON_CALL(*app, launchTest(testing::_)).WillByDefault(testing::Return(inst));
            ON_CALL(*app, hasInstances()).WillByDefault(testing::Return(true));
        }
        else
        {
            ON_CALL(*app, hasInstances()).WillByDefault(testing::Return(false));
        }

        std::list<std::shared_ptr<ubuntu::app_launch::app_store::Base>> list;
        list.push_back(store);
        registry->impl->setAppStores(list);
    }

    std::shared_ptr<MockStore> storeForHelper(const ubuntu::app_launch::AppID& appid)
    {
        /* Setup a store for looking up the AppID */
        auto store = std::make_shared<MockStore>(registry->impl);

        ON_CALL(*store, verifyPackage(appid.package)).WillByDefault(testing::Return(true));
        ON_CALL(*store, verifyAppname(appid.package, appid.appname)).WillByDefault(testing::Return(true));
        ON_CALL(*store, findAppname(appid.package, testing::_)).WillByDefault(testing::Return(appid.appname));
        ON_CALL(*store, findVersion(appid.package, appid.appname)).WillByDefault(testing::Return(appid.version));
        ON_CALL(*store, hasAppId(appid)).WillByDefault(testing::Return(true));

        std::list<std::shared_ptr<ubuntu::app_launch::app_store::Base>> list;
        list.push_back(store);
        registry->impl->setAppStores(list);

        return store;
    }
};

#define TASK_STATE(task)                                                   \
    std::function<DbusTestTaskState()>                                     \
    {                                                                      \
        [&task] { return dbus_test_task_get_state(DBUS_TEST_TASK(task)); } \
    }

/* Snapd mock data */
static std::pair<std::string, std::string> interfaces{
    "GET /v2/interfaces HTTP/1.1\r\nHost: snapd\r\nAccept: */*\r\n\r\n",
    SnapdMock::httpJsonResponse(SnapdMock::snapdOkay(
        SnapdMock::interfacesJson({{"mir", "unity8-package", {"foo", "single"}}})))};
static std::pair<std::string, std::string> u8Package{
    "GET /v2/snaps/unity8-package HTTP/1.1\r\nHost: snapd\r\nAccept: */*\r\n\r\n",
    SnapdMock::httpJsonResponse(SnapdMock::snapdOkay(SnapdMock::packageJson(
        "unity8-package", "active", "app", "1.2.3.4", "x123", {"foo", "single", "xmir", "noxmir"})))};
static std::pair<std::string, std::string> helloPackage{
    "GET /v2/snaps/hello HTTP/1.1\r\nHost: snapd\r\nAccept: */*\r\n\r\n",
    SnapdMock::httpJsonResponse(
        SnapdMock::snapdOkay(SnapdMock::packageJson("hello", "active", "app", "1.0", "1", {"hello"})))};

TEST_F(LibUAL, ApplicationIdSnap)
{
    SnapdMock snapd{LOCAL_SNAPD_TEST_SOCKET,
                    {u8Package, u8Package, u8Package, u8Package, u8Package, u8Package, u8Package, u8Package, u8Package,
                     u8Package, u8Package, u8Package, u8Package, u8Package, u8Package, u8Package}};
    registry = std::make_shared<ubuntu::app_launch::Registry>();

    EXPECT_EQ("unity8-package_foo_x123", (std::string)ubuntu::app_launch::AppID::discover(registry, "unity8-package"));
    EXPECT_EQ("unity8-package_foo_x123",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "unity8-package", "foo"));
    EXPECT_EQ("unity8-package_single_x123",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "unity8-package", "single"));
    EXPECT_EQ("unity8-package_xmir_x123",
              (std::string)ubuntu::app_launch::AppID::discover(
                  registry, "unity8-package", ubuntu::app_launch::AppID::ApplicationWildcard::LAST_LISTED));
    EXPECT_EQ("unity8-package_foo_x123",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "unity8-package", "foo", "x123"));

    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "unity7-package"));
}

TEST_F(LibUAL, ApplicationIconSnap)
{
    /* Queries come in threes, apparently */
    SnapdMock snapd{LOCAL_SNAPD_TEST_SOCKET,
                    {
                        u8Package, interfaces, u8Package, /* App 1 */
                        u8Package, interfaces, u8Package, /* App 2 */
                        u8Package, interfaces, u8Package, /* App 3 */
                        u8Package, interfaces, u8Package, /* App 4 */
                    }};
    registry = std::make_shared<ubuntu::app_launch::Registry>();

    std::string snapRoot{SNAP_BASEDIR};

    /* Check the /snap/foo/current/ prefixed case */
    auto appid = ubuntu::app_launch::AppID::parse("unity8-package_foo_x123");
    auto app = ubuntu::app_launch::Application::create(appid, registry);
    auto expected = snapRoot + "/unity8-package/x123/foo.png";
    EXPECT_EQ(expected, app->info()->iconPath().value());

    /* Check the ${SNAP}/ prefixed case */
    appid = ubuntu::app_launch::AppID::parse("unity8-package_single_x123");
    app = ubuntu::app_launch::Application::create(appid, registry);
    expected = snapRoot + "/unity8-package/x123/single.png";
    EXPECT_EQ(expected, app->info()->iconPath().value());

    /* Check the un-prefixed "foo.png" case in meta/gui dir */
    appid = ubuntu::app_launch::AppID::parse("unity8-package_xmir_x123");
    app = ubuntu::app_launch::Application::create(appid, registry);
    expected = snapRoot + "/unity8-package/x123/meta/gui/xmir.png";
    EXPECT_EQ(expected, app->info()->iconPath().value());

    /* Check the un-prefixed "foo.png" case in snap's root dir */
    appid = ubuntu::app_launch::AppID::parse("unity8-package_noxmir_x123");
    app = ubuntu::app_launch::Application::create(appid, registry);
    expected = snapRoot + "/unity8-package/x123/no-xmir.png";
    EXPECT_EQ(expected, app->info()->iconPath().value());
}

TEST_F(LibUAL, NoGraphicalSnapInterface)
{
    SnapdMock snapd{LOCAL_SNAPD_TEST_SOCKET, {helloPackage, interfaces, helloPackage}};
    registry = std::make_shared<ubuntu::app_launch::Registry>();

    auto appid = ubuntu::app_launch::AppID::parse("hello_hello_1");

    EXPECT_THROW(ubuntu::app_launch::Application::create(appid, registry), std::runtime_error);
}

TEST_F(LibUAL, ApplicationPid)
{
    /* Queries come in threes, apparently */
    SnapdMock snapd{LOCAL_SNAPD_TEST_SOCKET,
                    {
                        u8Package, interfaces, u8Package, /* App */
                    }};
    registry = std::make_shared<ubuntu::app_launch::Registry>();

    /* Check bad params */
    auto appid = ubuntu::app_launch::AppID::parse("unity8-package_foo_x123");
    auto app = ubuntu::app_launch::Application::create(appid, registry);

    ASSERT_LT(0, int(app->instances().size()));

    /* Look at PIDs from cgmanager */
    EXPECT_FALSE(app->instances()[0]->hasPid(1));
    EXPECT_TRUE(app->instances()[0]->hasPid(100));
    EXPECT_TRUE(app->instances()[0]->hasPid(200));
    EXPECT_TRUE(app->instances()[0]->hasPid(300));

    /* Check primary pid, which comes from Upstart */
    EXPECT_TRUE(app->instances()[0]->isRunning());
    EXPECT_EQ(getpid(), app->instances()[0]->primaryPid());

    auto multiappid = ubuntu::app_launch::AppID::find(registry, "multiple");
    auto multiapp = ubuntu::app_launch::Application::create(multiappid, registry);
    auto instances = multiapp->instances();
    ASSERT_LT(0, int(instances.size()));
    EXPECT_EQ(5678, instances[0]->primaryPid());

    /* Legacy Single Instance */
    auto singleappid = ubuntu::app_launch::AppID::find(registry, "single");
    auto singleapp = ubuntu::app_launch::Application::create(singleappid, registry);

    ASSERT_LT(0, int(singleapp->instances().size()));
    EXPECT_TRUE(singleapp->instances()[0]->hasPid(getpid()));
}

TEST_F(LibUAL, ApplicationId)
{
    auto mockstore = std::make_shared<MockStore>(registry->impl);
    registry =
        std::make_shared<RegistryMock>(std::list<std::shared_ptr<ubuntu::app_launch::app_store::Base>>{mockstore},
                                       std::shared_ptr<ubuntu::app_launch::jobs::manager::Base>{});

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.good")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, verifyAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                          ubuntu::app_launch::AppID::AppName::from_raw("application")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findVersion(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                        ubuntu::app_launch::AppID::AppName::from_raw("application")))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::Version::from_raw("1.2.3")));

    /* Test with current-user-version, should return the version in the manifest */
    EXPECT_EQ("com.test.good_application_1.2.3",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.good", "application"));

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.good")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, verifyAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                          ubuntu::app_launch::AppID::AppName::from_raw("application")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore,
                hasAppId(ubuntu::app_launch::AppID{ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                                   ubuntu::app_launch::AppID::AppName::from_raw("application"),
                                                   ubuntu::app_launch::AppID::Version::from_raw("1.2.4")}))
        .WillOnce(testing::Return(true));

    /* Test with version specified, shouldn't even read the manifest */
    EXPECT_EQ("com.test.good_application_1.2.4",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.good", "application", "1.2.4"));

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.good")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                        ubuntu::app_launch::AppID::ApplicationWildcard::FIRST_LISTED))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::AppName::from_raw("application")));
    EXPECT_CALL(*mockstore, findVersion(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                        ubuntu::app_launch::AppID::AppName::from_raw("application")))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::Version::from_raw("1.2.3")));

    /* Test with out a version or app, should return the version in the manifest */
    EXPECT_EQ("com.test.good_application_1.2.3",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.good", "first-listed-app",
                                                               "current-user-version"));

    /* Make sure we can select the app from a list correctly */
    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::ApplicationWildcard::FIRST_LISTED))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::AppName::from_raw("first")));
    EXPECT_CALL(*mockstore, findVersion(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::AppName::from_raw("first")))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::Version::from_raw("1.2.3")));
    EXPECT_EQ("com.test.multiple_first_1.2.3",
              (std::string)ubuntu::app_launch::AppID::discover(
                  registry, "com.test.multiple", ubuntu::app_launch::AppID::ApplicationWildcard::FIRST_LISTED));

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::ApplicationWildcard::FIRST_LISTED))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::AppName::from_raw("first")));
    EXPECT_CALL(*mockstore, findVersion(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::AppName::from_raw("first")))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::Version::from_raw("1.2.3")));
    EXPECT_EQ("com.test.multiple_first_1.2.3",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.multiple"));

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::ApplicationWildcard::LAST_LISTED))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::AppName::from_raw("fifth")));
    EXPECT_CALL(*mockstore, findVersion(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::AppName::from_raw("fifth")))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::Version::from_raw("1.2.3")));
    EXPECT_EQ("com.test.multiple_fifth_1.2.3",
              (std::string)ubuntu::app_launch::AppID::discover(
                  registry, "com.test.multiple", ubuntu::app_launch::AppID::ApplicationWildcard::LAST_LISTED));

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.multiple"),
                                        ubuntu::app_launch::AppID::ApplicationWildcard::ONLY_LISTED))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::AppName::from_raw("")));
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(
                      registry, "com.test.multiple", ubuntu::app_launch::AppID::ApplicationWildcard::ONLY_LISTED));

    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.good")))
        .WillOnce(testing::Return(true));
    EXPECT_CALL(*mockstore, findAppname(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                        ubuntu::app_launch::AppID::ApplicationWildcard::ONLY_LISTED))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::AppName::from_raw("application")));
    EXPECT_CALL(*mockstore, findVersion(ubuntu::app_launch::AppID::Package::from_raw("com.test.good"),
                                        ubuntu::app_launch::AppID::AppName::from_raw("application")))
        .WillOnce(testing::Return(ubuntu::app_launch::AppID::Version::from_raw("1.2.3")));
    EXPECT_EQ("com.test.good_application_1.2.3",
              (std::string)ubuntu::app_launch::AppID::discover(
                  registry, "com.test.good", ubuntu::app_launch::AppID::ApplicationWildcard::ONLY_LISTED));

    /* A bunch that should be NULL */
    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.no-hooks")))
        .WillOnce(testing::Return(false));
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.no-hooks"));
    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.no-json")))
        .WillOnce(testing::Return(false));
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.no-json"));
    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.no-object")))
        .WillOnce(testing::Return(false));
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.no-object"));
    EXPECT_CALL(*mockstore, verifyPackage(ubuntu::app_launch::AppID::Package::from_raw("com.test.no-version")))
        .WillOnce(testing::Return(false));
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "com.test.no-version"));
}

#ifdef HAVE_LIBERTINE
TEST_F(LibUAL, ApplicationIdLibertine)
{
    /* Libertine tests */
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "container-name"));
    EXPECT_EQ("", (std::string)ubuntu::app_launch::AppID::discover(registry, "container-name", "not-exist"));
    EXPECT_EQ("container-name_test_0.0",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "container-name", "test"));
    EXPECT_EQ("container-name_user-app_0.0",
              (std::string)ubuntu::app_launch::AppID::discover(registry, "container-name", "user-app"));
}
#endif

TEST_F(LibUAL, AppIdParse)
{
    EXPECT_FALSE(ubuntu::app_launch::AppID::parse("com.ubuntu.test_test_123").empty());
    EXPECT_FALSE(ubuntu::app_launch::AppID::parse("chatter.robert-ancell_chatter_2").empty());

    auto id = ubuntu::app_launch::AppID::parse("com.ubuntu.test_test_123");

    ASSERT_FALSE(id.empty());
    EXPECT_EQ("com.ubuntu.test", id.package.value());
    EXPECT_EQ("test", id.appname.value());
    EXPECT_EQ("123", id.version.value());

    return;
}

TEST_F(LibUAL, DBusID)
{
    auto id = ubuntu::app_launch::AppID::parse("container-name_test_0.0");
    ASSERT_FALSE(id.empty());
    EXPECT_EQ("container_2dname_5ftest_5f0_2e0", id.dbusID());
    EXPECT_FALSE(ubuntu::app_launch::AppID::valid(id.dbusID()));

    auto parsed = ubuntu::app_launch::AppID::parseDBusID(id.dbusID());
    ASSERT_FALSE(parsed.empty());
    EXPECT_EQ(id, parsed);
}

#ifdef HAVE_LIBERTINE
/* FIXME: This test seems to only work when libertine is available.
 * However, it shouldn't need it, and should be testable with the snapd
 * interface, though that too is not working properly when added here.
 */
TEST_F(LibUAL, PersistentID)
{
    auto id = ubuntu::app_launch::AppID::parse("container-name_test_0.0");
    ASSERT_FALSE(id.empty());
    EXPECT_EQ("container-name_test", id.persistentID());
    EXPECT_FALSE(ubuntu::app_launch::AppID::valid(id.persistentID()));

    auto found = ubuntu::app_launch::AppID::find(registry, id.persistentID());
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(id, found);
}
#endif

TEST_F(LibUAL, ApplicationList)
{
    SnapdMock snapd{LOCAL_SNAPD_TEST_SOCKET, {u8Package, interfaces, u8Package}};
    registry = std::make_shared<ubuntu::app_launch::Registry>();

    auto apps = ubuntu::app_launch::Registry::runningApps(registry);

    ASSERT_EQ(3u, apps.size());

    apps.sort([](const std::shared_ptr<ubuntu::app_launch::Application>& a,
                 const std::shared_ptr<ubuntu::app_launch::Application>& b) {
        std::string sa = a->appId();
        std::string sb = b->appId();

        return sa < sb;
    });

    EXPECT_EQ("multiple", (std::string)apps.front()->appId());
    EXPECT_EQ("unity8-package_foo_x123", (std::string)apps.back()->appId());
}

TEST_F(LibUAL, Equality)
{
    auto appid = ubuntu::app_launch::AppID::find(registry, "single");

    auto black = std::make_shared<MockApp>(appid, registry->impl);
    auto white = std::make_shared<MockApp>(appid, registry->impl);

    /* The outside isn't equal */
    EXPECT_NE(black, white);

    /* The inside is */
    EXPECT_EQ(*black, *white);

    auto multiappid = ubuntu::app_launch::AppID::find(registry, "multiple");

    auto cat = std::make_shared<MockApp>(appid, registry->impl);
    auto dog = std::make_shared<MockApp>(multiappid, registry->impl);

    EXPECT_NE(*cat, *dog);
}

TEST_F(LibUAL, StartingResponses)
{
    /* Get Bus */
    GDBusConnection* session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

    /* Setup filter to count signals out */
    int starting_count = 0;
    guint filter = g_dbus_connection_add_filter(
        session,
        [](GDBusConnection* conn, GDBusMessage* message, gboolean incomming, gpointer user_data) -> GDBusMessage* {
            if (g_strcmp0(g_dbus_message_get_member(message), "UnityStartingSignal") == 0)
            {
                auto count = static_cast<int*>(user_data);
                (*count)++;
                g_object_unref(message);
                return NULL;
            }

            return message;
        },
        &starting_count, NULL);

    /* Emit a signal */
    g_dbus_connection_emit_signal(
        session, NULL,                                                    /* destination */
        "/",                                                              /* path */
        "com.canonical.UbuntuAppLaunch",                                  /* interface */
        "UnityStartingBroadcast",                                         /* signal */
        g_variant_new("(ss)", "no-exec", "goodinstance"), /* params, the same */
        NULL);

    /* Make sure we run our observer */
    EXPECT_EVENTUALLY_EQ(ubuntu::app_launch::AppID::parse("no-exec"),
                         manager->lastStartedApp);

    /* Make sure we return */
    EXPECT_EVENTUALLY_EQ(1, starting_count);

    g_dbus_connection_remove_filter(session, filter);
    g_object_unref(session);
}

TEST_F(LibUAL, AppIdTest)
{
    auto appid = ubuntu::app_launch::AppID::find(registry, "single");
    auto app = ubuntu::app_launch::Application::create(appid, registry);
    app->launch();

    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastFocusedApp);
    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastResumedApp);
}

TEST_F(LibUAL, UrlSendTest)
{
    auto appid = ubuntu::app_launch::AppID::find(registry, "foo");
    auto app = ubuntu::app_launch::Application::create(appid, registry);
    std::vector<ubuntu::app_launch::Application::URL> uris = {
        ubuntu::app_launch::Application::URL::from_raw("http://www.test.com")};

    app->launch(uris);

    std::list<SystemdMock::TransientUnit> calls;
    ASSERT_EVENTUALLY_FUNC_LT(0u, std::function<unsigned int(void)>([&]() {
                                  calls = systemd->unitCalls();
                                  return calls.size();
                              }));
    EXPECT_EQ("http://www.test.com", *calls.begin()->execline.rbegin());
}

TEST_F(LibUAL, UnityTimeoutTest)
{
    this->resume_timeout = 100;

    auto appid = ubuntu::app_launch::AppID::find(registry, "single");
    auto app = ubuntu::app_launch::Application::create(appid, registry);

    app->launch();

    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastResumedApp);
    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastFocusedApp);
}

TEST_F(LibUAL, UnityTimeoutUriTest)
{
    this->resume_timeout = 200;

    auto appid = ubuntu::app_launch::AppID::find(registry, "single");
    auto app = ubuntu::app_launch::Application::create(appid, registry);
    std::vector<ubuntu::app_launch::Application::URL> uris = {
        ubuntu::app_launch::Application::URL::from_raw("http://www.test.com")};

    app->launch(uris);

    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastFocusedApp);
    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastResumedApp);
}

GDBusMessage* filter_respawn(GDBusConnection* conn, GDBusMessage* message, gboolean incomming, gpointer user_data)
{
    if (g_strcmp0(g_dbus_message_get_member(message), "UnityResumeResponse") == 0)
    {
        g_object_unref(message);
        return NULL;
    }

    return message;
}

TEST_F(LibUAL, UnityLostTest)
{
    GDBusConnection* session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    guint filter = g_dbus_connection_add_filter(session, filter_respawn, NULL, NULL);

    guint start = g_get_monotonic_time();

    auto appid = ubuntu::app_launch::AppID::find(registry, "single");
    auto app = ubuntu::app_launch::Application::create(appid, registry);
    std::vector<ubuntu::app_launch::Application::URL> uris = {
        ubuntu::app_launch::Application::URL::from_raw("http://www.test.com")};

    app->launch(uris);

    guint end = g_get_monotonic_time();

    g_debug("Start call time: %d ms", (end - start) / 1000);
    EXPECT_LT(end - start, guint(2000 * 1000));

    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastFocusedApp);
    EXPECT_EVENTUALLY_EQ(appid, this->manager->lastResumedApp);

    g_dbus_connection_remove_filter(session, filter);
    g_object_unref(session);
}

TEST_F(LibUAL, LegacySingleInstance)
{
    /* Check for a single-instance app */
    auto singleappid = ubuntu::app_launch::AppID::find(registry, "single");
    auto singleapp = ubuntu::app_launch::Application::create(singleappid, registry);

    singleapp->launch();

    auto singleStart = systemd->unitCalls();
    ASSERT_EQ(1u, singleStart.size());
    EXPECT_EQ(SystemdMock::instanceName({"application-legacy", "single", "", 0, {}}), singleStart.begin()->name);

    systemd->managerClear();

    /* Check for a multi-instance app */
    auto multipleappid = ubuntu::app_launch::AppID::find(registry, "multiple");
    auto multipleapp = ubuntu::app_launch::Application::create(multipleappid, registry);

    auto inst = multipleapp->launch();

    auto multiStart = systemd->unitCalls();
    ASSERT_EQ(1u, multiStart.size());
    EXPECT_EQ(SystemdMock::instanceName(
                  {"application-legacy",
                   "multiple",
                   std::dynamic_pointer_cast<ubuntu::app_launch::jobs::instance::Base>(inst)->getInstanceId(),
                   0,
                   {}}),
              multiStart.begin()->name);
}

TEST_F(LibUAL, StartHelper)
{
    auto appid = ubuntu::app_launch::AppID::parse("com.test.multiple_first_1.2.3");
    auto untrusted = ubuntu::app_launch::Helper::Type::from_raw("untrusted-type");

    storeForHelper(appid);

    /* Basic make sure we can send the event */
    auto helper = ubuntu::app_launch::Helper::create(untrusted, appid, registry);

    auto inst = helper->launch();

    auto helperStart = systemd->unitCalls();

    ASSERT_EQ(1u, helperStart.size());
    EXPECT_EQ(SystemdMock::instanceName(
                  {"untrusted-type",
                   "com.test.multiple_first_1.2.3",
                   std::dynamic_pointer_cast<ubuntu::app_launch::helper_impls::BaseInstance>(inst)->getInstanceId(),
                   0,
                   {}}),
              helperStart.begin()->name);

    systemd->managerClear();

    /* Now check a multi out */
    auto inst2 = helper->launch();

    auto helperStart2 = systemd->unitCalls();

    ASSERT_EQ(1u, helperStart2.size());
    EXPECT_EQ(SystemdMock::instanceName(
                  {"untrusted-type",
                   "com.test.multiple_first_1.2.3",
                   std::dynamic_pointer_cast<ubuntu::app_launch::helper_impls::BaseInstance>(inst2)->getInstanceId(),
                   0,
                   {}}),
              helperStart2.begin()->name);

    systemd->managerClear();

    /* Let's pass some URLs */
    std::vector<ubuntu::app_launch::Helper::URL> urls = {
        ubuntu::app_launch::Helper::URL::from_raw("http://ubuntu.com/"),
        ubuntu::app_launch::Helper::URL::from_raw("https://ubuntu.com/"),
        ubuntu::app_launch::Helper::URL::from_raw("file:///home/phablet/test.txt")};

    auto inst3 = helper->launch(urls);

    auto helperStart3 = systemd->unitCalls();

    ASSERT_EQ(1u, helperStart3.size());
    EXPECT_EQ(SystemdMock::instanceName(
                  {"untrusted-type",
                   "com.test.multiple_first_1.2.3",
                   std::dynamic_pointer_cast<ubuntu::app_launch::helper_impls::BaseInstance>(inst3)->getInstanceId(),
                   0,
                   {}}),
              helperStart3.begin()->name);

    /* TODO: Check URLS in exec */

    return;
}

TEST_F(LibUAL, StopHelper)
{
    /* Multi helper */
    auto untrusted = ubuntu::app_launch::Helper::Type::from_raw("untrusted-helper");

    auto appid = ubuntu::app_launch::AppID::parse("com.bar_foo_8432.13.1");
    auto helper = ubuntu::app_launch::Helper::create(untrusted, appid, registry);

    ASSERT_TRUE(helper->hasInstances());

    auto instances = helper->instances();

    EXPECT_EQ(1, int(instances.size()));

    instances[0]->stop();

    auto calls = systemd->stopCalls();

    ASSERT_EQ(1u, calls.size());

    EXPECT_EQ(SystemdMock::instanceName({"untrusted-helper", "com.bar_foo_8432.13.1", "24034582324132", 0, {}}),
              *calls.begin());

    return;
}

TEST_F(LibUAL, HelperList)
{
    auto nothelper = ubuntu::app_launch::Helper::Type::from_raw("not-a-type");
    auto notlist = ubuntu::app_launch::Registry::runningHelpers(nothelper, registry);

    EXPECT_EQ(0, int(notlist.size()));

    auto goodhelper = ubuntu::app_launch::Helper::Type::from_raw("untrusted-helper");
    auto goodlist = ubuntu::app_launch::Registry::runningHelpers(goodhelper, registry);

    ASSERT_EQ(2, int(goodlist.size()));

    goodlist.sort(
        [](const std::shared_ptr<ubuntu::app_launch::Helper>& a, const std::shared_ptr<ubuntu::app_launch::Helper>& b) {
            std::string sa = a->appId();
            std::string sb = b->appId();

            return sa < sb;
        });

    EXPECT_EQ("com.bar_foo_8432.13.1", (std::string)goodlist.front()->appId());
    EXPECT_EQ("com.foo_bar_43.23.12", (std::string)goodlist.back()->appId());

    EXPECT_TRUE(goodlist.front()->hasInstances());
    EXPECT_TRUE(goodlist.back()->hasInstances());

    EXPECT_EQ(1, int(goodlist.front()->instances().size()));
    EXPECT_EQ(1, int(goodlist.back()->instances().size()));

    EXPECT_TRUE(goodlist.front()->instances()[0]->isRunning());
    EXPECT_TRUE(goodlist.back()->instances()[0]->isRunning());
}

TEST_F(LibUAL, StartStopHelperObserver)
{
    auto type = ubuntu::app_launch::Helper::Type::from_raw("my-type-is-scorpio");
    auto appid = ubuntu::app_launch::AppID::parse("com.foo_foo_1.2.3");

    storeForHelper(appid);

    int start_count = 0;
    int stop_count = 0;

    ubuntu::app_launch::Registry::helperStarted(type, registry)
        .connect([&](const std::shared_ptr<ubuntu::app_launch::Helper>& helper,
                     const std::shared_ptr<ubuntu::app_launch::Helper::Instance>& inst) {
            if (helper->appId() != appid)
            {
                return;
            }

            start_count++;
        });
    ubuntu::app_launch::Registry::helperStopped(type, registry)
        .connect([&](const std::shared_ptr<ubuntu::app_launch::Helper>& helper,
                     const std::shared_ptr<ubuntu::app_launch::Helper::Instance>& inst) {
            if (helper->appId() != appid)
            {
                return;
            }

            stop_count++;
        });

    /* Basic start */
    systemd->managerEmitNew(SystemdMock::instanceName({"my-type-is-scorpio", "com.foo_foo_1.2.3", "1234", 0, {}}),
                            "/foo");

    EXPECT_EVENTUALLY_EQ(1, start_count);

    /* Basic stop */
    systemd->managerEmitRemoved(SystemdMock::instanceName({"my-type-is-scorpio", "com.foo_foo_1.2.3", "1234", 0, {}}),
                                "/foo");

    EXPECT_EVENTUALLY_EQ(1, stop_count);
}

// DISABLED: Skipping these tests to not block on bug #1584849
TEST_F(LibUAL, DISABLED_PauseResume)
{
    g_setenv("UBUNTU_APP_LAUNCH_OOM_PROC_PATH", CMAKE_BINARY_DIR "/libual-proc", 1);

    /* Setup some spew */
    SpewMaster spew;

    /* Setup ZG Mock */
    auto zgmock = std::make_shared<ZeitgeistMock>();

    /* New Systemd Mock */
    dbus_test_service_remove_task(service, *systemd);
    kill(dbus_test_process_get_pid(*systemd), SIGTERM);
    EXPECT_EVENTUALLY_FUNC_EQ(DBUS_TEST_TASK_STATE_FINISHED, systemd->stateFunc());
    systemd.reset();

    auto systemd2 = std::make_shared<SystemdMock>(
        std::list<SystemdMock::Instance>{
            {"application-click", "com.test.good_application_1.2.3", {}, spew.pid(), {spew.pid()}}},
        CGROUP_DIR);

    /* Give things a chance to start */
    EXPECT_EVENTUALLY_FUNC_EQ(DBUS_TEST_TASK_STATE_RUNNING, systemd2->stateFunc());
    EXPECT_EVENTUALLY_FUNC_EQ(DBUS_TEST_TASK_STATE_RUNNING, zgmock->stateFunc());

    /* Setup signal handling */
    guint paused_count = 0;
    guint resumed_count = 0;

    ubuntu::app_launch::Registry::appPaused(registry).connect([&paused_count](
        const std::shared_ptr<ubuntu::app_launch::Application>& app,
        const std::shared_ptr<ubuntu::app_launch::Application::Instance>& inst, const std::vector<pid_t>& pids) {
        g_debug("App paused: %s (%s)", std::string(app->appId()).c_str(),
                std::accumulate(pids.begin(), pids.end(), std::string{}, [](const std::string& accum, pid_t pid) {
                    return accum.empty() ? std::to_string(pid) : accum + ", " + std::to_string(pid);
                }).c_str());
        paused_count++;
    });
    ubuntu::app_launch::Registry::appResumed(registry).connect([&resumed_count](
        const std::shared_ptr<ubuntu::app_launch::Application>& app,
        const std::shared_ptr<ubuntu::app_launch::Application::Instance>& inst, const std::vector<pid_t>& pids) {
        g_debug("App resumed: %s (%s)", std::string(app->appId()).c_str(),
                std::accumulate(pids.begin(), pids.end(), std::string{}, [](const std::string& accum, pid_t pid) {
                    return accum.empty() ? std::to_string(pid) : accum + ", " + std::to_string(pid);
                }).c_str());
        resumed_count++;
    });

    /* Get our app object */
    auto appid = ubuntu::app_launch::AppID::find(registry, "com.test.good_application_1.2.3");
    auto app = ubuntu::app_launch::Application::create(appid, registry);

    ASSERT_EQ(1, int(app->instances().size()));

    auto instance = app->instances()[0];

    /* Test it */
    EXPECT_NE(0u, spew.dataCnt());
    paused_count = 0;

    /* Pause the app */
    instance->pause();

    pause(0);     /* Flush queued events */
    spew.reset(); /* clear it */

    /* Check data coming out */
    EXPECT_EVENTUALLY_EQ(1u, paused_count);
    EXPECT_EQ(0u, spew.dataCnt());

    /* Check to make sure we sent the event to ZG */
    auto inserts = zgmock->insertCalls();
    EXPECT_EQ(1u, inserts.size());

    zgmock->clear();

    /* Check to ensure we set the OOM score */
    EXPECT_EQ("900", spew.oomScore());

    resumed_count = 0;

    /* Now Resume the App */
    instance->resume();

    EXPECT_EVENTUALLY_EQ(1u, resumed_count);
    EXPECT_NE(0u, spew.dataCnt());

    /* Check to make sure we sent the event to ZG */
    auto inserts2 = zgmock->insertCalls();
    EXPECT_EQ(1u, inserts2.size());

    zgmock->clear();

    /* Check to ensure we set the OOM score */
    EXPECT_EQ("100", spew.oomScore());

    g_spawn_command_line_sync("rm -rf " CMAKE_BINARY_DIR "/libual-proc", NULL, NULL, NULL, NULL);
}

TEST_F(LibUAL, MultiPause)
{
    auto appid = ubuntu::app_launch::AppID::find(registry, "single");
    g_setenv("UBUNTU_APP_LAUNCH_OOM_PROC_PATH", CMAKE_BINARY_DIR "/libual-proc", 1);

    /* Setup A TON OF spew */
    std::array<SpewMaster, 50> spews;

    /* Setup ZG Mock */
    auto zgmock = std::make_shared<ZeitgeistMock>();

    /* New Systemd Mock */
    dbus_test_service_remove_task(service, *systemd);
    kill(dbus_test_process_get_pid(*systemd), SIGTERM);
    EXPECT_EVENTUALLY_FUNC_EQ(DBUS_TEST_TASK_STATE_FINISHED, systemd->stateFunc());
    systemd.reset();

    std::vector<pid_t> spewpids{int(spews.size())};
    for (const auto& spew : spews)
    {
        spewpids.push_back(spew.pid());
    }
    auto systemd2 = std::make_shared<SystemdMock>(
        std::list<SystemdMock::Instance>{{"application-legacy", "single", {}, spews.begin()->pid(), spewpids}},
        CGROUP_DIR);

    /* Add mocks */
    dbus_test_service_add_task(service, *systemd2);
    dbus_test_service_add_task(service, *zgmock);
    dbus_test_task_run(*systemd2);
    dbus_test_task_run(*zgmock);

    /* Give things a chance to start */
    EXPECT_EVENTUALLY_FUNC_EQ(DBUS_TEST_TASK_STATE_RUNNING, systemd2->stateFunc());
    EXPECT_EVENTUALLY_FUNC_EQ(DBUS_TEST_TASK_STATE_RUNNING, zgmock->stateFunc());

    /* Resetup the registry with the new systemd */
    registry = std::make_shared<ubuntu::app_launch::Registry>();

    /* Setup signal handling */
    guint paused_count = 0;
    guint resumed_count = 0;

    ubuntu::app_launch::Registry::appPaused(registry).connect([&paused_count](
        const std::shared_ptr<ubuntu::app_launch::Application>& app,
        const std::shared_ptr<ubuntu::app_launch::Application::Instance>& inst, const std::vector<pid_t>& pids) {
        g_debug("App paused: %s (%s)", std::string(app->appId()).c_str(),
                std::accumulate(pids.begin(), pids.end(), std::string{}, [](const std::string& accum, pid_t pid) {
                    return accum.empty() ? std::to_string(pid) : accum + ", " + std::to_string(pid);
                }).c_str());
        paused_count++;
    });
    ubuntu::app_launch::Registry::appResumed(registry).connect([&resumed_count](
        const std::shared_ptr<ubuntu::app_launch::Application>& app,
        const std::shared_ptr<ubuntu::app_launch::Application::Instance>& inst, const std::vector<pid_t>& pids) {
        g_debug("App resumed: %s (%s)", std::string(app->appId()).c_str(),
                std::accumulate(pids.begin(), pids.end(), std::string{}, [](const std::string& accum, pid_t pid) {
                    return accum.empty() ? std::to_string(pid) : accum + ", " + std::to_string(pid);
                }).c_str());
        resumed_count++;
    });

    /* Get our app object */
    auto app = ubuntu::app_launch::Application::create(appid, registry);

    ASSERT_NE(nullptr, app);
    ASSERT_EQ(1u, app->instances().size());

    auto instance = app->instances()[0];

    /* Test it */
    EXPECT_NE(0, std::accumulate(spews.begin(), spews.end(), int{0},
                                 [](const int& acc, SpewMaster& spew) { return acc + spew.dataCnt(); }));

    /* Pause the app */
    instance->pause();

    EXPECT_EVENTUALLY_EQ(1u, paused_count);

    std::for_each(spews.begin(), spews.end(), [](SpewMaster& spew) { spew.reset(); });
    pause(50);

    /* Check data coming out */
    EXPECT_EQ(0, std::accumulate(spews.begin(), spews.end(), int{0},
                                 [](const int& acc, SpewMaster& spew) { return acc + spew.dataCnt(); }));

    /* Now Resume the App */
    instance->resume();

    EXPECT_EVENTUALLY_EQ(1u, resumed_count);

    pause(50);

    EXPECT_NE(0, std::accumulate(spews.begin(), spews.end(), int{0},
                                 [](const int& acc, SpewMaster& spew) { return acc + spew.dataCnt(); }));

    /* Pause the app */
    instance->pause();

    EXPECT_EVENTUALLY_EQ(2u, paused_count);

    std::for_each(spews.begin(), spews.end(), [](SpewMaster& spew) { spew.reset(); });
    pause(50);

    /* Check data coming out */
    EXPECT_EQ(0, std::accumulate(spews.begin(), spews.end(), int{0},
                                 [](const int& acc, SpewMaster& spew) { return acc + spew.dataCnt(); }));

    /* Now Resume the App */
    instance->resume();

    EXPECT_EVENTUALLY_EQ(2u, resumed_count);

    pause(50);

    EXPECT_NE(0, std::accumulate(spews.begin(), spews.end(), int{0},
                                 [](const int& acc, SpewMaster& spew) { return acc + spew.dataCnt(); }));

    g_spawn_command_line_sync("rm -rf " CMAKE_BINARY_DIR "/libual-proc", NULL, NULL, NULL, NULL);
}

TEST_F(LibUAL, AppInfoSignals)
{
    /* Setup the stores mock */
    auto mockstore = std::make_shared<MockStore>(registry->impl);
    registry =
        std::make_shared<RegistryMock>(std::list<std::shared_ptr<ubuntu::app_launch::app_store::Base>>{mockstore},
                                       std::shared_ptr<ubuntu::app_launch::jobs::manager::Base>{});

    /* Build an app */
    auto singleappid = ubuntu::app_launch::AppID::find(registry, "single");
    auto myapp = std::make_shared<MockApp>(singleappid, registry->impl);

    /* Setup an app added signal handler */
    std::promise<ubuntu::app_launch::AppID> addedAppId;
    ubuntu::app_launch::Registry::appAdded(registry).connect(
        [&](const std::shared_ptr<ubuntu::app_launch::Application>& app) { addedAppId.set_value(app->appId()); });

    mockstore->mock_signalAppAdded(myapp);

    EXPECT_EVENTUALLY_FUTURE_EQ(singleappid, addedAppId.get_future());

    /* Setup an info changed signal handler */
    std::promise<ubuntu::app_launch::AppID> changedAppId;
    ubuntu::app_launch::Registry::appInfoUpdated(registry).connect(
        [&](const std::shared_ptr<ubuntu::app_launch::Application>& app) { changedAppId.set_value(app->appId()); });

    mockstore->mock_signalAppInfoChanged(myapp);

    EXPECT_EVENTUALLY_FUTURE_EQ(singleappid, changedAppId.get_future());

    /* Setup an app removed signal handler */
    std::promise<ubuntu::app_launch::AppID> removedAppId;
    ubuntu::app_launch::Registry::appRemoved(registry).connect(
        [&](const ubuntu::app_launch::AppID& appid) { removedAppId.set_value(appid); });

    mockstore->mock_signalAppRemoved(singleappid);

    EXPECT_EVENTUALLY_FUTURE_EQ(singleappid, removedAppId.get_future());
}

TEST_F(LibUAL, OOMSet)
{
    g_setenv("UBUNTU_APP_LAUNCH_OOM_PROC_PATH", CMAKE_BINARY_DIR "/libual-proc", 1);

    GPid testpid = getpid();

    /* Setup our OOM adjust file */
    gchar* procdir = g_strdup_printf(CMAKE_BINARY_DIR "/libual-proc/%d", testpid);
    ASSERT_EQ(0, g_mkdir_with_parents(procdir, 0700));
    gchar* oomadjfile = g_strdup_printf("%s/oom_score_adj", procdir);
    g_free(procdir);
    ASSERT_TRUE(g_file_set_contents(oomadjfile, "0", -1, NULL));

    /* Get our app object */
    auto appid = ubuntu::app_launch::AppID::find(registry, "single");
    auto app = ubuntu::app_launch::Application::create(appid, registry);

    ASSERT_EQ(1u, app->instances().size());

    auto instance = app->instances()[0];

    /* Set the OOM Score */
    instance->setOomAdjustment(ubuntu::app_launch::oom::paused());

    /* Check to ensure we set the OOM score */
    gchar* oomscore = NULL;
    ASSERT_TRUE(g_file_get_contents(oomadjfile, &oomscore, NULL, NULL));
    EXPECT_STREQ("900", oomscore);
    g_free(oomscore);

    /* Set the OOM Score */
    instance->setOomAdjustment(ubuntu::app_launch::oom::focused());

    /* Check to ensure we set the OOM score */
    ASSERT_TRUE(g_file_get_contents(oomadjfile, &oomscore, NULL, NULL));
    EXPECT_STREQ("100", oomscore);
    g_free(oomscore);

    /* Custom Score */
    auto custom = ubuntu::app_launch::oom::fromLabelAndValue(432, "Custom");
    instance->setOomAdjustment(custom);

    /* Check to ensure we set the OOM score */
    ASSERT_TRUE(g_file_get_contents(oomadjfile, &oomscore, NULL, NULL));
    EXPECT_STREQ("432", oomscore);
    g_free(oomscore);

    /* Check we can read it too! */
    EXPECT_EQ(custom, instance->getOomAdjustment());

    /* Remove write access from it */
    auto nowrite = std::string("chmod -w ") + oomadjfile;
    g_spawn_command_line_sync(nowrite.c_str(), nullptr, nullptr, nullptr, nullptr);
    instance->setOomAdjustment(ubuntu::app_launch::oom::focused());

    /* Cleanup */
    g_spawn_command_line_sync("rm -rf " CMAKE_BINARY_DIR "/libual-proc", NULL, NULL, NULL, NULL);

    /* Test no entry */
    instance->setOomAdjustment(ubuntu::app_launch::oom::focused());

    g_free(oomadjfile);
}

TEST_F(LibUAL, StartSessionHelper)
{
    MirConnection* conn = mir_connect_sync("libual-test", "start-session-helper");  // Mocked, doesn't need cleaning up
    MirPromptSession* msession = mir_connection_create_prompt_session_sync(conn, 5, nullptr, nullptr);

    /* Building a temporary file and making an FD for it */
    const char* filedata = "This is some data that we should get on the other side\n";
    ASSERT_TRUE(g_file_set_contents(SESSION_TEMP_FILE, filedata, strlen(filedata), nullptr) == TRUE);
    int mirfd = open(SESSION_TEMP_FILE, 0);
    mir_mock_set_trusted_fd(mirfd);

    /* Basic make sure we can send the event */
    auto untrusted = ubuntu::app_launch::Helper::Type::from_raw("untrusted-type");
    auto appid = ubuntu::app_launch::AppID::parse("com.test.multiple_first_1.2.3");
    auto helper = ubuntu::app_launch::Helper::create(untrusted, appid, registry);

    helper->launch(msession);

    auto calls = systemd->unitCalls();
    ASSERT_EQ(1u, calls.size());

    /* Check the environment */
    auto& env = calls.begin()->environment;
    EXPECT_TRUE(check_env(env, "APP_ID", "com.test.multiple_first_1.2.3"));
    EXPECT_TRUE(check_env(env, "HELPER_TYPE", "untrusted-type"));

    auto demanglename = find_env(env, "UBUNTU_APP_LAUNCH_DEMANGLE_NAME");
    ASSERT_FALSE(demanglename.empty());
    EXPECT_EQ(g_dbus_connection_get_unique_name(bus), split_env(demanglename).second);
    auto demanglepath = find_env(env, "UBUNTU_APP_LAUNCH_DEMANGLE_PATH");
    ASSERT_FALSE(demanglepath.empty());

    /* Setup environment for call */
    g_setenv("UBUNTU_APP_LAUNCH_DEMANGLE_NAME", split_env(demanglename).second.c_str(), TRUE);
    g_setenv("UBUNTU_APP_LAUNCH_DEMANGLE_PATH", split_env(demanglepath).second.c_str(), TRUE);

    /* Exec our tool */
    std::promise<std::string> outputpromise;
    std::thread t([&outputpromise]() {
        gchar* socketstdout = nullptr;
        GError* error = nullptr;
        g_unsetenv("G_MESSAGES_DEBUG");

        g_spawn_command_line_sync(SOCKET_DEMANGLER " " SOCKET_TOOL, &socketstdout, nullptr, nullptr, &error);

        if (error != nullptr)
        {
            fprintf(stderr, "Unable to spawn '" SOCKET_DEMANGLER " " SOCKET_TOOL "': %s\n", error->message);
            g_error_free(error);
            outputpromise.set_value(std::string(""));
        }
        else
        {
            outputpromise.set_value(std::string(socketstdout));
            g_free(socketstdout);
        }
    });
    t.detach();

    EXPECT_EVENTUALLY_FUTURE_EQ(std::string{filedata}, outputpromise.get_future());

    return;
}

/* Hardcore socket stuff */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

TEST_F(LibUAL, SetExec)
{
    /* Create a socket */
    class SmartSocket
    {
    public:
        int fd;
        SmartSocket()
            : fd(socket(AF_UNIX, SOCK_STREAM, 0))
        {
        }
        ~SmartSocket()
        {
            close(fd);
        }
    };
    SmartSocket sock;
    ASSERT_NE(0, sock.fd);

    std::string socketname{"/ual-setexec-test-12445343"};

    struct sockaddr_un socketaddr = {0};
    socketaddr.sun_family = AF_UNIX;
    strncpy(socketaddr.sun_path, socketname.c_str(), sizeof(socketaddr.sun_path) - 1);
    socketaddr.sun_path[0] = 0;

    ASSERT_EQ(0, bind(sock.fd, (const struct sockaddr*)&socketaddr, sizeof(struct sockaddr_un)));
    listen(sock.fd, 1); /* 1 is the number of people who can connect */

    setenv("UBUNTU_APP_LAUNCH_HELPER_EXECTOOL_SETEXEC_SOCKET", socketname.c_str(), 1);

    std::promise<std::vector<std::string>> socketpromise;
    std::thread socketreader([&]() {
        std::vector<std::string> socketvals;

        int readsocket = accept(sock.fd, NULL, NULL);

        /* Keeping this similar to the helper-helper code as that's what
         * we're running against. Not making it C++-style. */
        char readbuf[2048] = {0};
        int thisread = 0;
        int amountread = 0;
        while ((thisread = read(readsocket, readbuf + amountread, 2048 - amountread)) > 0)
        {
            amountread += thisread;

            if (amountread == 2048)
            {
                try
                {
                    throw std::runtime_error{"Read too many bytes from socket"};
                }
                catch (...)
                {
                    socketpromise.set_exception(std::current_exception());
                }
                return;
            }
        }

        close(readsocket);

        /* Parse data */
        if (amountread > 0)
        {
            char* startvar = readbuf;

            do
            {
                socketvals.emplace_back(std::string(startvar));

                startvar = startvar + strlen(startvar) + 1;
            } while (startvar < readbuf + amountread);
        }

        /* Read socket */
        socketpromise.set_value(socketvals);
    });
    socketreader.detach(); /* avoid thread cleanup code when we don't really care */

    std::vector<std::string> execList{"Foo", "Bar", "Really really really long value", "Another value"};
    ubuntu::app_launch::Helper::setExec(execList);

    EXPECT_EVENTUALLY_FUTURE_EQ(execList, socketpromise.get_future());
}

TEST_F(LibUAL, AppInfo)
{
    /* Correct values from a legacy */
    auto barid = ubuntu::app_launch::AppID::find(registry, "bar");
    EXPECT_THROW(ubuntu::app_launch::Application::create(barid, registry), std::runtime_error);

#ifdef HAVE_LIBERTINE
    /* Correct values for libertine */
    auto libertineid = ubuntu::app_launch::AppID::parse("container-name_test_0.0");
    auto libertine = ubuntu::app_launch::Application::create(libertineid, registry);

    EXPECT_TRUE((bool)libertine->info());
    EXPECT_EQ("Test", libertine->info()->name().value());

    /* Correct values for nested libertine */
    auto nestedlibertineid = ubuntu::app_launch::AppID::parse("container-name_test-nested_0.0");
    auto nestedlibertine = ubuntu::app_launch::Application::create(nestedlibertineid, registry);

    EXPECT_TRUE((bool)nestedlibertine->info());
    EXPECT_EQ("Test Nested", nestedlibertine->info()->name().value());
#endif
}
