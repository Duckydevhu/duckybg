// Resource.h – az erőforrások (ikon, dialógus, vezérlők, menüpontok) azonosítói.
// Ezt a fájlt a C++ kód (duckybg.cpp) és az erőforrás-szkript (duckybg.rc) is
// beolvassa, így mindkettő ugyanazokat a számokat használja.
#pragma once

// Erőforrások
#define IDI_DUCKYBG     107     // a program ikonja (duckybg.ico) – exe, tálca és dialógus is ezt használja
#define IDD_SETTINGS    129     // a beállítások dialógus

// A beállítások dialógus vezérlői
#define IDC_FOLDER      1001    // szövegmező: a képek mappája
#define IDC_BROWSE      1002    // "..." gomb: mappaválasztó
#define IDC_INTERVAL    1003    // szövegmező: csere időköze percben

// A tálcamenü pontjai (a WM_COMMAND üzenet wParam-jában érkeznek).
// A 40000 feletti tartomány a szokásos a menüparancsoknak, így nem ütköznek a
// vezérlők és a Windows saját azonosítóival (IDOK = 1, IDCANCEL = 2).
#define IDM_NEXT        40001   // Következő kép
#define IDM_SETTINGS    40002   // Beállítások…
#define IDM_EXIT        40003   // Kilépés

#ifndef IDC_STATIC
#define IDC_STATIC      -1      // feliratok, amikre a kódból nem hivatkozunk
#endif
