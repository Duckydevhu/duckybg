// =============================================================================
//  duckybg – pici, gyors, tálcán ülő háttérkép-cserélő Windowsra
// =============================================================================
//
//  Mit csinál?
//    Egy megadott mappából (alapból: <exe mappája>\bg) véletlenszerű sorrendben,
//    megadott percenként beállítja az asztal háttérképét. A tálcán (az óra
//    mellett) egy ikon ül, jobb klikkre menü: Következő kép / Beállítások / Kilépés.
//    A beállítások a program mellett lévő duckybg.ini fájlba kerülnek.
//
//  Hogyan épül fel? (felülről lefelé ebben a fájlban)
//    1. CRT-pótlás        – memset/memcpy, mert Release-ben nincs C runtime
//    2. Konstansok, globálisak
//    3. Segédfüggvények   – véletlenszám, dinamikus tömb, kiterjesztés-ellenőrzés
//    4. "Pakli"           – a mappa beolvasása és megkeverése
//    5. Háttérkép-csere   – külön szálon, hogy a tálca ne akadjon
//    6. INI kezelés       – betöltés / mentés
//    7. Beállítások ablak – dialógus, mappaválasztó
//    8. Tálcaikon, menü, ablakeljárás
//    9. Belépési pont     – Run(), entry() / wWinMain()
//
//  Miért ilyen "nyers" Win32 a kód?
//    A cél az volt, hogy ici-pici és gyors legyen. Ezért:
//      - nincs MFC, Qt, .NET, de még std:: sem – csak Win32 API;
//      - Release-ben a C runtime-ot (CRT) sem linkeljük be (/NODEFAULTLIB),
//        így az exe kb. 14 KB + az ikon mérete;
//      - két csere között a program semmit nem csinál: egyetlen Windows timer
//        ébreszti, CPU-t nem használ.
//
//  Szabályok, amik a CRT nélküli build miatt FONTOSAK, ha bővíted a kódot:
//      - ne használj std::-t, new/delete-et, kivételeket, RTTI-t, float-ot;
//      - ne legyen olyan globális objektum, aminek konstruktora van
//        (azokat a CRT futtatná le indulás előtt – nálunk nincs ki futtassa);
//      - a CRT függvények (wcscpy, sprintf, malloc...) helyett a Win32 megfelelőit
//        használd (lstrcpyW, wsprintfW, HeapAlloc...);
//      - egy függvény lokális változói ne legyenek 4 KB-nál nagyobbak, mert
//        akkor a fordító __chkstk hívást szúr be, ami szintén a CRT-ben van.
//        A nagy puffereket ezért tesszük globálissá.
//    Debug buildben a CRT benne van (hibakereséshez kényelmesebb), ott ezek a
//    hibák nem jönnek elő – mindig Release-ben is fordítsd le, mielőtt kiadod!
// =============================================================================

#include "framework.h"
#include "duckybg.h"

// A Common Controls 6-os verzióját kérjük a manifestben. Enélkül:
//   - a dialógus gombjai Windows 95-ös kinézetűek lennének (nincs vizuális stílus),
//   - a LoadIconMetric függvény nem is létezne (csak a v6 comctl32.dll-ben van),
//     és a program el sem indulna.
// A linker ebből a sorból automatikusan generálja és beágyazza a manifestet.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")


// =============================================================================
//  1. CRT-pótlás (csak Release / DUCKY_NOCRT esetén)
// =============================================================================
//
//  Akkor is, ha mi sehol nem hívunk memset-et, a fordító magától generálhat ilyen
//  hívást – pl. egy "WNDCLASSW wc = {};" nullázásnál vagy egy struktúra
//  másolásánál. CRT nélkül ezek "unresolved external" linker hibát adnának,
//  ezért itt megírjuk őket.
//
//  Miért __stosb / __movsb és nem egy sima for ciklus?
//    Mert az optimalizáló egy "bájtonként nullázó" ciklust felismer, és... egy
//    memset hívásra cseréli. A memset-en belül ez végtelen rekurzió lenne.
//    Az __stosb / __movsb intrinsic egyetlen "rep stosb" / "rep movsb" processzor-
//    utasításra fordul, amit a fordító nem cserél le, és modern CPU-n gyors is.
//
//  A "#pragma function" azt mondja a fordítónak, hogy ezeket ne beépített
//  (intrinsic) függvényként kezelje, hanem engedje, hogy mi definiáljuk őket.
#ifdef DUCKY_NOCRT
extern "C" {
#pragma function(memset)
void* __cdecl memset(void* d, int c, size_t n) { __stosb((unsigned char*)d, (unsigned char)c, n); return d; }
#pragma function(memcpy)
void* __cdecl memcpy(void* d, const void* s, size_t n) { __movsb((unsigned char*)d, (const unsigned char*)s, n); return d; }
}
#endif


// =============================================================================
//  2. Konstansok és globális állapot
// =============================================================================
//
//  Minden állapot globális, statikus változóban van. Egy ilyen kicsi, egyszálú
//  (a cserélő szálat leszámítva) programnál ez a legegyszerűbb és a legkisebb kód.
//  A statikus változók a program indulásakor automatikusan nullázva vannak
//  (a .bss szekcióban élnek), így konstruktor sem kell hozzájuk.

static const wchar_t kClass[]   = L"duckybg_tray";  // a rejtett ablak osztályneve – ezzel keresi meg a 2. példány az elsőt
static const wchar_t kSection[] = L"settings";      // az INI fájl szekciója: [settings]

// Saját ablaküzenetek. A WM_APP feletti tartomány az alkalmazásé, nem ütközik
// a Windows saját üzeneteivel.
static const UINT WM_TRAY       = WM_APP + 1;       // a tálcaikon ezzel jelez (kattintás, dupla klikk...)
static const UINT WM_WALLDONE   = WM_APP + 2;       // a cserélő szál ezzel jelzi, hogy végzett

static const UINT_PTR TIMER_ID  = 1;                // a cserét ütemező timer azonosítója
static const UINT DEF_INTERVAL  = 15;               // alapértelmezett időköz percben
static const UINT MAX_INTERVAL  = 10080;            // max. 1 hét; így interval*60000 ms biztosan elfér 32 biten

static HINSTANCE g_inst;                // a program "modul" leírója – erőforrások (ikon, dialógus) betöltéséhez kell
static HWND      g_wnd;                 // a rejtett főablak; ő kapja a timer-, tálca- és menüüzeneteket
static HWND      g_dlg;                 // a beállítások ablak, ha épp nyitva van (különben nullptr)
static UINT      g_msgTaskbarCreated;   // az Explorer ezt az üzenetet küldi, ha (újra)indult a tálca
static NOTIFYICONDATAW g_nid;           // a tálcaikon leírója (ikon, tooltip, visszahívó üzenet)

static wchar_t g_exeDir[MAX_PATH];      // a program mappája, záró '\' nélkül
static wchar_t g_iniPath[MAX_PATH];     // <exe mappa>\duckybg.ini
static wchar_t g_folder[MAX_PATH];      // a képek mappája, már feloldott, abszolút útvonalként
static wchar_t g_wallPath[MAX_PATH];    // a beállítandó kép teljes útvonala – ezt olvassa a cserélő szál
static wchar_t g_lastName[MAX_PATH];    // az utoljára beállított kép fájlneve (tooltiphez, ismétlés elkerüléséhez)
static UINT    g_interval = DEF_INTERVAL;
static volatile LONG g_busy;            // 1, amíg a cserélő szál dolgozik – így nem indul két csere egyszerre

// A "pakli": a mappában talált képek, megkeverve. Úgy működik, mint egy kártya-
// pakli – sorban húzunk belőle, és ha elfogyott, újrakeverjük (és közben a mappát
// is újraolvassuk, így az időközben bemásolt képek is bekerülnek).
//
// Tárolás: a fájlnevek EGYETLEN nagy pufferben vannak, egymás után, mindegyik
// végén '\0'-val:   "a.jpg\0b.png\0c.jpg\0"
// A g_order tömb az egyes nevek kezdő pozícióját (offszetjét) tartalmazza ebben a
// pufferben – a keverés ezt a tömböt keveri, nem a szövegeket mozgatja.
// Miért így? Mert így 1000 kép is csak néhány tíz KB memória, és nincs ezernyi
// apró foglalás. Egy "wchar_t nevek[1000][MAX_PATH]" tömb 520 KB lenne.
static wchar_t* g_names;                // a fájlnevek puffere
static UINT     g_namesLen, g_namesCap; // használt hossz / lefoglalt kapacitás (wchar_t-ben)
static UINT*    g_order;                // a nevek offszetjei, megkevert sorrendben
static UINT     g_count, g_orderCap;    // hány kép van / a g_order kapacitása
static UINT     g_pos;                  // hol tartunk a pakliban (a következő húzandó lap indexe)
static UINT     g_rng;                  // a véletlenszám-generátor állapota


// =============================================================================
//  3. Segédfüggvények
// =============================================================================

// Xorshift32 véletlenszám-generátor.
// Miért nem rand()? Mert az a CRT-ben van. Ez 3 sor, gyors, és keveréshez bőven
// elég jó (kriptográfiai minőség itt nem kell). Fontos: az állapot soha nem lehet
// 0, mert akkor örökre 0 maradna – ezért a seedelésnél "| 1"-gyel biztosítjuk.
static UINT Rand()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// Egyszerű "növekvő tömb" (mint egy std::vector, csak kézzel).
//   p    – a tömb mutatójának címe (lehet nullptr, ha még nincs lefoglalva)
//   cap  – a jelenlegi kapacitás (elemszámban)
//   need – ennyi elemnek kell beleférnie
//   elem – egy elem mérete bájtban
// Ha kell, a kapacitást duplázza (256-tól indulva), így sok elem hozzáadása is
// csak kevés újrafoglalással jár. Hiba esetén false-t ad, a régi tömb érintetlen.
// A process saját heap-jét használjuk (HeapAlloc) – ez a malloc Win32-es megfelelője.
static bool Grow(void** p, UINT* cap, UINT need, UINT elem)
{
    if (need <= *cap) return true;          // belefér, nincs teendő
    UINT nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    HANDLE heap = GetProcessHeap();
    void* np = *p ? HeapReAlloc(heap, 0, *p, (SIZE_T)nc * elem) : HeapAlloc(heap, 0, (SIZE_T)nc * elem);
    if (!np) return false;
    *p = np;
    *cap = nc;
    return true;
}

// Kép-e a fájl a kiterjesztése alapján? Az UTOLSÓ pontot keressük, így a
// "nyaralas.2024.jpg" is helyesen ".jpg"-nek számít. A lstrcmpiW kis/nagybetű-
// független, tehát a ".JPG" is jó.
// Ezeket a formátumokat a Windows 8+ háttérkép-beállítása natívan kezeli.
static bool IsImage(const wchar_t* name)
{
    const wchar_t* dot = nullptr;
    for (const wchar_t* p = name; *p; ++p)
        if (*p == L'.') dot = p;
    if (!dot) return false;
    static const wchar_t* const exts[] = { L".jpg", L".jpeg", L".jfif", L".png", L".bmp" };
    for (const wchar_t* e : exts)
        if (lstrcmpiW(dot, e) == 0) return true;
    return false;
}


// =============================================================================
//  4. A "pakli": mappa beolvasása és keverése
// =============================================================================

// Beolvassa a g_folder mappában lévő képeket, és megkeveri őket.
// Csak akkor hívjuk, amikor a pakli elfogyott (vagy mappát váltottunk), tehát
// cserénként nem olvasunk lemezt feleslegesen.
static void ScanFolder()
{
    // A pakli "kiürítése": a puffereket nem szabadítjuk fel, csak elölről
    // kezdjük tölteni – a következő beolvasásnál újrahasznosítjuk őket.
    g_namesLen = g_count = g_pos = 0;

    // Keresési minta: "<mappa>\*" – minden fájlt listázunk, a szűrést mi végezzük.
    // (A FindFirstFile csak egy mintát fogad, több kiterjesztést nem tudnánk
    // egyszerre megadni neki.)
    wchar_t pattern[MAX_PATH + 4];
    lstrcpynW(pattern, g_folder, MAX_PATH);
    lstrcatW(pattern, L"\\*");

    // FindFirstFileEx két gyorsítással:
    //   FindExInfoBasic           – nem kéri le a régi 8.3-as rövid fájlneveket;
    //   FIND_FIRST_EX_LARGE_FETCH – nagyobb adagokban kéri a listát a fájlrendszertől.
    // Sok képnél, vagy hálózati meghajtón ez érezhetően gyorsabb.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;  // nincs ilyen mappa / üres / nincs jog -> üres pakli
    do {
        // Almappákat (és a "." / ".." bejegyzéseket) kihagyjuk, csak képfájl kell.
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImage(fd.cFileName)) continue;

        UINT len = (UINT)lstrlenW(fd.cFileName) + 1;   // +1 a záró '\0' miatt
        if (!Grow((void**)&g_names, &g_namesCap, g_namesLen + len, sizeof(wchar_t))) break;
        if (!Grow((void**)&g_order, &g_orderCap, g_count + 1, sizeof(UINT))) break;

        lstrcpyW(g_names + g_namesLen, fd.cFileName);  // név a puffer végére
        g_order[g_count++] = g_namesLen;                // megjegyezzük, hol kezdődik
        g_namesLen += len;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Fisher–Yates keverés: hátulról haladva minden elemet kicserélünk egy
    // véletlenszerűen választott, előtte (vagy rajta) álló elemmel.
    // Ez egyenletes eloszlású keverést ad, és O(n) idejű.
    // Az eredmény: egy körön belül minden kép pontosan egyszer jön, és nincs
    // olyan, hogy ugyanaz a kép kétszer egymás után jön, a többi meg sosem.
    for (UINT i = g_count; i > 1; --i) {
        UINT j = Rand() % i;
        UINT t = g_order[i - 1]; g_order[i - 1] = g_order[j]; g_order[j] = t;
    }

    // Kör határán előfordulhatna, hogy az új kör első képe ugyanaz, mint az előző
    // kör utolsója – ez a felhasználónak úgy tűnne, mintha nem cserélt volna.
    // Ilyenkor az elsőt kicseréljük az utolsóval.
    if (g_count > 1 && lstrcmpiW(g_names + g_order[0], g_lastName) == 0) {
        UINT t = g_order[0]; g_order[0] = g_order[g_count - 1]; g_order[g_count - 1] = t;
    }
}


// =============================================================================
//  5. Háttérkép-csere
// =============================================================================

// Frissíti a tálcaikon tooltipjét (az egérrel föléállva megjelenő szöveget):
// melyik kép van beállítva, és milyen időközzel cserélünk.
static void UpdateTip()
{
    wchar_t name[64], buf[256];
    // A tooltip max. 128 karakter, ezért a fájlnevet 60 karakterre vágjuk.
    // Fontos, hogy a wsprintfW-nek NINCS méretkorlátja – csak akkor biztonságos,
    // ha előre tudjuk, hogy a kimenet belefér a pufferbe (itt: max. ~100 karakter).
    if (g_lastName[0]) lstrcpynW(name, g_lastName, 60);
    else lstrcpyW(name, L"–");                  // még nincs kép (pl. üres a mappa)
    wsprintfW(buf, L"duckybg\nKép: %s\nCsere: %u percenként", name, g_interval);
    lstrcpynW(g_nid.szTip, buf, ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_TIP;                     // csak a tooltipet módosítjuk
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// A cserélő szál. Egyetlen dolgot csinál: beállítja a háttérképet, és szól a
// főszálnak, hogy kész.
//
// Miért kell külön szál?
//   A SPIF_SENDCHANGE miatt a Windows minden futó programnak elküld egy
//   WM_SETTINGCHANGE üzenetet, és megvárja (időkorláttal), hogy feldolgozzák.
//   Ha valamelyik program épp lassú, ez akár másodpercekig is tarthat – ha a
//   főszálon csinálnánk, addig a tálcaikon/menü nem reagálna.
//
// A g_wallPath-t itt csak olvassuk; a főszál addig nem írja felül, amíg a
// g_busy jelző 1 (lásd NextWallpaper).
static DWORD WINAPI WallThread(void*)
{
    // SPIF_UPDATEINIFILE – a beállítás megmarad újraindítás után is (a registrybe kerül)
    // SPIF_SENDCHANGE    – a többi program (Explorer) azonnal értesül és frissít
    BOOL ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, g_wallPath,
                                    SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    // Nem közvetlenül állítjuk vissza a g_busy-t és frissítjük a tooltipet, hanem
    // üzenetet küldünk a főszálnak: így minden állapotváltozás egy szálon történik,
    // és nem kell zárakkal (lock) bajlódni. A wParam-ban a siker/hiba megy át.
    PostMessageW(g_wnd, WM_WALLDONE, ok, 0);
    return 0;
}

// A következő képet húzza a pakliból és beállítja. Ezt hívja a timer, a
// "Következő kép" menüpont, az indulás és a mappaváltás.
static void NextWallpaper()
{
    // Ha épp folyik egy csere, ezt kihagyjuk. Az InterlockedCompareExchange
    // atomi művelet: "ha g_busy == 0, akkor legyen 1" – és visszaadja a régi
    // értéket. Ha az nem 0 volt, valaki már dolgozik.
    if (InterlockedCompareExchange(&g_busy, 1, 0)) return;

    // Elfogyott a pakli (vagy még be sem olvastuk) -> újraolvasás + keverés.
    if (g_pos >= g_count) ScanFolder();

    // Nincs egyetlen kép sem a mappában: nincs mit csinálni, de a tooltipben
    // jelezzük ("Kép: –"). A következő timer-ütésnél újra megnézzük a mappát,
    // így ha közben bemásolsz képeket, magától elindul.
    if (!g_count) {
        InterlockedExchange(&g_busy, 0);
        g_lastName[0] = 0;
        UpdateTip();
        return;
    }

    // A következő lap: g_order[g_pos] a név kezdete a g_names pufferben.
    const wchar_t* name = g_names + g_order[g_pos++];

    // Teljes útvonal összerakása: "<mappa>\<fájlnév>". A lstrcpynW mindig
    // lezárja a stringet és nem ír a pufferen túl, így túl hosszú név esetén
    // sem lesz memóriahiba (legfeljebb a SPI hibát ad, amit lent lekezelünk).
    lstrcpynW(g_wallPath, g_folder, MAX_PATH);
    lstrcatW(g_wallPath, L"\\");
    int used = lstrlenW(g_wallPath);
    lstrcpynW(g_wallPath + used, name, MAX_PATH - used);
    lstrcpynW(g_lastName, name, MAX_PATH);

    // Indítjuk a cserélő szálat. A szál leíróját azonnal bezárjuk – ez NEM
    // állítja le a szálat, csak jelezzük, hogy nem akarunk rá várni.
    // Ha a szál valamiért nem indulna el (szinte lehetetlen), akkor a főszálon
    // végezzük el a cserét – lassabb, de működik.
    HANDLE t = CreateThread(nullptr, 0, WallThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else WallThread(nullptr);
}

// (Újra)indítja a timert a beállított időközzel. A SetTimer ugyanazzal az
// azonosítóval meghívva lecseréli a régit, tehát ez egyben "nullázza" is a
// visszaszámlálást – ezért hívjuk kézi csere után is: a következő automatikus
// csere a kézi cserétől számítva teljes időközt vár.
// A timer ismétlődő, minden lejáratkor WM_TIMER üzenetet kap a g_wnd ablak.
static void RestartTimer()
{
    SetTimer(g_wnd, TIMER_ID, g_interval * 60000u, nullptr);  // perc -> ezredmásodperc
}


// =============================================================================
//  6. Beállítások: INI fájl kezelése
// =============================================================================
//
//  A duckybg.ini a program mellett van, így a program "hordozható": a mappát
//  bárhová másolod, viszi magával a beállításait, és nem szemetel a registryben.
//  Formátum:
//      [settings]
//      folder=bg
//      interval=15
//
//  A folder lehet relatív (a program mappájához képest) vagy abszolút útvonal.

// A felhasználó/INI által megadott (esetleg relatív) mappát abszolút útvonallá
// alakítja.
//   raw – a megadott útvonal; ha üres, az alapértelmezett "bg" mappát jelenti
//   out – ide kerül az eredmény (MAX_PATH méretű puffer)
static void ResolveFolder(const wchar_t* raw, wchar_t* out)
{
    wchar_t tmp[MAX_PATH * 2 + 2];          // exe mappa + relatív rész is beleférjen
    if (!raw[0]) raw = L"bg";

    // Abszolút-e? "C:..." (meghajtóbetűs) vagy "\\szerver\..." (hálózati, UNC) alak.
    bool absolute = (raw[0] && raw[1] == L':') || (raw[0] == L'\\' && raw[1] == L'\\');
    if (absolute) {
        lstrcpynW(tmp, raw, MAX_PATH);
    } else {
        // Relatív: a program mappájához fűzzük. Szándékosan NEM az aktuális
        // munkakönyvtárhoz (current directory) – az attól függ, honnan és
        // hogyan indították a programot (pl. autostartnál C:\Windows\System32).
        lstrcpyW(tmp, g_exeDir);
        lstrcatW(tmp, L"\\");
        int used = lstrlenW(tmp);
        lstrcpynW(tmp + used, raw, MAX_PATH);
    }

    // A GetFullPathNameW "kitakarítja" az útvonalat: feloldja a "." és ".."
    // részeket, a '/' jeleket '\'-re cseréli. Pl. "C:\app\.\bg" -> "C:\app\bg".
    DWORD n = GetFullPathNameW(tmp, MAX_PATH, out, nullptr);
    if (!n || n >= MAX_PATH) lstrcpynW(out, tmp, MAX_PATH);   // hiba / túl hosszú: marad, ami volt

    // Záró '\' levágása, hogy a mappaneveket egyszerűen össze tudjuk hasonlítani
    // és a fájlnév elé mindig pontosan egy '\' kerüljön. A meghajtó gyökerét
    // ("C:\") nem bántjuk, mert a "C:" mást jelent (az adott meghajtó aktuális mappáját).
    int len = lstrlenW(out);
    while (len > 3 && (out[len - 1] == L'\\' || out[len - 1] == L'/')) out[--len] = 0;
}

// Ha még nincs INI fájl, létrehozza üresen, UTF-16 BOM-mal (FF FE).
//
// Miért kell ez? A WritePrivateProfileStringW csapdája: ha a fájl NEM létezik,
// akkor ANSI (rendszer-kódlapos) fájlt hoz létre, és az olyan karakterek, amik
// abban nem szerepelnek, '?'-lé válnak. Ha viszont a fájl UTF-16 BOM-mal kezdődik,
// akkor Unicode-ban írja, és minden ékezet (sőt bármilyen karakter) megmarad.
// A CREATE_NEW miatt meglévő fájlt soha nem írunk felül.
static void EnsureUnicodeIni()
{
    HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;   // már létezik (vagy nincs írási jog) – nincs teendő
    static const BYTE bom[2] = { 0xFF, 0xFE };
    DWORD w;
    WriteFile(f, bom, 2, &w, nullptr);
    CloseHandle(f);
}

// Kiírja a jelenlegi beállításokat (g_folder, g_interval) az INI-be.
static void SaveSettings()
{
    EnsureUnicodeIni();
    wchar_t def[MAX_PATH], num[16];
    ResolveFolder(L"", def);   // az alapértelmezett mappa abszolút útvonala

    // Ha a választott mappa épp az alapértelmezett <exe>\bg, akkor relatívan
    // ("bg") mentjük, nem abszolút útvonallal. Így ha a programot máshová
    // másolod, továbbra is a saját bg mappáját fogja használni.
    WritePrivateProfileStringW(kSection, L"folder", lstrcmpiW(g_folder, def) == 0 ? L"bg" : g_folder, g_iniPath);

    wsprintfW(num, L"%u", g_interval);  // szám -> szöveg (az INI-ben minden szöveg)
    WritePrivateProfileStringW(kSection, L"interval", num, g_iniPath);
}

// Beolvassa a beállításokat az INI-ből. Ha az INI még nem létezik (első indítás),
// létrehozza az alapértelmezett értékekkel, és a bg mappát is.
static void LoadSettings()
{
    bool exists = GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES;

    // A GetPrivateProfileInt a 3. paraméterben kapott alapértéket adja vissza, ha
    // a kulcs (vagy maga a fájl) hiányzik. Az értéket a biztonság kedvéért a
    // megengedett tartományba szorítjuk – kézzel szerkesztett INI-ben bármi lehet.
    g_interval = GetPrivateProfileIntW(kSection, L"interval", DEF_INTERVAL, g_iniPath);
    if (g_interval < 1) g_interval = 1;
    if (g_interval > MAX_INTERVAL) g_interval = MAX_INTERVAL;

    wchar_t raw[MAX_PATH];
    GetPrivateProfileStringW(kSection, L"folder", L"", raw, MAX_PATH, g_iniPath);
    ResolveFolder(raw, g_folder);   // üres -> <exe>\bg

    if (!exists) {
        // Első indítás: létrehozzuk a bg mappát (ha már létezik, a hívás
        // egyszerűen nem csinál semmit), és kiírjuk az INI-t, hogy a felhasználó
        // lássa, mit lehet benne állítani.
        CreateDirectoryW(g_folder, nullptr);
        SaveSettings();
    }
}


// =============================================================================
//  7. Beállítások ablak
// =============================================================================
//
//  A dialógus kinézete a duckybg.rc fájlban van leírva (IDD_SETTINGS). Modális
//  ablakként nyitjuk meg (DialogBoxParam), ami azt jelenti, hogy a függvény csak
//  akkor tér vissza, amikor az ablakot bezárták – addig a saját üzenethurkát
//  futtatja. A timer és a tálcaikon ezalatt is működik, mert azok üzenetei ezen a
//  hurkon keresztül is eljutnak a rejtett főablakhoz.

// Levágja a szóközöket, tabokat és idézőjeleket a szöveg elejéről és végéről.
// Miért az idézőjelet is? Mert az Explorer "Másolás elérési útként" funkciója
// idézőjelek közé teszi az útvonalat ("C:\Képek"), és ha a felhasználó így
// illeszti be, azt is el kell fogadni.
// Helyben dolgozik: ha az elejéről vágtunk, a maradékot a puffer elejére másolja.
static void TrimPath(wchar_t* s)
{
    wchar_t* b = s;
    while (*b == L' ' || *b == L'"' || *b == L'\t') ++b;
    int len = lstrlenW(b);
    while (len && (b[len - 1] == L' ' || b[len - 1] == L'"' || b[len - 1] == L'\t')) b[--len] = 0;
    if (b != s) {
        wchar_t* d = s;
        while ((*d++ = *b++)) {}   // másolás a lezáró '\0'-val együtt
    }
}

// A "..." gomb: megnyitja a Windows modern mappaválasztó ablakát.
//
// Miért IFileOpenDialog és nem a régebbi, egyszerűbb SHBrowseForFolder?
//   Az utóbbi egy apró, fa-nézetes ablak, amiben nem lehet útvonalat beírni,
//   nincs gyorselérés stb. Az IFileOpenDialog ugyanaz az ablak, amit a "Megnyitás"
//   párbeszédekből ismersz, csak mappaválasztó módban (FOS_PICKFOLDERS).
//   Ez egy COM objektum, ezért kellett a Run()-ban a CoInitializeEx.
//
// COM-szabály: minden objektumot, amit kaptunk, a végén Release()-elni kell,
// a kapott stringet pedig CoTaskMemFree-vel felszabadítani.
static void BrowseFolder(HWND dlg)
{
    IFileOpenDialog* fd;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd))))
        return;

    // A meglévő opciókat kiegészítjük:
    //   FOS_PICKFOLDERS      – mappát választunk, nem fájlt;
    //   FOS_FORCEFILESYSTEM  – csak valódi fájlrendszerbeli hely (pl. a "Ez a gép"
    //                          virtuális mappa vagy egy telefon nem választható);
    //   FOS_PATHMUSTEXIST    – csak létező mappa.
    DWORD opt;
    if (SUCCEEDED(fd->GetOptions(&opt)))
        fd->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->SetTitle(L"Képek mappája");

    // A választó a szövegmezőben lévő mappában nyíljon meg, ne valahol máshol.
    // Ehhez az útvonalból egy "shell item"-et kell készíteni.
    wchar_t cur[MAX_PATH];
    GetDlgItemTextW(dlg, IDC_FOLDER, cur, MAX_PATH);
    TrimPath(cur);
    IShellItem* si;
    if (cur[0] && SUCCEEDED(SHCreateItemFromParsingName(cur, nullptr, IID_PPV_ARGS(&si)))) {
        fd->SetFolder(si);
        si->Release();
    }

    // Show() modálisan megjeleníti az ablakot; ha a felhasználó a Mégse gombot
    // nyomja, hibakódot ad vissza, és akkor nem csinálunk semmit.
    if (SUCCEEDED(fd->Show(dlg))) {
        IShellItem* res;
        if (SUCCEEDED(fd->GetResult(&res))) {
            PWSTR path;
            if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &path))) {  // valódi útvonal kell, nem "megjelenítendő név"
                SetDlgItemTextW(dlg, IDC_FOLDER, path);   // csak a mezőbe írjuk, menteni az OK gomb fog
                CoTaskMemFree(path);
            }
            res->Release();
        }
    }
    fd->Release();
}

// Az OK gomb: ellenőrzi a beírt értékeket, és ha jók, alkalmazza és menti őket.
// Hibás érték esetén üzenetet ír, a hibás mezőre teszi a fókuszt, és false-t ad –
// ilyenkor az ablak nyitva marad, hogy javítani lehessen.
static bool ApplySettings(HWND dlg)
{
    // --- Időköz ---
    // A GetDlgItemInt a mező szövegét számmá alakítja; az "ok" jelzi, hogy
    // sikerült-e (üres mezőnél vagy túl nagy számnál nem). Az ES_NUMBER stílus
    // (lásd .rc) miatt betűt be sem lehet gépelni, de beilleszteni igen.
    BOOL ok;
    UINT iv = GetDlgItemInt(dlg, IDC_INTERVAL, &ok, FALSE);
    if (!ok || iv < 1 || iv > MAX_INTERVAL) {
        MessageBoxW(dlg, L"Az időköz 1 és 10080 perc között legyen.", L"duckybg", MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, IDC_INTERVAL));
        SendDlgItemMessageW(dlg, IDC_INTERVAL, EM_SETSEL, 0, -1);   // kijelöljük a hibás szöveget, hogy könnyű legyen átírni
        return false;
    }

    // --- Mappa ---
    // Üres mező -> alapértelmezett bg mappa (ezt a ResolveFolder intézi).
    wchar_t raw[MAX_PATH], res[MAX_PATH];
    GetDlgItemTextW(dlg, IDC_FOLDER, raw, MAX_PATH);
    TrimPath(raw);
    ResolveFolder(raw, res);
    DWORD a = GetFileAttributesW(res);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        // Nem létezik, vagy fájl és nem mappa. Szándékosan nem hozzuk létre:
        // egy elgépelt útvonalból ne keletkezzen csendben egy üres mappa.
        MessageBoxW(dlg, L"A megadott mappa nem létezik.", L"duckybg", MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, IDC_FOLDER));
        return false;
    }

    // --- Alkalmazás ---
    bool folderChanged = lstrcmpiW(res, g_folder) != 0;   // Windowson a mappanevek kis/nagybetű-függetlenek
    lstrcpyW(g_folder, res);
    g_interval = iv;
    SaveSettings();
    RestartTimer();   // az új időköz azonnal érvényes, a visszaszámlálás elölről indul
    if (folderChanged) {
        // Új mappa: a régi pakli érvénytelen. Kiürítjük, így a NextWallpaper
        // azonnal beolvassa az új mappát – és rögtön látszik az eredmény.
        g_pos = g_count = 0;
        NextWallpaper();
    }
    UpdateTip();
    return true;
}

// A beállítások ablak üzenetkezelője (dialógus-eljárás). A Windows hívja meg
// minden, az ablaknak szóló eseménynél. TRUE visszatérés = "lekezeltem",
// FALSE = "intézze a Windows az alapértelmezett módon".
static INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        // Az ablak létrejött, de még nem látszik – itt töltjük fel a mezőket.
        g_dlg = dlg;   // megjegyezzük, hogy ne nyíljon meg kétszer (lásd OpenSettings)

        // Ikon a címsorba (kicsi) és az Alt+Tab / tálca nézetbe (nagy).
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_DUCKYBG)));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)g_nid.hIcon);

        SetDlgItemTextW(dlg, IDC_FOLDER, g_folder);           // a teljes, feloldott útvonalat mutatjuk
        SetDlgItemInt(dlg, IDC_INTERVAL, g_interval, FALSE);
        SendDlgItemMessageW(dlg, IDC_INTERVAL, EM_LIMITTEXT, 5, 0);   // max. 5 számjegy (10080)

        // Előtérbe hozzuk. Mivel tálcaikonról nyílik, a Windows különben
        // hajlamos a többi ablak mögé tenni.
        SetForegroundWindow(dlg);
        return TRUE;   // TRUE: a Windows tegye a fókuszt az első mezőre

    case WM_COMMAND:
        // Gombnyomás. Az IDOK-t az Enter, az IDCANCEL-t az Esc és a címsor X
        // gombja is kiváltja – ezt a Windows magától intézi.
        switch (LOWORD(wp)) {
        case IDC_BROWSE: BrowseFolder(dlg); return TRUE;
        case IDOK:       if (ApplySettings(dlg)) EndDialog(dlg, IDOK); return TRUE;   // csak érvényes adatnál zárunk
        case IDCANCEL:   EndDialog(dlg, IDCANCEL); return TRUE;                       // Mégse: semmit nem mentünk
        }
        break;
    }
    return FALSE;
}

// Megnyitja a beállítások ablakot – vagy ha már nyitva van, előtérbe hozza.
// Ez a védelem azért kell, mert a modális ablak alatt a tálcamenü továbbra is
// elérhető, és onnan újra rá lehetne kattintani a "Beállítások"-ra.
static void OpenSettings()
{
    if (g_dlg) {
        SetForegroundWindow(g_dlg);
        return;
    }
    // A szülő a rejtett főablak. A dialógus az .rc-ben WS_EX_APPWINDOW stílust
    // kapott, különben – rejtett szülő miatt – nem jelenne meg a tálcán és az
    // Alt+Tab listában, és könnyű lenne "elveszíteni" más ablakok mögött.
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), g_wnd, SettingsProc, 0);
    g_dlg = nullptr;   // ide csak az ablak bezárása után jutunk
}


// =============================================================================
//  8. Tálcaikon, menü, főablak
// =============================================================================

// Felteszi az ikont a tálcára (az óra melletti értesítési területre).
// Indításkor, és akkor is hívjuk, ha az Explorer újraindult (lásd WndProc).
static void AddTray()
{
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;                 // ennek az ablaknak küldi az ikon az eseményeit...
    g_nid.uID = 1;                      // (egy programnak több ikonja is lehetne – nekünk ez az 1-es)
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;   // ...ezzel az üzenettel
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    UpdateTip();
}

// A jobb klikkes menü. Minden alkalommal újra felépítjük – 4 sor, ez gyorsabb,
// mint egy erőforrásból betölteni, és nem kell külön menü az .rc fájlba.
static void ShowMenu()
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_NEXT, L"Következő kép");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Beállítások…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Kilépés");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);   // félkövér = ez történik dupla klikkre

    POINT pt;
    GetCursorPos(&pt);   // az egér helyén jelenjen meg

    // Két ismert Windows-furcsaság kezelése tálcamenünél (a Microsoft is így
    // javasolja a dokumentációban):
    //   1. SetForegroundWindow előtte – különben a menü nem tűnik el, ha
    //      mellé kattintasz.
    //   2. PostMessage(WM_NULL) utána – különben a második megnyitáskor a menü
    //      azonnal bezárulhat.
    SetForegroundWindow(g_wnd);
    // A menü kiválasztott pontja WM_COMMAND üzenetként érkezik a g_wnd-hez.
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON, pt.x, pt.y, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// A rejtett főablak üzenetkezelője – ez a program "agya". Minden esemény
// (timer, tálcaikon, menü, a cserélő szál jelzése) ide fut be, egyetlen szálon,
// ezért itt nem kell szinkronizációval foglalkozni.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        // A tálcaikon eseménye; az lParam mondja meg, milyen egéresemény történt.
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:     ShowMenu(); break;       // jobb klikk (felengedéskor, ahogy a Windowsban szokás)
        case WM_LBUTTONDBLCLK: OpenSettings(); break;   // dupla bal klikk
        }
        return 0;

    case WM_COMMAND:
        // Menüpont kiválasztva (vagy a 2. példány kérte a beállításokat, lásd Run).
        switch (LOWORD(wp)) {
        case IDM_NEXT:
            NextWallpaper();
            RestartTimer();   // a következő automatikus csere innen számítva teljes időköz múlva jön
            break;
        case IDM_SETTINGS:
            OpenSettings();
            break;
        case IDM_EXIT:
            // Ha a beállítások ablak nyitva van, előbb azt zárjuk be (mentés nélkül).
            if (g_dlg) EndDialog(g_dlg, IDCANCEL);
            DestroyWindow(hwnd);   // -> WM_DESTROY -> kilépés
            break;
        }
        return 0;

    case WM_TIMER:
        // Letelt az időköz – jöhet a következő kép.
        NextWallpaper();
        return 0;

    case WM_WALLDONE:
        // A cserélő szál végzett. Most már indulhat újabb csere.
        InterlockedExchange(&g_busy, 0);
        // Ha nem sikerült (pl. a fájlt közben törölték, vagy sérült a kép),
        // a pakli "elfogyottnak" jelölésével kikényszerítjük, hogy a következő
        // cserénél újraolvassuk a mappát – így a törölt fájlok kiesnek.
        if (!wp) g_pos = g_count;
        UpdateTip();
        return 0;

    case WM_DESTROY:
        // Takarítás kilépéskor. Az ikont KÖTELEZŐ levenni, különben "szellem
        // ikon" marad a tálcán, ami csak akkor tűnik el, ha egérrel fölé mész.
        KillTimer(hwnd, TIMER_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);   // kilépteti a Run()-beli üzenethurkot
        return 0;
    }

    // Az Explorer összeomlás vagy újraindítás után újraépíti a tálcát, és minden
    // ablaknak elküldi a "TaskbarCreated" üzenetet. Ilyenkor az összes tálcaikon
    // eltűnt, nekünk újra fel kell tenni a sajátunkat.
    // A száma futásidőben derül ki (RegisterWindowMessage), ezért nem lehet a
    // fenti switch egyik case-e. A "&& g_msgTaskbarCreated" azért kell, hogy a
    // regisztráció előtt a 0-s értékkel ne egyezzen semmi.
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        AddTray();
        return 0;
    }

    // Minden mást a Windows alapértelmezett módon kezel.
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// =============================================================================
//  9. Indulás
// =============================================================================

// A program tényleges "main"-je. Debug és Release build is ezt hívja, csak más
// belépési ponton keresztül (lásd a fájl alján).
static int Run()
{
    g_inst = GetModuleHandleW(nullptr);   // a saját exe modulja (nullptr = "a futó program")

    // --- Csak egy példány fusson ---
    // Egy névvel ellátott mutex rendszerszinten (a felhasználó munkamenetén belül)
    // egyedi. Ha már létezik, egy másik duckybg fut. Ilyenkor nem indulunk el
    // másodszor, hanem megkérjük a futót, hogy nyissa meg a beállításokat – így
    // az exe újbóli elindítása "Beállítások" gombként is működik.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"duckybg_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(kClass, nullptr)) {
            // A Windows nem engedi, hogy egy háttérben futó program magától
            // előtérbe hozza az ablakát. Mi (akit épp a felhasználó indított el)
            // jogosultak vagyunk rá, és ezt a jogot átadjuk a másik példánynak.
            DWORD pid;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(w, WM_COMMAND, IDM_SETTINGS, 0);   // ugyanaz, mintha a menüből választotta volna
        }
        return 0;
    }

    // --- DPI ---
    // "Per-monitor v2" DPI-tudatosság: nagy felbontású vagy nagyított (pl. 150%)
    // kijelzőn éles az ikon és a dialógus, és ha az ablakot egy másik DPI-jű
    // monitorra húzod, a Windows automatikusan átméretezi. Enélkül a Windows
    // egyszerűen felnagyítaná a képet, ami elmosódott lenne.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --- COM ---
    // A mappaválasztó (IFileOpenDialog) COM objektum, ehhez kell. Az "apartment
    // threaded" mód a UI-szálaknak való; az OLE1DDE kikapcsolása régi, felesleges
    // kompatibilitási réteget hagy ki (a Microsoft ajánlása).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // --- Véletlenszám-generátor seedelése ---
    // Több, indításonként eltérő értéket keverünk össze, hogy minden indításkor
    // más sorrend jöjjön. A "| 1" garantálja, hogy nem 0 (lásd Rand).
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    g_rng = (c.LowPart ^ GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;

    // --- Útvonalak ---
    // A program saját exe-jének teljes útvonalából levágjuk a fájlnevet, így
    // megkapjuk a mappáját. Ehhez képest keressük az INI-t és a bg mappát.
    GetModuleFileNameW(nullptr, g_exeDir, MAX_PATH);
    for (int i = lstrlenW(g_exeDir); i > 0; --i)
        if (g_exeDir[i - 1] == L'\\') { g_exeDir[i - 1] = 0; break; }
    lstrcpyW(g_iniPath, g_exeDir);
    lstrcatW(g_iniPath, L"\\duckybg.ini");
    LoadSettings();

    // --- Rejtett főablak ---
    // Ablak nélkül nem kaphatnánk timer-, tálca- és menüüzeneteket, ezért kell
    // egy ablak, ami sosem jelenik meg (nem hívunk rá ShowWindow-t).
    // Miért nem "message-only" ablak (HWND_MESSAGE szülővel), ami pont erre
    // való? Mert az nem kapja meg a broadcast üzeneteket, így a TaskbarCreated-et
    // sem – és akkor Explorer-újraindítás után eltűnne az ikonunk.
    // A WS_EX_TOOLWINDOW biztosítja, hogy az Alt+Tab listában se jelenjen meg.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"duckybg", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return 1;
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // --- Tálcaikon ---
    // A LoadIconMetric az ikonfájlból a rendszer által kért "kis ikon" méretet
    // tölti be (DPI-tól függően 16, 20, 24, 32 px...). Ha a fájlban nincs pont
    // olyan méret, szépen (nem pixelesen) méretezi át. Ha valamiért nem megy, a
    // sima LoadIcon is megteszi tartaléknak.
    if (FAILED(LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_DUCKYBG), LIM_SMALL, &g_nid.hIcon)))
        g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_DUCKYBG));
    AddTray();

    // --- Indulás ---
    // Elindítjuk az időzítőt, és rögtön cserélünk is egyet, hogy a felhasználó
    // azonnal lássa: a program működik.
    RestartTimer();
    NextWallpaper();

    // --- Üzenethurok ---
    // Ez a program "élete": a GetMessage alvó állapotban vár (0% CPU), amíg
    // esemény nem jön, majd továbbítja a megfelelő ablakeljárásnak (WndProc).
    // Akkor lép ki, amikor a PostQuitMessage-ből érkező WM_QUIT megjön
    // (GetMessage 0-t ad), vagy hiba történik (-1) – ezért a "> 0" feltétel.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    CloseHandle(mutex);
    return (int)msg.wParam;
}

// --- Belépési pont ---
//
// Release (DUCKY_NOCRT): nincs CRT, tehát nincs, aki a WinMain-t meghívná.
// A linkernek megmondjuk (/ENTRY:entry, lásd a .vcxproj-ban), hogy közvetlenül
// ezt a függvényt hívja indításkor. A végén ExitProcess-szel lépünk ki: egy
// sima return csak a fő szálat állítaná le, és ha a cserélő szál épp fut, a
// folyamat életben maradna. (Debugban ezt a CRT intézi helyettünk.)
//
// Debug: a szokásos wWinMain, amit a CRT hív meg a saját inicializálása után.
// A paramétereket nem használjuk (a parancssort nem dolgozzuk fel).
#ifdef DUCKY_NOCRT
extern "C" void WINAPI entry() { ExitProcess((UINT)Run()); }
#else
int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) { return Run(); }
#endif
