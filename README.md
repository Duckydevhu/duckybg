# duckybg

Pici, gyors háttérkép-cserélő Windowsra. A tálcán, az óra mellett ül, és egy megadott mappából megadott percenként lecseréli az asztal háttérképét.

- kb. **17 KB** (x64, ikonnal együtt), egyetlen exe, telepítés nélkül
- nincs .NET, nincs Visual C++ Redistributable, nincs C runtime, csak a Windows saját DLL-jeit használja
- két csere között 0% CPU
- hordozható: a beállítások az exe melletti `duckybg.ini`-be kerülnek, a registrybe nem ír (a háttérkép-beállítást kivéve, azt maga a Windows menti)

## Használat

1. Másold a `duckybg.exe`-t egy tetszőleges mappába.
2. Indítsd el. Első indításkor létrehozza mellette:
   - a `bg\` mappát: ide tedd a képeket,
   - a `duckybg.ini` fájlt az alapbeállításokkal.
3. Másold a képeket a `bg\` mappába. A következő cserénél (vagy a *Következő kép* menüponttal azonnal) már ezeket használja.

### Tálcaikon

| Művelet | Mit csinál |
|---|---|
| Jobb klikk | menü: **Következő kép**, **Beállítások…**, **Kilépés** |
| Dupla bal klikk | beállítások megnyitása |
| Egér az ikon fölött | tooltip: aktuális kép neve és az időköz |
| Az exe újbóli elindítása | nem indul el másodszor, hanem a futó példány megnyitja a beállításokat |

A *Következő kép* a visszaszámlálást is elölről kezdi, a következő automatikus csere tehát innen számítva egy teljes időköz múlva jön.

### Beállítások

- **Képek mappája**: beírható vagy a `...` gombbal tallózható. Lehet relatív útvonal is (a program mappájához képest). Ha üresen hagyod, az alapértelmezett `bg\` mappát használja. Idézőjelek közé tett útvonalat (pl. az Explorer *Másolás elérési útként* funkciójából) is elfogad.
- **Csere ennyi percenként**: 1 és 10080 perc (1 hét) között.

Az OK után a beállítás azonnal érvénybe lép. Mappaváltáskor rögtön cserél is egy képet.

### Támogatott formátumok

`.jpg`, `.jpeg`, `.jfif`, `.png`, `.bmp`. Az almappákat nem nézi.

### A képek sorrendje

Úgy működik, mint egy kártyapakli: a mappa képeit megkeveri, és sorban végigmegy rajtuk. Egy körön belül minden kép pontosan egyszer jön. Ha a pakli elfogyott, újraolvassa a mappát (így az időközben bemásolt vagy törölt képek is számítanak), és újrakeveri. Az új kör nem kezdődhet ugyanazzal a képpel, amivel az előző véget ért.

A háttérkép elrendezését (kitöltés, igazítás, nyújtás, mozaik…) nem állítja, az marad, ami a Windows *Testreszabás → Háttér* beállításában van.

## duckybg.ini

```ini
[settings]
folder=bg
interval=15
```

| Kulcs | Jelentés | Alapérték |
|---|---|---|
| `folder` | a képek mappája; relatív (a program mappájához képest) vagy abszolút | `bg` |
| `interval` | csere időköze percben (1–10080) | `15` |

Kézzel is szerkeszthető, de a programnak újra kell indulnia, hogy beolvassa. Egyszerűbb a Beállítások ablakot használni. A fájl UTF-16 kódolású, így az ékezetes mappanevek is gond nélkül működnek.

Ha a program olyan helyen van, ahová nincs írási jog (pl. `C:\Program Files`), a beállítások nem menthetők. Ilyenkor tedd egy saját mappába.

## Automatikus indítás a Windowsszal

Nyomj `Win+R`-t, írd be: `shell:startup`, és a megnyíló mappába tegyél egy parancsikont a `duckybg.exe`-ről.

## Fordítás

Szükséges: **Visual Studio 2022** a *Desktop development with C++* komponenssel.

- Visual Studióban: nyisd meg a `duckybg.sln`-t, válaszd a **Release | x64** konfigurációt, majd *Build → Build Solution*.
- Parancssorból (Developer PowerShell / Developer Command Prompt):

  ```bash
  msbuild duckybg.sln /p:Configuration=Release /p:Platform=x64
  ```

  32 bites változathoz: `/p:Platform=x86`.

Kimenet: `x64\Release\duckybg.exe` (32 bitesnél `Release\duckybg.exe`).

### Debug és Release különbsége

| | Debug | Release |
|---|---|---|
| C runtime | benne van | **nincs** (`/NODEFAULTLIB`, saját belépési pont: `entry`) |
| Optimalizálás | nincs | méretre (`/O1`) |
| Biztonsági ellenőrzések | `/sdl`, `/GS` | kikapcsolva (CRT kellene hozzájuk) |
| Méret (x64, ikonnal) | ~85 KB | ~17 KB |

**Fontos, ha módosítod a kódot:** Release-ben nincs C runtime, ezért ott **nem** használható:

- `std::` bármi, `new`/`delete`, kivételek, RTTI, `float`/`double`,
- CRT függvények (`wcscpy`, `sprintf`, `malloc`…): helyettük a Win32 megfelelők (`lstrcpyW`, `wsprintfW`, `HeapAlloc`…),
- konstruktoros globális objektum,
- 4 KB-nál nagyobb lokális tömb egy függvényben (a nagy puffereket tedd globálissá).

Debugban ezek lefordulnak, Release-ben linker hibát adnak. Ezért kiadás előtt mindig fordíts Release-ben is. A részletek a `duckybg.cpp` elején, kommentben vannak.

## Fájlok

| Fájl | Tartalom |
|---|---|
| `duckybg.cpp` | a teljes program, részletesen kommentelve |
| `duckybg.rc` | erőforrások: ikon és a beállítások ablak elrendezése |
| `Resource.h` | az erőforrások és menüpontok azonosítói |
| `framework.h` | rendszer-fejlécek |
| `targetver.h` | Windows SDK verzió |
| `duckybg.ico` | a program ikonja |
| `duckybg.vcxproj` | Visual Studio projekt (itt vannak a CRT nélküli Release beállítások) |

## Rendszerkövetelmény

Windows 10 (1703) vagy újabb, x64 vagy x86.
