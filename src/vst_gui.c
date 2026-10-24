/* vst_gui.c - Win32 (plain C) GUI: launches vst_stream_record / vst_stream_playback,
 * captures child-process output, parses the two fullness values into progress bars.
 *
 * Layout (top to bottom):
 *   shared parameters / record parameters / playback parameters (three group boxes)
 *   buttons: Start Record / Stop / Playback
 *   progress bars: DMA FPGA Buffer Fullness, File Write Buffer Fullness
 *   log: child-process output (read-only multiline edit control)
 *
 * Project: https://github.com/oopsdump/vst-pxie-5644r-streaming-in-c
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 the vst-pxie-5644r-streaming-in-c authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <process.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")

#define ID_RATE     1001
#define ID_CENTER   1002
#define ID_REF      1003
#define ID_SECONDS  1004
#define ID_OUT      1005
#define ID_START    1006
#define ID_STOP     1007
#define ID_LOG      1008
#define ID_BAR_FPGA 1009
#define ID_BAR_FILE 1010
#define ID_LBL_FPGA 1011
#define ID_LBL_FILE 1012
#define ID_PLAY     1013
#define ID_RESOURCE 1014
#define ID_HBLOCKS  1015
#define ID_RBLOCKMB 1016
#define ID_POWER    1017
#define ID_MAXMB    1018
#define ID_PHBLOCKS 1019
#define ID_PRBLOCK  1020
#define ID_RINGMB   1021
#define ID_PBUFMB   1022
#define ID_BITFILE  1023
#define ID_REFCLK   1024

static HWND g_hRate, g_hCenter, g_hRef, g_hSeconds, g_hOut;
static HWND g_hStart, g_hStop, g_hPlay, g_hLog;
static HWND g_hBarFpga, g_hBarFile, g_hLblFpga, g_hLblFile;
static HWND g_hResource;
static HWND g_hHBlocks;
static HWND g_hRBlockMB;
static HWND g_hPower;
static HWND g_hMaxMB;
static HWND g_hPHBlocks;
static HWND g_hPRBlockMB;
static HWND g_hRingMB;
static HWND g_hPBufMB;
static HWND g_hBitfile;
static HWND g_hRefClk;
static HWND g_hMain;
static HWND g_hStat;
static char g_outPath[MAX_PATH];
static unsigned long long g_lastBytes;
static DWORD g_lastTick;

static void update_stats(void);   /* forward declaration: used by reader_thread */

static HFONT g_font = NULL;

static BOOL CALLBACK set_font_cb(HWND child, LPARAM font)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static HANDLE g_proc = NULL;
static HANDLE g_thread = NULL;
static volatile LONG g_running = 0;

/* Resolve the child executable path. */
/* Recorder/player live in the same directory (NI-PAL requires everything on the C: volume). */
static void find_exe(const char* name, char* out, size_t cap)
{
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, self, sizeof self);
    char* slash = strrchr(self, '\\');
    if (slash) *slash = '\0';
    snprintf(out, cap, "%s\\%s", self, name);
}
static void append_log(const char* text)
{
    /* Child output is UTF-8; the log control is wide-char, so convert before writing. */
    wchar_t wbuf[8192];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf,
                               (int)(sizeof wbuf / sizeof wbuf[0]));
    if (n <= 1) return;
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* good_high = 1 when a full buffer is healthy (playback: plenty of data queued);
 * good_high = 0 when a full buffer is a warning (record: overrun risk). */
static void set_bar(HWND bar, HWND lbl, const char* tag, double pct, double mb, int good_high)
{
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    SendMessageA(bar, PBM_SETPOS, (WPARAM)(int)(pct + 0.5), 0);
    {
        double  badness = good_high ? (100.0 - pct) : pct;
        COLORREF color = (badness < 30.0) ? RGB(0x2E, 0xA0, 0x43)
                       : (badness < 70.0) ? RGB(0xE0, 0xA8, 0x00)
                                          : RGB(0xD1, 0x34, 0x38);
        SendMessageA(bar, PBM_SETBARCOLOR, 0, (LPARAM)color);
    }
    char buf[128];
    snprintf(buf, sizeof buf, "%s %.1f%% (%.0f MB)", tag, pct, mb);
    SetWindowTextA(lbl, buf);
}

/* Parse "[Progress] sent/total percent% MB MS/s" - playback progress. */
static void parse_progress(const char* line)
{
    const char* p = strstr(line, "[Progress]");
    if (!p) return;
    unsigned long long sent = 0, total = 0;
    double pct = 0.0, mb = 0.0, mbs = 0.0;
    double msps = 0.0;
    if (sscanf(p + 8, "%llu/%llu %lf%% %lf MB %lf MS/s", &sent, &total, &pct, &mb, &msps) < 3) return;
    mbs = msps * 4.0;
    wchar_t buf[192];
    _snwprintf(buf, sizeof buf / sizeof buf[0],
               L"Playback %.1f%%  %.1f MB / %.1f MB  %.1f MS/s",
               pct, mb, total ? (double)total * 4.0 / (1024.0 * 1024.0) : 0.0, msps);
    buf[(sizeof buf / sizeof buf[0]) - 1] = L'\0';
    if (g_hStat) SetWindowTextW(g_hStat, buf);
}

/* Parse "[Status] ... DMA FPGA Buffer Fullness=x.x% | File Write Buffer Fullness=y.y% (z MB)". */
static void parse_status(const char* line)
{
    /* Both sides now log "DMA FPGA Buffer=" and "Buffer=". Playback lines also
 * carry "underflows", which the recorder never prints, so use that as the
 * discriminator. A full buffer is healthy for playback (green) but is an
 * overrun risk while recording (red). */
    const char* fa = strstr(line, "DMA FPGA Buffer=");
    /* Match the second field only: "DMA FPGA Buffer=" also ends with "Buffer=", */
    const char* fb = strstr(line, "| Buffer=");
    if (!fa && !fb) return;

    int playback = (strstr(line, "underflows") != NULL);
    double mb = 0.0;
    if (fb) {
        const char* p = strchr(fb, '(');
        if (p) mb = atof(p + 1);
    }
    if (fa) set_bar(g_hBarFpga, g_hLblFpga, "DMA FPGA Buffer",
                    atof(fa + strlen("DMA FPGA Buffer=")), 0.0, playback);
    if (fb) set_bar(g_hBarFile, g_hLblFile,
                    playback ? "File Read Buffer" : "File Write Buffer",
                    atof(fb + strlen("| Buffer=")), mb, playback);
}
static unsigned __stdcall reader_thread(void* arg)
{
    HANDLE rd = (HANDLE)arg;
    char buf[4096];
    char lineBuf[4096];
    size_t linePos = 0;
    DWORD n = 0;

    while (ReadFile(rd, buf, sizeof buf - 1, &n, NULL) && n > 0) {
        buf[n] = '\0';
        append_log(buf);
        /* parse line by line */
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (linePos > 0) {
                    lineBuf[linePos] = '\0';
                    parse_status(lineBuf);
                    parse_progress(lineBuf);
                    linePos = 0;
                }
            } else if (linePos + 1 < sizeof lineBuf) {
                lineBuf[linePos++] = buf[i];
            }
        }
    }
    append_log("\r\n[END] child process output closed\r\n");
    CloseHandle(rd);
    InterlockedExchange(&g_running, 0);
    KillTimer(g_hMain, 1);
    update_stats();
    EnableWindow(g_hStart, TRUE);
    EnableWindow(g_hStop, FALSE);
    return 0;
}

/* Enumerate local NI PXIe devices: returns the count, name[i] = model, res[i] = RIO resource name.
 * RIO indices follow enumeration order (single card => RIO0). */
static int enum_ni_devices(char names[8][128], char res[8][32])
{
    int found = 0;
    HDEVINFO devs = SetupDiGetClassDevsA(NULL, "PCI", NULL,
                                         DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof dd;
    for (DWORD i = 0; found < 8 && SetupDiEnumDeviceInfo(devs, i, &dd); ++i) {
        char desc[256] = {0};
        DWORD type = 0, size = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(devs, &dd, SPDRP_DEVICEDESC,
                                               &type, (PBYTE)desc, sizeof desc, &size)) {
            continue;
        }
        /* Keep only NI VST devices (PXIe-5644R/45R/46R). */
        if (!strstr(desc, "PXIe-5644") && !strstr(desc, "PXIe-5645") &&
            !strstr(desc, "PXIe-5646")) {
            continue;
        }
        snprintf(names[found], 128, "%s", desc);
        snprintf(res[found], 32, "RIO%d", found);
        ++found;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

/* Resolve the executable from %LOCALAPPDATA%\vst_rf\run first:
 * NI-PAL requires the exe on the C: volume, otherwise sessions fail with 0xBFFA0000. */
static void get_text(HWND h, char* out, size_t cap)
{
    GetWindowTextA(h, out, (int)cap);
}

/* Refresh the "file size + write throughput" display (called every 500 ms). */
static void update_stats(void)
{
    if (!g_hStat) return;
    unsigned long long bytes = 0;
    if (g_outPath[0]) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_outPath, GetFileExInfoStandard, &fad)) {
            bytes = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }
    }
    DWORD now = GetTickCount();
    DWORD dt = now - g_lastTick;
    double mbps = 0.0;
    if (dt > 0 && bytes >= g_lastBytes) {
        mbps = (double)(bytes - g_lastBytes) / (1024.0 * 1024.0) / ((double)dt / 1000.0);
    }
    g_lastBytes = bytes;
    g_lastTick = now;

    /* The static control is a wide-char window; use the wide-char API. */
    wchar_t buf[160];
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (mb < 1024.0) {
        _snwprintf(buf, sizeof buf / sizeof buf[0],
                   L"File %.1f MB    Write %.0f MB/s", mb, mbps);
    } else {
        _snwprintf(buf, sizeof buf / sizeof buf[0],
                   L"File %.2f GB    Write %.0f MB/s", mb / 1024.0, mbps);
    }
    buf[(sizeof buf / sizeof buf[0]) - 1] = L'\0';
    SetWindowTextW(g_hStat, buf);
}

static void start_record(void)
{
    if (InterlockedCompareExchange(&g_running, 1, 1)) return;

    char rate[64], center[64], ref[64], secs[64], out[MAX_PATH], res[64];
    get_text(g_hRate, rate, sizeof rate);
    get_text(g_hCenter, center, sizeof center);
    get_text(g_hRef, ref, sizeof ref);
    get_text(g_hSeconds, secs, sizeof secs);
    get_text(g_hOut, out, sizeof out);
    if (!out[0]) snprintf(out, sizeof out, "%s", "D:\\vst_out.vstiq");
    get_text(g_hResource, res, sizeof res);
    if (!res[0]) {
        snprintf(res, sizeof res, "%s", "RIO0");
    } else {
        /* Combo item looks like "RIO0  (PXIe-5644R)"; keep only the resource name. */
        char* sp = strstr(res, "  (");
        if (sp) *sp = '\0';
    }

    char exe[MAX_PATH];
    find_exe("vst_stream_record.exe", exe, sizeof exe);

    char cmd[2048];
    {
        char hb[32] = "16", rm[32] = "32", mx[32] = "0", rb[32] = "512";
        wchar_t bfw[512] = L"";
        char rcf[64] = "";
        if (g_hBitfile) GetWindowTextW(g_hBitfile, bfw, (int)(sizeof bfw / sizeof bfw[0]));
        if (g_hRefClk) {
            wchar_t rcw[64] = L"";
            GetWindowTextW(g_hRefClk, rcw, (int)(sizeof rcw / sizeof rcw[0]));
            WideCharToMultiByte(CP_UTF8, 0, rcw, -1, rcf, (int)sizeof rcf, NULL, NULL);
        }
        if (g_hHBlocks)  get_text(g_hHBlocks, hb, sizeof hb);
        if (g_hRBlockMB) get_text(g_hRBlockMB, rm, sizeof rm);
        if (g_hMaxMB)    get_text(g_hMaxMB, mx, sizeof mx);
        if (g_hRingMB)   get_text(g_hRingMB, rb, sizeof rb);
        {
            char bfa[1024] = "";
            WideCharToMultiByte(CP_UTF8, 0, bfw, -1, bfa, (int)sizeof bfa, NULL, NULL);
            snprintf(cmd, sizeof cmd,
                     "\"%s\" --out \"%s\" --seconds %s --rate %s --center %s --ref %s"
                     " --resource %s --host-depth-blocks %s --read-block-mb %s --max-file-mb %s"
                     " --record-ring-mb %s%s%s",
                     exe, out, secs, rate, center, ref, res, hb, rm, mx, rb,
                     bfa[0] ? " --bitfile \"" : "", bfa);
            if (bfa[0]) strncat(cmd, "\"", sizeof cmd - strlen(cmd) - 1);
            if (rcf[0]) {
                strncat(cmd, " --ref-clock ", sizeof cmd - strlen(cmd) - 1);
                strncat(cmd, rcf, sizeof cmd - strlen(cmd) - 1);
            }
        }
    }

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { append_log("[ERR] CreatePipe failed\r\n"); return; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;
    memset(&pi, 0, sizeof pi);

    append_log("==================================================\r\n");
    append_log(cmd);
    append_log("\r\n");

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        append_log("[ERR] cannot launch recorder\r\n");
        InterlockedExchange(&g_running, 0);
        return;
    }
    CloseHandle(wr);
    g_proc = pi.hProcess;
    CloseHandle(pi.hThread);

    InterlockedExchange(&g_running, 1);
    EnableWindow(g_hStart, FALSE);
    EnableWindow(g_hStop, TRUE);

    snprintf(g_outPath, sizeof g_outPath, "%s", out);
    g_lastBytes = 0;
    g_lastTick = GetTickCount();
    SetTimer(g_hMain, 1, 500, NULL);
    if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
    g_thread = (HANDLE)_beginthreadex(NULL, 0, reader_thread, rd, 0, NULL);
}

static void stop_record(void)
{
    if (!g_proc) return;
    append_log("\r\n[STOP] sending CTRL_BREAK\r\n");
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)GetProcessId(g_proc));
    if (WaitForSingleObject(g_proc, 3000) == WAIT_TIMEOUT) {
        TerminateProcess(g_proc, 1);
        append_log("[STOP] timed out, terminated forcibly\r\n");
    }
    CloseHandle(g_proc);
    g_proc = NULL;
}

static void start_play(void)
{
    char out[MAX_PATH];
    get_text(g_hOut, out, sizeof out);
    if (!out[0]) { append_log("[HINT] please specify the input file path\r\n"); return; }

    char exe[MAX_PATH];
    find_exe("vst_stream_playback.exe", exe, sizeof exe);
    char cmd[2048];
    {
        char hb[32] = "16", rm[32] = "4", pw[32] = "-5", bm[32] = "256";
        char bfa[1024] = "";
        char rcf[64] = "";
        if (g_hBitfile) {
            wchar_t bfw[512] = L"";
            GetWindowTextW(g_hBitfile, bfw, (int)(sizeof bfw / sizeof bfw[0]));
            WideCharToMultiByte(CP_UTF8, 0, bfw, -1, bfa, (int)sizeof bfa, NULL, NULL);
        }
        if (g_hRefClk) {
            wchar_t rcw[64] = L"";
            GetWindowTextW(g_hRefClk, rcw, (int)(sizeof rcw / sizeof rcw[0]));
            WideCharToMultiByte(CP_UTF8, 0, rcw, -1, rcf, (int)sizeof rcf, NULL, NULL);
        }
        if (g_hPHBlocks)  get_text(g_hPHBlocks, hb, sizeof hb);
        if (g_hPRBlockMB) get_text(g_hPRBlockMB, rm, sizeof rm);
        if (g_hPower)     get_text(g_hPower, pw, sizeof pw);
        if (g_hPBufMB)    get_text(g_hPBufMB, bm, sizeof bm);
        snprintf(cmd, sizeof cmd,
                 "\"%s\" --in \"%s\" --host-depth-blocks %s --read-block-mb %s --power %s"
                 " --playback-ring-mb %s%s%s",
                 exe, out, hb, rm, pw, bm,
                 bfa[0] ? " --bitfile \"" : "", bfa);
        if (bfa[0]) strncat(cmd, "\"", sizeof cmd - strlen(cmd) - 1);
        if (rcf[0]) {
            strncat(cmd, " --ref-clock ", sizeof cmd - strlen(cmd) - 1);
            strncat(cmd, rcf, sizeof cmd - strlen(cmd) - 1);
        }
    }

    append_log("==================================================\r\n");
    append_log(cmd);
    append_log("\r\n");

    /* Same as record: capture child output to see playback progress and errors. */
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { append_log("[ERR] CreatePipe failed\r\n"); return; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        append_log("[ERR] cannot launch playback\r\n");
        return;
    }
    CloseHandle(wr);
    g_proc = pi.hProcess;
    CloseHandle(pi.hThread);

    /* Playback does not track output-file growth; clear path and stop the stats timer. */
    g_outPath[0] = '\0';
    KillTimer(g_hMain, 1);
    if (g_hStat) SetWindowTextW(g_hStat, L"Playing...");

    InterlockedExchange(&g_running, 1);
    EnableWindow(g_hStart, FALSE);
    EnableWindow(g_hStop, TRUE);
    if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
    g_thread = (HANDLE)_beginthreadex(NULL, 0, reader_thread, rd, 0, NULL);
}

static void make_label(HWND parent, const wchar_t* text, int x, int y, int w)
{
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                    x, y, w, 20, parent, NULL, NULL, NULL);
}

static HWND make_edit(HWND parent, int id, const wchar_t* text, int x, int y, int w)
{
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             x, y, w, 24, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    return h;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        int x, y, gx, gy;
        HWND grp;

        g_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             VARIABLE_PITCH, L"Microsoft YaHei UI");

        x = 12; y = 10;

        /* ---------- group 1: shared parameters ---------- */
        grp = CreateWindowExW(0, L"BUTTON", L" Common ",
                              WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                              x, y, 850, 108, h, NULL, NULL, NULL);
        gx = x + 14; gy = y + 22;
        make_label(h, L"Device", gx, gy + 3, 42);
        g_hResource = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN,
                                      gx + 45, gy, 300, 220, h, (HMENU)ID_RESOURCE, NULL, NULL);
        {
            char names[8][128], res[8][32];
            int n = enum_ni_devices(names, res);
            int i;
            if (n <= 0) SendMessageA(g_hResource, CB_ADDSTRING, 0, (LPARAM)"RIO0");
            else for (i = 0; i < n; ++i) {
                char item[192];
                snprintf(item, sizeof item, "%s  (%s)", res[i], names[i]);
                SendMessageA(g_hResource, CB_ADDSTRING, 0, (LPARAM)item);
            }
            SendMessageA(g_hResource, CB_SETCURSEL, 0, 0);
        }
        /* row 1: device, file, stop (stop is shared) */
        make_label(h, L"File", gx + 360, gy + 3, 30);
        g_hOut = make_edit(h, ID_OUT, L"D:\\vst_out.vstiq", gx + 400, gy, 250);
        g_hStop = CreateWindowExW(0, L"BUTTON", L"Stop",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  gx + 660, gy - 1, 130, 26, h, (HMENU)ID_STOP, NULL, NULL);
        EnableWindow(g_hStop, FALSE);

        /* row 2: streaming bitfile location */
        gy += 30;
        make_label(h, L"Bitfile", gx, gy + 3, 46);
        g_hBitfile = make_edit(h, ID_BITFILE,
                               L"C:\\Users\\Public\\Documents\\National Instruments\\FPGA Extensions Bitfiles\\NI PXIe-5644R\\NI Streaming for VST.lvbitx",
                               gx + 50, gy, 730);

        /* row 3: Center, IQ Rate */
        gy += 30;
        make_label(h, L"Center (Hz)", gx, gy + 3, 84);
        g_hCenter = make_edit(h, ID_CENTER, L"1000000000", gx + 88, gy, 110);
        make_label(h, L"IQ Rate (Hz)", gx + 210, gy + 3, 88);
        g_hRate = make_edit(h, ID_RATE, L"120000000", gx + 302, gy, 110);
        make_label(h, L"Ref Clock", gx + 430, gy + 3, 62);
        g_hRefClk = CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                    gx + 496, gy, 150, 200, h, (HMENU)ID_REFCLK, NULL, NULL);
        {
            static const wchar_t* kClocks[] = { L"OnboardClock", L"RefIn", L"PXI_Clk", L"ClkIn" };
            int ci;
            for (ci = 0; ci < 4; ++ci) SendMessageW(g_hRefClk, CB_ADDSTRING, 0, (LPARAM)kClocks[ci]);
            SendMessageW(g_hRefClk, CB_SETCURSEL, 0, 0);
        }


        /* ---------- group 2: record ---------- */
        y += 118;
        grp = CreateWindowExW(0, L"BUTTON", L" Record ",
                              WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                              x, y, 850, 92, h, NULL, NULL, NULL);
        gx = x + 14; gy = y + 24;
        make_label(h, L"Ref (dBm)", gx, gy + 3, 70);
        g_hRef = make_edit(h, ID_REF, L"-10", gx + 74, gy, 55);
        make_label(h, L"Seconds", gx + 140, gy + 3, 52);
        g_hSeconds = make_edit(h, ID_SECONDS, L"5", gx + 196, gy, 55);
        make_label(h, L"Max File Size (MB)", gx + 262, gy + 3, 118);
        g_hMaxMB = make_edit(h, ID_MAXMB, L"0", gx + 384, gy, 55);
        g_hStart = CreateWindowExW(0, L"BUTTON", L"Record",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   gx + 660, gy - 1, 130, 26, h, (HMENU)ID_START, NULL, NULL);
        gy += 32;
        make_label(h, L"Host Depth (Blocks)", gx, gy + 3, 125);
        g_hHBlocks = make_edit(h, ID_HBLOCKS, L"16", gx + 128, gy, 50);
        make_label(h, L"Read Block (MB)", gx + 200, gy + 3, 100);
        g_hRBlockMB = make_edit(h, ID_RBLOCKMB, L"32", gx + 302, gy, 50);
        make_label(h, L"Ring Buffer (MB)", gx + 380, gy + 3, 115);
        g_hRingMB = make_edit(h, ID_RINGMB, L"512", gx + 498, gy, 60);

        /* ---------- group 3: playback ---------- */
        y += 102;
        grp = CreateWindowExW(0, L"BUTTON", L" Playback ",
                              WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                              x, y, 850, 92, h, NULL, NULL, NULL);
        gx = x + 14; gy = y + 24;
        make_label(h, L"Output Power (dBm)", gx, gy + 3, 130);
        g_hPower = make_edit(h, ID_POWER, L"-5", gx + 134, gy, 60);
        g_hPlay = CreateWindowExW(0, L"BUTTON", L"Play",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  gx + 660, gy - 1, 130, 26, h, (HMENU)ID_PLAY, NULL, NULL);
        gy += 32;
        make_label(h, L"Host Depth (Blocks)", gx, gy + 3, 125);
        g_hPHBlocks = make_edit(h, ID_PHBLOCKS, L"16", gx + 128, gy, 50);
        make_label(h, L"Write Block (MB)", gx + 200, gy + 3, 105);
        g_hPRBlockMB = make_edit(h, ID_PRBLOCK, L"4", gx + 308, gy, 50);
        make_label(h, L"Ring Buffer (MB)", gx + 380, gy + 3, 115);
        g_hPBufMB = make_edit(h, ID_PBUFMB, L"256", gx + 498, gy, 60);

        /* ---------- status area ---------- */
        y = 332;
        g_hStat = CreateWindowExW(0, L"STATIC", L"Ready",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  x, y, 850, 22, h, NULL, NULL, NULL);

        y += 26;
        g_hLblFpga = CreateWindowExW(0, L"STATIC", L"DMA FPGA Buffer  -",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     x, y + 2, 250, 20, h, (HMENU)ID_LBL_FPGA, NULL, NULL);
        g_hBarFpga = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                     WS_CHILD | WS_VISIBLE, x + 256, y, 580, 20,
                                     h, (HMENU)ID_BAR_FPGA, NULL, NULL);
        SendMessageW(g_hBarFpga, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        y += 26;
        g_hLblFile = CreateWindowExW(0, L"STATIC", L"File Write Buffer  -",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     x, y + 2, 250, 20, h, (HMENU)ID_LBL_FILE, NULL, NULL);
        g_hBarFile = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                     WS_CHILD | WS_VISIBLE, x + 256, y, 580, 20,
                                     h, (HMENU)ID_BAR_FILE, NULL, NULL);
        SendMessageW(g_hBarFile, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        y += 28;
        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                 x, y, 850, 260, h, (HMENU)ID_LOG, NULL, NULL);

        EnumChildWindows(h, set_font_cb, (LPARAM)g_font);
        return 0;
    }    case WM_TIMER:
        if (wp == 1) { update_stats(); return 0; }
        return 0;
    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        if (g_hLog) {
            MoveWindow(g_hLog, 12, 412, rc.right - 24, rc.bottom - 422, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_START: start_record(); return 0;
        case ID_STOP:  stop_record();  return 0;
        case ID_PLAY:  start_play();   return 0;
        }
        return 0;
    case WM_CLOSE:
        stop_record();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev; (void)cmdline;

    /* NI-PAL requires the executable on the C: volume; otherwise all sessions fail (0xBFFA0000). */
    {
        char self[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, self, sizeof self);
        if (self[0] != 'C' && self[0] != 'c') {
            /* Source is UTF-8; must use the wide-char API. */
            wchar_t wself[MAX_PATH] = {0};
            MultiByteToWideChar(CP_ACP, 0, self, -1, wself,
                                (int)(sizeof wself / sizeof wself[0]));
            wchar_t msg[MAX_PATH + 256];
            _snwprintf(msg, sizeof msg / sizeof msg[0],
                       L"This program must run from the C: drive.\n\n"
                       L"Current path:\n%ls\n\n"
                       L"Reason: NI-PAL requires the executable on the C: volume;\n"
                       L"running from another drive prevents opening the device.\n\n"
                       L"Copy the whole program folder to the C: drive and run it from there (tools\\run\\stage_exes.ps1).",
                       wself);
            msg[(sizeof msg / sizeof msg[0]) - 1] = L'\0';
            MessageBoxW(NULL, msg, L"Invalid path", MB_ICONERROR | MB_OK);
            return 1;
        }
    }
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VstGuiC";
    RegisterClassW(&wc);

    HWND h = CreateWindowExW(0, L"VstGuiC", L"VST Record / Playback",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 870,
                             NULL, NULL, inst, NULL);
    if (!h) return 1;
    g_hMain = h;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
