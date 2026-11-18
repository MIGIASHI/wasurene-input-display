// SPDX-License-Identifier: GPL-2.0-or-later
// A plain Win32 host, without Python's Common Controls activation context.
#include <windows.h>
#include <iostream>
int wmain(int argc,wchar_t **argv)
{
 if(argc!=3)return 2;
 if(!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) || !AddDllDirectory(argv[1]))return 3;
 HMODULE module=LoadLibraryExW(argv[2],nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
 if(!module){std::cerr<<"Plain Win32 DLL loading failed: "<<GetLastError()<<'\n';return 1;}
 FreeLibrary(module);std::cout<<"PASS: plugin imports resolve in plain Win32 host without Common Controls manifest\n";return 0;
}
