# 建置與使用指南 / Build and Usage Guide

本文件使用繁體中文與英文說明如何安裝工具、建置、啟動及排查問題。/ This guide explains tool installation, building, launching, and troubleshooting in Traditional Chinese and English.

## 1. 準備工具 / Install prerequisites

### 繁體中文

需要 Windows 10/11 x64、Git、CMake 3.24 或更新版本，以及以下其中一套 C++ 編譯器：

- **MSVC（建議）**：安裝 Visual Studio 2022，勾選「使用 C++ 的桌面開發」與 Windows 10/11 SDK。
- **MinGW-w64**：安裝 WinLibs 或 MSYS2 的 x64 GCC，並確認 `g++`、`gcc`、`mingw32-make` 在 PATH 中。

第一次設定 CMake 時，FetchContent 會下載 Dear ImGui 與 nlohmann/json，因此需要網路連線。EXE 執行時不需要安裝這兩個套件。

### English

Use Windows 10/11 x64, Git, CMake 3.24 or later, and one of these C++ toolchains:

- **MSVC (recommended):** Install Visual Studio 2022 with the **Desktop development with C++** workload and a Windows 10/11 SDK.
- **MinGW-w64:** Install x64 GCC through WinLibs or MSYS2, then make sure `g++`, `gcc`, and `mingw32-make` are on `PATH`.

The first CMake configure downloads Dear ImGui and nlohmann/json through FetchContent, so an internet connection is required during setup. The built executable does not need these packages installed separately.

## 2. 取得原始碼 / Get the source

在 PowerShell 開啟專案根目錄，也就是有 `CMakeLists.txt` 的資料夾。若從 GitHub 下載：/ Open the project root in PowerShell (the folder containing `CMakeLists.txt`). To clone from GitHub:

```powershell
git clone <repository-url> MiniWorldEsp
cd MiniWorldEsp
```

若已下載 ZIP，請先解壓縮，再 `cd` 到 `MiniWorldEsp` 目錄。/ If you downloaded a ZIP, extract it and `cd` into the `MiniWorldEsp` directory.

## 3. 使用 MSVC 編譯 / Build with MSVC

### 繁體中文

可在一般 PowerShell 執行；CMake 會呼叫已安裝的 Visual Studio 編譯工具。第一次設定建立 `build-msvc`，發佈 EXE 固定輸出到 `dist`：

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release --parallel
```

成功訊息應包含 `Built target mcp_esp_overlay`。最終檔案是 `dist\mcp_esp_overlay.exe`。如要完全重新編譯，可刪除 `build-msvc` 後重跑兩個指令；不要刪除 `src`、`assets` 或 `schemas`。

### English

Run these commands from a regular PowerShell window. CMake locates the installed Visual Studio compiler. The build tree is `build-msvc`; the release executable is always written to `dist`:

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release --parallel
```

Look for `Built target mcp_esp_overlay`. The final file is `dist\mcp_esp_overlay.exe`. For a clean rebuild, remove `build-msvc` and run both commands again. Do not remove `src`, `assets`, or `schemas`.

## 4. 使用 MinGW-w64 編譯 / Build with MinGW-w64

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw --parallel
```

若 CMake 找不到工具，請重新開啟終端，並檢查 `$env:PATH` 包含 MinGW 的 `bin` 目錄。/ If CMake cannot find the compiler, reopen the terminal and check that `$env:PATH` contains the MinGW `bin` directory.

## 5. 啟動程式 / Launch the application

從專案根目錄啟動：/ Launch from the project root:

```powershell
.\dist\mcp_esp_overlay.exe --demo
```

`--demo` 使用標示為合成資料的離線範例。即時讀取相容版本的遊戲進程：/ `--demo` uses an offline synthetic snapshot. To read a compatible running game process:

```powershell
.\dist\mcp_esp_overlay.exe --live
```

也可以指定符合 schema 的 JSON 快照：/ Or supply a JSON snapshot conforming to the schema:

```powershell
.\dist\mcp_esp_overlay.exe --data .\sample_data.json
```

鍵盤操作：`Insert` 顯示／隱藏控制面板；`End` 離開。拖曳控制面板標題列或雷達標題列可以移動位置，位置會限制在遊戲視窗範圍內。Settings 分頁可切換繁體中文／英文與深色／白藍主題，選擇會保存在 EXE 旁的 `mcp-esp-ui.ini`。Entities 分頁的「目標數量」可調文字大小與螢幕 X/Y 位置，並有黑色描邊。即時模式每個繪圖影格更新相機矩陣，減少快速轉動時的方框延遲。/ Controls: press `Insert` to show or hide the panel and `End` to exit. Drag the panel title bar or radar title bar to move it; both stay inside the game window. The Settings tab switches between Traditional Chinese / English and Dark / White + Blue themes. Choices are saved in `mcp-esp-ui.ini` next to the executable. In Entities, enable Entity count to adjust its font size and screen X/Y position; the text has a black outline. Live mode refreshes camera matrices every render frame to reduce box lag during fast turns.

## 6. 執行時檔案 / Runtime files

| 檔案 / File | 說明 / Purpose |
|---|---|
| `dist/mcp_esp_overlay.exe` | [下載 Windows 執行檔 / Download the Windows executable](https://raw.githubusercontent.com/infycat/MiniWorldEsp/main/dist/mcp_esp_overlay.exe)，包含 Windows 圖標。/ Final app with embedded Windows icon. |
| `dist/mcp-esp-ui.ini` | 語言與主題選擇；執行時自動建立。/ Language and theme preferences; created at runtime. |
| `mcp-esp.ini` | Dear ImGui 視窗位置與版面設定。/ Dear ImGui window positions and layout. |
| `logs/esp.log` | 限頻資料來源、讀取及繪圖診斷。/ Rate-limited source, read, and drawing diagnostics. |
| `sample_data*.json` | 離線示範或 JSON 輸入範例。/ Offline demo or JSON input samples. |

主題偏好、ImGui 狀態和日誌均屬於本機執行資料，不應提交到 GitHub。`dist` 內只應保留最終 EXE。/ Theme preferences, ImGui state, and logs are local runtime data and should not be committed. Keep only the final executable in `dist`.

## 7. 常見問題 / Troubleshooting

### CMake 找不到編譯器 / CMake cannot find a compiler

關閉並重新開啟終端。MSVC 請確認 Visual Studio C++ workload 已安裝；MinGW 請確認 `g++ --version` 和 `mingw32-make --version` 可執行。/ Reopen the terminal. For MSVC, verify the C++ workload is installed. For MinGW, verify `g++ --version` and `mingw32-make --version` work.

### 第一次設定無法下載依賴 / First configure cannot download dependencies

確認網路與 GitHub 連線可用，再重新執行 CMake configure。若 FetchContent 下載到一半失敗，可刪除對應 `build-*` 目錄後重試。/ Check network access to the dependency hosts and rerun CMake configure. If FetchContent was interrupted, remove the affected `build-*` directory and retry.

### 即時模式顯示沒有資料 / Live mode reports no data

確認遊戲視窗可見且正在執行，並查看 GUI 的 Source、process、matrix 與 draw diagnostics。讀取器依賴開發時驗證的遊戲版本；遊戲更新可能改變記憶體佈局。可先用 `--demo` 驗證視窗和繪圖管線。/ Confirm the game window is visible and running, then inspect Source, process, matrix, and draw diagnostics. The reader targets the game version validated during development; an update may change memory layout. Run `--demo` to test the window and rendering pipeline separately.

### 沒有即時骨架或隊伍分類 / Live skeletons or teams are missing

目前的唯讀進程來源沒有呼叫遊戲骨骼函數，也沒有完整取得隊伍規則。只有資料來源提供真實骨點／隊伍欄位時才能繪製骨架或準確分類；程式會保留未知狀態，不會虛構結果。/ The read-only process source does not call in-game bone functions and does not have complete team rules. Real bones and accurate team classification require those fields from the data source; missing values remain unknown rather than being fabricated.

## 8. 專案模組 / Project modules

| 路徑 / Path | 職責 / Responsibility |
|---|---|
| `src/esp/types.hpp` | 共用型別與設定 / Shared types and settings |
| `src/esp/data_source.*` | JSON source、輪詢器 / JSON source and poller |
| `src/esp/live_process_source.cpp` | Windows 唯讀進程讀取 / Read-only Windows process reader |
| `src/esp/projection.*` | 矩陣投影與裁切 / Matrix projection and clipping |
| `src/esp/draw_esp.*` | ESP、雷達、準星繪製 / ESP, radar, and crosshair drawing |
| `src/esp/overlay_app.*` | Win32、DX11、DirectComposition、ImGui / Win32, DX11, DirectComposition, and ImGui |
| `src/esp/diagnostics.*` | 日誌與診斷 / Logging and diagnostics |
| `schemas/world_snapshot.schema.json` | JSON 快照契約 / JSON snapshot contract |

完整設計說明：[`ARCHITECTURE.md`](ARCHITECTURE.md)。/ Full design notes: [`ARCHITECTURE.md`](ARCHITECTURE.md).
