/* vst_stream_record.c - streaming record, structurally identical to the NI example:
 *   data from the FPGA FIFO + niRFSA(DriverSetup) configuring RF.
 *
 * Order (measured; all steps are required):
 *   1) NiFpgaDll_Open(streaming bitfile) and keep it open
 *   2) niRFSA_InitWithOptions(..., "DriverSetup=Bitfile:NI Streaming for VST.lvbitx")
 *   3) niRFSA Configure*(AcqType / Carrier / Rate / RefLevel)
 *   4) Input Enable(enable dma / burst / reset)
 *   5) ConfigureFifo2(4, depth) + StartFifo(4)
 *   6) niRFSA_Initiate            <-- must come after FIFO configuration
 *   7) AcquireFifoReadElementsU32(fifo 4, 262144) + ReleaseFifoElements loop
 *   8) cleanup: enable dma=0, StopFifo, RFSA Abort/Close, NiFpga Close
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
#include <time.h>

#include "niRFSA.h"

#include <process.h>

#include "vst_error.h"
#include "vst_file.h"
#include "vst_ring.h"

typedef int32_t  NiStatus;
typedef uint32_t NiSession;
typedef NiStatus (*fn_open)(const char*, const char*, const char*, uint32_t, NiSession*);
typedef NiStatus (*fn_close)(NiSession, uint32_t);
typedef NiStatus (*fn_w32)(NiSession, uint32_t, uint32_t);
typedef NiStatus (*fn_r32)(NiSession, uint32_t, uint32_t*);
typedef NiStatus (*fn_cfg)(NiSession, uint32_t, size_t, size_t*);
typedef NiStatus (*fn_fifo)(NiSession, uint32_t);
typedef NiStatus (*fn_acq)(NiSession, uint32_t, uint32_t**, size_t, uint32_t, size_t*, size_t*);
typedef NiStatus (*fn_rel)(NiSession, uint32_t, size_t);

#define BITFILE \
    "C:\\Users\\Public\\Documents\\National Instruments\\FPGA Extensions Bitfiles\\" \
    "NI PXIe-5644R\\NI Streaming for VST.lvbitx"
#define SIG "F9744DDE15670B63D27A69F7CB821C89"
#define OPT_DRIVER_SETUP "DriverSetup=Bitfile:NI Streaming for VST.lvbitx"
#define READ_SIZE 262144u
#define FIFO_IN 4u

#define A_ENABLE_DMA  0x005cu
#define A_BURST_SIZE  0x0068u
#define A_INPUT_RESET 0x007cu
#define A_IN_STATE    0x0080u

enum { EXIT_OK = 0, EXIT_RUNTIME = 1, EXIT_USAGE = 2 };

static volatile LONG g_stop = 0;
static BOOL WINAPI on_ctrl(DWORD t)
{
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) { InterlockedExchange(&g_stop, 1); return TRUE; }
    return FALSE;
}

/* Writer thread: take the currently available bytes from the ring and write them.
 * Key point: do not wait for a fixed length (vst_ring_read returns only when len is full);
 * query available() first and read that much, so the producer is not throttled. */
typedef struct {
    VstRing*     ring;
    VstiqWriter* writer;
    volatile LONG stop;
    volatile LONG done;
    volatile LONG failed;
    uint64_t      written_bytes;
} WriterCtx;

static unsigned __stdcall writer_main(void* arg)
{
    WriterCtx* w = (WriterCtx*)arg;
    const size_t CHUNK = 4u * 1024u * 1024u;   /* 4 MB buffer, aligned to 4-byte samples */
    uint8_t* buf = (uint8_t*)malloc(CHUNK);
    if (!buf) { InterlockedExchange(&w->failed, 1); InterlockedExchange(&w->done, 1); return 1; }

    for (;;) {
        size_t avail = vst_ring_available(w->ring);
        if (avail == 0) {
            if (InterlockedCompareExchange(&w->stop, 0, 0)) break;
            Sleep(0);
            continue;
        }
        size_t want = avail < CHUNK ? avail : CHUNK;
        want &= ~(size_t)3;                    /* align to 4 bytes (one sample) */
        if (want == 0) { Sleep(0); continue; }

        size_t got = vst_ring_read(w->ring, buf, want, 200);
        if (got == 0) {
            if (InterlockedCompareExchange(&w->stop, 0, 0) && vst_ring_available(w->ring) == 0) break;
            continue;
        }
        size_t samples = got / 4;
        if (samples && vstiq_write(w->writer, (const void*)buf, (uint32_t)samples) != 0) {
            InterlockedExchange(&w->failed, 1);
            break;
        }
        w->written_bytes += got;
    }
    free(buf);
    InterlockedExchange(&w->done, 1);
    return 0;
}

static void usage(const char* exe)
{
    printf("usage: %s --out <file> --seconds <s> [--rate Hz] [--center Hz] [--ref dBm]\n"
           "        [--fifo-depth N] [--read-size N]\n\n"
           "Streaming record: FPGA FIFO + niRFSA(DriverSetup) configuring RF (same as the example).\n", exe);
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    const char* out_path = NULL;
    const char* resource = "RIO0";
    double seconds = 0.0, rate = 5.0e6, center = 1.0e9, ref = -10.0;
    size_t fifo_depth = 4000000u, read_size = READ_SIZE;
    uint32_t read_block_mb = 0u;        /* >0: override read_size with MB x 262144 */
    uint32_t max_file_mb = 0u;          /* >0: stop once the file reaches this size */
    /* Record direction: 16 blocks maps to the input FIFO depth 4,000,000, i.e. 250,000 per block. */
    uint32_t host_depth_blocks = 0u;
    const char* ref_clock = NULL;       /* --ref-clock: Reference Clock Source */
    const char* bitfile = BITFILE;      /* --bitfile overrides the streaming image */
    char setup[512];
    uint32_t ring_mb = 0u;              /* >0: ring buffer size in MB (default 512), --record-ring-mb */

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return EXIT_OK; }
        if (i + 1 >= argc) { fprintf(stderr, "error: %s requires a value\n", a); return EXIT_USAGE; }
        const char* v = argv[++i];
        if (!strcmp(a, "--out")) out_path = v;
        else if (!strcmp(a, "--seconds")) seconds = atof(v);
        else if (!strcmp(a, "--rate")) rate = atof(v);
        else if (!strcmp(a, "--center")) center = atof(v);
        else if (!strcmp(a, "--ref")) ref = atof(v);
        else if (!strcmp(a, "--fifo-depth")) fifo_depth = (size_t)strtoull(v, NULL, 0);
        else if (!strcmp(a, "--read-size")) read_size = (size_t)strtoull(v, NULL, 0);
        else if (!strcmp(a, "--resource")) resource = v;
        else if (!strcmp(a, "--read-block-mb")) read_block_mb = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--max-file-mb")) max_file_mb = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--host-depth-blocks")) host_depth_blocks = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--record-ring-mb")) ring_mb = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--bitfile")) bitfile = v;
        else if (!strcmp(a, "--ref-clock")) ref_clock = v;
        else { fprintf(stderr, "error: unknown option %s\n", a); return EXIT_USAGE; }
    }
    if (read_block_mb > 0) read_size = (size_t)read_block_mb * 262144u;
    if (host_depth_blocks > 0 && fifo_depth == 4000000u) {
        fifo_depth = (size_t)host_depth_blocks * 250000u;
    }
    if (!out_path || seconds <= 0.0 || rate <= 0.0 || read_size == 0) {
        fprintf(stderr, "error: --out and positive --seconds/--rate/--read-size are required\n");
        usage(argv[0]);
        return EXIT_USAGE;
    }

    HMODULE dll = LoadLibraryA("NiFpga.dll");
    if (!dll) { fprintf(stderr, "error: cannot load NiFpga.dll\n"); return EXIT_RUNTIME; }
    fn_open  op  = (fn_open)GetProcAddress(dll, "NiFpgaDll_Open");
    fn_close cl  = (fn_close)GetProcAddress(dll, "NiFpgaDll_Close");
    fn_w32   w32 = (fn_w32)GetProcAddress(dll, "NiFpgaDll_WriteU32");
    fn_r32   r32 = (fn_r32)GetProcAddress(dll, "NiFpgaDll_ReadU32");
    fn_cfg   cfg = (fn_cfg)GetProcAddress(dll, "NiFpgaDll_ConfigureFifo2");
    fn_fifo  stt = (fn_fifo)GetProcAddress(dll, "NiFpgaDll_StartFifo");
    fn_fifo  stp = (fn_fifo)GetProcAddress(dll, "NiFpgaDll_StopFifo");
    fn_acq   acq = (fn_acq)GetProcAddress(dll, "NiFpgaDll_AcquireFifoReadElementsU32");
    fn_rel   rel = (fn_rel)GetProcAddress(dll, "NiFpgaDll_ReleaseFifoElements");
    if (!op || !cl || !w32 || !r32 || !cfg || !stt || !stp || !acq || !rel) {
        fprintf(stderr, "error: NiFpga.dll missing required exports\n");
        return EXIT_RUNTIME;
    }

    NiSession sess = 0;
    size_t fifoDepthActual = fifo_depth;
    ViSession vi = VI_NULL;
    int opened = 0, rfsa_open = 0, started = 0, writer_open = 0, failed = 0;
    uint64_t written = 0;
    VstiqWriter writer;
    memset(&writer, 0, sizeof writer);
    VstRing ring;
    int ring_ready = 0;
    WriterCtx wctx;
    memset(&wctx, 0, sizeof wctx);
    HANDLE writer_thread = NULL;
    NiStatus st;
    DWORD t0 = GetTickCount();

    /* 1) open the streaming bitfile and keep it open */
    {
        const char* base = bitfile;
        const char* s;
        for (s = bitfile; *s; ++s) if (*s == '\\' || *s == '/') base = s + 1;
        snprintf(setup, sizeof setup, "DriverSetup=Bitfile:%s", base);
    }
    printf("[0] bitfile: %s\n", bitfile);
    st = op(bitfile, SIG, resource, 0x88000000u, &sess);
    printf("[1] NiFpga Open = 0x%08X session=%u\n", (unsigned)st, (unsigned)sess);
    if (st != 0) { failed = 1; goto cleanup; }
    opened = 1;

    /* 2) niRFSA init (DriverSetup makes RFSA reuse the loaded bitfile) */
    ViStatus rc = niRFSA_InitWithOptions(resource, VI_TRUE, VI_FALSE, setup, &vi);
    printf("[2] niRFSA init(DriverSetup) = 0x%08X\n", (unsigned)rc);
    if (rc != 0) { vst_check(rc, "niRFSA_InitWithOptions"); failed = 1; goto cleanup; }
    rfsa_open = 1;

    if (ref_clock) {
        ViStatus rc2 = niRFSA_SetAttributeViString(vi, "", NIRFSA_ATTR_REF_CLOCK_SOURCE, ref_clock);
        printf("[3a] reference clock source = %s (0x%08X)\n", ref_clock, (unsigned)rc2);
    }

    /* 3) RF configuration */
    if ((rc = niRFSA_ConfigureAcquisitionType(vi, NIRFSA_VAL_IQ)) != 0 ||
        (rc = niRFSA_ConfigureIQCarrierFrequency(vi, "", center)) != 0 ||
        (rc = niRFSA_ConfigureIQRate(vi, "", rate)) != 0 ||
        (rc = niRFSA_ConfigureReferenceLevel(vi, "", ref)) != 0 ||
        (rc = niRFSA_ConfigureNumberOfSamples(vi, "", VI_TRUE, (ViInt64)read_size)) != 0) {
        vst_check(rc, "niRFSA Configure*");
        failed = 1;
        goto cleanup;
    }
    printf("[3] RF configured: %.1f MS/s @ %.3f GHz, ref %.1f dBm\n", rate / 1e6, center / 1e9, ref);

    /* output container */
    VstiqHeader header;
    memset(&header, 0, sizeof header);
    header.version = 1u;
    header.header_bytes = VSTIQ_HEADER_BYTES;
    header.sample_format = VSTIQ_FORMAT_I16IQ;
    {
        uint64_t want = (uint64_t)(rate * seconds);
        if (max_file_mb > 0) {
            uint64_t cap = (uint64_t)max_file_mb * 1024u * 1024u / 4u;   /* bytes -> complex samples */
            if (cap < want) want = cap;
        }
        header.total_samples = want;
    }
    header.iq_rate_hz = rate;
    header.center_freq_hz = center;
    header.ref_level_dbm = ref;
    header.start_utc_seconds = (uint64_t)time(NULL);
    snprintf(header.resource, sizeof header.resource, "%s", resource);
    snprintf(header.device_model, sizeof header.device_model, "%s", "PXIe-5644R");
    snprintf(header.note, sizeof header.note, "%s", "fpga fifo + rfsa driversetup");
    if (vstiq_create(out_path, &header, &writer) != 0) { failed = 1; goto cleanup; }
    writer_open = 1;

    {
        size_t ring_bytes = 512u * 1024u * 1024u;
        if (ring_mb > 0) ring_bytes = (size_t)ring_mb * 1024u * 1024u;
    if (vst_ring_init(&ring, ring_bytes) != 0) {
            fprintf(stderr, "error: ring buffer allocation failed\n");
            failed = 1;
            goto cleanup;
        }
        ring_ready = 1;
        wctx.ring = &ring;
        wctx.writer = &writer;
        writer_thread = (HANDLE)_beginthreadex(NULL, 0, writer_main, &wctx, 0, NULL);
        if (!writer_thread) {
            fprintf(stderr, "error: cannot create writer thread\n");
            failed = 1;
            goto cleanup;
        }
        printf("[ring] ring buffer %zu MB + writer thread started\n", ring_bytes / (1024u * 1024u));
    }

    /* 4) Input Enable */
    if ((st = w32(sess, A_ENABLE_DMA, 1u)) != 0 ||
        (st = w32(sess, A_BURST_SIZE, 0u)) != 0 ||
        (st = w32(sess, A_INPUT_RESET, 1u)) != 0) {
        fprintf(stderr, "error: Input Enable failed 0x%08X\n", (unsigned)st);
        failed = 1;
        goto cleanup;
    }
    w32(sess, A_INPUT_RESET, 0u);

    /* 5) FIFO configuration */
    {
        size_t actual = 0;
        if ((st = cfg(sess, FIFO_IN, fifo_depth, &actual)) != 0) {
            fprintf(stderr, "error: ConfigureFifo2 failed 0x%08X\n", (unsigned)st);
            failed = 1;
            goto cleanup;
        }
        printf("[4] ConfigureFifo2 fifo=%u requested=%zu actual=%zu\n", FIFO_IN, fifo_depth, actual);
        fifoDepthActual = actual ? actual : fifo_depth;
        /* The request must not exceed the FIFO depth (Acquire errors otherwise: 2542 failures, 0 samples). */
        if (read_size > fifoDepthActual) {
            printf("     read_size %zu > FIFO depth %zu, clamping to depth\n", read_size, fifoDepthActual);
            read_size = fifoDepthActual;
        }
        if ((st = stt(sess, FIFO_IN)) != 0) {
            fprintf(stderr, "error: StartFifo failed 0x%08X\n", (unsigned)st);
            failed = 1;
            goto cleanup;
        }
        started = 1;
    }

    /* 6) Initiate - must come after FIFO configuration */
    rc = niRFSA_Initiate(vi);
    printf("[5] niRFSA Initiate = 0x%08X\n", (unsigned)rc);
    if (rc != 0) { vst_check(rc, "niRFSA_Initiate"); failed = 1; goto cleanup; }

    {
        uint32_t state = 0;
        r32(sess, A_IN_STATE, &state);
        printf("[6] input.state=%u\n", (unsigned)state);
    }

    /* 7) FIFO acquisition loop */
    DWORD tAcq = GetTickCount();      /* acquisition window start (after Initiate) */
    DWORD tAcqEnd = 0;                /* acquisition window end, before cleanup/disk flush */
    DWORD tTail = 0;                  /* cleanup start: measures the disk write-back */
    {
        const uint64_t target = header.total_samples;
        DWORD deadline = t0 + (DWORD)(seconds * 3000.0) + 5000u;
        int acquires = 0, errors = 0;
        DWORD t_status = GetTickCount();
        while (written < target && !InterlockedCompareExchange(&g_stop, 0, 0)) {
            if (GetTickCount() > deadline) {
                fprintf(stderr, "warning: timed out, captured %llu / %llu\n",
                        (unsigned long long)written, (unsigned long long)target);
                failed = 1;
                break;
            }
            uint32_t* data = NULL;
            size_t got = 0, rem = 0;
            st = acq(sess, FIFO_IN, &data, read_size, 200u, &got, &rem);
            if (st != 0) { ++errors; Sleep(1); continue; }
            ++acquires;
            if (got == 0) { Sleep(1); continue; }
            uint64_t want = target - written;
            uint32_t n = (got > want) ? (uint32_t)want : (uint32_t)got;
            {
                size_t bytes = (size_t)n * 4u;
                size_t off = 0;
                while (off < bytes) {
                    size_t put = vst_ring_write(&ring, (const uint8_t*)data + off, bytes - off, 1000);
                    if (put == 0) {
                        if (InterlockedCompareExchange(&wctx.failed, 0, 0)) {
                            fprintf(stderr, "error: writer thread failed\n");
                            failed = 1;
                            break;
                        }
                        if (InterlockedCompareExchange(&g_stop, 0, 0)) { failed = 1; break; }
                        continue;
                    }
                    off += put;
                }
                if (failed) { rel(sess, FIFO_IN, got); break; }
            }
            written += n;
            rel(sess, FIFO_IN, got);

            /* Periodically read FIFO status (the example Monitor Buffer Fullness does the same). */
            if ((GetTickCount() - t_status) >= 500u) {   /* time-throttled: once per 500 ms */
                uint32_t emptyElem = 0, minElem = 0, st2 = 0;
                r32(sess, 0x0024u, &emptyElem);
                r32(sess, 0x0020u, &minElem);
                r32(sess, 0x0080u, &st2);
                /* 0x24 is a 16-bit counter (saturates at 65535); normalize by 65535 for occupancy. */
                double occupancy = 100.0 * (1.0 - (double)emptyElem / 65535.0);
                if (occupancy < 0.0) occupancy = 0.0;
                if (occupancy > 100.0) occupancy = 100.0;
                {
                    double used = (double)vst_ring_available(&ring);
                    double pct = ring.cap ? 100.0 * used / (double)ring.cap : 0.0;
                    printf("    [Status] blocks=%d | DMA FPGA Buffer=%.1f%% | Buffer=%.1f%% (%.0f MB) | state=%u\n",
                           acquires, occupancy, pct, used / (1024.0 * 1024.0), (unsigned)st2);
                    t_status = GetTickCount();
                }
            }
        }
        printf("[7] %d acquisitions, %d errors\n", acquires, errors);
    }

cleanup:
    /* End of the acquisition window: stop counting here, so the reported rate
 * reflects capture only and not the disk write-back that follows. */
    if (!tAcqEnd) { tAcqEnd = GetTickCount(); tTail = tAcqEnd; }
    if (opened) w32(sess, A_ENABLE_DMA, 0u);
    if (writer_thread) {
        DWORD tw = GetTickCount();
        InterlockedExchange(&wctx.stop, 1);
        vst_ring_close(&ring);
        /* Give feedback while the ring drains to disk: on slow volumes this can take
 * seconds and the user would otherwise see nothing until [done]. */
        {
            size_t lastLeft = (size_t)-1;
            int    stalledFor = 0;
            for (;;) {
                DWORD w = WaitForSingleObject(writer_thread, 500);
                DWORD ex = 0;
                if (w == WAIT_OBJECT_0) break;
                if (GetExitCodeThread(writer_thread, &ex) && ex != STILL_ACTIVE) break;
                {
                    size_t left = vst_ring_available(&ring);
                    double el = (double)(GetTickCount() - tw) / 1000.0;
                    printf("    [flush] %.1f s | %zu MB still being written to disk\n",
                           el, left / (1024u * 1024u));
                    fflush(stdout);
                    /* If nothing moves for 10 s the volume is not accepting writes;
 * stop waiting instead of hanging forever. */
                    if (left == lastLeft) { if (++stalledFor >= 20) {
                        printf("    [flush] stalled %d s at %zu MB: giving up on this write-back\n",
                               stalledFor / 2, left / (1024u * 1024u));
                        fflush(stdout);
                        break;
                    } } else { stalledFor = 0; lastLeft = left; }
                    if (el > 120.0) {
                        printf("    [flush] 120 s timeout, %zu MB not written\n", left / (1024u * 1024u));
                        fflush(stdout);
                        break;
                    }
                }
            }
        }
        CloseHandle(writer_thread);
        writer_thread = NULL;
        if (InterlockedCompareExchange(&wctx.failed, 0, 0)) failed = 1;
    }
    if (ring_ready) { vst_ring_free(&ring); ring_ready = 0; }
    if (started) stp(sess, FIFO_IN);
    if (rfsa_open) { niRFSA_Abort(vi); niRFSA_close(vi); }
    if (opened) cl(sess, 0u);
    if (writer_open) {
        if (!failed && written > 0) { if (vstiq_finish(&writer) != 0) failed = 1; }
        else vstiq_abort(&writer);
    }

    {
        double secs = (double)(GetTickCount() - t0) / 1000.0;
        double acq = (double)(tAcqEnd - tAcq) / 1000.0;
        double mb = (double)written * 4.0 / (1024.0 * 1024.0);
        printf("[done] samples %llu / %llu | %.1f MB | %.2f s%s\n",
               (unsigned long long)written,
               (unsigned long long)header.total_samples,
               mb, secs, failed ? " (target not reached)" : "");
        printf("    tail %.2f s (disk write-back) | startup %.2f s\n",
               (double)(GetTickCount() - tTail) / 1000.0,
               (double)(tAcq - t0) / 1000.0);
        printf("    acquisition %.2f s -> %.1f MS/s, %.0f MB/s\n",
               acq, acq > 0 ? (double)written / acq / 1e6 : 0.0,
               acq > 0 ? mb / acq : 0.0);
    }
    return failed ? EXIT_RUNTIME : EXIT_OK;
}
