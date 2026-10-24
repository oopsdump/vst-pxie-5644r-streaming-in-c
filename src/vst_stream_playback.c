/* vst_stream_playback.c - streaming playback, structurally identical to the NI example.
 *
 * Basis (all from the example, nothing guessed):
 *   1) block diagram TDMS Playback for VST.vi:
 *      DriverSetup=Bitfile:NI Streaming for VST.lvbitx -> niRFSG(Arb Waveform)
 *      -> write dummy data (0+0i x 256) -> DMA Output Configure -> Initiate (after primed)
 *      -> write in blocks -> drain before stopping -> Abort/Close
 *   2) block diagram support\Output Select and Enable.vi:
 *      streaming output.select / output.burst size / output.reset(F) /
 *      output.threshold(150) / output.enable(T), then read output.dma fullness
 *   3) block diagram support\dma\DMA Output Configure.vi:
 *      output.dma.Configure(Requested Depth) -> output.dma.Start  => FIFO 0
 *   4) block diagram support\dma\DMA Output Status.vi:
 *      output.dma.Acquire Write Region(n, timeout) + read min elements / fullness
 *   5) addresses come from tools\recon\streaming_regs.txt (same streaming image)
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
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "niRFSG.h"

#include "vst_error.h"
#include "vst_file.h"
#include <process.h>

typedef int32_t  NiStatus;
typedef uint32_t NiSession;
typedef NiStatus (*fn_open)(const char*, const char*, const char*, uint32_t, NiSession*);
typedef NiStatus (*fn_close)(NiSession, uint32_t);
typedef NiStatus (*fn_w32)(NiSession, uint32_t, uint32_t);
typedef NiStatus (*fn_r32)(NiSession, uint32_t, uint32_t*);
typedef NiStatus (*fn_cfg)(NiSession, uint32_t, size_t, size_t*);
typedef NiStatus (*fn_fifo)(NiSession, uint32_t);
typedef NiStatus (*fn_acqw)(NiSession, uint32_t, uint32_t**, size_t, uint32_t, size_t*, size_t*);
typedef NiStatus (*fn_relw)(NiSession, uint32_t, size_t);

#define BITFILE \
    "C:\\Users\\Public\\Documents\\National Instruments\\FPGA Extensions Bitfiles\\" \
    "NI PXIe-5644R\\NI Streaming for VST.lvbitx"
#define SIG "F9744DDE15670B63D27A69F7CB821C89"
#define OPT_DRIVER_SETUP "DriverSetup=Bitfile:NI Streaming for VST.lvbitx"

/* ---------- read-ahead queue (double buffering) ----------
 * A private 4-slot queue owned by this file; the reader thread fills slots
 * while the main thread drains them into the FIFO. Deliberately independent
 * of vst_ring.c: the playback direction has different edge cases. */
#define DB_SLOTS      4u
#define DB_SLOT_BYTES (8u * 1024u * 1024u)      /* 8 MB = 2M samples per slot */

typedef struct {
    unsigned char*      buf;
    size_t              bytes;
    int                 ready;
} DbSlot;

static DbSlot             g_db[DB_SLOTS];
static CRITICAL_SECTION   g_db_cs;
static CONDITION_VARIABLE g_db_cv;
static VstiqReader*       g_db_rd;
static unsigned           g_db_head;
static size_t             g_db_off;
static int                g_db_stop;
static int                g_db_eof;
static size_t             g_db_slot_samples;

static unsigned __stdcall db_reader_main(void* unused)
{
    unsigned slot = 0;
    (void)unused;
    for (;;) {
        EnterCriticalSection(&g_db_cs);
        while (!g_db_stop && g_db[slot].ready)
            SleepConditionVariableCS(&g_db_cv, &g_db_cs, INFINITE);
        if (g_db_stop) { LeaveCriticalSection(&g_db_cs); break; }
        LeaveCriticalSection(&g_db_cs);

        size_t got = vstiq_read(g_db_rd, g_db[slot].buf, g_db_slot_samples);

        EnterCriticalSection(&g_db_cs);
        g_db[slot].bytes = got * 4;
        g_db[slot].ready = 1;
        if (got == 0) g_db_eof = 1;
        WakeAllConditionVariable(&g_db_cv);
        LeaveCriticalSection(&g_db_cs);
        if (got == 0) break;
        slot = (slot + 1) % DB_SLOTS;
    }
    return 0;
}

/* Copy up to `samples` complex samples out of the queue; returns what was copied. */
static size_t db_take(void* dst, size_t samples)
{
    size_t need = samples * 4, done = 0;
    unsigned char* out = (unsigned char*)dst;

    EnterCriticalSection(&g_db_cs);
    while (done < need) {
        while (!g_db[g_db_head].ready && !g_db_stop)
            SleepConditionVariableCS(&g_db_cv, &g_db_cs, 1000);
        if (!g_db[g_db_head].ready) break;              /* stopped */
        if (g_db[g_db_head].bytes == 0) {               /* EOF slot */
            g_db[g_db_head].ready = 0;
            g_db_head = (g_db_head + 1) % DB_SLOTS;
            WakeAllConditionVariable(&g_db_cv);
            break;
        }
        size_t avail = g_db[g_db_head].bytes - g_db_off;
        size_t n = need - done;
        if (n > avail) n = avail;
        if (n) {
            memcpy(out + done, g_db[g_db_head].buf + g_db_off, n);
            done += n;
            g_db_off += n;
        }
        if (g_db_off >= g_db[g_db_head].bytes) {        /* slot drained */
            g_db[g_db_head].ready = 0;
            g_db_off = 0;
            g_db_head = (g_db_head + 1) % DB_SLOTS;
            WakeAllConditionVariable(&g_db_cv);
        }
    }
    LeaveCriticalSection(&g_db_cs);
    return done / 4;
}
/* Streaming-image output registers (names in streaming_regs.txt). */
#define A_OUT_SELECT     0x002cu   /* streaming output.select */
#define A_OUT_BURST      0x0030u   /* streaming output.burst size */
#define A_OUT_THRESHOLD  0x0044u   /* streaming output.threshold */
#define A_OUT_ENABLE     0x0048u   /* streaming output.enable */
#define A_OUT_RESET      0x0050u   /* streaming output.reset */
#define A_OUT_FULLNESS   0x001cu   /* streaming output.dma fullness */
#define A_OUT_MIN_ELEM   0x0018u   /* streaming output.dma min elements */
#define A_OUT_SAMPLES    0x0038u   /* streaming output.samples processed */
#define A_OUT_UNDERFLOWS 0x003cu   /* streaming output.underflows */

/* FIFO numbers come from the bitfile XML DmaChannelAllocationList:
 *   2 = streaming output.dma (HostToTarget, 65541 elements)
 *   4 = streaming input.dma  (TargetToHost, 65535 elements)  <- matches the record path
 * Note: LabVIEW FIFO indices differ from these raw numbers (LV uses 5 for input, 0 for output). */
#define FIFO_OUT 2u
/* Constants from the example Output Select and Enable.vi. */
#define EXAMPLE_THRESHOLD 150u
#define DUMMY_SAMPLES     256

enum { EXIT_OK = 0, EXIT_RUNTIME = 1, EXIT_USAGE = 2 };

static volatile LONG g_stop = 0;
static BOOL WINAPI on_ctrl(DWORD t)
{
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) { InterlockedExchange(&g_stop, 1); return TRUE; }
    return FALSE;
}

static void usage(const char* exe)
{
    printf("usage: %s --in <file.vstiq> [--resource RIO0] [--seconds S]\n"
           "        [--fifo-depth N] [--chunk N] [--power dBm]\n\n"
           "Streaming playback: FPGA FIFO + niRFSG(DriverSetup) driving RF (same as the example).\n", exe);
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    const char* in_path = NULL;
    const char* resource = "RIO0";
    double seconds = 0.0;
    size_t fifo_depth = 8000000u;      /* example DMA Output Configure Requested Depth */
    size_t chunk = 262144u;            /* example Read Block Size */
    uint32_t fifo_out = FIFO_OUT;      /* raw output FIFO index (override with --fifo) */
    /* The example AcquireWriteRegionFifo default Timeout(ms) is -1 (wait forever).
 * Default matches the example; use --timeout for a bounded value when debugging. */
    uint32_t timeout_ms = 0xFFFFFFFFu;
    /* Example front panel: DMA Host Buffer Depth (Blocks)=16, Read Block Size (MB)=32.
 * Read Block Size conversion (from the diagram): samples = MB x 262144 (262144 = 1 MB / 4 bytes).
 * 16 blocks maps to the measured FIFO depth 8,000,000 elements, i.e. 500,000 per block. */
    uint32_t host_depth_blocks = 16u;
    const char* ref_clock = NULL;       /* --ref-clock: Reference Clock Source */
    const char* bitfile = BITFILE;
    char setup[512];
    uint32_t buffer_mb = 32u;          /* read-ahead queue total size in MB, --playback-ring-mb */
    uint32_t read_block_mb = 0u;       /* 0 = keep the --chunk value */
    /* Output power and record reference level are different things; the example has a separate Output Power (dBm) control, default -5. */
    double power_dbm = -5.0;
    int    have_power = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return EXIT_OK; }
        if (i + 1 >= argc) { fprintf(stderr, "error: %s requires a value\n", a); return EXIT_USAGE; }
        const char* v = argv[++i];
        if (!strcmp(a, "--in")) in_path = v;
        else if (!strcmp(a, "--resource")) resource = v;
        else if (!strcmp(a, "--seconds")) seconds = atof(v);
        else if (!strcmp(a, "--fifo-depth")) fifo_depth = (size_t)strtoull(v, NULL, 0);
        else if (!strcmp(a, "--chunk")) chunk = (size_t)strtoull(v, NULL, 0);
        else if (!strcmp(a, "--fifo")) fifo_out = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--timeout")) timeout_ms = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--host-depth-blocks")) host_depth_blocks = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--playback-ring-mb")) buffer_mb = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--bitfile")) bitfile = v;
        else if (!strcmp(a, "--ref-clock")) ref_clock = v;
        else if (!strcmp(a, "--read-block-mb")) read_block_mb = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--power")) { power_dbm = atof(v); have_power = 1; }
        else { fprintf(stderr, "error: unknown option %s\n", a); return EXIT_USAGE; }
    }
    if (!in_path || chunk == 0) { usage(argv[0]); return EXIT_USAGE; }
    if (read_block_mb > 0) {
        chunk = (size_t)read_block_mb * 262144u;      /* example: 262144 complex samples per MB */
    }
    if (host_depth_blocks > 0) {
        /* Only convert from blocks when --fifo-depth was not given explicitly. */
        if (fifo_depth == 8000000u) {
            fifo_depth = (size_t)host_depth_blocks * 500000u;
        }
    }
    printf("[a] Read Block Size=%u MB -> chunk=%zu samples | Host Buffer Depth=%u blocks -> FIFO=%zu\n",
           (unsigned)read_block_mb, chunk, (unsigned)host_depth_blocks, fifo_depth);

    /* open the input file */
    VstiqReader reader;
    memset(&reader, 0, sizeof reader);
    if (vstiq_open(in_path, &reader) != 0) {
        fprintf(stderr, "error: cannot open %s\n", in_path);
        return EXIT_RUNTIME;
    }
    uint64_t total = reader.total_samples;
    if (seconds > 0.0 && reader.header.iq_rate_hz > 0.0) {
        uint64_t want = (uint64_t)(seconds * reader.header.iq_rate_hz);
        if (want < total) total = want;
    }
    printf("[0] file: %s | %llu samples | %.3f MS/s | %.3f GHz\n",
           in_path, (unsigned long long)total,
           reader.header.iq_rate_hz / 1e6, reader.header.center_freq_hz / 1e9);

    HMODULE dll = LoadLibraryA("NiFpga.dll");
    if (!dll) { fprintf(stderr, "error: cannot load NiFpga.dll\n"); return EXIT_RUNTIME; }
    fn_open  op  = (fn_open)GetProcAddress(dll, "NiFpgaDll_Open");
    fn_close cl  = (fn_close)GetProcAddress(dll, "NiFpgaDll_Close");
    fn_w32   w32 = (fn_w32)GetProcAddress(dll, "NiFpgaDll_WriteU32");
    fn_r32   r32 = (fn_r32)GetProcAddress(dll, "NiFpgaDll_ReadU32");
    fn_cfg   cfg = (fn_cfg)GetProcAddress(dll, "NiFpgaDll_ConfigureFifo2");
    fn_fifo  stt = (fn_fifo)GetProcAddress(dll, "NiFpgaDll_StartFifo");
    fn_fifo  stp = (fn_fifo)GetProcAddress(dll, "NiFpgaDll_StopFifo");
    fn_acqw  acw = (fn_acqw)GetProcAddress(dll, "NiFpgaDll_AcquireFifoWriteElementsU32");
    fn_relw  rlw = (fn_relw)GetProcAddress(dll, "NiFpgaDll_ReleaseFifoElements");
    if (!op || !cl || !w32 || !r32 || !cfg || !stt || !stp || !acw || !rlw) {
        fprintf(stderr, "error: NiFpga.dll missing required exports\n");
        return EXIT_RUNTIME;
    }

    NiSession sess = 0;
    ViSession sg = VI_NULL;
    int opened = 0, rfsg_open = 0, started = 0, failed = 0;
    NiStatus st;
    ViStatus rc;
    uint64_t written = 0;
    size_t fifo_actual = fifo_depth;
    HANDLE   readerThread = NULL;
    DWORD t0 = GetTickCount();

    /* 1) open the streaming bitfile and keep it open */
    {
        const char* base = bitfile;
        const char* s;
        for (s = bitfile; *s; ++s) if (*s == '\\' || *s == '/') base = s + 1;
        snprintf(setup, sizeof setup, "DriverSetup=Bitfile:%s", base);
    }
    printf("[0a] bitfile: %s\n", bitfile);
    st = op(bitfile, SIG, resource, 0x88000000u, &sess);
    printf("[1] NiFpga Open(%s) = 0x%08X session=%u\n", resource, (unsigned)st, (unsigned)sess);
    if (st != 0) { failed = 1; goto cleanup; }
    opened = 1;

    /* 2) niRFSG init (DriverSetup makes the driver reuse the loaded streaming bitfile) */
    rc = niRFSG_InitWithOptions(resource, VI_TRUE, VI_FALSE, setup, &sg);
    printf("[2] niRFSG init(DriverSetup) = 0x%08X\n", (unsigned)rc);
    if (rc != 0) { vst_check(rc, "niRFSG_InitWithOptions"); failed = 1; goto cleanup; }
    rfsg_open = 1;

    /* 3) Arb Waveform mode + RF parameters (example Generation Mode = Arb Waveform) */
    if (!have_power && reader.header.ref_level_dbm != 0.0) {
        /* The file has no "output power" field; ref_level_dbm is the record reference level and must not be used as transmit power.
 * Keep the default -5 dBm and say so in the log. */
    }
    if (ref_clock) {
        /* RFSG spells the PXI backplane clock "PXI_CLK"; RFSA spells it "PXI_Clk". */
        const char* rc_name = ref_clock;
        ViStatus rc3;
        if (!_stricmp(ref_clock, "PXI_Clk")) rc_name = "PXI_CLK";
        rc3 = niRFSG_SetAttributeViString(sg, "", NIRFSG_ATTR_REF_CLOCK_SOURCE, rc_name);
        printf("[3a] reference clock source = %s (0x%08X)\n", rc_name, (unsigned)rc3);
    }

    if ((rc = niRFSG_SetAttributeViInt32(sg, "", NIRFSG_ATTR_GENERATION_MODE,
                                         NIRFSG_VAL_ARB_WAVEFORM)) != 0 ||
        (rc = niRFSG_SetAttributeViReal64(sg, "", NIRFSG_ATTR_IQ_RATE,
                                          reader.header.iq_rate_hz)) != 0 ||
        (rc = niRFSG_ConfigureRF(sg, reader.header.center_freq_hz, power_dbm)) != 0) {
        vst_check(rc, "niRFSG Configure*");
        failed = 1;
        goto cleanup;
    }
    printf("[3] Arb mode: %.3f MS/s @ %.3f GHz, output power %.1f dBm%s\n",
           reader.header.iq_rate_hz / 1e6, reader.header.center_freq_hz / 1e9,
           power_dbm, have_power ? "" : " (default, not the record reference level)");

    /* 4) dummy data: write dummy data so the VST has something to generate */
    {
        static ViReal64 iData[DUMMY_SAMPLES];
        static ViReal64 qData[DUMMY_SAMPLES];
        memset(iData, 0, sizeof iData);
        memset(qData, 0, sizeof qData);
        {
            const char* mv = getenv("VST_MORE");
            ViBoolean more = (mv && mv[0] == '1') ? VI_TRUE : VI_FALSE;
            rc = niRFSG_WriteArbWaveform(sg, "", DUMMY_SAMPLES, iData, qData, more);
            printf("     moreDataPending=%d\n", (int)more);
        }
        printf("[4] dummy data %d samples (0+0i) = 0x%08X\n", DUMMY_SAMPLES, (unsigned)rc);
        if (rc != 0) { vst_check(rc, "niRFSG_WriteArbWaveform"); failed = 1; goto cleanup; }
    }

    /* 5) DMA Output Configure: Configure(Requested Depth) -> Start (FIFO 0) */
    {
        size_t actual = 0;
        if ((st = cfg(sess, fifo_out, fifo_depth, &actual)) != 0) {
            /* LV FIFO indices differ from raw ones; probe the available raw indices. */
            fprintf(stderr, "hint: ConfigureFifo2(%u) failed 0x%08X, probing raw FIFO indices\n",
                    (unsigned)fifo_out, (unsigned)st);
            for (uint32_t probe = 0; probe < 12; ++probe) {
                size_t pa = 0;
                NiStatus pr = cfg(sess, probe, fifo_depth, &pa);
                printf("    debug: fifo %u -> 0x%08X actual=%zu\n",
                       (unsigned)probe, (unsigned)pr, pa);
            }
            failed = 1;
            goto cleanup;
        }
        printf("[5] streaming output.dma.Configure requested=%zu actual=%zu\n", fifo_depth, actual);
        if (actual > 0) fifo_actual = actual;
        /* The request must not exceed the FIFO depth (Acquire errors immediately otherwise). */
        if (chunk > fifo_actual) {
            printf("     chunk %zu > FIFO depth %zu, clamping to depth\n", chunk, fifo_actual);
            chunk = fifo_actual;
        }
        if ((st = stt(sess, fifo_out)) != 0) {
            fprintf(stderr, "error: streaming output.dma.Start failed 0x%08X\n", (unsigned)st);
            failed = 1;
            goto cleanup;
        }
        started = 1;
    }

    /* 6) Output Select and Enable (order and constants copied from the example) */
    {
        const char* sv = getenv("VST_SELECT");
        uint32_t sel = (sv && sv[0]) ? (uint32_t)strtoul(sv, NULL, 0) : 1u;
        w32(sess, A_OUT_SELECT, sel);         /* output.select */
        printf("     output.select=%u\n", (unsigned)sel);
    }
    w32(sess, A_OUT_BURST, 0u);               /* output.burst size */
    w32(sess, A_OUT_RESET, 1u);               /* output.reset = TRUE */
    w32(sess, A_OUT_RESET, 0u);               /* output.reset = FALSE */
    w32(sess, A_OUT_THRESHOLD, EXAMPLE_THRESHOLD);
    w32(sess, A_OUT_ENABLE, 1u);              /* output.enable = TRUE */
    {
        const char* sv = getenv("VST_STARTPRIMED");
        if (sv && sv[0] == '1') {
            w32(sess, 0x0028u, 1u);           /* streaming output.start on primed */
            printf("     start on primed = 1 written\n");
        }
    }
    {
        uint32_t fullness = 0;
        r32(sess, A_OUT_FULLNESS, &fullness);
        printf("[6] output.enable=1 | output.dma fullness=0x%x\n", (unsigned)fullness);
    }


    /* 7) Initiate - must happen after the buffer is primed (per the diagram comment) */
    rc = niRFSG_Initiate(sg);
    printf("[7] niRFSG Initiate = 0x%08X\n", (unsigned)rc);
    if (rc != 0) { vst_check(rc, "niRFSG_Initiate"); failed = 1; goto cleanup; }

    /* Example order: ConfigureOutputEnabled is called after Initiate */
    {
        ViStatus oe = niRFSG_ConfigureOutputEnabled(sg, VI_TRUE);
        printf("[7b] niRFSG ConfigureOutputEnabled = 0x%08X\n", (unsigned)oe);
    }

    /* KEY: write one block first so streaming output.primed becomes 1, then write start on primed.
 * Measured: writing 0x0028 while primed=0 has no effect; after data is written primed=1 and
 * writing 0x0028=1 makes state go 0->1 and samples start increasing (see the diagram comment
 *       "The streaming IP uses the start trigger, so this must be called"). */
    {
        /* Prefill: once the engine starts it pulls samples at the IQ rate, so a single
 * block in the FIFO would starve at once and emit a long run of zeros. Fill to
 * 80% of FIFO capacity (20% headroom) before writing start-on-primed. */
        /* start the read-ahead thread before any data is needed */
        size_t slotBytes = ((size_t)buffer_mb * 1024u * 1024u) / DB_SLOTS;
        if (slotBytes < 65536u) slotBytes = 65536u;
        g_db_slot_samples = slotBytes / 4;
        printf("[b] read-ahead buffer %u MB (%u slots x %zu MB)\n",
               (unsigned)buffer_mb, DB_SLOTS, slotBytes / (1024u * 1024u));
        InitializeCriticalSection(&g_db_cs);
        InitializeConditionVariable(&g_db_cv);
        g_db_rd = &reader; g_db_head = 0; g_db_off = 0; g_db_stop = 0; g_db_eof = 0;
        {
            unsigned i;
            for (i = 0; i < DB_SLOTS; ++i) {
                g_db[i].buf = (unsigned char*)malloc(slotBytes);
                g_db[i].bytes = 0;
                g_db[i].ready = 0;
                if (!g_db[i].buf) {
                    fprintf(stderr, "error: read-ahead buffer allocation failed\n");
                    return EXIT_RUNTIME;
                }
            }
        }
        readerThread = (HANDLE)_beginthreadex(NULL, 0, db_reader_main, NULL, 0, NULL);
        if (!readerThread) {
            fprintf(stderr, "error: cannot create reader thread\n");
            return EXIT_RUNTIME;
        }
        int fillBlocks = 0;
        double emptiness = 100.0;
        while (written < total && fillBlocks < 64) {
            uint32_t* buf = NULL;
            size_t got = 0, rem = 0;
            NiStatus a = acw(sess, fifo_out, &buf, chunk, timeout_ms, &got, &rem);
            if (a != 0 || got == 0) break;
            uint64_t want = total - written;
            uint32_t n = (got > want) ? (uint32_t)want : (uint32_t)got;
            size_t rd = db_take((void*)buf, n);
            if (rd > 0) written += rd;
            rlw(sess, fifo_out, got);
            ++fillBlocks;
            if (fifo_actual > 0) emptiness = 100.0 * (double)rem / (double)fifo_actual;
            if (emptiness <= 20.0) break;         /* prefill to ~80% capacity */
        }
        uint32_t pr = 0;
        r32(sess, 0x004cu, &pr);
        printf("[7c] prefill %d blocks | emptiness=%.1f%% | primed=%u | sent=%llu/%llu\n",
               fillBlocks, emptiness, (unsigned)pr,
               (unsigned long long)written, (unsigned long long)total);
        w32(sess, 0x0028u, 1u);            /* start on primed */
        {
            uint32_t st2 = 0;
            r32(sess, 0x0054u, &st2);
            printf("[7d] after start on primed: state=%u\n", (unsigned)st2);
        }
    }

    /* No diagnostic pause here: any delay after start-on-primed starves the engine
 * and is counted as underflow (a 300 ms sleep used to cost ~29.5M samples at 120 MS/s). */

    /* 8) main loop: write blocks as space becomes available (Acquire Write Region) */
    DWORD t_loop = GetTickCount();          /* playback rate is measured from here, excluding startup */
    uint32_t played0 = 0, played_prev = 0;
    DWORD t_prev = t_loop;
    r32(sess, A_OUT_SAMPLES, &played0);
    played_prev = played0;
    {
        int blocks = 0, errors = 0, stalls = 0;
        DWORD t_status2 = GetTickCount();
        while (written < total && !InterlockedCompareExchange(&g_stop, 0, 0)) {
            uint32_t* data = NULL;
            size_t got = 0, rem = 0;
            st = acw(sess, fifo_out, &data, chunk, timeout_ms, &got, &rem);
            if (st != 0 || got == 0) {
                if (st != 0) ++errors;
                /* Stall diagnostic: print registers when no writable region arrives, to avoid hanging forever. */
                if (++stalls > 300) {
                    uint32_t full = 0, samples = 0, under = 0, state = 0, minEl = 0;
                    r32(sess, A_OUT_FULLNESS, &full);
                    r32(sess, A_OUT_SAMPLES, &samples);
                    r32(sess, A_OUT_UNDERFLOWS, &under);
                    r32(sess, 0x0054u, &state);
                    r32(sess, A_OUT_MIN_ELEM, &minEl);
                    fprintf(stderr,
                            "stall: acq=0x%08X got=%zu | fullness=0x%x samples=%u under=%u"
                            " state=%u min=%u | sent %llu/%llu\n",
                            (unsigned)st, got, (unsigned)full, (unsigned)samples,
                            (unsigned)under, (unsigned)state, (unsigned)minEl,
                            (unsigned long long)written, (unsigned long long)total);
                    failed = 1;
                    break;
                }
                Sleep(1);
                continue;
            }
            stalls = 0;

            uint64_t want = total - written;
            uint32_t n = (got > want) ? (uint32_t)want : (uint32_t)got;
            /* read the file into that region; vstiq reads by sample count */
            size_t rd = db_take((void*)data, n);
            if (rd == 0) {
                fprintf(stderr, "error: file read failed\n");
                if (got) rlw(sess, fifo_out, got);
                failed = 1;
                break;
            }
            n = (uint32_t)rd;
            written += n;
            rlw(sess, fifo_out, got);
            ++blocks;

            if ((GetTickCount() - t_prev) >= 500u) {   /* time-throttled: once per 500 ms */
                /* real playback rate = delta of streaming output.samples processed (0x0038) */
                uint32_t played = 0;
                r32(sess, A_OUT_SAMPLES, &played);
                DWORD now = GetTickCount();
                DWORD dt = now - t_prev;
                uint64_t dplayed = (uint64_t)(uint32_t)(played - played_prev);   /* handle 32-bit wrap */
                double rate = dt > 0 ? (double)dplayed / ((double)dt / 1000.0) / 1e6 : 0.0;
                /* Percentage uses sent samples (= file position); rate uses the samples-processed delta
 * (underflow makes the engine produce empty samples, so it cannot be used for percentage). */
                double pct = total ? 100.0 * (double)written / (double)total : 0.0;
                if (pct > 100.0) pct = 100.0;
                double mb = (double)written * 4.0 / (1024.0 * 1024.0);
                printf("[Progress] %llu/%llu %.1f%% %.1f MB %.1f MS/s\n",
                       (unsigned long long)written, (unsigned long long)total,
                       pct, mb, rate);
                played_prev = played;
                t_prev = now;
            }
            if ((GetTickCount() - t_status2) >= 500u) {   /* time-throttled: once per 500 ms */
                uint32_t full = 0, samples = 0, under = 0;
                t_status2 = GetTickCount();
                r32(sess, A_OUT_FULLNESS, &full);
                r32(sess, A_OUT_SAMPLES, &samples);
                r32(sess, A_OUT_UNDERFLOWS, &under);
                /* Two indicators, matching the NI playback example:
 *   DMA FPGA Buffer = 100 * (1 - empty/capacity), i.e. buffer occupancy
 *   Buffer          = data currently resident in the buffer, in MB
 * The two are complements of each other; the real playback rate comes from the
 * MS/s field on the [Progress] line. */
    double emptyPct = 0.0;
                if (fifo_actual > 0) {
                    emptyPct = 100.0 * (double)rem / (double)fifo_actual;
                    if (emptyPct < 0.0) emptyPct = 0.0;
                    if (emptyPct > 100.0) emptyPct = 100.0;
                }
                /* Data currently resident in the buffer (DMA/FPGA memory),
 * i.e. capacity minus the empty elements, in MB. */
                double bufferedMB = 0.0, bufferedPct = 0.0;
                if (fifo_actual > 0) {
                    double used = (double)fifo_actual - (double)rem;
                    if (used < 0.0) used = 0.0;
                    bufferedMB  = used * 4.0 / (1024.0 * 1024.0);
                    bufferedPct = 100.0 * used / (double)fifo_actual;
                    if (bufferedPct > 100.0) bufferedPct = 100.0;
                }
                {
                    uint32_t stNow = 0;
                    r32(sess, 0x0054u, &stNow);
                    printf("    [Status] blocks=%d | DMA FPGA Buffer=%.1f%%"
                           " | Buffer=%.1f%% (%.0f MB) | state=%u"
                           " | samples %u | underflows %u\n",
                           blocks, 100.0 - emptyPct, bufferedPct, bufferedMB, (unsigned)stNow,
                           (unsigned)samples, (unsigned)under);
                }
            }
        }
        /* stop the reader thread and release the slots */
    EnterCriticalSection(&g_db_cs);
    g_db_stop = 1;
    WakeAllConditionVariable(&g_db_cv);
    LeaveCriticalSection(&g_db_cs);
    if (readerThread) {
        WaitForSingleObject(readerThread, 5000);
        CloseHandle(readerThread);
        readerThread = NULL;
    }
    {
        unsigned i;
        for (i = 0; i < DB_SLOTS; ++i) { free(g_db[i].buf); g_db[i].buf = NULL; }
    }
    DeleteCriticalSection(&g_db_cs);

    printf("[8] wrote %d blocks, %d errors, %llu samples\n",
               blocks, errors, (unsigned long long)written);
    }

    /* 9) wait for the DMA buffer to drain before stopping */
    {
        int wait_ms = 0;
        for (;;) {
            uint32_t full = 0;
            r32(sess, A_OUT_FULLNESS, &full);
            if (full == 0 || wait_ms > 5000) break;
            Sleep(50);
            wait_ms += 50;
        }
        printf("[9] DMA drain wait %d ms\n", wait_ms);
    }

cleanup:
    if (started) { w32(sess, A_OUT_ENABLE, 0u); stp(sess, fifo_out); }
    if (rfsg_open) { niRFSG_Abort(sg); niRFSG_close(sg); }
    if (opened) cl(sess, 0u);
    vstiq_close(&reader);

    {
        double secs = (double)(GetTickCount() - t0) / 1000.0;
        printf("[done] sent %llu / %llu samples | %.1f MB | %.2f s%s\n",
               (unsigned long long)written, (unsigned long long)total,
               (double)written * 4.0 / (1024.0 * 1024.0), secs,
               failed ? " (incomplete)" : "");
    }
    return failed ? EXIT_RUNTIME : EXIT_OK;
}
