// framework.h – a rendszer-fejlécek egy helyen.
// Szándékosan nincs itt egyetlen C/C++ szabványos fejléc sem (stdlib.h, string,
// vector...), mert a Release build C runtime nélkül készül (lásd duckybg.cpp eleje).
#pragma once

#include "targetver.h"          // a legújabb telepített Windows SDK API-jait engedélyezi
#define WIN32_LEAN_AND_MEAN     // kihagyja a ritkán használt részeket a windows.h-ból -> gyorsabb fordítás
#include <windows.h>            // alap Win32 API: ablakok, fájlok, szálak, INI, SystemParametersInfo
#include <shellapi.h>           // Shell_NotifyIcon – a tálcaikon
#include <shobjidl.h>           // IFileOpenDialog – a modern mappaválasztó ablak (COM)
#include <commctrl.h>           // LoadIconMetric – DPI-helyes ikonméret a tálcához
#include <intrin.h>             // __stosb / __movsb – a saját memset/memcpy-hez
