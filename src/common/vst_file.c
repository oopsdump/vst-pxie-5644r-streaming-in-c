#include "vst_file.h"

#include <stdlib.h>
#include <string.h>

#define SAMPLES_TO_BYTES(n) ((size_t)(n) * 4u)

static const uint32_t kCrcTable[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

/* Nibble lookup instead of a 256-entry table; CRC is only computed when opening/closing files. */
uint32_t vstiq_crc32(uint32_t seed, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 4) ^ kCrcTable[(crc ^ p[i]) & 0x0F];
        crc = (crc >> 4) ^ kCrcTable[(crc ^ (p[i] >> 4)) & 0x0F];
    }
    return crc ^ 0xFFFFFFFFu;
}

static void put_u16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static void put_f64(uint8_t* p, double v)   { memcpy(p, &v, 8); }

static uint16_t get_u16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get_u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get_u64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static double   get_f64(const uint8_t* p) { double v;   memcpy(&v, p, 8); return v; }

static void copy_str(char* dst, size_t cap, const char* src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
    }
    if (n) {
        memcpy(dst, src, n);
    }
    memset(dst + n, 0, cap - n);
}

/* The header is serialized by fixed offsets: struct layout varies by compiler, so raw structs must not be written. */
static void header_pack(const VstiqHeader* h, uint8_t* buf)
{
    memset(buf, 0, VSTIQ_HEADER_BYTES);
    memcpy(buf + 0, VSTIQ_MAGIC, 8);
    put_u32(buf + 8, h->version);
    put_u32(buf + 12, h->header_bytes ? h->header_bytes : VSTIQ_HEADER_BYTES);
    put_u16(buf + 16, (uint16_t)h->sample_format);
    put_u16(buf + 18, h->flags);
    put_u32(buf + 20, 0);
    put_u64(buf + 24, h->total_samples);
    put_f64(buf + 32, h->iq_rate_hz);
    put_f64(buf + 40, h->center_freq_hz);
    put_f64(buf + 48, h->ref_level_dbm);
    put_f64(buf + 56, h->full_scale_dbfs);
    put_u64(buf + 64, h->start_utc_seconds);
    memcpy(buf + 72, h->resource, VSTIQ_NAME_BYTES);
    memcpy(buf + 136, h->device_model, VSTIQ_NAME_BYTES);
    memcpy(buf + 200, h->note, VSTIQ_NOTE_BYTES);
    put_u32(buf + 328, 0);   /* the CRC field is treated as 0 while computing */
    uint32_t crc = vstiq_crc32(0, buf, 328);
    put_u32(buf + 328, crc);
}

static int header_unpack(const uint8_t* buf, VstiqHeader* h, int* crc_ok)
{
    if (memcmp(buf, VSTIQ_MAGIC, 8) != 0) {
        return 0;
    }
    h->version          = get_u32(buf + 8);
    h->header_bytes     = get_u32(buf + 12);
    h->sample_format    = (int16_t)get_u16(buf + 16);
    h->flags            = get_u16(buf + 18);
    h->total_samples    = get_u64(buf + 24);
    h->iq_rate_hz       = get_f64(buf + 32);
    h->center_freq_hz   = get_f64(buf + 40);
    h->ref_level_dbm    = get_f64(buf + 48);
    h->full_scale_dbfs  = get_f64(buf + 56);
    h->start_utc_seconds = get_u64(buf + 64);
    memcpy(h->resource, buf + 72, VSTIQ_NAME_BYTES);
    memcpy(h->device_model, buf + 136, VSTIQ_NAME_BYTES);
    memcpy(h->note, buf + 200, VSTIQ_NOTE_BYTES);
    h->resource[VSTIQ_NAME_BYTES - 1] = '\0';
    h->device_model[VSTIQ_NAME_BYTES - 1] = '\0';
    h->note[VSTIQ_NOTE_BYTES - 1] = '\0';
    h->header_crc32 = get_u32(buf + 328);

    uint8_t tmp[VSTIQ_HEADER_BYTES];
    memcpy(tmp, buf, VSTIQ_HEADER_BYTES);
    put_u32(tmp + 328, 0);
    *crc_ok = (vstiq_crc32(0, tmp, 328) == h->header_crc32);
    return 1;
}

int vstiq_create(const char* path, const VstiqHeader* header, VstiqWriter* writer)
{
    if (!path || !header || !writer) {
        return -1;
    }
    memset(writer, 0, sizeof *writer);
    writer->fp = fopen(path, "wb");
    if (!writer->fp) {
        fprintf(stderr, "error: cannot create file %s\n", path);
        return -1;
    }
    writer->header = *header;
    writer->header.version = 1;
    writer->header.header_bytes = VSTIQ_HEADER_BYTES;
    writer->header.sample_format = VSTIQ_FORMAT_I16IQ;
    writer->header.full_scale_dbfs = 0.0;

    /* totalSamples is unknown while recording; set the flag and let vstiq_finish fill it in. */
    writer->header.total_samples = 0;
    writer->header.flags |= VSTIQ_FLAG_TOTAL_UNKNOWN;

    uint8_t buf[VSTIQ_HEADER_BYTES];
    header_pack(&writer->header, buf);
    if (fwrite(buf, 1, sizeof buf, writer->fp) != sizeof buf) {
        fprintf(stderr, "error: writing the file header failed\n");
        fclose(writer->fp);
        writer->fp = NULL;
        return -1;
    }
    return 0;
}

int vstiq_write(VstiqWriter* writer, const void* iq, uint32_t samples)
{
    if (!writer || !writer->fp || samples == 0) {
        return 0;
    }
    size_t bytes = SAMPLES_TO_BYTES(samples);
    if (fwrite(iq, 1, bytes, writer->fp) != bytes) {
        fprintf(stderr, "error: writing data failed (%llu samples written)\n",
                (unsigned long long)writer->samples_written);
        return -1;
    }
    writer->samples_written += samples;
    return 0;
}

int vstiq_finish(VstiqWriter* writer)
{
    if (!writer || !writer->fp) {
        return -1;
    }
    writer->header.total_samples = writer->samples_written;
    writer->header.flags &= (uint16_t)~VSTIQ_FLAG_TOTAL_UNKNOWN;

    uint8_t buf[VSTIQ_HEADER_BYTES];
    header_pack(&writer->header, buf);
    if (fseek(writer->fp, 0, SEEK_SET) != 0 ||
        fwrite(buf, 1, sizeof buf, writer->fp) != sizeof buf) {
        fprintf(stderr, "error: back-filling the file header failed\n");
        fclose(writer->fp);
        writer->fp = NULL;
        return -1;
    }
    if (fclose(writer->fp) != 0) {
        writer->fp = NULL;
        return -1;
    }
    writer->fp = NULL;
    return 0;
}

void vstiq_abort(VstiqWriter* writer)
{
    if (writer && writer->fp) {
        fclose(writer->fp);
        writer->fp = NULL;
    }
}

int vstiq_open(const char* path, VstiqReader* reader)
{
    if (!path || !reader) {
        return -1;
    }
    memset(reader, 0, sizeof *reader);
    reader->fp = fopen(path, "rb");
    if (!reader->fp) {
        fprintf(stderr, "error: cannot open file %s\n", path);
        return -1;
    }

    uint8_t buf[VSTIQ_HEADER_BYTES];
    if (fread(buf, 1, sizeof buf, reader->fp) != sizeof buf) {
        fprintf(stderr, "error: file too short to contain a header\n");
        fclose(reader->fp);
        reader->fp = NULL;
        return -1;
    }
    int crc_ok = 0;
    if (!header_unpack(buf, &reader->header, &crc_ok)) {
        fprintf(stderr, "error: %s is not a VSTIQ file\n", path);
        fclose(reader->fp);
        reader->fp = NULL;
        return -1;
    }

    long long file_size = -1;
    if (fseek(reader->fp, 0, SEEK_END) == 0) {
        file_size = _ftelli64(reader->fp);
    }
    /* Data starts at headerBytes from the header, leaving room for future extended headers. */
    uint32_t hb = reader->header.header_bytes;
    if (hb < VSTIQ_HEADER_BYTES) {
        fprintf(stderr, "error: header headerBytes=%u is smaller than the minimum %u\n",
                (unsigned)hb, VSTIQ_HEADER_BYTES);
        fclose(reader->fp);
        reader->fp = NULL;
        return -1;
    }
    uint64_t data_bytes = (file_size > (long long)hb) ? (uint64_t)file_size - hb : 0;
    uint64_t by_length = data_bytes / 4u;
    reader->data_bytes = data_bytes;

    /* Header CRC invalid or still recording -> use the actual file length and tell the caller. */
    int head_untrusted = (!crc_ok) || (reader->header.flags & VSTIQ_FLAG_TOTAL_UNKNOWN);
    if (head_untrusted) {
        reader->recovered = 1;
        reader->total_samples = by_length;
        reader->truncated = (data_bytes % 4u) != 0;
        fprintf(stderr, "warning: header untrusted (%s), deriving sample count %llu from the file length\n",
                crc_ok ? "still recording" : "CRC mismatch",
                (unsigned long long)by_length);
    } else {
        reader->total_samples = reader->header.total_samples;
        if (reader->total_samples > by_length) {
            fprintf(stderr, "warning: header declares %llu samples but only %llu exist; reading the actual amount\n",
                    (unsigned long long)reader->total_samples,
                    (unsigned long long)by_length);
            reader->total_samples = by_length;
            reader->truncated = 1;
        } else if (data_bytes % 4u) {
            /* Header trusted but the data area has a partial sample: warn and ignore it. */
            fprintf(stderr, "warning: %llu trailing bytes (less than 4) ignored in the data area\n",
                    (unsigned long long)(data_bytes % 4u));
            reader->truncated = 1;
        }
    }

    if (fseek(reader->fp, (long)hb, SEEK_SET) != 0) {
        fclose(reader->fp);
        reader->fp = NULL;
        return -1;
    }
    return 0;
}

size_t vstiq_read(VstiqReader* reader, void* buf, size_t samples)
{
    if (!reader || !reader->fp || !buf || samples == 0) {
        return 0;
    }
    uint64_t remain = reader->total_samples - reader->samples_read;
    if (samples > remain) {
        samples = (size_t)remain;
    }
    size_t got = fread(buf, 4, samples, reader->fp);
    reader->samples_read += got;
    return got;
}

void vstiq_close(VstiqReader* reader)
{
    if (reader && reader->fp) {
        fclose(reader->fp);
        reader->fp = NULL;
    }
}
