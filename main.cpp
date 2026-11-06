#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <windowsx.h>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "cpu.hpp"

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"BroadwayCPUWindow";
constexpr UINT_PTR kRunTimer = 1;
constexpr size_t kMemorySize = 32 * 1024 * 1024;
constexpr uint32_t kRawLoadAddress = 0x80004000u;

std::unique_ptr<Broadway::CPU> g_cpu;
HWND g_window = nullptr;
HWND g_runButton = nullptr;
HWND g_stepButton = nullptr;
HWND g_resetButton = nullptr;
HWND g_loadButton = nullptr;
std::wstring g_loadedPath;
std::wstring g_statusText = L"Ready — open a DOL, ELF or raw binary to begin.";
std::wstring g_lastError;
uint32_t g_entryPoint = 0;
bool g_loaded = false;
bool g_running = false;
int g_selectedTab = 0;

uint32_t BE32(const std::vector<uint8_t>& b, size_t o) {
    if (o + 4 > b.size()) throw std::runtime_error("File is truncated.");
    return (uint32_t(b[o]) << 24) | (uint32_t(b[o+1]) << 16) | (uint32_t(b[o+2]) << 8) | b[o+3];
}
uint16_t BE16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("File is truncated.");
    return uint16_t((uint16_t(b[o]) << 8) | b[o+1]);
}
void WriteSegment(Broadway::CPU& cpu, const std::vector<uint8_t>& bytes, uint32_t address, uint32_t offset, uint32_t size) {
    if (uint64_t(offset) + size > bytes.size()) throw std::runtime_error("Executable section extends past end of file.");
    for (uint32_t i = 0; i < size; ++i) cpu.memory.Write8(address + i, bytes[offset + i]);
}
uint32_t LoadDol(Broadway::CPU& cpu, const std::vector<uint8_t>& b) {
    if (b.size() < 0x100) throw std::runtime_error("DOL header is incomplete.");
    bool loadedAny = false;
    for (unsigned i = 0; i < 18; ++i) {
        const uint32_t off = BE32(b, i * 4);
        const uint32_t addr = BE32(b, 0x48 + i * 4);
        const uint32_t size = BE32(b, 0x90 + i * 4);
        if (size) { WriteSegment(cpu, b, addr, off, size); loadedAny = true; }
    }
    if (!loadedAny) throw std::runtime_error("No loadable DOL sections were found.");
    const uint32_t bssAddr = BE32(b, 0xD8), bssSize = BE32(b, 0xDC);
    for (uint32_t i = 0; i < bssSize; ++i) cpu.memory.Write8(bssAddr + i, 0);
    return BE32(b, 0xE0);
}
uint32_t LoadElf(Broadway::CPU& cpu, const std::vector<uint8_t>& b) {
    if (b.size() < 52 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
        throw std::runtime_error("Not a valid ELF file.");
    if (b[4] != 1 || b[5] != 2) throw std::runtime_error("Only 32-bit big-endian ELF files are supported.");
    const uint32_t entry = BE32(b, 24), phoff = BE32(b, 28);
    const uint16_t phentsize = BE16(b, 42), phnum = BE16(b, 44);
    if (phentsize < 32) throw std::runtime_error("Invalid ELF program-header size.");
    bool loadedAny = false;
    for (unsigned i = 0; i < phnum; ++i) {
        const size_t p = size_t(phoff) + size_t(i) * phentsize;
        if (p + 32 > b.size()) throw std::runtime_error("ELF program headers are truncated.");
        if (BE32(b, p) != 1) continue; // PT_LOAD
        const uint32_t offset = BE32(b, p + 4), vaddr = BE32(b, p + 8);
        const uint32_t filesz = BE32(b, p + 16), memsz = BE32(b, p + 20);
        if (filesz) WriteSegment(cpu, b, vaddr, offset, filesz);
        if (memsz > filesz) for (uint32_t n = filesz; n < memsz; ++n) cpu.memory.Write8(vaddr + n, 0);
        loadedAny = loadedAny || memsz != 0;
    }
    if (!loadedAny) throw std::runtime_error("ELF contains no loadable segments.");
    return entry;
}
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
    if (g_runButton) SetWindowTextW(g_runButton, L"Run");
}
void Refresh() { if (g_window) InvalidateRect(g_window, nullptr, FALSE); }
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
        g_cpu->memory.Clear(); g_cpu->Reset();
        std::wstring ext(path); const size_t dot = ext.find_last_of(L'.');
        if (dot != std::wstring::npos) ext = ext.substr(dot);
        for (auto& c : ext) c = wchar_t(towlower(c));
        if (ext == L".dol") g_entryPoint = LoadDol(*g_cpu, bytes);
        else if (ext == L".elf" || (bytes.size() > 4 && bytes[0] == 0x7F && bytes[1] == 'E')) g_entryPoint = LoadElf(*g_cpu, bytes);
        else { WriteSegment(*g_cpu, bytes, kRawLoadAddress, 0, uint32_t(bytes.size())); g_entryPoint = kRawLoadAddress; }
        g_cpu->registers.PC = g_entryPoint; g_loaded = true; g_loadedPath = path;
        StopRun();
        SetStatus(L"Loaded " + std::wstring(path) + L"  |  Entry: 0x" + Hex(g_entryPoint));
    } catch (const std::exception& e) {
        g_loaded = false; g_lastError = ToWide(e.what());
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
    g_cpu->Reset(); g_cpu->registers.PC = g_entryPoint;
    SetStatus(L"CPU reset to entry point 0x" + Hex(g_entryPoint)); Refresh();
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
        g_runButton=CreateWindowW(L"BUTTON",L"Run",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,168,62,76,27,hwnd,(HMENU)101,nullptr,nullptr);
        g_stepButton=CreateWindowW(L"BUTTON",L"Step",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,252,62,76,27,hwnd,(HMENU)102,nullptr,nullptr);
        g_resetButton=CreateWindowW(L"BUTTON",L"Reset",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,336,62,76,27,hwnd,(HMENU)103,nullptr,nullptr);
        return 0;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)){case 100:LoadExecutable(hwnd);break;case 101:ToggleRun();break;case 102:if(!g_running)ExecuteStep();break;case 103:ResetCPU();break;case 105:DestroyWindow(hwnd);break;case 200:g_selectedTab=1-g_selectedTab;Refresh();break;}
        return 0;
    case WM_TIMER:
        if(wp==kRunTimer && g_running) {
            try { g_cpu->Run(1000); if(g_cpu->halted){StopRun();SetStatus(L"CPU halted.");} else if(g_cpu->InstructionsExecuted()%10000<1000) SetStatus(L"Running — " + std::to_wstring(g_cpu->InstructionsExecuted()) + L" instructions"); }
            catch(const std::exception& e){StopRun();g_lastError=ToWide(e.what());SetStatus(L"CPU exception: "+g_lastError);}
            Refresh();
        }
        return 0;
    case WM_PAINT: PaintMain(hwnd); return 0;
    case WM_SIZE: InvalidateRect(hwnd,nullptr,TRUE); return 0;
    case WM_DESTROY: StopRun(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    try { g_cpu=std::make_unique<Broadway::CPU>(kMemorySize); }
    catch (...) { MessageBoxW(nullptr,L"Could not initialize the Broadway CPU.",L"BroadwayCPU",MB_ICONERROR); return 1; }
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WindowProc;
    wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName=kClassName; wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION); RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(0,kClassName,L"BroadwayCPU — Broadway Processor Workbench",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1180,820,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 2;
    HMENU menu=CreateMenu(); HMENU file=CreatePopupMenu(); AppendMenuW(file,MF_STRING,100,L"Load executable...\tCtrl+O"); AppendMenuW(file,MF_SEPARATOR,0,nullptr); AppendMenuW(file,MF_STRING,105,L"Exit"); AppendMenuW(menu,MF_POPUP,(UINT_PTR)file,L"File");
    HMENU view=CreatePopupMenu(); AppendMenuW(view,MF_STRING,200,L"Toggle GPR / FPR view"); AppendMenuW(menu,MF_POPUP,(UINT_PTR)view,L"View");
    HMENU emu=CreatePopupMenu(); AppendMenuW(emu,MF_STRING,101,L"Run / Pause\tF5"); AppendMenuW(emu,MF_STRING,102,L"Single step\tF10"); AppendMenuW(emu,MF_STRING,103,L"Reset\tCtrl+R"); AppendMenuW(menu,MF_POPUP,(UINT_PTR)emu,L"Emulation"); SetMenu(hwnd,menu);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0){
        if(msg.message==WM_KEYDOWN){if((GetKeyState(VK_CONTROL)&0x8000)&&msg.wParam=='O')LoadExecutable(hwnd);else if(msg.wParam==VK_F5)ToggleRun();else if(msg.wParam==VK_F10&&!g_running)ExecuteStep();else if((GetKeyState(VK_CONTROL)&0x8000)&&msg.wParam=='R')ResetCPU();}
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}
