// SPDX-License-Identifier: GPL-2.0-or-later
#include "core.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <miniz.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>

namespace wasurene_update {
namespace {
[[noreturn]] void fail(const char *text) { throw std::runtime_error(text); }
std::string lower(std::string value) { for (char &c : value) c = char(std::tolower((unsigned char)c)); return value; }
std::array<int,3> version(std::string value)
{
 if (value.rfind("native-v",0)==0) value.erase(0,8);
 else if (value.rfind("v",0)==0) value.erase(0,1);
 std::array<int,3> out{};size_t pos=0;
 for (int i=0;i<3;++i) {
  auto end=value.find('.',pos);auto part=value.substr(pos,end==std::string::npos?end:end-pos);
  if(part.empty() || part.size()>5 || part.find_first_not_of("0123456789")!=std::string::npos) fail("Invalid stable version");
  out[i]=std::stoi(part);if((i<2)==(end==std::string::npos))fail("Invalid stable version");pos=end+1;
 }
 return out;
}
struct Internet { HINTERNET handle=nullptr; ~Internet(){if(handle)WinHttpCloseHandle(handle);} };
struct Request {
 Network *network;HINTERNET handle;
 Request(Network *context,HINTERNET value):network(context),handle(value){
  std::lock_guard<std::mutex> lock(network->mutex);
  if(network->cancelled){if(handle)WinHttpCloseHandle(handle);fail("Update cancelled");}
  network->request=handle;
 }
 ~Request(){std::lock_guard<std::mutex> lock(network->mutex);if(network->request==handle && handle){WinHttpCloseHandle(handle);network->request=nullptr;}}
};
bool hex_digest(const std::string &s) {return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}
void replace_file(const fs::path &source,const fs::path &target)
{
 no_reparse(target);auto temp=target;temp+=L".wasurene-updating";
 if(fs::exists(temp))fail("An unfinished update file already exists");
 fs::copy_file(source,temp);
 if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
  fs::remove(temp);fail("Cannot replace plugin file (close all OBS instances and check permissions)");
 }
}
bool safe_name(const std::string &name)
{
 if(name.empty() || name.size()>240 || name.front()=='/' || name.find_first_of("\\:")!=std::string::npos)return false;
 size_t start=0;
 while(start<name.size()){
  auto end=name.find('/',start);auto p=name.substr(start,end==std::string::npos?end:end-start);
  if(p.empty() || p=="." || p==".." || p.back()=='.' || p.back()==' ' || p.find_first_of("<>\"|?*")!=std::string::npos)return false;
  for(unsigned char c:p)if(c<32)return false;
  auto stem=lower(p.substr(0,p.find('.')));
  if(stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" || (stem.size()==4 && (stem.rfind("com",0)==0 || stem.rfind("lpt",0)==0) && stem[3]>='1' && stem[3]<='9'))return false;
  if(end==std::string::npos)break;start=end+1;
 }
 return true;
}
}
bool newer(const std::string &candidate,const std::string &current) {return version(candidate)>version(current);}
Release release_info(const json &data,const std::string &repo,const std::string &asset)
{
 if(data.value("draft",true) || data.value("prerelease",true))fail("Not a stable release");
 Release out;out.tag=data.at("tag_name").get<std::string>();version(out.tag);
 for(const auto &item:data.at("assets"))if(item.value("name","")==asset){
  out.url=item.at("browser_download_url").get<std::string>();auto prefix="https://github.com/"+repo+"/releases/download/"+out.tag+"/";
  if(out.url!=prefix+asset)fail("Unexpected release asset URL");
  auto digest=item.value("digest","");if(digest.rfind("sha256:",0)!=0)fail("Release asset has no SHA256 digest");out.digest=digest.substr(7);
  if(!hex_digest(out.digest))fail("Invalid SHA256 digest");
  out.size=item.at("size").get<uint64_t>();if(!out.size || out.size>256ull*1024*1024)fail("Release ZIP exceeds size limit");return out;
 }
 fail("Windows ZIP is missing from the latest release");
}
std::wstring wide(const std::string &value)
{
 int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
 if(!size && !value.empty())fail("Invalid UTF8");std::wstring out(size,L'\0');
 MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),size);return out;
}
void Network::cancel(){cancelled=true;std::lock_guard<std::mutex> lock(mutex);if(request){WinHttpCloseHandle(request);request=nullptr;}}
std::vector<unsigned char> download(const std::string &initial,size_t limit,Network *network)
{
 Network local;if(!network)network=&local;
 auto url=wide(initial);auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(2);
 Internet session{WinHttpOpen(L"WASURENE-OBS-Updater/1",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
 if(!session.handle)fail("Cannot initialize HTTPS");
 WinHttpSetTimeouts(session.handle,5000,5000,5000,15000);
 for(int redirects=0;redirects<=5;++redirects){
  if(network->cancelled)fail("Update cancelled");
  URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
  if(!WinHttpCrackUrl(url.c_str(),DWORD(url.size()),0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS || parts.nPort!=443 || parts.dwUserNameLength || parts.dwPasswordLength)fail("Only trusted HTTPS downloads are allowed");
  std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
  if(host!=L"api.github.com" && host!=L"github.com" && host!=L"release-assets.githubusercontent.com")fail("Unexpected download host");
  std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
  Internet connection{WinHttpConnect(session.handle,host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0)};
  Request request(network,connection.handle?WinHttpOpenRequest(connection.handle,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr);
  if(!request.handle)fail("Cannot open HTTPS request");
  DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy)))fail("Cannot enforce HTTPS redirect policy");
  if(!WinHttpSendRequest(request.handle,L"Accept: application/vnd.github+json\r\n",DWORD(-1),nullptr,0,0,0) || !WinHttpReceiveResponse(request.handle,nullptr))fail("HTTPS request failed");
  DWORD status=0,length=sizeof(status);
  if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr))fail("No HTTP status");
  if(status==301 || status==302 || status==303 || status==307 || status==308){
   DWORD size=0;WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_LOCATION,nullptr,nullptr,&size,nullptr);
   if(!size || size>16384)fail("Invalid HTTPS redirect");std::wstring next(size/sizeof(wchar_t),L'\0');
   if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_LOCATION,nullptr,next.data(),&size,nullptr))fail("Invalid HTTPS redirect");
   next.resize(wcslen(next.c_str()));url=next;continue;
  }
  if(status!=200)fail("GitHub download failed (offline, rate limit or missing release)");
  std::vector<unsigned char> out;std::array<unsigned char,65536> buffer{};
  for(;;){
   if(network->cancelled)fail("Update cancelled");if(std::chrono::steady_clock::now()>deadline)fail("Download timed out");DWORD read=0;
   if(!WinHttpReadData(request.handle,buffer.data(),DWORD(buffer.size()),&read))fail("Download interrupted");if(!read)return out;
   if(out.size()+read>limit)fail("Download exceeds size limit");out.insert(out.end(),buffer.begin(),buffer.begin()+read);
  }
 }
 fail("Too many HTTPS redirects");
}
std::string sha256(const std::vector<unsigned char> &data)
{
 BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)fail("Cannot initialize SHA256");
 unsigned char digest[32];bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
 if(ok)ok=BCryptHashData(hash,const_cast<PUCHAR>(data.data()),ULONG(data.size()),0)>=0 && BCryptFinishHash(hash,digest,32,0)>=0;
 if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);if(!ok)fail("SHA256 verification failed");
 const char *hex="0123456789abcdef";std::string out;for(auto c:digest){out+=hex[c>>4];out+=hex[c&15];}return out;
}
std::vector<unsigned char> read_bytes(const fs::path &path,size_t limit)
{
 auto size=fs::file_size(path);if(size>limit)fail("File exceeds size limit");std::vector<unsigned char> out(static_cast<size_t>(size));
 std::ifstream input(path,std::ios::binary);if(!input.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size())))fail("Cannot read update file");return out;
}
void write_bytes(const fs::path &path,const std::vector<unsigned char> &data)
{
 std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));out.close();if(!out)fail("Cannot save update file");
}
void no_reparse(const fs::path &path)
{
 auto current=fs::absolute(path).lexically_normal();
 for(;;){auto attrs=GetFileAttributesW(current.c_str());if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT))fail("Update paths must not contain junctions or symbolic links");auto parent=current.parent_path();if(parent==current || parent.empty())break;current=parent;}
}
Layout layout(const std::string &plugin,const fs::path &dll,const fs::path &data)
{
 if(plugin!="sf6-tracker-native" && plugin!="wasurene-input-display")fail("Unknown plugin");
 auto binary=fs::absolute(dll).lexically_normal();auto dir=binary.parent_path();fs::path expected;
 if(lower(binary.filename().u8string())!=plugin+".dll" || lower(dir.filename().u8string())!="64bit")fail("Unknown plugin DLL layout");
 if(lower(dir.parent_path().filename().u8string())=="obs-plugins")expected=dir.parent_path().parent_path()/"data"/"obs-plugins"/plugin;
 else if(lower(dir.parent_path().filename().u8string())=="bin" && lower(dir.parent_path().parent_path().filename().u8string())==plugin)expected=dir.parent_path().parent_path()/"data";
 else fail("Unknown plugin installation layout");
 auto actual=fs::absolute(data).lexically_normal();if(actual.filename().empty())actual=actual.parent_path();
 if(lower(expected.u8string())!=lower(actual.u8string()))fail("Plugin data path does not match the DLL");
 no_reparse(binary);no_reparse(expected);return {plugin,binary,expected};
}
std::vector<File> unpack(const fs::path &zip,const fs::path &stage,const Layout &target)
{
 auto bytes=read_bytes(zip,256u*1024*1024);mz_zip_archive archive{};
 if(!mz_zip_reader_init_mem(&archive,bytes.data(),bytes.size(),0))fail("Invalid update ZIP");
 struct Guard {mz_zip_archive *zip;~Guard(){mz_zip_reader_end(zip);}} guard{&archive};
 if(fs::exists(stage))fail("Update payload directory already exists");fs::create_directory(stage);
 std::vector<File> files;std::set<std::string> seen;uint64_t total=0;bool dll=false,html=false,helper=false;
 auto count=mz_zip_reader_get_num_files(&archive);if(count>2048)fail("Too many ZIP entries");
 for(mz_uint i=0;i<count;++i){
  mz_zip_archive_file_stat stat{};if(!mz_zip_reader_file_stat(&archive,i,&stat))fail("Invalid ZIP entry");std::string name=stat.m_filename;
  if(!safe_name(name) || ((stat.m_external_attr>>16)&0170000)==0120000 || (stat.m_external_attr&FILE_ATTRIBUTE_REPARSE_POINT))fail("Unsafe ZIP entry");
  if(stat.m_is_directory)continue;
  if(!seen.insert(lower(name)).second)fail("Duplicate ZIP entry");
  const std::string dll_name="obs-plugins/64bit/"+target.plugin+".dll",prefix="data/obs-plugins/"+target.plugin+"/";
  fs::path destination,relative;
  if(name==dll_name){destination=target.dll;relative="plugin.dll";dll=true;}
  else if(name.rfind(prefix,0)==0){auto suffix=name.substr(prefix.size());destination=target.data/fs::u8path(suffix);relative=fs::path("data")/fs::u8path(suffix);html|=suffix=="web/input.html";helper|=suffix=="updater/wasurene-update.exe";}
  else if(name=="README.md" || name=="README.txt" || name=="UPDATE-UNINSTALL.md")continue;
  else fail("ZIP contains files outside this plugin");
  total+=stat.m_uncomp_size;if(total>512ull*1024*1024 || stat.m_uncomp_size>256ull*1024*1024)fail("Expanded ZIP exceeds size limit");
  size_t size=0;void *data=mz_zip_reader_extract_to_heap(&archive,i,&size,0);if(!data || size!=stat.m_uncomp_size){if(data)mz_free(data);fail("ZIP extraction or CRC verification failed");}
  std::unique_ptr<void,decltype(&mz_free)> owned(data,&mz_free);auto source=stage/relative;fs::create_directories(source.parent_path());
  std::ofstream out(source,std::ios::binary);out.write(static_cast<const char*>(data),std::streamsize(size));out.close();if(!out)fail("Cannot stage ZIP payload");
  no_reparse(destination);files.push_back({source,destination});
 }
 if(!dll || !html || !helper)fail("ZIP is missing plugin DLL, web data or updater");return files;
}
void apply_files(const std::vector<File> &files,const fs::path &backup,int fail_after)
{
 if(fs::exists(backup))fail("Backup already exists");fs::create_directory(backup);std::vector<bool> existed;
 for(size_t i=0;i<files.size();++i){no_reparse(files[i].target);existed.push_back(fs::exists(files[i].target));if(existed.back())fs::copy_file(files[i].target,backup/std::to_string(i));}
 json manifest=json::array();for(size_t i=0;i<files.size();++i)manifest.push_back({{"target",files[i].target.u8string()},{"backup",std::to_string(i)},{"existed",existed[i]}});
 std::ofstream journal(backup/"manifest.json",std::ios::binary);journal<<manifest.dump(2);journal.close();if(!journal)fail("Cannot save rollback manifest");
 size_t applied=0;
 try{
  for(const auto &file:files){if(int(applied)==fail_after)fail("Simulated update failure");fs::create_directories(file.target.parent_path());replace_file(file.source,file.target);++applied;}
 }catch(...){
  bool restored=true;while(applied){--applied;try{if(existed[applied])replace_file(backup/std::to_string(applied),files[applied].target);else fs::remove(files[applied].target);}catch(...){restored=false;}}
  if(!restored)fail("Rollback incomplete: keep OBS closed and restore files from the update backup");throw;
 }
}
std::wstring quote(const std::wstring &value)
{
 std::wstring out=L"\"";size_t slashes=0;for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}if(c==L'\"')out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');return out+L'\"';
}
fs::path executable() {std::wstring value(32768,L'\0');auto n=GetModuleFileNameW(nullptr,value.data(),DWORD(value.size()));if(!n || n>=value.size())fail("Cannot locate executable");value.resize(n);return value;}
void start_process(const fs::path &exe,const std::wstring &args,const fs::path &cwd)
{
 std::wstring command=quote(exe.wstring())+L" "+args;STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};
 if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,cwd.c_str(),&start,&process))fail("Cannot start updater/OBS (check permissions and antivirus)");CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
}
