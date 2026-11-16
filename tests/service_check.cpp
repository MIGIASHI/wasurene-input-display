// Test harness only; never packaged for users.
#include "service.hpp"
#include <filesystem>
#include <iostream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
int run(const std::vector<std::string> &args) {
    int port=17861;std::filesystem::path data;
    for(size_t i=1;i<args.size();++i){if(args[i]=="--port")port=std::stoi(args[++i]);else if(args[i]=="--data-dir")data=std::filesystem::u8path(args[++i]);}
#ifdef _WIN32
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);auto folder=std::filesystem::path(exe).parent_path();
#else
    auto folder=std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
    wasurene_input::DisplayService service(folder/"web/input.html",data/"settings.json",port);
    std::string error;if(!service.start(error)){std::cerr<<error;return 1;}
    std::cout<<"READY"<<std::endl;std::string line;std::getline(std::cin,line);service.stop();return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t **argv){std::vector<std::string> args;for(int i=0;i<argc;++i)args.push_back(std::filesystem::path(argv[i]).u8string());return run(args);}
#else
int main(int argc,char **argv){return run({argv,argv+argc});}
#endif
