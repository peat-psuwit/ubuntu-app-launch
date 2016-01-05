
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include "application-impl-base.h"

#pragma once

namespace Ubuntu {
namespace AppLaunch {
namespace AppImpls {

class Click : public Base {
public:
	Click (const AppID &appid,
	      std::shared_ptr<Registry> registry);
	Click (const AppID &appid,
	      std::shared_ptr<JsonObject> manifest,
	      std::shared_ptr<Registry> registry);

	static std::list<std::shared_ptr<Application>> list (std::shared_ptr<Registry> registry);

	AppID appId() override;

	std::shared_ptr<Info> info() override;

private:
	AppID _appid;

	std::shared_ptr<JsonObject> _manifest;

	std::string _clickDir;
	std::shared_ptr<GDesktopAppInfo> _appinfo;

	static AppID::Version manifestVersion (std::shared_ptr<JsonObject> manifest);
	static std::list<AppID::AppName> manifestApps (std::shared_ptr<JsonObject> manifest);
	static std::shared_ptr<GDesktopAppInfo> manifestAppDesktop (std::shared_ptr<JsonObject> manifest, const std::string &app, const std::string &clickDir);
};

}; // namespace AppImpls
}; // namespace AppLaunch
}; // namespace Ubuntu
