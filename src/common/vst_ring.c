#include "vst_ring.h"

#include <stdlib.h>
#include <string.h>

int vst_ring_init(VstRing* ring, size_t capacity_bytes)
{
    if (!ring || capacity_bytes < 4096) {
        return -1;
    }
    memset(ring, 0, sizeof *ring);
    ring->buf = (uint8_t*)malloc(capacity_bytes);
    if (!ring->buf) {
        return -1;
    }
    ring->cap = capacity_bytes;
    InitializeCriticalSection(&ring->cs);
    InitializeConditionVariable(&ring->not_empty);
    InitializeConditionVariable(&ring->not_full);
    return 0;
}

void vst_ring_free(VstRing* ring)
{
    if (!ring) {
        return;
    }
    free(ring->buf);
    ring->buf = NULL;
    DeleteCriticalSection(&ring->cs);
}

size_t vst_ring_available(const VstRing* ring)
{
    return ring ? ring->used : 0;
}

static size_t write_locked(VstRing* ring, const uint8_t* src, size_t len)
{
    size_t space = ring->cap - ring->used;
    if (len > space) {
        len = space;
    }
    size_t first = ring->cap - ring->tail;
    if (first > len) {
        first = len;
    }
    memcpy(ring->buf + ring->tail, src, first);
    if (len > first) {
        memcpy(ring->buf, src + first, len - first);
    }
    ring->tail = (ring->tail + len) % ring->cap;
    ring->used += len;
    return len;
}

static size_t read_locked(VstRing* ring, uint8_t* dst, size_t len)
{
    if (len > ring->used) {
        len = ring->used;
    }
    size_t first = ring->cap - ring->head;
    if (first > len) {
        first = len;
    }
    memcpy(dst, ring->buf + ring->head, first);
    if (len > first) {
        memcpy(dst + first, ring->buf, len - first);
    }
    ring->head = (ring->head + len) % ring->cap;
    ring->used -= len;
    return len;
}

size_t vst_ring_write(VstRing* ring, const void* data, size_t len, DWORD timeout_ms)
{
    if (!ring || !data || len == 0) {
        return 0;
    }
    const uint8_t* src = (const uint8_t*)data;
    size_t total = 0;
    DWORD start = GetTickCount();

    EnterCriticalSection(&ring->cs);
    while (total < len && !ring->closed) {
        size_t n = write_locked(ring, src + total, len - total);
        total += n;
        if (n > 0) {
            WakeAllConditionVariable(&ring->not_empty);
        }
        if (total == len) {
            break;
        }
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) {
            break;
        }
        SleepConditionVariableCS(&ring->not_full, &ring->cs, timeout_ms - elapsed);
    }
    if (total < len) {
        ring->overruns++;
    }
    LeaveCriticalSection(&ring->cs);
    return total;
}

size_t vst_ring_read(VstRing* ring, void* dst, size_t len, DWORD timeout_ms)
{
    if (!ring || !dst || len == 0) {
        return 0;
    }
    uint8_t* out = (uint8_t*)dst;
    size_t total = 0;
    DWORD start = GetTickCount();

    EnterCriticalSection(&ring->cs);
    while (total < len) {
        size_t n = read_locked(ring, out + total, len - total);
        total += n;
        if (n > 0) {
            WakeAllConditionVariable(&ring->not_full);
        }
        if (total == len) {
            break;
        }
        /* Closed means "no more data will arrive", not "discard what is queued":
         * drain the remainder first, then stop. */
        if (ring->closed) {
            break;
        }
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) {
            break;
        }
        SleepConditionVariableCS(&ring->not_empty, &ring->cs, timeout_ms - elapsed);
    }
    if (total < len) {
        ring->underruns++;
    }
    LeaveCriticalSection(&ring->cs);
    return total;
}

void vst_ring_close(VstRing* ring)
{
    if (!ring) {
        return;
    }
    EnterCriticalSection(&ring->cs);
    ring->closed = 1;
    WakeAllConditionVariable(&ring->not_empty);
    WakeAllConditionVariable(&ring->not_full);
    LeaveCriticalSection(&ring->cs);
}
