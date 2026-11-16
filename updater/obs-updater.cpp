// SPDX-License-Identifier: GPL-2.0-or-later
#include "obs-updater.hpp"
#ifdef _WIN32
#include "core.hpp"
#include "configuration.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <cstring>
#include <atomic>
#include <fstream>
#include <memory>
#include <thread>

namespace wasurene_update {
namespace {
constexpr UINT message=WM_APP+172;
struct Result {Release release;fs::path stage;std::string error;bool manual=false,prepared=false;};
struct State {
 HWND window=nullptr;std::thread worker;std::atomic<bool> stop{false},busy{false};
 fs::path data,config,dll;bool automatic_started=false;
 Network network;bool owns_pending=false;
 bool streaming_starting=false,recording_starting=false,replay_starting=false;
};
std::unique_ptr<State> state;
HWND parent(){return static_cast<HWND>(obs_frontend_get_main_window_handle());}
bool active(){return (state && (state->streaming_starting || state->recording_starting || state->replay_starting)) || obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active() || obs_frontend_virtualcam_active();}
void release_pending(){if(state && state->owns_pending){RemovePropW(parent(),L"WASURENE.Update.Pending");state->owns_pending=false;}}
void info(const std::wstring &text){MessageBoxW(parent(),text.c_str(),L"WASURENE. プラグイン更新",MB_OK|MB_ICONINFORMATION);}
struct DialogText {std::wstring body;const wchar_t *accept;};
INT_PTR CALLBACK confirm_proc(HWND window,UINT event,WPARAM wparam,LPARAM lparam)
{
 if(event==WM_INITDIALOG){
  auto text=reinterpret_cast<DialogText*>(lparam);
  SetDlgItemTextW(window,201,text->body.c_str());SetDlgItemTextW(window,IDOK,text->accept);
  SetFocus(GetDlgItem(window,IDCANCEL));return FALSE;
 }
 if(event==WM_COMMAND && (LOWORD(wparam)==IDOK || LOWORD(wparam)==IDCANCEL)){EndDialog(window,LOWORD(wparam));return TRUE;}
 if(event==WM_CLOSE){EndDialog(window,IDCANCEL);return TRUE;}return FALSE;
}
bool ask(const std::wstring &text,const wchar_t *accept)
{
 auto owner=parent();
 if(GetPropW(owner,L"WASURENE.Update.Dialog") || !SetPropW(owner,L"WASURENE.Update.Dialog",reinterpret_cast<HANDLE>(1)))return false;
 struct DialogGuard {HWND owner;~DialogGuard(){RemovePropW(owner,L"WASURENE.Update.Dialog");}} dialog_guard{owner};
 // Use USER32 dialogs only: importing TaskDialogIndirect requires a particular
 // Common Controls activation context that differs between OBS/Python hosts.
 std::vector<WORD> words;
 auto raw=[&](const auto &value){size_t offset=words.size();words.resize(offset+(sizeof(value)+1)/2);std::memcpy(words.data()+offset,&value,sizeof(value));};
 auto string=[&](const wchar_t *value){while(*value)words.push_back(WORD(*value++));words.push_back(0);};
 DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cdit=3;dialog.cx=310;dialog.cy=165;
 raw(dialog);words.push_back(0);words.push_back(0);string(WASURENE_UPDATE_TITLE);words.push_back(9);string(L"Segoe UI");
 auto control=[&](DWORD style,short x,short y,short width,short height,WORD id,WORD type,const wchar_t *title){
  if(words.size()%2)words.push_back(0);DLGITEMTEMPLATE item{};item.style=WS_CHILD|WS_VISIBLE|style;item.x=x;item.y=y;item.cx=width;item.cy=height;item.id=id;
  raw(item);words.push_back(0xffff);words.push_back(type);string(title);words.push_back(0);
 };
 control(SS_LEFT|SS_NOPREFIX,12,10,286,120,201,0x0082,L"");
 control(WS_TABSTOP|BS_PUSHBUTTON,100,138,108,18,IDOK,0x0080,accept);
 control(WS_TABSTOP|BS_DEFPUSHBUTTON,218,138,80,18,IDCANCEL,0x0080,L"あとで");
 DialogText body{text,accept};return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(words.data()),owner,confirm_proc,reinterpret_cast<LPARAM>(&body))==IDOK;
}
fs::path unique_stage()
{
 GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot create update ID");wchar_t name[40];StringFromGUID2(guid,name,40);
 auto stage=state->config/L"updates"/name;no_reparse(stage);fs::create_directories(stage);return stage;
}
std::string narrow(const std::wstring &s){int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;}
bool portable()
{
 int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);bool result=false;if(args){for(int i=1;i<count;++i)if(std::wstring(args[i])==L"--portable" || std::wstring(args[i])==L"-p")result=true;LocalFree(args);}return result;
}
void begin(bool manual,bool prepare=false,Release release={});
#ifdef WASURENE_UPDATER_TESTING
void test_menu(void*);
#endif
LRESULT CALLBACK window_proc(HWND window,UINT event,WPARAM wparam,LPARAM lparam)
{
 if(event!=message)return DefWindowProcW(window,event,wparam,lparam);
 std::unique_ptr<Result> result(reinterpret_cast<Result*>(lparam));
 if(!state || state->stop)return 0;state->busy=false;
 if(!result->error.empty()){
  release_pending();
  blog(LOG_WARNING,"[WASURENE updater] %s",result->error.c_str());
  if(result->manual || result->prepared)info(L"更新を確認・準備できませんでした。現在のプラグインはそのまま使えます。\n\n"+wide(result->error)+L"\n\n書き込み権限の問題なら、OBSを管理者として起動して再度お試しください。");return 0;
 }
 if(result->prepared){
  if(active()){release_pending();info(L"配信・録画・リプレイバッファ・仮想カメラが動作中のため、更新を保留しました。停止後にツールメニューから再度お試しください。");return 0;}
  if(!ask(L"更新ファイルの検証が完了しました。\n\nOBSを終了してプラグインを更新し、OBSを再起動します。設定・履歴・配置は保持します。保存が必要な作業は先に保存してください。",L"再起動して更新")){release_pending();return 0;}
  if(active()){release_pending();info(L"OBSが配信・録画等を開始したため、更新は保留しました。");return 0;}
  try{
   GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot create update handshake");wchar_t id[40];StringFromGUID2(guid,id,40);
   auto ready_name=std::wstring(L"Local\\WASURENE.Update.Ready.")+id;
   HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,ready_name.c_str());if(!ready)throw std::runtime_error("Cannot create update handshake");
   auto cancel_name=ready_name+L".Cancel";HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,cancel_name.c_str());
   if(!cancel){CloseHandle(ready);throw std::runtime_error("Cannot create update cancellation event");}
   struct Guard {HANDLE ready,cancel;bool armed=false;~Guard(){if(!armed)SetEvent(cancel);CloseHandle(cancel);CloseHandle(ready);}} guard{ready,cancel};
   json plan{{"schema",1},{"plugin",WASURENE_UPDATE_PLUGIN},{"dll",state->dll.u8string()},{"data",state->data.u8string()},
    {"obs",executable().u8string()},{"pid",GetCurrentProcessId()},{"portable",portable()},
    {"digest",result->release.digest},{"version",result->release.tag},{"ready",narrow(ready_name)},{"cancel",narrow(cancel_name)}};
   std::ofstream out(result->stage/"plan.json",std::ios::binary);out<<plan.dump(2);out.close();if(!out)throw std::runtime_error("Cannot save update plan");
   auto helper=result->stage/L"wasurene-update.exe";fs::copy_file(state->data/L"updater"/L"wasurene-update.exe",helper);
   start_process(helper,quote(result->stage.wstring()),result->stage);
   if(WaitForSingleObject(ready,15000)!=WAIT_OBJECT_0)throw std::runtime_error("Updater did not become ready; OBS was not closed");
   if(active())throw std::runtime_error("OBS became active; close OBS manually only after stopping output");
   DWORD parent_pid=0;GetWindowThreadProcessId(parent(),&parent_pid);
   if(parent_pid!=GetCurrentProcessId() || !PostMessageW(parent(),WM_CLOSE,0,0))throw std::runtime_error("Cannot request OBS shutdown");guard.armed=true;
  }catch(const std::exception &error){release_pending();info(L"更新を開始できませんでした。\n\n"+wide(error.what()));}
  return 0;
 }
 if(result->release.tag.empty()){
  if(result->manual)info(L"現在のバージョンが最新です。");
#ifdef WASURENE_UPDATER_TESTING
  else if(!active() && !GetPropW(parent(),L"WASURENE.Update.Dialog")){
   try{
    auto marker=state->config/L"updater-dialog-tested";
    if(!fs::exists(marker)){test_menu(nullptr);fs::create_directories(state->config);std::ofstream(marker)<<"test dialog shown";}
   }catch(const std::exception &e){blog(LOG_WARNING,"[WASURENE updater test] %s",e.what());}
  }
#endif
  return 0;
 }
 if(GetPropW(parent(),L"WASURENE.Update.Pending"))return 0;
 if(active()){if(result->manual)info(L"更新がありますが、OBSの出力が動作中です。停止後に再度確認してください。");return 0;}
 auto text=L"新しいバージョンがあります。\n\n現在: "+wide(WASURENE_UPDATE_VERSION)+L"\n最新版: "+wide(result->release.tag)+L"\n\n更新ファイルをダウンロードしますか？ 更新にはOBSの再起動が必要です。";
 if(ask(text,L"更新する")){
  if(!SetPropW(parent(),L"WASURENE.Update.Pending",reinterpret_cast<HANDLE>(1))){info(L"更新の排他制御を開始できませんでした。");return 0;}
  state->owns_pending=true;begin(true,true,result->release);
 }return 0;
}
void create_window()
{
 if(state->window)return;
 WNDCLASSW cls{};cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(nullptr);auto name=wide(std::string("WASURENE.Update.")+WASURENE_UPDATE_PLUGIN);cls.lpszClassName=name.c_str();
 if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register updater window");
 state->window=CreateWindowExW(0,name.c_str(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
 if(!state->window)throw std::runtime_error("Cannot create updater window");
}
void begin(bool manual,bool prepare,Release release)
{
 if(!state || state->stop || state->busy.exchange(true))return;
 try{create_window();}catch(const std::exception &e){state->busy=false;blog(LOG_WARNING,"[WASURENE updater] %s",e.what());return;}
 if(state->worker.joinable())state->worker.join();
 state->worker=std::thread([manual,prepare,release]{
  auto result=std::make_unique<Result>();result->manual=manual;result->prepared=prepare;
  try{
   if(prepare){
    auto target=layout(WASURENE_UPDATE_PLUGIN,state->dll,state->data);
    // Probe only the dedicated installation directories before requesting restart.
    result->stage=unique_stage();
    for(auto dir:{target.dll.parent_path(),target.data}){
     auto probe=dir/(L".wasurene-write-test-"+result->stage.filename().wstring());
     HANDLE handle=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
     if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Plugin directory is not writable; run OBS as administrator to update");CloseHandle(handle);
    }
    auto bytes=download(release.url,256u*1024*1024,&state->network);if(bytes.size()!=release.size || sha256(bytes)!=release.digest)throw std::runtime_error("Downloaded ZIP does not match GitHub SHA256/size");
    write_bytes(result->stage/"payload.zip",bytes);result->release=release;
   }else{
    auto bytes=download(std::string("https://api.github.com/repos/")+WASURENE_UPDATE_REPO+"/releases/latest",2u*1024*1024,&state->network);
    auto latest=release_info(json::parse(bytes),WASURENE_UPDATE_REPO,WASURENE_UPDATE_ASSET);
    if(newer(latest.tag,WASURENE_UPDATE_VERSION))result->release=latest;
   }
  }catch(const std::exception &e){result->error=e.what();}
  if(!state->stop && PostMessageW(state->window,message,0,reinterpret_cast<LPARAM>(result.get())))result.release();
 });
}
void menu(void*){begin(true);}
#ifdef WASURENE_UPDATER_TESTING
void test_menu(void*)
{
 if(active()){info(L"出力が動作中のため、ダイアログテストを保留しました。");return;}
 if(ask(L"【テスト表示】新しいバージョンがあります。\n\nこれは確認ダイアログのテストです。実際のファイル更新・OBS終了は行いません。",L"更新する"))
  info(L"「更新する」を受け付けました。\n\n実更新では、ZIPの検証後に「再起動して更新」の確認が表示されます。このテストではファイルを変更していません。");
}
#endif
void event(enum obs_frontend_event event,void*)
{
 if(!state)return;
 if(event==OBS_FRONTEND_EVENT_STREAMING_STARTING)state->streaming_starting=true;
 if(event==OBS_FRONTEND_EVENT_STREAMING_STARTED || event==OBS_FRONTEND_EVENT_STREAMING_STOPPED)state->streaming_starting=false;
 if(event==OBS_FRONTEND_EVENT_RECORDING_STARTING)state->recording_starting=true;
 if(event==OBS_FRONTEND_EVENT_RECORDING_STARTED || event==OBS_FRONTEND_EVENT_RECORDING_STOPPED)state->recording_starting=false;
 if(event==OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING)state->replay_starting=true;
 if(event==OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED || event==OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED)state->replay_starting=false;
 if(event==OBS_FRONTEND_EVENT_FINISHED_LOADING && state && !state->automatic_started){state->automatic_started=true;begin(false);}
}
}
void initialize()
{
 state=std::make_unique<State>();
 std::unique_ptr<char,decltype(&bfree)> data(obs_module_file(""),bfree),config(obs_module_config_path(""),bfree);
 if(!data || !config){state.reset();return;}state->data=fs::absolute(fs::u8path(data.get())).lexically_normal();state->config=fs::absolute(fs::u8path(config.get())).lexically_normal();
 HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&initialize),&module);
 wchar_t path[32768];if(!module || !GetModuleFileNameW(module,path,32768)){state.reset();return;}state->dll=path;
 obs_frontend_add_event_callback(event,nullptr);obs_frontend_add_tools_menu_item(WASURENE_UPDATE_MENU,menu,nullptr);
#ifdef WASURENE_UPDATER_TESTING
 obs_frontend_add_tools_menu_item(WASURENE_UPDATE_TEST_MENU,test_menu,nullptr);
#endif
}
void shutdown()
{
 if(!state)return;obs_frontend_remove_event_callback(event,nullptr);state->stop=true;state->network.cancel();release_pending();
 if(state->worker.joinable())state->worker.join();
 if(state->window){MSG msg;while(PeekMessageW(&msg,state->window,message,message,PM_REMOVE))delete reinterpret_cast<Result*>(msg.lParam);DestroyWindow(state->window);auto name=wide(std::string("WASURENE.Update.")+WASURENE_UPDATE_PLUGIN);UnregisterClassW(name.c_str(),GetModuleHandleW(nullptr));}
 state.reset();
}
}
#else
namespace wasurene_update {void initialize(){} void shutdown(){}}
#endif
