#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#ifdef _MSC_VER
#pragma comment(lib,"shell32.lib")
#endif
#else
#include <unistd.h>
#include <signal.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__SWITCH__)
#include "switch/file_io.h"
#endif
#endif
namespace cane {
namespace fs = std::filesystem;
inline fs::path dataOverride, userOverride;
inline std::string utf8(const fs::path& path) { auto s=path.u8string(); return {s.begin(),s.end()}; }
inline fs::path path(const std::string& s) { return fs::path(std::u8string(s.begin(),s.end())); }
// fs::absolute, except for Switch device paths ("sdmc:/..."), which newlib's std::filesystem does not see as absolute.
inline fs::path absolutePath(const fs::path& p) {
#if defined(__SWITCH__)
 const std::string s=p.generic_string(); const auto colon=s.find(':');
 if(colon!=std::string::npos && colon>0 && colon+1<s.size() && s[colon+1]=='/' && s.find('/')==colon+1) return p;
#endif
 return fs::absolute(p);
}
inline fs::path executableDirectory() {
#if defined(CANE_ANDROID)
 // Android: the app's native library folder (the programs are lib<name>.so there; android_app.cpp sets it)
 if(const char* libraries=std::getenv("SAFEHOUSE_ANDROID_LIBS");libraries && *libraries) return path(libraries);
#endif
#ifdef _WIN32
 std::wstring p(32768,L'\0'); auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));
 if(n && n<p.size()) {p.resize(n);return fs::path(p).parent_path();}
#elif defined(__SWITCH__)
 return fs::path("sdmc:/switch/SafeHouse");
#elif defined(__APPLE__)
 uint32_t size=0; _NSGetExecutablePath(nullptr,&size);
 std::string p(size,'\0');
 if(size && _NSGetExecutablePath(p.data(),&size)==0) {
  std::error_code ec; auto resolved=fs::canonical(fs::path(p.c_str()),ec);
  return (ec?fs::path(p.c_str()):resolved).parent_path();
 }
#else
 std::vector<char> p(4096); auto n=readlink("/proc/self/exe",p.data(),p.size());
 if(n>0 && static_cast<size_t>(n)<p.size()) return fs::path(std::string(p.data(),n)).parent_path();
#endif
 return fs::current_path();
}
// <prefix>/share beside bin/; inside a macOS app bundle, Contents/Resources/share.
inline fs::path shareDirectory() {
 auto contents=executableDirectory().parent_path();
#if defined(__APPLE__)
 if(fs::is_directory(contents/"Resources"/"share")) return contents/"Resources"/"share";
#endif
 return contents/"share";
}
inline fs::path dataDirectory() {return dataOverride.empty()?executableDirectory():dataOverride;}
inline fs::path userDirectory() {return userOverride.empty()?executableDirectory():userOverride;}
inline std::vector<std::string> arguments(int argc,char** argv) {
 std::vector<std::string> result;
#ifdef _WIN32
 int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
 if(!args) throw std::runtime_error("Cannot read command line");
 for(int i=1;i<count;++i) result.push_back(utf8(fs::path(args[i])));
 LocalFree(args);
#else
 for(int i=1;i<argc;++i) result.emplace_back(argv[i]);
#endif
 return result;
}
inline void configureRuntime(const std::vector<std::string>& args) {
 for(size_t i=0;i<args.size();++i) {
  if(args[i]!="--data-dir" && args[i]!="--user-dir") throw std::runtime_error("Unknown argument: "+args[i]);
  auto option=args[i]; if(++i==args.size()) throw std::runtime_error("Missing path for "+option);
  auto p=absolutePath(path(args[i])); if(option=="--data-dir") dataOverride=p;else userOverride=p;
 }
 fs::create_directories(userDirectory());
}
inline bool atomicReplace(const fs::path& temporary,const fs::path& destination) {
#ifdef _WIN32
 return MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#elif defined(__SWITCH__)
 // The SD card's FAT cannot rename over a file: the old one is kept as <file>.previous until the new one is in place.
 return CaneIo::AtomicReplace(temporary,destination);
#else
 std::error_code ec;fs::rename(temporary,destination,ec);return !ec;
#endif
}
// Debug Mode.ini ("[Debug]" / "Debug Mode=1"): the launcher's Settings > Advanced > Debug mode, the one switch for the
// games' debug keys, Sly 2's debug menu and Sly 1's developer debug mode. A missing file or key is off.
inline bool debugModeIn(const fs::path& p) {
 std::ifstream in(p); std::string line;
 while(std::getline(in,line)) {auto eq=line.find('='); if(eq==std::string::npos)continue;
 auto key=line.substr(0,eq);key.erase(std::remove_if(key.begin(),key.end(),[](unsigned char c){return std::isspace(c);}),key.end());
 if(key!="DebugMode")continue;auto value=line.substr(eq+1);
 value.erase(std::remove_if(value.begin(),value.end(),[](unsigned char c){return std::isspace(c);}),value.end());
 std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);});
 return value=="true" || std::strtol(value.c_str(),nullptr,10)!=0;
 } return false;
}
inline bool debugMode() {
 auto p=userDirectory()/"Debug Mode.ini";
 if(!fs::exists(p)) {std::ofstream(p)<<"[Debug]\nDebug Mode=0\n";return false;}
 return debugModeIn(p);
}
inline int compareInsensitive(const char* a,const char* b) {
 while(*a && *b) {int d=std::tolower(static_cast<unsigned char>(*a++))-std::tolower(static_cast<unsigned char>(*b++));if(d)return d;}
 return static_cast<unsigned char>(*a)-static_cast<unsigned char>(*b);
}
}
#ifndef _WIN32
#define _stricmp cane::compareInsensitive
#define __debugbreak() raise(SIGTRAP)
#endif
