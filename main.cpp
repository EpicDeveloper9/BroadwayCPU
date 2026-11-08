#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>
#include <cstdint>
#include <array>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "cpu.hpp"
#include "executable_loader.hpp"
#include "video_output.hpp"
#include "gx.hpp"
#include "exi.hpp"
#include "wii_boot.hpp"
#include "disc_image.hpp"
#include "wii_firmware.hpp"

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"BroadwayCPUWindow";
constexpr UINT_PTR kRunTimer = 1;
constexpr UINT_PTR kVideoDiagnosticTimer = 2;
constexpr size_t kMemorySize = 32 * 1024 * 1024;
constexpr uint32_t kRawLoadAddress = 0x80004000u;
constexpr UINT kVideoOutputCommand = 201;
constexpr UINT kOpenWiiIsoCommand = 110;
constexpr UINT kStopCommand = 111;
constexpr UINT kFirmwareCommand = 113;
constexpr UINT kVideoDiagnosticCommand = 202;

std::unique_ptr<Broadway::CPU> g_cpu;
std::unique_ptr<Broadway::GXProcessor> g_gx;
std::unique_ptr<Broadway::EXIController> g_exi;
class MmioHub : public Broadway::MemoryMappedDevice {
public:
    explicit MmioHub(Broadway::GXProcessor* gxDevice) : gx(gxDevice), storage(0x10000, 0) {}
    bool Handles(uint32_t address, unsigned widthBytes) const noexcept override {
        const uint32_t normalized = address >= 0xC0000000u ? address & 0x3FFFFFFFu : address;
        if (normalized >= 0x0C000000u && normalized < 0x0C010000u) return widthBytes == 1 || widthBytes == 2 || widthBytes == 4;
        return false;
    }
    uint32_t Read(uint32_t address, unsigned widthBytes) override {
        const uint32_t normalized = address >= 0xC0000000u ? address & 0x3FFFFFFFu : address;
        if (gx && gx->Handles(address, widthBytes)) return gx->Read(address, widthBytes);
        const uint32_t offset = normalized - 0x0C000000u;
        if (widthBytes == 1) return storage[offset];
        if (widthBytes == 2) return (storage[offset] << 8) | storage[offset + 1];
        return (storage[offset] << 24) | (storage[offset + 1] << 16) |
               (storage[offset + 2] << 8) | storage[offset + 3];
    }
    void Write(uint32_t address, unsigned widthBytes, uint32_t value) override {
        const uint32_t normalized = address >= 0xC0000000u ? address & 0x3FFFFFFFu : address;
        if (gx && gx->Handles(address, widthBytes)) { gx->Write(address, widthBytes, value); return; }
        const uint32_t offset = normalized - 0x0C000000u;
        if (widthBytes == 1) storage[offset] = uint8_t(value);
        else if (widthBytes == 2) { storage[offset] = uint8_t(value >> 8); storage[offset + 1] = uint8_t(value); }
        else {
            storage[offset] = uint8_t(value >> 24);
            storage[offset + 1] = uint8_t(value >> 16);
            storage[offset + 2] = uint8_t(value >> 8);
            storage[offset + 3] = uint8_t(value);
        }
    }
private:
    Broadway::GXProcessor* gx;
    std::vector<uint8_t> storage;
};
Broadway::VideoFrameMailbox g_videoFrames;
std::unique_ptr<Broadway::VideoOutputWindow> g_videoOutput;
HMENU g_viewMenu = nullptr;
std::unique_ptr<Broadway::WiiBootSession> g_isoBoot;
HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_runButton = nullptr;
HWND g_stepButton = nullptr;
HWND g_resetButton = nullptr;
HWND g_loadButton = nullptr;
HWND g_isoButton = nullptr;
HWND g_stopButton = nullptr;
HWND g_keyEdit = nullptr;
std::wstring g_loadedPath;
std::wstring g_isoPath;
Broadway::WiiFirmwareInventory g_firmwareInventory;
bool g_firmwareConfigured = false;
std::wstring g_statusText = L"Ready — open a Wii ISO or executable to begin.";
std::wstring g_lastError;
uint32_t g_entryPoint = 0;
bool g_loaded = false;
bool g_running = false;
bool g_isoSelected = false;
bool g_isoBooted = false;
bool g_videoDiagnosticEnabled = false;
uint64_t g_videoDiagnosticFrame = 0;
std::array<uint8_t,16> g_commonKey{};
bool g_haveCommonKey = false;
int g_selectedTab = 0;

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    out.resize(size_t(n - 1)); return out;
}
std::wstring Hex(uint32_t v) {
    wchar_t b[16]; swprintf_s(b, L"%08X", v); return b;
}
void SetStatus(const std::wstring& s) {
    g_statusText = s;
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
}
void StopRun() {
    g_running = false;
    if (g_window) KillTimer(g_window, kRunTimer);
    if (g_runButton) SetWindowTextW(g_runButton, L"Start");
}
void Refresh() { if (g_window) InvalidateRect(g_window, nullptr, FALSE); }
void OpenVideoOutput();
void ConfigureWiiSystemData(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Select an extracted Wii NAND root (folder containing title, ticket and shared1)";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browse);
    if (!selected) return;
    wchar_t path[MAX_PATH]{};
    const bool gotPath = SHGetPathFromIDListW(selected, path) != FALSE;
    CoTaskMemFree(selected);
    if (!gotPath) {
        SetStatus(L"Could not read the selected folder path.");
        return;
    }
    try {
        auto inventory = Broadway::WiiFirmwareInspector::InspectNand(std::filesystem::path(path));
        g_firmwareInventory = std::move(inventory);
        g_firmwareConfigured = true;
        const auto ios = g_firmwareInventory.requiredIosTitleId
            ? (L"Required IOS title 0x" + Hex(uint32_t(g_firmwareInventory.requiredIosTitleId)))
            : L"Required IOS unknown";
        SetStatus(L"Wii system data at " + std::wstring(path) + L": " + ToWide(g_firmwareInventory.status) + L"  |  " + ios +
                  L"  |  files are inventoried only; encrypted content is not verified or executed.");
    } catch (const std::exception& e) {
        g_lastError = ToWide(e.what());
        SetStatus(L"Wii system data inspection failed: " + g_lastError);
        MessageBoxW(owner, g_lastError.c_str(), L"BroadwayCPU — System data", MB_ICONERROR | MB_OK);
    }
}
bool ReadCommonKey(std::array<uint8_t,16>& key) {
    if (!g_keyEdit) return false;
    wchar_t text[64]{};
    GetWindowTextW(g_keyEdit, text, int(std::size(text)));
    if (wcslen(text) != 32) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        wchar_t pair[3]{text[i * 2], text[i * 2 + 1], 0};
        wchar_t* end = nullptr;
        const unsigned long value = wcstoul(pair, &end, 16);
        if (!end || *end != 0 || value > 255) return false;
        key[i] = uint8_t(value);
    }
    return true;
}
void OpenWiiIso(HWND owner) {
    wchar_t path[32768]{};
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Wii disc images (*.iso;*.gcm)\0*.iso;*.gcm\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path; ofn.nMaxFile = DWORD(std::size(path));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Open Wii ISO";
    if (!GetOpenFileNameW(&ofn)) return;
    try {
        Broadway::WiiDiscImage image{std::filesystem::path(path)};
        if (!image.Header().encrypted)
            throw std::runtime_error("The selected disc image is not an encrypted retail Wii disc.");
        size_t partition = image.Partitions().size();
        for (size_t i = 0; i < image.Partitions().size(); ++i)
            if (image.Partitions()[i].type == 0) { partition = i; break; }
        if (partition == image.Partitions().size())
            throw std::runtime_error("No Wii game data partition was found.");
        g_isoPath = path;
        g_loadedPath = g_isoPath;
        g_isoSelected = true;
        g_isoBooted = false;
        g_loaded = false;
        g_isoBoot.reset();
        StopRun();
        SetStatus(L"ISO opened: " + g_isoPath + L"  |  " + ToWide(image.Header().gameId) +
                  L" rev " + std::to_wstring(image.Header().revision) +
                  L"  |  game partition " + std::to_wstring(partition) +
                  L"  |  enter the 32-digit Wii common key, then Start.");
    } catch (const std::exception& e) {
        g_lastError = ToWide(e.what());
        SetStatus(L"ISO open failed: " + g_lastError);
        MessageBoxW(owner, g_lastError.c_str(), L"BroadwayCPU — ISO error", MB_ICONERROR | MB_OK);
    }
}
bool BootWiiIso(bool startRunning) {
    if (!g_isoSelected) return false;
    std::array<uint8_t,16> key{};
    if (!ReadCommonKey(key) && !g_haveCommonKey) {
        SetStatus(L"Enter the 32-digit Wii common key to decrypt this retail partition.");
        return false;
    }
    if (ReadCommonKey(key)) { g_commonKey = key; g_haveCommonKey = true; }
    StopRun();
    if (g_firmwareConfigured) {
        SetStatus(L"Selected firmware files are inventory-only; Starlet/IOS/System Menu boot is not implemented. "
                  L"Running the isolated apploader diagnostic.");
    } else {
        SetStatus(L"Decrypting the game partition and executing its apploader diagnostic...");
    }
    UpdateWindow(g_window);
    try {
        OpenVideoOutput();
        g_isoBoot = std::make_unique<Broadway::WiiBootSession>(std::filesystem::path(g_isoPath), g_commonKey);
        g_entryPoint = g_isoBoot->BootInformation().apploaderEntry;
        g_loaded = true;
        g_loadedPath = g_isoPath;
        const auto result = g_isoBoot->Boot(*g_cpu, *g_gx);
        g_entryPoint = result.gameEntry;
        g_loaded = true;
        g_isoBooted = true;
        g_loadedPath = g_isoPath;
        SetStatus(L"Apploader complete; DOL entry 0x" + Hex(g_entryPoint) +
                  L"  |  " + std::to_wstring(result.discReadRequests) + L" disc reads, " +
                  std::to_wstring(result.discBytesLoaded) + L" bytes loaded, " +
                  std::to_wstring(result.instructionsExecuted) + L" apploader instructions.");
        if (startRunning) {
            g_running = true;
            if (g_runButton) SetWindowTextW(g_runButton, L"Pause");
            SetTimer(g_window, kRunTimer, 15, nullptr);
            SetStatus(L"Running RGHP52 from apploader entry 0x" + Hex(g_entryPoint) + L"...");
        }
        Refresh();
        return true;
    } catch (const Broadway::CPUException& e) {
        StopRun();
        uint32_t raw = 0; try { raw = g_cpu->memory.Read32(e.FaultPC()); } catch (...) {}
        g_lastError = ToWide(e.what());
        SetStatus(L"Wii boot CPU exception at 0x" + Hex(e.FaultPC()) + L" raw 0x" + Hex(raw) +
                  L" (" + ToWide(Broadway::Decoder::Format(Broadway::Decoder::Decode(raw),e.FaultPC())) + L"): " + g_lastError);
        Refresh();
        return false;
    } catch (const std::exception& e) {
        StopRun();
        g_lastError = ToWide(e.what());
        SetStatus(L"Wii boot stopped: " + g_lastError + L"  |  PC 0x" + Hex(g_cpu->registers.PC));
        Refresh();
        return false;
    }
}
void LoadExecutable(HWND owner) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Wii executables (*.dol;*.elf;*.bin)\0*.dol;*.elf;*.bin\0DOL files (*.dol)\0*.dol\0ELF files (*.elf)\0*.elf\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Load a Broadway executable";
    if (!GetOpenFileNameW(&ofn)) return;
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Could not open the selected file.");
        std::vector<uint8_t> bytes;
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (bytes.empty()) throw std::runtime_error("The selected file is empty.");
        std::wstring ext(path); const size_t dot = ext.find_last_of(L'.');
        if (dot != std::wstring::npos) ext = ext.substr(dot);
        for (auto& c : ext) c = wchar_t(towlower(c));
        Broadway::ExecutableFormat format = Broadway::ExecutableFormat::Raw;
        if (ext == L".dol") format = Broadway::ExecutableFormat::Dol;
        else if (ext == L".elf" || (bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E'))
            format = Broadway::ExecutableFormat::Elf;
        const auto loadResult = Broadway::ExecutableLoader::Load(*g_cpu, bytes, format, kRawLoadAddress);
        g_gx->Reset();
        g_exi->Reset();
        g_entryPoint = loadResult.entryPoint;
        g_cpu->registers.PC = g_entryPoint; g_loaded = true; g_loadedPath = path;
        g_isoSelected = false; g_isoBooted = false; g_isoBoot.reset();
        StopRun();
        SetStatus(L"Loaded " + std::wstring(path) + L"  |  Entry: 0x" + Hex(g_entryPoint));
    } catch (const std::exception& e) {
        g_lastError = ToWide(e.what());
        SetStatus(L"Load failed: " + g_lastError);
        MessageBoxW(owner, g_lastError.c_str(), L"BroadwayCPU — Load error", MB_ICONERROR | MB_OK);
    }
}
void ExecuteStep() {
    if (!g_loaded) { SetStatus(L"Load a DOL, ELF or raw binary first."); return; }
    try { g_cpu->Step(); SetStatus(L"Executed instruction " + std::to_wstring(g_cpu->InstructionsExecuted()) + L"  |  PC: 0x" + Hex(g_cpu->registers.PC)); }
    catch (const std::exception& e) { StopRun(); g_lastError = ToWide(e.what()); SetStatus(L"CPU exception: " + g_lastError); }
    Refresh();
}
void ToggleRun() {
    if (!g_loaded) { SetStatus(L"Load an executable before starting emulation."); return; }
    if (g_running) { StopRun(); SetStatus(L"Paused at 0x" + Hex(g_cpu->registers.PC)); }
    else { g_running = true; SetWindowTextW(g_runButton, L"Pause"); SetTimer(g_window, kRunTimer, 15, nullptr); SetStatus(L"Running..."); }
}
void ResetCPU() {
    StopRun(); if (!g_loaded) { SetStatus(L"Nothing is loaded."); return; }
    if (g_isoSelected) {
        g_isoBooted = false;
        BootWiiIso(false);
        return;
    }
    g_cpu->Reset(); g_gx->Reset(); g_exi->Reset(); g_cpu->registers.PC = g_entryPoint;
    SetStatus(L"CPU reset to entry point 0x" + Hex(g_entryPoint)); Refresh();
}
void OpenVideoOutput() {
    try {
        if (!g_videoOutput)
            g_videoOutput = std::make_unique<Broadway::VideoOutputWindow>(g_instance, g_videoFrames);
        g_videoOutput->Show();
        g_videoOutput->SetDiagnosticMode(g_videoDiagnosticEnabled);
    } catch (const std::exception& e) {
        const std::wstring message = ToWide(e.what());
        MessageBoxW(g_window, message.c_str(), L"BroadwayCPU — Video Output", MB_ICONERROR | MB_OK);
    }
}

void ToggleVideoDiagnostic() {
    if (!g_videoDiagnosticEnabled) {
        OpenVideoOutput();
        if (!g_videoOutput || !g_videoOutput->IsOpen()) return;
        g_videoDiagnosticEnabled = true;
        g_videoDiagnosticFrame = 0;
        g_videoOutput->SetDiagnosticMode(true);
        g_videoOutput->ClearFrame();
        g_videoFrames.Publish(Broadway::CreateDiagnosticVideoFrame(640, 480, g_videoDiagnosticFrame++));
        if (!SetTimer(g_window, kVideoDiagnosticTimer, 33, nullptr)) {
            g_videoDiagnosticEnabled = false;
            g_videoOutput->ClearFrame();
            g_videoOutput->SetDiagnosticMode(false);
            SetStatus(L"Could not start the video diagnostic update timer.");
            return;
        }
        if (g_viewMenu)
            CheckMenuItem(g_viewMenu, kVideoDiagnosticCommand, MF_BYCOMMAND | MF_CHECKED);
        SetStatus(L"Video Output diagnostic pattern enabled - generated test pixels, not Wii output.");
        return;
    }

    g_videoDiagnosticEnabled = false;
    KillTimer(g_window, kVideoDiagnosticTimer);
    g_videoFrames.Clear();
    if (g_videoOutput) {
        g_videoOutput->ClearFrame();
        g_videoOutput->SetDiagnosticMode(false);
    }
    if (g_viewMenu)
        CheckMenuItem(g_viewMenu, kVideoDiagnosticCommand, MF_BYCOMMAND | MF_UNCHECKED);
    SetStatus(L"Video Output diagnostic pattern disabled; genuine GX frames may display when produced.");
}
void DrawTextLine(HDC dc, int x, int y, const std::wstring& s, COLORREF color, HFONT font) {
    SetTextColor(dc, color); SelectObject(dc, font); TextOutW(dc, x, y, s.c_str(), int(s.size()));
}
void DrawPanel(HDC dc, RECT r, const wchar_t* title) {
    HBRUSH bg = CreateSolidBrush(RGB(29, 33, 41)); FillRect(dc, &r, bg); DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(53, 61, 73)); HGDIOBJ old = SelectObject(dc, pen);
    Rectangle(dc, r.left, r.top, r.right, r.bottom); SelectObject(dc, old); DeleteObject(pen);
    RECT h{r.left + 1, r.top + 1, r.right - 1, r.top + 31};
    HBRUSH hb = CreateSolidBrush(RGB(38, 45, 56)); FillRect(dc, &h, hb); DeleteObject(hb);
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontW(16,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    DrawTextLine(dc, r.left + 12, r.top + 7, title, RGB(226,232,240), f); DeleteObject(f);
}
void PaintMain(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
    RECT client{}; GetClientRect(hwnd, &client);
    HBRUSH bg = CreateSolidBrush(RGB(20, 23, 29)); FillRect(dc, &client, bg); DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    HFONT mono = CreateFontW(15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    HFONT title = CreateFontW(23,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    DrawTextLine(dc, 22, 16, L"BroadwayCPU", RGB(239,244,250), title);
    DrawTextLine(dc, 24, 48, L"IBM Broadway / PowerPC debugger", RGB(133,149,170), mono);
    const int top = 92, bottom = client.bottom - 52, gap = 12;
    const int leftW = (client.right - 48) / 2;
    RECT regs{16,top,16+leftW,bottom}; RECT dis{28+leftW,top,client.right-16,bottom};
    DrawPanel(dc, regs, L"CPU REGISTERS"); DrawPanel(dc, dis, L"DISASSEMBLY / MEMORY");
    int y = top + 43;
    if (g_loaded) {
        DrawTextLine(dc, regs.left+14,y,g_selectedTab==0?L"GENERAL PURPOSE REGISTERS  (View > Floating-point registers)":L"FLOATING-POINT REGISTERS  (View > General-purpose registers)",RGB(106,190,255),mono); y+=25;
        for (unsigned i=0;i<32;i+=2) {
            wchar_t line[112];
            if (g_selectedTab==0) swprintf_s(line,L"r%-2u  %08X      r%-2u  %08X",i,g_cpu->registers.GPR[i],i+1,g_cpu->registers.GPR[i+1]);
            else swprintf_s(line,L"f%-2u  % .9g    f%-2u  % .9g",i,g_cpu->registers.FPR[i],i+1,g_cpu->registers.FPR[i+1]);
            DrawTextLine(dc,regs.left+14,y,line,RGB(205,214,226),mono); y+=22;
        }
        y += 8;
        wchar_t special[160]; swprintf_s(special,L"PC   %08X    LR   %08X",g_cpu->registers.PC,g_cpu->registers.LR);
        DrawTextLine(dc,regs.left+14,y,special,RGB(255,203,112),mono); y+=22;
        swprintf_s(special,L"CTR  %08X    XER  %08X",g_cpu->registers.CTR,g_cpu->registers.XER);
        DrawTextLine(dc,regs.left+14,y,special,RGB(205,214,226),mono); y+=22;
        swprintf_s(special,L"CR   %08X    MSR  %08X",g_cpu->registers.CR,g_cpu->registers.MSR);
        DrawTextLine(dc,regs.left+14,y,special,RGB(205,214,226),mono); y+=22;
        swprintf_s(special,L"FPSCR %08X   Executed %llu",g_cpu->registers.FPSCR,(unsigned long long)g_cpu->InstructionsExecuted());
        DrawTextLine(dc,regs.left+14,y,special,RGB(205,214,226),mono);
        const int dx=dis.left+14; y=dis.top+43;
        DrawTextLine(dc,dx,y,L"ADDRESS   RAW       DECODE",RGB(106,190,255),mono); y+=25;
        uint32_t start=g_cpu->registers.PC;
        for (int i=-3;i<19;++i) {
            uint32_t addr=start+uint32_t(i*4); uint32_t raw=0; const char* name="(unmapped)";
            try { raw=g_cpu->memory.Read32(addr); name=Broadway::Decoder::Name(Broadway::Decoder::Decode(raw)); } catch (...) {}
            wchar_t line[128]; swprintf_s(line,L"%08X  %08X  %S",addr,raw,name);
            DrawTextLine(dc,dx,y,line,addr==start?RGB(255,205,112):RGB(205,214,226),mono); y+=21;
            if (y>dis.bottom-145) break;
        }
        y=dis.bottom-42;
        DrawTextLine(dc,dx,y,L"MEMORY  (big-endian)  |  64 bytes at PC",RGB(106,190,255),mono); y+=21;
        for (int row=0;row<4 && y<dis.bottom-4;++row) {
            wchar_t line[160]{}; uint32_t addr=start+uint32_t(row*16); swprintf_s(line,L"%08X  ",addr);
            size_t len=wcslen(line);
            for(int col=0;col<16;++col){ wchar_t b[4]; try{swprintf_s(b,L"%02X ",g_cpu->memory.Read8(addr+uint32_t(row*16+col)));}catch(...){wcscpy_s(b,L"?? ");} wcscat_s(line,b); }
            DrawTextLine(dc,dx,y,line,RGB(163,177,196),mono); y+=18;
        }
    } else {
        DrawTextLine(dc,regs.left+16,regs.top+58,L"No executable loaded.",RGB(177,190,207),mono);
        DrawTextLine(dc,regs.left+16,regs.top+88,L"Use File > Load Executable to open a Wii DOL or ELF.",RGB(130,148,170),mono);
        DrawTextLine(dc,dis.left+16,dis.top+58,L"The disassembly and memory views will appear here.",RGB(177,190,207),mono);
        DrawTextLine(dc,dis.left+16,dis.top+88,L"You can also load a raw binary at 0x80004000.",RGB(130,148,170),mono);
    }
    RECT footer{0,client.bottom-34,client.right,client.bottom}; HBRUSH fb=CreateSolidBrush(RGB(27,32,40)); FillRect(dc,&footer,fb); DeleteObject(fb);
    HFONT sf=CreateFontW(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    DrawTextLine(dc,18,client.bottom-27,g_statusText,RGB(161,178,199),sf); DeleteObject(sf);
    DeleteObject(mono); DeleteObject(title); EndPaint(hwnd,&ps);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        g_window=hwnd;
        g_loadButton=CreateWindowW(L"BUTTON",L"Load executable",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,18,62,142,27,hwnd,(HMENU)100,nullptr,nullptr);
        g_isoButton=CreateWindowW(L"BUTTON",L"Open Wii ISO...",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,168,62,130,27,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenWiiIsoCommand)),nullptr,nullptr);
        g_runButton=CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,306,62,76,27,hwnd,(HMENU)101,nullptr,nullptr);
        g_stopButton=CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,390,62,76,27,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopCommand)),nullptr,nullptr);
        g_stepButton=CreateWindowW(L"BUTTON",L"Step",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,474,62,68,27,hwnd,(HMENU)102,nullptr,nullptr);
        g_resetButton=CreateWindowW(L"BUTTON",L"Reset",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,550,62,68,27,hwnd,(HMENU)103,nullptr,nullptr);
        CreateWindowW(L"STATIC",L"Wii common key",WS_CHILD|WS_VISIBLE,632,68,100,20,hwnd,nullptr,nullptr,nullptr);
        g_keyEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_PASSWORD|ES_AUTOHSCROLL,734,63,260,25,hwnd,(HMENU)112,nullptr,nullptr);
        return 0;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)){case 100:LoadExecutable(hwnd);break;case kOpenWiiIsoCommand:OpenWiiIso(hwnd);break;case kFirmwareCommand:ConfigureWiiSystemData(hwnd);break;case 101:if(g_isoSelected&&!g_isoBooted)BootWiiIso(true);else ToggleRun();break;case kStopCommand:StopRun();SetStatus(L"Stopped at 0x"+Hex(g_cpu->registers.PC));break;case 102:if(!g_running)ExecuteStep();break;case 103:ResetCPU();break;case 105:DestroyWindow(hwnd);break;case 200:g_selectedTab=1-g_selectedTab;Refresh();break;case kVideoOutputCommand:OpenVideoOutput();break;case kVideoDiagnosticCommand:ToggleVideoDiagnostic();break;}
        return 0;
    case WM_TIMER:
        if(wp==kVideoDiagnosticTimer && g_videoDiagnosticEnabled) {
            g_videoFrames.Publish(Broadway::CreateDiagnosticVideoFrame(640, 480, g_videoDiagnosticFrame++));
            return 0;
        }
        if(wp==kRunTimer && g_running) {
            try { g_cpu->Run(1000); if(g_cpu->halted){StopRun();SetStatus(L"CPU halted.");} else if(g_cpu->InstructionsExecuted()%10000<1000) SetStatus(L"Running — " + std::to_wstring(g_cpu->InstructionsExecuted()) + L" instructions"); }
            catch(const Broadway::CPUException& e){StopRun();g_lastError=ToWide(e.what());uint32_t raw=0;try{raw=g_cpu->memory.Read32(e.FaultPC());}catch(...){}SetStatus(L"CPU stopped at 0x"+Hex(e.FaultPC())+L" raw 0x"+Hex(raw)+L": "+g_lastError);}
            catch(const std::exception& e){StopRun();g_lastError=ToWide(e.what());SetStatus(L"CPU exception: "+g_lastError);}
            Refresh();
        }
        return 0;
    case WM_PAINT: PaintMain(hwnd); return 0;
    case WM_SIZE: InvalidateRect(hwnd,nullptr,TRUE); return 0;
    case WM_DESTROY: StopRun(); KillTimer(hwnd,kVideoDiagnosticTimer); g_videoDiagnosticEnabled=false; g_videoOutput.reset(); if(g_cpu) g_cpu->memory.AttachMmioDevice(nullptr); g_exi.reset(); g_gx.reset(); g_videoFrames.Clear(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    try {
        g_cpu=std::make_unique<Broadway::CPU>(kMemorySize);
        g_gx=std::make_unique<Broadway::GXProcessor>(g_cpu->memory,g_videoFrames);
        g_exi=std::make_unique<Broadway::EXIController>();
        g_cpu->memory.AttachMmioDevice(g_gx.get());
        g_cpu->memory.AddMmioDevice(g_exi.get());
    }
    catch (...) { MessageBoxW(nullptr,L"Could not initialize the Broadway CPU.",L"BroadwayCPU",MB_ICONERROR); return 1; }
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WindowProc;
    wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName=kClassName; wc.hIcon=LoadIconW(nullptr,MAKEINTRESOURCEW(32512)); RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(0,kClassName,L"BroadwayCPU — Broadway Processor Workbench",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1180,820,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 2;
    HMENU menu=CreateMenu(); HMENU file=CreatePopupMenu(); AppendMenuW(file,MF_STRING,kOpenWiiIsoCommand,L"Open Wii ISO...\tCtrl+Shift+O"); AppendMenuW(file,MF_STRING,100,L"Load executable...\tCtrl+O"); AppendMenuW(file,MF_SEPARATOR,0,nullptr); AppendMenuW(file,MF_STRING,105,L"Exit"); AppendMenuW(menu,MF_POPUP,(UINT_PTR)file,L"File");
    g_viewMenu=CreatePopupMenu(); AppendMenuW(g_viewMenu,MF_STRING,200,L"Toggle GPR / FPR view"); AppendMenuW(g_viewMenu,MF_SEPARATOR,0,nullptr); AppendMenuW(g_viewMenu,MF_STRING,kVideoOutputCommand,L"Video Output\tCtrl+Shift+V"); AppendMenuW(g_viewMenu,MF_STRING,kVideoDiagnosticCommand,L"Enable video output diagnostic pattern\tCtrl+Shift+D"); AppendMenuW(menu,MF_POPUP,(UINT_PTR)g_viewMenu,L"View");
    HMENU emu=CreatePopupMenu(); AppendMenuW(emu,MF_STRING,101,L"Start / Pause\tF5"); AppendMenuW(emu,MF_STRING,kStopCommand,L"Stop"); AppendMenuW(emu,MF_STRING,102,L"Single step\tF10"); AppendMenuW(emu,MF_STRING,103,L"Reset\tCtrl+R"); AppendMenuW(menu,MF_POPUP,(UINT_PTR)emu,L"CPU");
    HMENU tools=CreatePopupMenu(); AppendMenuW(tools,MF_STRING,kFirmwareCommand,L"Configure Wii system data..."); AppendMenuW(menu,MF_POPUP,(UINT_PTR)tools,L"Tools"); SetMenu(hwnd,menu);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0){
        if(msg.message==WM_KEYDOWN){if((GetKeyState(VK_CONTROL)&0x8000)&&(GetKeyState(VK_SHIFT)&0x8000)&&msg.wParam=='V')OpenVideoOutput();else if((GetKeyState(VK_CONTROL)&0x8000)&&(GetKeyState(VK_SHIFT)&0x8000)&&msg.wParam=='D')ToggleVideoDiagnostic();else if((GetKeyState(VK_CONTROL)&0x8000)&&(GetKeyState(VK_SHIFT)&0x8000)&&msg.wParam=='O')OpenWiiIso(hwnd);else if((GetKeyState(VK_CONTROL)&0x8000)&&msg.wParam=='O')LoadExecutable(hwnd);else if(msg.wParam==VK_F5){if(g_isoSelected&&!g_isoBooted)BootWiiIso(true);else ToggleRun();}else if(msg.wParam==VK_F10&&!g_running)ExecuteStep();else if((GetKeyState(VK_CONTROL)&0x8000)&&msg.wParam=='R')ResetCPU();}
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    g_videoOutput.reset();
    return int(msg.wParam);
}
