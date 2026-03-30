# GUI-Erweiterung für video-compare: Umsetzungsplan

## 1. Überblick und Zielsetzung

Dieser Plan beschreibt die schrittweise Erweiterung des [pixop/video-compare](https://github.com/pixop/video-compare) Forks um eine komfortable grafische Benutzeroberfläche. Das Ziel ist ein Tool, das die volle Codec-Unterstützung von video-compare (H.264, H.265, MPEG-2, MP4, MKV, u.v.m.) mit einer intuitiven GUI verbindet — ohne die bestehende CLI-Funktionalität zu opfern.

### Designprinzipien

- **Nicht-invasiv:** Die bestehende Architektur (FFmpeg-Decoding → SDL2-Rendering) bleibt erhalten. Die GUI wird als zusätzliche Schicht darübergelegt.
- **CLI-Kompatibilität:** Alle bestehenden CLI-Parameter bleiben funktionsfähig. Die GUI ist ein optionaler Modus, der aktiviert wird, wenn keine Dateipfade als Argumente übergeben werden.
- **Plattformübergreifend:** macOS, Windows, Linux — ohne Code-Signing oder Developer-Accounts.
- **Minimale Dependencies:** Nur Dear ImGui (Header-only + wenige .cpp-Dateien) und eine native File-Dialog-Bibliothek kommen hinzu.

---

## 2. Bestandsaufnahme: Aktuelle Architektur

Die aktuelle Codebasis ist flach strukturiert (alle `.cpp`/`.h` im Root) mit einem einfachen Makefile. Die wesentlichen Komponenten:

```
main.cpp                 → Einstiegspunkt, CLI-Parsing (argagg.h)
video_compare.cpp/h      → Zentrale Vergleichslogik, Orchestrierung
display.cpp/h            → SDL2-Fenster, Textur-Rendering, HUD-Overlay
controls.cpp/h           → Tastatur-/Maus-Event-Handling
demuxer.cpp/h            → FFmpeg-Demuxing (Container-Öffnung)
video_decoder.cpp/h      → FFmpeg-Decoding (Codec-Verarbeitung)
video_filterer.cpp/h     → FFmpeg-Filtergraphen
format_converter.cpp/h   → Pixel-Format-Konvertierung (sws_scale)
scope_manager.cpp/h      → Histogram/Vectorscope/Waveform-Fenster
scope_window.cpp/h       → SDL2-Rendering der Scope-Fenster
vmaf_calculator.cpp/h    → VMAF-Metrik-Berechnung
png_saver.cpp/h          → Screenshot-Export (stb_image_write)
timer.cpp/h              → Playback-Timing
queue.h                  → Thread-sichere Queue für Frames
circular_buffer.h        → Ring-Buffer für Frame-Pufferung
makefile                 → GNU Make Build-System
```

### Kritische Erkenntnis zum Rendering

video-compare nutzt `SDL_Renderer` mit `SDL_Texture` für das Video-Rendering — nicht OpenGL direkt. Das hat Konsequenzen für die ImGui-Integration: Das offizielle ImGui-Backend `imgui_impl_sdlrenderer2` arbeitet direkt mit `SDL_Renderer` und erfordert kein OpenGL-Setup. Dies ist der einfachste Integrationspfad, weil er keine Änderung am bestehenden Rendering-Pipeline erfordert.

---

## 3. Technologie-Entscheidungen

### 3.1 GUI-Framework: Dear ImGui

**Warum Dear ImGui (und nicht Qt, GTK, wxWidgets)?**

- **Null externe Dependencies:** Dear ImGui ist Header-only + ~6 .cpp-Dateien, die direkt in den Build eingebunden werden. Keine Shared Libraries, kein Package-Management, keine Lizenzprobleme.
- **SDL2-nativer Backend:** `imgui_impl_sdl2` + `imgui_impl_sdlrenderer2` integrieren sich direkt in die bestehende SDL2-Render-Loop. Die Video-Frames werden weiterhin als SDL_Texture gerendert; ImGui zeichnet seine UI-Elemente darüber.
- **Immediate Mode:** Perfekt für Tools — kein komplexes State-Management, kein Signal/Slot-System. Die gesamte UI wird in der Render-Loop deklariert.
- **Docking-Branch:** Erlaubt es Nutzern, Panels frei anzuordnen und zu docken (Scopes, Metrik-Fenster, Einstellungen etc.).
- **Kein Einfluss auf Distribution:** Kein zusätzliches Framework, das verlinkt oder mitgeliefert werden muss.

### 3.2 Datei-Dialoge: Portable File Dialogs

Für native Open/Save-Dialoge wird [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs) verwendet:

- Header-only (eine einzige .h-Datei)
- Nutzt native OS-Dialoge (Cocoa auf macOS, GTK/KDE auf Linux, Win32 auf Windows)
- Keine zusätzlichen Link-Dependencies
- MIT-Lizenz (kompatibel mit GPL-2.0)

### 3.3 Build-System: Migration zu CMake

Das bestehende Makefile wird durch CMake ersetzt. Gründe:

- FetchContent für Dear ImGui (automatischer Download des docking-Branch)
- Sauberes Dependency-Management für FFmpeg, SDL2, SDL2_ttf
- Cross-Platform-Generator (Make auf Linux/macOS, MSVC/Ninja auf Windows)
- Bessere IDE-Integration (CLion, VS Code, Visual Studio)
- Voraussetzung für CPack-basierte Packaging-Automatisierung

Das Original-Makefile bleibt als `makefile.legacy` für Abwärtskompatibilität erhalten.

---

## 4. Phasenplan

### Phase 1: Fundament (Wochen 1–2)

> **Ziel:** Dear ImGui in die bestehende Render-Loop integrieren, ohne bestehendes Verhalten zu ändern.

#### 1.1 Repository-Struktur reorganisieren

```
video-compare-gui/
├── CMakeLists.txt
├── makefile.legacy              ← Original-Makefile
├── src/
│   ├── main.cpp
│   ├── video_compare.cpp/h
│   ├── display.cpp/h
│   ├── controls.cpp/h
│   ├── demuxer.cpp/h
│   ├── video_decoder.cpp/h
│   ├── video_filterer.cpp/h
│   ├── format_converter.cpp/h
│   ├── scope_manager.cpp/h
│   ├── scope_window.cpp/h
│   ├── vmaf_calculator.cpp/h
│   ├── png_saver.cpp/h
│   ├── timer.cpp/h
│   ├── queue.h
│   ├── circular_buffer.h
│   ├── sorted_flat_deque.h
│   ├── row_workers.h
│   ├── side_aware.h
│   ├── core_types.cpp/h
│   ├── string_utils.cpp/h
│   ├── filtered_logger.cpp/h
│   ├── side_aware_logger.cpp/h
│   ├── sdl_event_info.cpp/h
│   ├── ffmpeg.cpp/h
│   ├── config.h
│   └── version.cpp/h
├── gui/
│   ├── gui_manager.cpp/h        ← Zentrale GUI-Klasse
│   ├── gui_launcher.cpp/h       ← Start-Screen (Dateiauswahl)
│   ├── gui_controls.cpp/h       ← GUI-Playback-Controls
│   ├── gui_settings.cpp/h       ← Einstellungs-Panel
│   └── gui_metrics.cpp/h        ← Metrik-Overlay-Panel
├── vendor/
│   ├── argagg.h
│   ├── stb_image_write.h
│   ├── source_code_pro_regular_ttf.h
│   ├── video_compare_icon.h
│   └── portable_file_dialogs.h
├── assets/
│   └── (Icons, Font-Dateien falls nötig)
├── .github/
│   └── workflows/
│       └── ci.yml
└── README.md
```

#### 1.2 CMakeLists.txt aufsetzen

```cmake
cmake_minimum_required(VERSION 3.20)
project(video-compare-gui VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavfilter libavutil libswscale libswresample)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

# Dear ImGui via FetchContent (docking branch)
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# ImGui als statische Library
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC PkgConfig::SDL2)

# --- Hauptprojekt ---
file(GLOB SRC_FILES src/*.cpp gui/*.cpp)
add_executable(video-compare-gui ${SRC_FILES})
target_include_directories(video-compare-gui PRIVATE
    src/ gui/ vendor/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(video-compare-gui PRIVATE
    PkgConfig::FFMPEG PkgConfig::SDL2 PkgConfig::SDL2_TTF imgui_lib
)
```

#### 1.3 ImGui in die Render-Loop einbinden

Der zentrale Eingriffspunkt ist `display.cpp`. Der aktuelle Rendering-Ablauf:

```
SDL_RenderClear()
  → Video-Texturen zeichnen (SDL_RenderCopy)
  → HUD-Overlay zeichnen (SDL_RenderCopy für Text-Texturen)
SDL_RenderPresent()
```

Wird erweitert zu:

```
SDL_RenderClear()
  → Video-Texturen zeichnen (SDL_RenderCopy)
  → HUD-Overlay zeichnen (SDL_RenderCopy für Text-Texturen)
  → ImGui_ImplSDLRenderer2_NewFrame()
  → ImGui_ImplSDL2_NewFrame()
  → ImGui::NewFrame()
  → GUI-Elemente zeichnen (gui_manager)
  → ImGui::Render()
  → ImGui_ImplSDLRenderer2_RenderDrawData()
SDL_RenderPresent()
```

**Wichtig:** ImGui muss die SDL-Events vor den bestehenden Controls verarbeiten. In der Event-Loop:

```cpp
while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL2_ProcessEvent(&event);

    // Nur an bestehende Controls weiterleiten, wenn ImGui
    // das Event nicht konsumiert hat
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !io.WantCaptureKeyboard) {
        // bestehende Event-Verarbeitung aus controls.cpp
    }
}
```

#### 1.4 Abnahmekriterium Phase 1

- [x] `cmake --build .` kompiliert auf allen drei Plattformen
- [x] Das Tool verhält sich identisch zum Original, wenn mit CLI-Argumenten gestartet
- [x] ImGui Demo-Fenster erscheint als Overlay über dem Video (nur im Debug-Modus)
- [x] ImGui-Widgets fangen Maus-Events korrekt ab, ohne Video-Controls zu stören

---

### Phase 2: Launcher-Screen (Wochen 3–4)

> **Ziel:** Beim Start ohne CLI-Argumente wird ein grafischer Launcher angezeigt, über den Dateien ausgewählt und Optionen konfiguriert werden können.

#### 2.1 GUI-Launcher (`gui_launcher.cpp/h`)

Wenn `video-compare-gui` ohne Argumente gestartet wird, öffnet sich statt des Vergleichsfensters ein Launcher-Screen. Dieser wird in einem SDL2-Fenster mit ImGui gerendert.

**Layout des Launchers:**

```
┌─────────────────────────────────────────────────────┐
│  video-compare GUI                          [─][□][×]│
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌─ Linkes Video (Referenz) ─────────────────────┐  │
│  │  [📂 Datei wählen...]  /pfad/zur/datei.mkv    │  │
│  │  Filter: [________________________]  (opt.)   │  │
│  │  Demuxer: [auto         ▼]                    │  │
│  │  Decoder: [auto         ▼]                    │  │
│  │  HW-Accel: [none        ▼]                    │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  ┌─ Rechtes Video (Encode) ──────────────────────┐  │
│  │  [📂 Datei wählen...]  /pfad/zur/datei.mp4    │  │
│  │  Filter: [________________________]  (opt.)   │  │
│  │  Demuxer: [auto         ▼]                    │  │
│  │  Decoder: [auto         ▼]                    │  │
│  │  HW-Accel: [none        ▼]                    │  │
│  │                                               │  │
│  │  [+ Weitere Rendition hinzufügen]             │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  ┌─ Allgemeine Einstellungen ────────────────────┐  │
│  │  Vergleichsmodus:  [Split-Screen  ▼]          │  │
│  │  Bit-Tiefe:        [8-bit ▼]   ☐ High DPI    │  │
│  │  Fenstergröße:     [auto  ] x [auto  ]        │  │
│  │  Auto-Loop:        [aus          ▼]           │  │
│  │  Time-Shift:       [0.000   ] Sekunden        │  │
│  │  HDR Peak Nits:    [500     ]                 │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  ┌─ Letzte Vergleiche ──────────────────────────┐   │
│  │  • video1.mkv ↔ video1_x265.mp4    [Öffnen]  │  │
│  │  • source.mp4 ↔ encode_crf18.mp4   [Öffnen]  │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│             [▶ Vergleich starten]                    │
│                                                     │
└─────────────────────────────────────────────────────┘
```

#### 2.2 Datei-Dialoge via portable-file-dialogs

```cpp
#include "portable_file_dialogs.h"

auto selection = pfd::open_file(
    "Video auswählen",
    "",
    {"Videodateien", "*.mp4 *.mkv *.avi *.mov *.ts *.m2ts *.webm *.y4m",
     "Alle Dateien", "*"},
    pfd::opt::none
).result();

if (!selection.empty()) {
    left_video_path = selection[0];
}
```

#### 2.3 Drag & Drop Support

SDL2 unterstützt Drag & Drop nativ über `SDL_DROPFILE`-Events. Im Launcher:

```cpp
case SDL_DROPFILE: {
    std::string dropped_file = event.drop.file;
    if (left_path.empty()) {
        left_path = dropped_file;
    } else if (right_path.empty()) {
        right_path = dropped_file;
    }
    SDL_free(event.drop.file);
    break;
}
```

#### 2.4 Persistente Konfiguration (Recent Files, Einstellungen)

Gespeichert als einfache JSON-Datei:

- **macOS:** `~/Library/Application Support/video-compare-gui/config.json`
- **Linux:** `~/.config/video-compare-gui/config.json`
- **Windows:** `%APPDATA%/video-compare-gui/config.json`

Für JSON-Parsing wird [nlohmann/json](https://github.com/nlohmann/json) (header-only, MIT-Lizenz) oder ein minimaler eigener Parser verwendet. Alternativ: Einfaches INI-Format, um die Dependency zu vermeiden.

#### 2.5 Programmablauf

```
main() ──┬── CLI-Argumente vorhanden? ── Ja ──→ Bestehender Ablauf (video_compare)
         │
         └── Nein ──→ gui_launcher::show()
                          │
                          ├── Nutzer wählt Dateien und Optionen
                          │
                          └── "Vergleich starten" geklickt
                                  │
                                  └── Argumente zusammenbauen
                                      → video_compare::run() aufrufen
```

#### 2.6 Abnahmekriterium Phase 2

- [x] Start ohne Argumente öffnet den Launcher
- [x] Dateien können per Dialog und per Drag & Drop geladen werden
- [x] Alle wesentlichen CLI-Optionen sind über die GUI konfigurierbar
- [x] "Vergleich starten" öffnet den Vergleichs-View mit den gewählten Einstellungen
- [x] Letzte Vergleiche werden gespeichert und können erneut geöffnet werden

---

### Phase 3: In-Video GUI-Controls (Wochen 5–7)

> **Ziel:** Während des Vergleichs werden interaktive GUI-Elemente über das Video gelegt, die die bestehenden Tastatur-Shortcuts ergänzen (nicht ersetzen).

#### 3.1 Toolbar (gui_controls.cpp/h)

Eine semi-transparente Toolbar am unteren Bildschirmrand, die bei Mausbewegung eingeblendet wird (Auto-Hide nach 3 Sekunden Inaktivität):

```
┌──────────────────────────────────────────────────────────────┐
│ [⏮][⏪][▶/⏸][⏩][⏭]  00:01:23.456 / 00:05:00.000  [🔍+][🔍-]│
│ ├──────────────●──────────────────────────────────────┤      │
│ Modus: [Split ▼]  Speed: [1.0x ▼]  Shift: [-0.04s]  [⚙]   │
└──────────────────────────────────────────────────────────────┘
```

**Elemente:**

| Element | Funktion | Bestehender Shortcut |
|---|---|---|
| Play/Pause-Button | Wiedergabe steuern | `Space` |
| Frame-Step-Buttons | Frame vor/zurück | `A` / `D` |
| Seek-Buttons | ±1s, ±15s springen | Pfeiltasten |
| Seek-Bar | Klick-basiertes Seeking | Mausklick (bereits vorhanden) |
| Modus-Dropdown | Split/Subtraction/VStack/HStack | `0`, `M` |
| Speed-Dropdown | 0.25x – 4x | `J` / `L` |
| Time-Shift-Anzeige | Aktueller Versatz mit ±-Buttons | `+` / `-` |
| Zahnrad-Icon | Öffnet Einstellungs-Panel | — |

#### 3.2 Einstellungs-Panel (gui_settings.cpp/h)

Ein Seitenleisten-Panel, das über das Zahnrad-Icon oder `F12` geöffnet wird:

```
┌─ Einstellungen ────────────────────┐
│                                    │
│  Anzeige                           │
│  ├ Bit-Tiefe:     [8 ▼] / [10 ▼]  │
│  ├ Texture-Filter: ☐ Bilinear     │
│  ├ Zoom:          [100% ──●──]     │
│  └ High DPI:      ☑                │
│                                    │
│  Subtraktion                       │
│  ├ Modus:   [Standard      ▼]     │
│  ├ Verstärkung: [1.0 ──●──]       │
│  └ Nur Luminanz: ☐                │
│                                    │
│  HDR                               │
│  ├ Peak Nits:    [500 ──●──]       │
│  ├ Tone-Mapping: [auto      ▼]    │
│  └ Methode:      [relative  ▼]    │
│                                    │
│  Filter                            │
│  ├ Links:  [________________]      │
│  ├ Rechts: [________________]      │
│  └ [Filter anwenden]               │
│                                    │
│  Renditions                        │
│  ├ ● rendition1.mp4                │
│  ├ ○ rendition2.mp4                │
│  └ ○ rendition3.mp4                │
│                                    │
│  [Zurück zum Launcher]             │
└────────────────────────────────────┘
```

#### 3.3 Metrik-Panel (gui_metrics.cpp/h)

Ein optionales Panel, das Qualitätsmetriken in Echtzeit oder on-demand anzeigt:

- **Frame-Info:** PTS, Frame-Typ (I/P/B), Auflösung, Pixel-Format
- **Pixel-Inspector:** RGB/YUV-Werte unter dem Cursor (bereits als `P`-Shortcut vorhanden, jetzt permanent sichtbar)
- **Metriken:** PSNR, SSIM pro Frame (on-demand berechenbar über bestehende `M`-Funktionalität)
- **VMAF-Graph:** Falls VMAF vorberechnet wurde, Anzeige des Scores über die Zeit

#### 3.4 Integration mit bestehendem Controls-System

Die neue GUI darf das bestehende Keyboard/Maus-System nicht brechen. Architektur:

```
SDL_Event
    │
    ├──→ ImGui_ImplSDL2_ProcessEvent()
    │        │
    │        └── io.WantCaptureMouse / io.WantCaptureKeyboard?
    │                │
    │                ├── Ja  → Event wird von ImGui konsumiert
    │                │        (GUI-Widget aktiv, z.B. Slider, Textfeld)
    │                │
    │                └── Nein → Event weiter an controls.cpp
    │
    └──→ controls.cpp (bestehende Shortcuts)
```

**Sonderfall Maus im Video-Bereich:** Wenn die Maus über dem Video-Bereich liegt und kein ImGui-Widget aktiv ist, verhält sich alles wie im Original (Slider-Position, Seek via Klick, Zoom via Scrollrad).

#### 3.5 Auto-Hide-Logik

```cpp
class AutoHideController {
    float opacity = 0.0f;
    float fade_speed = 3.0f;  // Sekunden zum Ein-/Ausblenden
    float idle_timeout = 3.0f;
    float idle_timer = 0.0f;
    bool mouse_over_gui = false;
    bool force_visible = false;  // z.B. wenn Einstellungs-Panel offen

    void update(float dt, bool mouse_moved) {
        if (mouse_moved || mouse_over_gui || force_visible) {
            idle_timer = 0.0f;
            opacity = std::min(1.0f, opacity + dt * fade_speed);
        } else {
            idle_timer += dt;
            if (idle_timer > idle_timeout) {
                opacity = std::max(0.0f, opacity - dt * fade_speed);
            }
        }
    }
};
```

ImGui-Rendering wird mit `ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity)` gesteuert.

#### 3.6 Abnahmekriterium Phase 3

- [x] Toolbar erscheint bei Mausbewegung, verschwindet nach Inaktivität
- [x] Alle Toolbar-Buttons funktionieren korrekt
- [x] Einstellungs-Panel kann geöffnet/geschlossen werden
- [x] Tastatur-Shortcuts funktionieren weiterhin uneingeschränkt
- [x] Maus-Interaktion im Video-Bereich (Slider, Zoom) unverändert

---

### Phase 4: Erweiterte Vergleichsmodi (Wochen 8–10)

> **Ziel:** Über die bestehenden Modi (Split, Subtraction, VStack, HStack) hinaus neue Vergleichsmethoden implementieren.

#### 4.1 Flicker/Toggle-Modus

Wechselt periodisch zwischen linkem und rechtem Frame. Extrem effektiv für subtile Unterschiede.

- Toggle-Intervall konfigurierbar (100ms – 2000ms)
- Manueller Toggle per Tastendruck
- Visueller Indikator, welches Video gerade sichtbar ist

**Implementierung:** In `display.cpp` einen Timer einbauen, der zwischen dem Rendern der linken und rechten Textur umschaltet. Kein neues Rendering nötig — nur die Auswahl, welche Textur per `SDL_RenderCopy` gezeichnet wird.

#### 4.2 Seite-an-Seite mit unabhängigem Zoom

Erweiterung des bestehenden HStack-Modus:

- Synchroner Zoom: Beide Seiten zoomen gleichzeitig (wie bisher)
- Unabhängiger Zoom: Jede Seite kann separat gezoomt werden
- Sync-Lock-Toggle für Pan-Position

#### 4.3 Pixel-Differenz-Heatmap

Erweiterung des bestehenden Subtraction-Modus:

- Statt einfacher Subtraktion: Farbcodierte Heatmap der Unterschiede
- Konfigurierbare Farbrampe (Grau → Gelb → Rot)
- Schwellwert-Filter: Nur Unterschiede über N% anzeigen
- Statistik-Overlay: Prozentsatz geänderter Pixel, mittlere Abweichung

**Implementierung:** In der bestehenden `row_workers.h`-Pipeline, die bereits Multi-Threaded Subtraction durchführt, eine alternative Farb-Mapping-Funktion einbauen.

#### 4.4 Modus-Architektur

Um die Modi sauber erweiterbar zu halten, wird ein einfaches Strategie-Pattern eingeführt:

```cpp
enum class CompareMode {
    Split,          // Bestehend
    Subtraction,    // Bestehend (mit Sub-Modi via Y-Taste)
    VStack,         // Bestehend
    HStack,         // Bestehend
    Flicker,        // Neu
    Heatmap,        // Neu
};

// In display.cpp:
void Display::render_comparison(CompareMode mode) {
    switch (mode) {
        case CompareMode::Split:
            render_split_screen();      // Bestehender Code
            break;
        case CompareMode::Flicker:
            render_flicker();           // Neuer Code
            break;
        case CompareMode::Heatmap:
            render_heatmap();           // Neuer Code
            break;
        // ...
    }
}
```

#### 4.5 Abnahmekriterium Phase 4

- [x] Flicker-Modus mit konfigurierbarem Intervall funktioniert
- [x] Heatmap-Modus zeigt farbcodierte Unterschiede
- [x] Alle Modi sind über GUI-Dropdown und Tastatur umschaltbar
- [x] Bestehende Modi (Split, Subtraction, VStack, HStack) unverändert

---

### Phase 5: Scope-Fenster-Migration und Metriken (Wochen 11–13)

> **Ziel:** Die bestehenden Scope-Fenster (Histogram, Vectorscope, Waveform) in ImGui-Panels migrieren und VMAF/SSIM-Graphen hinzufügen.

#### 5.1 Scopes als ImGui-Panels

Aktuell werden Scopes in separaten SDL2-Fenstern gerendert (`scope_window.cpp`). Diese werden in ImGui-Panels konvertiert, die:

- Im Hauptfenster als dockbare Panels dargestellt werden
- Per Drag & Drop angeordnet werden können (ImGui Docking-Branch)
- Die gleichen Daten nutzen, aber in ImGui-Zeichenbefehlen (`ImDrawList`) gerendert werden

**Übergangsphase:** Beide Systeme parallel betreiben (Umschalten per Flag), bis die ImGui-Variante Feature-Parität hat.

#### 5.2 VMAF/SSIM/PSNR-Timeline-Graph

- Horizontaler Graph über die gesamte Videolänge
- Aktuelle Position als vertikale Linie markiert
- Kann via FFmpeg-Filter (`libvmaf`, `ssim`, `psnr`) vorberechnet oder aus externer JSON/CSV-Datei geladen werden
- Click-to-Seek: Klick auf den Graphen springt zu dieser Position

#### 5.3 Abnahmekriterium Phase 5

- [x] Scopes als ImGui-Panels funktional
- [x] VMAF/SSIM-Graph anzeigbar (aus Datei oder vorberechnet)
- [x] Click-to-Seek im Metrik-Graphen

---

### Phase 6: Distribution und Polish (Wochen 14–16)

> **Ziel:** Packaging für alle Plattformen, CI/CD, Dokumentation.

#### 6.1 Homebrew Tap (macOS und Linux)

Eigenes Tap-Repository erstellen: `github.com/DEIN-USER/homebrew-video-compare-gui`

```ruby
class VideoCompareGui < Formula
  desc "Video comparison tool with GUI, based on pixop/video-compare"
  homepage "https://github.com/DEIN-USER/video-compare-gui"
  url "https://github.com/DEIN-USER/video-compare-gui/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "..."
  license "GPL-2.0-only"

  depends_on "cmake" => :build
  depends_on "ffmpeg"
  depends_on "sdl2"
  depends_on "sdl2_ttf"

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args
    system "cmake", "--build", "build"
    bin.install "build/video-compare-gui"
  end

  test do
    assert_match "Usage:", shell_output("#{bin}/video-compare-gui --help 2>&1")
  end
end
```

**Installation:** `brew tap DEIN-USER/video-compare-gui && brew install video-compare-gui`

Da Homebrew aus dem Quellcode baut, gibt es keine Gatekeeper-Probleme auf macOS — kein Code-Signing, keine Notarization, kein Developer-Account nötig.

#### 6.2 Linux: AppImage

AppImage-Build via [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy):

```bash
# In GitHub Actions:
linuxdeploy \
  --executable build/video-compare-gui \
  --desktop-file video-compare-gui.desktop \
  --icon-file icon.png \
  --appdir AppDir \
  --output appimage
```

Keine Signing-Anforderungen auf Linux.

#### 6.3 Windows: Portable ZIP

Wie beim Original video-compare: ZIP-Archiv mit Executable + FFmpeg/SDL2 DLLs auf GitHub Releases.

Optional: [Inno Setup](https://jrsoftware.org/isinfo.php) für einen Installer. Ohne EV-Zertifikat erscheint die SmartScreen-Warnung; dies ist für Developer-/Power-User-Tools akzeptabel.

#### 6.4 GitHub Actions CI/CD

```yaml
name: Build & Release
on:
  push:
    tags: ['v*']

jobs:
  build-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - run: brew install ffmpeg sdl2 sdl2_ttf
      - run: cmake -S . -B build && cmake --build build
      - uses: actions/upload-artifact@v4
        with: { name: macos, path: build/video-compare-gui }

  build-linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: |
          sudo apt-get update
          sudo apt-get install -y build-essential cmake \
            libavformat-dev libavcodec-dev libavfilter-dev \
            libavutil-dev libswscale-dev libswresample-dev \
            libsdl2-dev libsdl2-ttf-dev
      - run: cmake -S . -B build && cmake --build build
      # AppImage-Erstellung hier

  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      # vcpkg oder vorgebaute Dependencies
      - run: cmake -S . -B build && cmake --build build --config Release
```

#### 6.5 Abnahmekriterium Phase 6

- [x] `brew tap ... && brew install ...` funktioniert auf macOS und Linux
- [x] AppImage läuft auf gängigen Linux-Distributionen
- [x] Windows ZIP mit allen DLLs auf GitHub Releases
- [x] CI baut automatisch bei Tag-Push

---

## 5. Risiken und Mitigationsstrategien

| Risiko | Wahrscheinlichkeit | Impact | Mitigation |
|---|---|---|---|
| SDL_Renderer-Backend von ImGui zu langsam bei 4K | Mittel | Mittel | Fallback auf OpenGL3-Backend. Erfordert Migration zu `SDL_GL_CreateContext` + `imgui_impl_opengl3`, was das Video-Rendering auf GL-Texturen umstellt. Mehr Aufwand, aber deutlich performanter. |
| ImGui Docking-Branch API-Änderungen | Niedrig | Niedrig | Docking ist seit Jahren stabil. Version pinnen via FetchContent GIT_TAG. |
| `portable-file-dialogs` funktioniert nicht überall auf Linux | Mittel | Niedrig | Fallback: ImGui-basierter Datei-Browser (z.B. [ImFileDialog](https://github.com/dfranx/ImFileDialog)) als Alternative. |
| FFmpeg API-Änderungen brechen Build | Mittel | Mittel | CI mit mehreren FFmpeg-Versionen testen (5.x, 6.x, 7.x). Conditional Compilation für API-Unterschiede. |
| Upstream video-compare ändert Architektur grundlegend | Niedrig | Hoch | Regelmäßig Upstream-Commits mergen. GUI-Code maximal von Core getrennt halten (eigenes `gui/`-Verzeichnis). |

---

## 6. Aufwandsschätzung

| Phase | Umfang | Geschätzter Aufwand |
|---|---|---|
| Phase 1: Fundament | CMake, Restrukturierung, ImGui-Einbindung | 2 Wochen |
| Phase 2: Launcher | Dateiauswahl, Optionen, Recent Files | 2 Wochen |
| Phase 3: In-Video-GUI | Toolbar, Einstellungen, Auto-Hide | 3 Wochen |
| Phase 4: Neue Modi | Flicker, Heatmap | 3 Wochen |
| Phase 5: Scopes & Metriken | ImGui-Scopes, VMAF/SSIM-Graphen | 3 Wochen |
| Phase 6: Distribution | Homebrew, AppImage, CI/CD | 2 Wochen |
| **Gesamt** | | **~15 Wochen** |

Die Phasen 4 und 5 können parallelisiert werden, wenn mehrere Entwickler beteiligt sind. Phase 1–3 bilden ein nutzbares MVP (Minimum Viable Product) nach ca. 7 Wochen.

---

## 7. Lizenzhinweise

- **video-compare:** GPL-2.0-only — der Fork muss unter der gleichen Lizenz stehen.
- **Dear ImGui:** MIT — kompatibel mit GPL-2.0.
- **portable-file-dialogs:** WTFPL — kompatibel mit GPL-2.0.
- **nlohmann/json (falls verwendet):** MIT — kompatibel mit GPL-2.0.
- **FFmpeg-Libraries:** LGPL 2.1+ (Standard-Build) — kompatibel, solange dynamisch gelinkt. Bei statischem Linking oder Nutzung von GPL-lizenzierten FFmpeg-Komponenten (z.B. libx264) wird das Gesamtprojekt ohnehin GPL.

---

## 8. Offene Entscheidungen

Folgende Punkte sollten vor Beginn der Implementierung geklärt werden:

1. **Projektname:** `video-compare-gui` als Arbeitstitel, oder ein eigenständiger Name?
2. **SDL2 vs. SDL3:** SDL3 ist seit 2024 stabil. Migration jetzt oder später? ImGui hat bereits `imgui_impl_sdl3`-Backends. video-compare upstream nutzt aktuell SDL2.
3. **C++14 vs. C++17:** video-compare nutzt C++14. Für `std::filesystem` (Config-Pfade) und `std::optional` wäre C++17 sinnvoll.
4. **ImGui-Renderer-Backend:** Start mit `sdlrenderer2` (einfacher) oder direkt `opengl3` (performanter)? Empfehlung: Start mit `sdlrenderer2`, Migration zu OpenGL nur wenn Performance-Probleme auftreten.
5. **Upstream-Beziehung:** Soll der Fork regelmäßig Upstream-Commits mergen, oder sich langfristig als eigenständiges Projekt entwickeln?
