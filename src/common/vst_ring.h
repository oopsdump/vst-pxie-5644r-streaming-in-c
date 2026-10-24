/* Single-producer/single-consumer byte ring: the only channel between capture and writer threads. */
#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t*           buf;
    size_t             cap;
    size_t             head;    /* read position */
    size_t             tail;    /* write position */
    size_t             used;
    int                closed;
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
    uint64_t           overruns;    /* write-side timeouts: disk cannot keep up */
    uint64_t           underruns;   /* read-side timeouts: producer too slow */
} VstRing;

int    vst_ring_init(VstRing* ring, size_t capacity_bytes);
void   vst_ring_free(VstRing* ring);

/* Write as much as possible; returns the bytes written (may be < len). Returns 0 on timeout and bumps overruns. */
size_t vst_ring_write(VstRing* ring, const void* data, size_t len, DWORD timeout_ms);

/* Returns only when len bytes are available; on timeout returns what was read (maybe 0) and bumps underruns. */
size_t vst_ring_read(VstRing* ring, void* dst, size_t len, DWORD timeout_ms);

size_t vst_ring_available(const VstRing* ring);

/* Wake all waiters so both sides notice shutdown. */
void vst_ring_close(VstRing* ring);
