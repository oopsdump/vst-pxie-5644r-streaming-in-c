/* Minimal TDMS writer: same layout as the NI example recordings (group "vst" / channel "iq" / u32).
 *
 * The layout follows the actual byte structure produced by nptdms (verified against nptdms output):
 *   Lead In(28B): "TDSm" | ToC=0x0E | version=4712 | nextSegmentOffset u64 | rawDataOffset u64
 *   Metadata: numberOfObjects u32
 *     per object: pathLen u32 | path (**no 4-byte padding**) | rawDataIndex u32
 *       if rawDataIndex != 0xFFFFFFFF: dataType u32 | dimension u32 | numberOfValues u64
 *       properties: count u32
 *   raw data follows.
 *
 * The sample count is unknown up front, so placeholders are written and fixed up on close. */
#include "vst_tdms.h"

#include <string.h>

static const uint32_t TDMS_TYPE_U32 = 7u;
static const uint32_t TDMS_NO_DATA = 0xFFFFFFFFu;
static const uint32_t TDMS_INDEX_LEN = 0x14u;   /* type + dimension + count + props = 20 */

static int write_u32(FILE* fp, uint32_t v) { return fwrite(&v, sizeof v, 1, fp) == 1 ? 0 : -1; }
static int write_u64(FILE* fp, uint64_t v) { return fwrite(&v, sizeof v, 1, fp) == 1 ? 0 : -1; }

/* path: length + bytes, no padding (matches nptdms output) */
static int write_path(FILE* fp, const char* path)
{
    uint32_t len = (uint32_t)strlen(path);
    if (write_u32(fp, len) != 0) {
        return -1;
    }
    if (len > 0 && fwrite(path, 1, len, fp) != len) {
        return -1;
    }
    return 0;
}

int tdms_create(const char* path, TdmsWriter* w, uint64_t expected_samples)
{
    memset(w, 0, sizeof *w);
    w->fp = fopen(path, "wb+");
    if (!w->fp) {
        return -1;
    }

    if (fwrite("TDSm", 4, 1, w->fp) != 1) { goto fail; }
    if (write_u32(w->fp, 0x0Eu) != 0) { goto fail; }      /* metadata + raw data + interleaved */
    if (write_u32(w->fp, 4712u) != 0) { goto fail; }
    w->next_seg_pos = (uint64_t)ftell(w->fp);
    if (write_u64(w->fp, 0u) != 0) { goto fail; }
    uint64_t raw_off_pos = (uint64_t)ftell(w->fp);
    if (write_u64(w->fp, 0u) != 0) { goto fail; }

    if (write_u32(w->fp, 3u) != 0) { goto fail; }

    /* root object: path "/" */
    if (write_path(w->fp, "/") != 0) { goto fail; }
    if (write_u32(w->fp, TDMS_NO_DATA) != 0) { goto fail; }
    if (write_u32(w->fp, 0u) != 0) { goto fail; }

    /* group object */
    if (write_path(w->fp, "/'vst'") != 0) { goto fail; }
    if (write_u32(w->fp, TDMS_NO_DATA) != 0) { goto fail; }
    if (write_u32(w->fp, 0u) != 0) { goto fail; }

    /* channel object */
    if (write_path(w->fp, "/'vst'/'iq'") != 0) { goto fail; }
    uint64_t index_field_pos = (uint64_t)ftell(w->fp);
    if (write_u32(w->fp, TDMS_INDEX_LEN) != 0) { goto fail; }
    if (write_u32(w->fp, TDMS_TYPE_U32) != 0) { goto fail; }
    if (write_u32(w->fp, 1u) != 0) { goto fail; }
    w->count_pos = (uint64_t)ftell(w->fp);
    if (write_u64(w->fp, expected_samples) != 0) { goto fail; }
    if (write_u32(w->fp, 0u) != 0) { goto fail; }

    /* rawDataOffset is relative to the metadata start (28 bytes after lead-in) */
    uint64_t data_start = (uint64_t)ftell(w->fp);
    if (fseek(w->fp, (long)raw_off_pos, SEEK_SET) != 0) { goto fail; }
    if (write_u64(w->fp, data_start - 28u) != 0) { goto fail; }
    if (fseek(w->fp, 0L, SEEK_END) != 0) { goto fail; }

    w->samples = 0;
    return 0;

fail:
    fclose(w->fp);
    w->fp = NULL;
    return -1;
}

int tdms_write(TdmsWriter* w, const void* iq, uint32_t samples)
{
    if (!w->fp || samples == 0) {
        return w->fp ? 0 : -1;
    }
    size_t bytes = (size_t)samples * 4u;
    if (fwrite(iq, 1, bytes, w->fp) != bytes) {
        return -1;
    }
    w->samples += samples;
    return 0;
}

int tdms_finish(TdmsWriter* w)
{
    if (!w->fp) {
        return -1;
    }
    long end = ftell(w->fp);
    if (end < 0) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    if (fseek(w->fp, (long)w->count_pos, SEEK_SET) != 0 ||
        write_u64(w->fp, w->samples) != 0) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    if (fseek(w->fp, (long)w->next_seg_pos, SEEK_SET) != 0 ||
        write_u64(w->fp, (uint64_t)end) != 0) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    if (fseek(w->fp, end, SEEK_SET) != 0) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    int rc = fflush(w->fp) == 0 ? 0 : -1;
    if (fclose(w->fp) != 0) {
        rc = -1;
    }
    w->fp = NULL;
    return rc;
}

void tdms_abort(TdmsWriter* w)
{
    if (w->fp) {
        fclose(w->fp);
        w->fp = NULL;
    }
}
