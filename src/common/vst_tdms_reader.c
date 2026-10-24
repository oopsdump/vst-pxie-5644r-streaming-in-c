/* Minimal TDMS reader: supports only single-segment, single-channel, property-free files
 * (i.e. what vst_tdms.c writes and what the NI example records).
 *
 * The returned buffer holds raw u32 data; one u32 = one complex sample (I16 low half, Q16 high half),
 * equivalent to the VSTIQ interleaved int16 layout and usable directly by the RFSG ARB API. */
#include "vst_tdms.h"

#include <stdlib.h>
#include <string.h>

/* Read a TDMS file, allocate *out_bytes (caller frees), return 0 on success. */
int tdms_read_all(const char* path, uint8_t** out_bytes, uint64_t* out_samples)
{
    *out_bytes = NULL;
    *out_samples = 0;

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open TDMS file %s\n", path);
        return -1;
    }

    uint8_t lead[28];
    if (fread(lead, 1, sizeof lead, fp) != sizeof lead || memcmp(lead, "TDSm", 4) != 0) {
        fprintf(stderr, "error: not a TDMS file (missing TDSm tag)\n");
        fclose(fp);
        return -1;
    }

    uint64_t raw_off = 0;
    memcpy(&raw_off, lead + 20, sizeof raw_off);

    uint32_t objects = 0;
    if (fread(&objects, sizeof objects, 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    uint64_t data_samples = 0;
    int found = 0;

    for (uint32_t i = 0; i < objects; ++i) {
        uint32_t path_len = 0;
        if (fread(&path_len, sizeof path_len, 1, fp) != 1) { break; }
        if (path_len > 4096u) { break; }
        char pathbuf[4097];
        if (path_len > 0 && fread(pathbuf, 1, path_len, fp) != path_len) { break; }
        pathbuf[path_len] = '\0';

        uint32_t rdi = 0;
        if (fread(&rdi, sizeof rdi, 1, fp) != 1) { break; }

        if (rdi != 0xFFFFFFFFu) {
            uint32_t type = 0, dim = 0;
            uint64_t count = 0;
            if (fread(&type, sizeof type, 1, fp) != 1 ||
                fread(&dim, sizeof dim, 1, fp) != 1 ||
                fread(&count, sizeof count, 1, fp) != 1) {
                break;
            }
            if (type == 7u && dim == 1u) {   /* u32, one-dimensional: exactly the iq channel */
                data_samples = count;
                found = 1;
            }
        }

        uint32_t props = 0;
        if (fread(&props, sizeof props, 1, fp) != 1) { break; }
        for (uint32_t k = 0; k < props; ++k) {
            uint32_t name_len = 0, vtype = 0;
            if (fread(&name_len, sizeof name_len, 1, fp) != 1 || name_len > 4096u) { break; }
            if (name_len > 0 && fseek(fp, (long)name_len, SEEK_CUR) != 0) { break; }
            if (fread(&vtype, sizeof vtype, 1, fp) != 1) { break; }
            /* Only our own files are read: properties are skipped, not parsed. */
            size_t vlen = 8;                 /* conservatively skip 8 bytes */
            if (vtype == 0x20u) {            /* string: read the length first */
                uint32_t slen = 0;
                if (fread(&slen, sizeof slen, 1, fp) != 1) { break; }
                vlen = slen;
            }
            if (vlen > 0 && fseek(fp, (long)vlen, SEEK_CUR) != 0) { break; }
        }
    }

    if (!found || data_samples == 0) {
        fprintf(stderr, "error: no u32 iq channel data found in the TDMS file\n");
        fclose(fp);
        return -1;
    }
    if (data_samples > (uint64_t)SIZE_MAX / 4u) {
        fclose(fp);
        return -1;
    }

    /* data start = 28 bytes after lead-in + rawDataOffset (relative to the metadata start) */
    long data_pos = (long)(28u + raw_off);
    if (fseek(fp, data_pos, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    size_t bytes = (size_t)data_samples * 4u;
    uint8_t* buf = (uint8_t*)malloc(bytes);
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "error: failed to allocate %zu bytes\n", bytes);
        return -1;
    }
    if (fread(buf, 1, bytes, fp) != bytes) {
        fprintf(stderr, "warning: TDMS data area is short; playing back what was actually read\n");
        /* trim to the whole number of samples actually read */
        long got = ftell(fp);
        (void)got;
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    *out_bytes = buf;
    *out_samples = data_samples;
    return 0;
}
