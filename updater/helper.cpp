// SPDX-License-Identifier: GPL-2.0-or-later
#include "core.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <memory>

using namespace wasurene_update;
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
 int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 std::unique_ptr<wchar_t*,decltype(&LocalFree)> args(argv,&LocalFree);
 fs::path stage;bool test=false;
 try{
#ifdef WASURENE_UPDATER_TESTING
  test=argc==3 && std::wstring(argv[1])==L"--test-wait";
#endif
  if(argc!=2 && !test)throw std::runtime_error("Invalid updater arguments");
  stage=fs::absolute(argv[test?2:1]).lexically_normal();no_reparse(stage);
  auto raw=read_bytes(stage/"plan.json",64*1024);auto plan=json::parse(raw);
  if(plan.at("schema")!=1)throw std::runtime_error("Unknown update plan");
  auto target=layout(plan.at("plugin"),fs::u8path(plan.at("dll").get<std::string>()),fs::u8path(plan.at("data").get<std::string>()));
  auto obs=fs::u8path(plan.at("obs").get<std::string>());
  if(obs.filename()!=L"obs64.exe" || !fs::is_regular_file(obs))throw std::runtime_error("Invalid OBS executable");
  no_reparse(obs);
  DWORD pid=plan.at("pid").get<DWORD>();HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!process)throw std::runtime_error("OBS process is no longer available; update was not applied");
  struct Guard {HANDLE handle;~Guard(){CloseHandle(handle);}} process_guard{process};
  if(!test){
   wchar_t image[32768];DWORD length=32768;
   if(!QueryFullProcessImageNameW(process,0,image,&length) || !fs::equivalent(obs,fs::path(image)))throw std::runtime_error("OBS process does not match this update plan");
  }
  auto bytes=read_bytes(stage/"payload.zip",256u*1024*1024);
  if(sha256(bytes)!=plan.at("digest").get<std::string>())throw std::runtime_error("ZIP SHA256 mismatch; no files were changed");
  auto files=unpack(stage/"payload.zip",stage/"payload",target);
  HANDLE cancel=nullptr;
  struct CancelGuard {HANDLE &handle;~CancelGuard(){if(handle)CloseHandle(handle);}} cancel_guard{cancel};
  if(!test){
   auto name=wide(plan.at("ready").get<std::string>());HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str());
   auto cancel_name=wide(plan.at("cancel").get<std::string>());cancel=OpenEventW(SYNCHRONIZE,FALSE,cancel_name.c_str());
   if(!ready || !cancel){if(ready)CloseHandle(ready);throw std::runtime_error("OBS is not waiting for this updater");}SetEvent(ready);CloseHandle(ready);
  }else{
   std::ofstream(stage/"ready")<<"ready";
  }
  HANDLE waits[]={process,cancel};auto waited=test?WaitForSingleObject(process,30000):WaitForMultipleObjects(2,waits,FALSE,30*60*1000);
  if(waited!=WAIT_OBJECT_0)throw std::runtime_error("Update cancelled or OBS did not exit; no files were changed");
  // A second OBS instance must not still be using this DLL.
  HANDLE file=CreateFileW(target.dll.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Plugin DLL is still in use or not writable; no files were changed");CloseHandle(file);
  apply_files(files,stage/"backup");
  std::ofstream(stage/"result.json")<<json{{"ok",true},{"version",plan.at("version")}}.dump();
  if(!test)start_process(obs,plan.value("portable",false)?L"--portable":L"",obs.parent_path());
  return 0;
 }catch(const std::exception &error){
  if(!stage.empty()){std::ofstream(stage/"result.json")<<json{{"ok",false},{"error",error.what()}}.dump();}
  if(!test){auto text=L"更新を完了できませんでした。OBSを閉じたまま、更新フォルダのresult.jsonとbackupを確認してください。\n\n"+wide(error.what())+L"\n\n更新フォルダ: "+stage.wstring();MessageBoxW(nullptr,text.c_str(),L"WASURENE. プラグイン更新",MB_OK|MB_ICONERROR);}
  return 1;
 }
}
