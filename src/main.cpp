// SPDX-License-Identifier: GPL-2.0-or-later
#include "http.hpp"
#include "input.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace wasurene_input;
namespace {
std::atomic<bool> quit{false};
std::wstring settings_url;
HttpResponse response(const json &data, int code=200) {
    HttpResponse out; out.code=code; out.body=data.dump(); return out;
}
std::string read_file(const fs::path &path) {
    std::ifstream stream(path,std::ios::binary);
    if(!stream) throw std::runtime_error("file unavailable");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
fs::path executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    DWORD size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!size || size>=path.size()) throw std::runtime_error("executable path unavailable");
    return fs::path(std::wstring(path.data(),size));
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}
fs::path default_data_dir() {
#ifdef _WIN32
    PWSTR path=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,0,nullptr,&path)))
        throw std::runtime_error("settings directory unavailable");
    fs::path result=fs::path(path)/L"WASURENE"/L"InputDisplay";
    CoTaskMemFree(path); return result;
#else
    const char *home=std::getenv("HOME");
    return fs::path(home?home:".")/".config/wasurene-input-display";
#endif
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
#ifdef _WIN32
LRESULT CALLBACK tray_window(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_APP+1) {
        if(lparam==WM_LBUTTONDBLCLK) ShellExecuteW(nullptr,L"open",settings_url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        if(lparam==WM_RBUTTONUP) {
            HMENU menu=CreatePopupMenu();
            AppendMenuW(menu,MF_STRING,1,L"設定画面を開く"); AppendMenuW(menu,MF_STRING,2,L"終了");
            POINT point; GetCursorPos(&point); SetForegroundWindow(window);
            int selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,window,nullptr);
            DestroyMenu(menu);
            if(selected==1) ShellExecuteW(nullptr,L"open",settings_url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            if(selected==2) quit=true;
            PostMessageW(window,WM_NULL,0,0);
        }
        return 0;
    }
    if(message==WM_CLOSE || message==WM_ENDSESSION){quit=true;return 0;}
    return DefWindowProcW(window,message,wparam,lparam);
}
#endif
int run(const std::vector<std::string> &args) {
    int port=17861; bool headless=false; fs::path data_dir=default_data_dir();
    for(size_t i=1;i<args.size();++i) {
        if(args[i]=="--headless") headless=true;
        else if(args[i]=="--port" && i+1<args.size()) port=std::stoi(args[++i]);
        else if(args[i]=="--data-dir" && i+1<args.size()) data_dir=fs::u8path(args[++i]);
        else throw std::runtime_error("invalid command line");
    }
    if(port<1024 || port>65535) throw std::runtime_error("port out of range");
    const auto config_path=data_dir/"settings.json";
    json settings=json::object();
    if(fs::exists(config_path)) {try{auto saved=json::parse(read_file(config_path));if(saved.is_object())settings=saved;}catch(...) {}}
    std::mutex settings_mutex;
    const auto html=read_file(executable_path().parent_path()/"web/input.html");
    InputCapture input; input.start();
    HttpServer server([&](const HttpRequest &req) {
        const std::string host="127.0.0.1:"+std::to_string(port), local="localhost:"+std::to_string(port);
        const auto header=[&](const char *name){auto p=req.headers.find(name);return p==req.headers.end()?std::string():p->second;};
        if(header("host")!=host && header("host")!=local) return response({{"error","invalid host"}},403);
        if(!header("origin").empty() && header("origin")!="http://"+host && header("origin")!="http://"+local)
            return response({{"error","invalid origin"}},403);
        if(header("sec-fetch-site")=="cross-site") return response({{"error","cross-site request denied"}},403);
        if(req.method=="GET") {
            if(req.path=="/" || req.path=="/input") {HttpResponse out;out.type="text/html; charset=utf-8";out.body=html;return out;}
            if(req.path=="/api/status") return response({{"app","WASURENE. Input Display"},{"version","0.1.0"}});
            if(req.path=="/api/input") return response(input.snapshot());
            if(req.path=="/api/input-settings") {std::lock_guard<std::mutex> lock(settings_mutex);return response(settings);}
            if(req.path=="/api/keys") {
                json codes=json::array(); std::istringstream stream(req.param("codes"));std::string item;
                while(std::getline(stream,item,',')) {
                    if(!std::regex_match(item,std::regex("[0-9]{1,3}")) || codes.size()>=32) return response({{"error","invalid key codes"}},400);
                    int code=std::stoi(item);if(code<1 || code==255 || code>259)return response({{"error","invalid key code"}},400);
                    codes.push_back(code);
                }
                return response(read_keys(codes));
            }
        }
        if(req.method=="POST") {
            if(header("content-type").find("application/json")!=0)return response({{"error","JSON required"}},400);
            if(req.body.size()>16384)return response({{"error","settings too large"}},413);
            json data=json::parse(req.body,nullptr,false);
            if(!data.is_object())return response({{"error","invalid JSON object"}},400);
            if(req.path=="/api/input-settings") {
                if(data.contains("type") && data["type"]!="pad" && data["type"]!="keyboard")return response({{"error","unsupported display type"}},400);
                std::lock_guard<std::mutex> lock(settings_mutex);
                try {atomic_save(config_path,data);} catch(...) {return response({{"error","settings could not be saved"}},500);}
                settings=data;return response({{"ok",true}});
            }
            if(req.path=="/api/quit") {quit=true;return response({{"ok",true}});}
        }
        return response({{"error","not found"}},404);
    });
    std::string error;
    if(!server.start(port,error))throw std::runtime_error("Cannot start local server. Another app may be using this port.");
#ifdef _WIN32
    settings_url=L"http://127.0.0.1:"+std::to_wstring(port)+L"/input?setup=1";
    HWND window=nullptr; NOTIFYICONDATAW icon{};
    if(!headless) {
        WNDCLASSW cls{};cls.lpfnWndProc=tray_window;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"WASURENE.InputDisplay";
        if(!RegisterClassW(&cls))throw std::runtime_error("tray window registration failed");
        window=CreateWindowW(cls.lpszClassName,L"WASURENE. Input Display",0,0,0,0,0,nullptr,nullptr,cls.hInstance,nullptr);
        if(!window)throw std::runtime_error("tray window creation failed");
        icon.cbSize=sizeof(icon);icon.hWnd=window;icon.uID=1;icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
        icon.uCallbackMessage=WM_APP+1;icon.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        wcscpy_s(icon.szTip,L"WASURENE. Input Display — 右クリックで設定・終了");
        if(!Shell_NotifyIconW(NIM_ADD,&icon)){DestroyWindow(window);throw std::runtime_error("tray icon creation failed");}
        ShellExecuteW(nullptr,L"open",settings_url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
    while(!quit) {MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    if(window){Shell_NotifyIconW(NIM_DELETE,&icon);DestroyWindow(window);}
#else
    (void)headless;
    while(!quit)std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
    server.stop();input.stop();return 0;
}
}
#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        int count=0;LPWSTR *wide=CommandLineToArgvW(GetCommandLineW(),&count);
        if(!wide)throw std::runtime_error("command line unavailable");
        std::vector<std::string> args;for(int i=0;i<count;++i)args.push_back(fs::path(wide[i]).u8string());LocalFree(wide);
        return run(args);
    }catch(const std::exception &e){MessageBoxA(nullptr,e.what(),"WASURENE. Input Display",MB_OK|MB_ICONERROR);return 1;}
}
#else
int main(int argc,char **argv){try{return run({argv,argv+argc});}catch(const std::exception &e){std::cerr<<e.what()<<std::endl;return 1;}}
#endif
