// SPDX-License-Identifier: GPL-2.0-or-later
#include "service.hpp"
#include "input.hpp"
#include "http.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif
namespace wasurene_input {
namespace fs=std::filesystem;
using json=nlohmann::json;
namespace {
HttpResponse response(const json &data, int code=200) {
    HttpResponse out; out.code=code; out.body=data.dump(); return out;
}
std::string read_file(const fs::path &path) {
    std::ifstream stream(path,std::ios::binary);
    if(!stream) throw std::runtime_error("file unavailable");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
void atomic_save(const fs::path &path,const json &data) {
    fs::create_directories(path.parent_path());
    fs::path temp=path; temp+=L".tmp";
    {std::ofstream stream(temp,std::ios::binary|std::ios::trunc);
     stream<<data.dump(2); stream.flush(); if(!stream) throw std::runtime_error("settings save failed");}
#ifdef _WIN32
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("settings replace failed");
#else
    fs::rename(temp,path);
#endif
}
void apply_capture_options(const json &settings) {
    const auto enabled=[&](const char *key){return !settings.contains(key) || !settings[key].is_boolean() || settings[key].get<bool>();};
    set_mouse_capture(enabled("captureMotion"),enabled("captureWheel"));
}
}
struct DisplayService::Impl {
    fs::path config_path;
    int port;
    std::string html;
    json settings=json::object();
    std::mutex settings_mutex;
    InputCapture input;
    std::unique_ptr<HttpServer> server;
    Impl(fs::path html_path,fs::path config,int number):config_path(std::move(config)),port(number) {
        html=read_file(html_path);
        if(fs::exists(config_path)) {try{auto saved=json::parse(read_file(config_path));if(saved.is_object())settings=saved;}catch(...) {}}
        apply_capture_options(settings);
    server=std::make_unique<HttpServer>([this](const HttpRequest &req) {
        const std::string host="127.0.0.1:"+std::to_string(port), local="localhost:"+std::to_string(port);
        const auto header=[&](const char *name){auto p=req.headers.find(name);return p==req.headers.end()?std::string():p->second;};
        if(header("host")!=host && header("host")!=local) return response({{"error","invalid host"}},403);
        if(!header("origin").empty() && header("origin")!="http://"+host && header("origin")!="http://"+local)
            return response({{"error","invalid origin"}},403);
        if(header("sec-fetch-site")=="cross-site") return response({{"error","cross-site request denied"}},403);
        if(req.method=="GET") {
            if(req.path=="/" || req.path=="/input") {HttpResponse out;out.type="text/html; charset=utf-8";out.body=html;return out;}
            if(req.path=="/api/status") return response({{"app","WASURENE. Input Display"},{"version","0.2.4"}});
            if(req.path=="/api/capture") return response(mouse_capture_options());
            if(req.path=="/api/input") return response(input.snapshot());
            if(req.path=="/api/input-settings") {std::lock_guard<std::mutex> lock(settings_mutex);return response(settings);}
            if(req.path=="/api/keys") {
                json codes=json::array(); std::istringstream stream(req.param("codes"));std::string item;
                while(std::getline(stream,item,',')) {
                    if(item.empty() || item.size()>3 || item.find_first_not_of("0123456789")!=std::string::npos || codes.size()>=32) return response({{"error","invalid key codes"}},400);
                    int code=std::stoi(item);if(code<1 || code==255 || code>259)return response({{"error","invalid key code"}},400);
                    codes.push_back(code);
                }
                auto result=read_keys(codes);
                result["capture"]=mouse_capture_options();
                if(req.param("motion")=="1") result["motion"]=read_mouse_motion();
                return response(result);
            }
        }
        if(req.method=="POST") {
            if(header("content-type").find("application/json")!=0)return response({{"error","JSON required"}},400);
            if(req.body.size()>16384)return response({{"error","settings too large"}},413);
            json data=json::parse(req.body,nullptr,false);
            if(!data.is_object())return response({{"error","invalid JSON object"}},400);
            if(req.path=="/api/capture") {
                if(!data.contains("motion") || !data["motion"].is_boolean() || !data.contains("wheel") || !data["wheel"].is_boolean())
                    return response({{"error","boolean capture options required"}},400);
                std::lock_guard<std::mutex> lock(settings_mutex);
                auto updated=settings;updated["captureMotion"]=data["motion"];updated["captureWheel"]=data["wheel"];
                try {atomic_save(config_path,updated);} catch(...) {return response({{"error","settings could not be saved"}},500);}
                settings=std::move(updated);apply_capture_options(settings);return response(mouse_capture_options());
            }
            if(req.path=="/api/input-settings") {
                if(data.contains("type") && data["type"]!="pad" && data["type"]!="keyboard")return response({{"error","unsupported display type"}},400);
                std::lock_guard<std::mutex> lock(settings_mutex);
                for(const auto *key : {"captureMotion","captureWheel"}) {
                    if(settings.contains(key)) data[key]=settings[key];
                    else data.erase(key);
                }
                try {atomic_save(config_path,data);} catch(...) {return response({{"error","settings could not be saved"}},500);}
                settings=data;return response({{"ok",true}});
            }
        }
        return response({{"error","not found"}},404);
    });

    }
};
DisplayService::DisplayService(std::filesystem::path html,std::filesystem::path config,int port)
    :impl_(std::make_unique<Impl>(std::move(html),std::move(config),port)) {}
DisplayService::~DisplayService(){stop();}
bool DisplayService::start(std::string &error) {
    impl_->input.start();
    if(!impl_->server->start(impl_->port,error)){impl_->input.stop();return false;}
    return true;
}
void DisplayService::stop(){impl_->server->stop();impl_->input.stop();}
}
