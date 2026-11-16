// SPDX-License-Identifier: GPL-2.0-or-later
#include "core.hpp"
#include <iostream>
using namespace wasurene_update;
int main(int argc,char **argv)
{
 try{
  if(argc!=2)throw std::runtime_error("Expected JSON request path");auto input=json::parse(read_bytes(fs::u8path(argv[1]),65536));auto op=input.at("op").get<std::string>();
  if(op=="newer")std::cout<<json{{"newer",newer(input.at("candidate"),input.at("current"))}};
  else if(op=="release"){auto r=release_info(input.at("release"),input.at("repo"),input.at("asset"));std::cout<<json{{"tag",r.tag},{"digest",r.digest}};}
  else if(op=="hash")std::cout<<json{{"hash",sha256(read_bytes(fs::u8path(input.at("path").get<std::string>()),256u*1024*1024))}};
  else if(op=="cancel"){Network n;n.cancel();download("https://api.github.com",1024,&n);return 3;}
  else if(op=="unsafe-url"){download(input.at("url"),1024);return 3;}
  else if(op=="apply"){
   auto target=layout(input.at("plugin"),fs::u8path(input.at("dll").get<std::string>()),fs::u8path(input.at("data").get<std::string>()));
   auto stage=fs::u8path(input.at("stage").get<std::string>());auto files=unpack(fs::u8path(input.at("zip").get<std::string>()),stage/"payload",target);apply(files,stage/"backup",input.value("fail_after",-1));std::cout<<json{{"files",files.size()}};
  }else throw std::runtime_error("Unknown operation");
  return 0;
 }catch(const std::exception &error){std::cerr<<error.what();return 2;}
}
