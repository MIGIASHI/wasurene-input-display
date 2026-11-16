// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "service.hpp"
#include "../updater/obs-updater.hpp"
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("wasurene-input-display", "en-US")
namespace {
std::unique_ptr<wasurene_input::DisplayService> service;
void open_settings(void *) {
#ifdef _WIN32
    ShellExecuteW(nullptr,L"open",L"http://127.0.0.1:17861/input?setup=1",nullptr,nullptr,SW_SHOWNORMAL);
#endif
}
}
MODULE_EXPORT const char *obs_module_name(void){return "WASURENE. Input Display";}
MODULE_EXPORT const char *obs_module_description(void){return "FPS stream input overlay for gamepads, keyboards and mice. Made by WASURENE.";}
bool obs_module_load(void) {
    try {
        std::unique_ptr<char,decltype(&bfree)> html(obs_module_file("web/input.html"),bfree);
        std::unique_ptr<char,decltype(&bfree)> config(obs_module_config_path("settings.json"),bfree);
        if(!html || !config)throw std::runtime_error("plugin data paths unavailable");
        auto candidate=std::make_unique<wasurene_input::DisplayService>(std::filesystem::u8path(html.get()),std::filesystem::u8path(config.get()));
        std::string error;
        if(!candidate->start(error))throw std::runtime_error(error);
        service=std::move(candidate);
        obs_frontend_add_tools_menu_item("WASURENE. Input Display",open_settings,nullptr);
        wasurene_update::initialize();
        blog(LOG_INFO,"[wasurene-input-display] loaded; settings: http://127.0.0.1:17861/input?setup=1");
        return true;
    }catch(const std::exception &error){blog(LOG_ERROR,"[wasurene-input-display] %s",error.what());return false;}
}
void obs_module_unload(void){wasurene_update::shutdown();service.reset();blog(LOG_INFO,"[wasurene-input-display] unloaded");}
