/* VSTIQ v1 container reader/writer. Field offsets documented in the source comments. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define VSTIQ_MAGIC         "VSTIQ001"
#define VSTIQ_HEADER_BYTES  512u
#define VSTIQ_NAME_BYTES    64u
#define VSTIQ_NOTE_BYTES    128u
#define VSTIQ_FORMAT_I16IQ  1

enum { VSTIQ_FLAG_TOTAL_UNKNOWN = 1u << 0 };

typedef struct {
    uint32_t version;
    uint32_t header_bytes;       /* Offset where data starts; fixed at 512 in v1, leaving room for extended headers */
    int16_t  sample_format;
    uint16_t flags;
    uint64_t total_samples;      /* number of complex samples; 1 sample = 4 bytes */
    double   iq_rate_hz;
    double   center_freq_hz;
    double   ref_level_dbm;
    double   full_scale_dbfs;
    uint64_t start_utc_seconds;
    char     resource[VSTIQ_NAME_BYTES];
    char     device_model[VSTIQ_NAME_BYTES];
    char     note[VSTIQ_NOTE_BYTES];
    uint32_t header_crc32;
} VstiqHeader;

typedef struct {
    FILE*        fp;
    VstiqHeader  header;
    uint64_t     samples_written;
} VstiqWriter;

typedef struct {
    FILE*        fp;
    VstiqHeader  header;
    uint64_t     total_samples;
    uint64_t     samples_read;
    uint64_t     data_bytes;     /* actual data-area byte count, so callers can detect a partial sample */
    int          recovered;   /* header untrusted; derived from the file length */
    int          truncated;
} VstiqReader;

uint32_t vstiq_crc32(uint32_t seed, const void* data, size_t len);

/* recording side */
int  vstiq_create(const char* path, const VstiqHeader* header, VstiqWriter* writer);
int  vstiq_write(VstiqWriter* writer, const void* iq, uint32_t samples);
int  vstiq_finish(VstiqWriter* writer);   /* back-fill the sample count, clear the flag, write the CRC */
void vstiq_abort(VstiqWriter* writer);

/* playback side */
int  vstiq_open(const char* path, VstiqReader* reader);
size_t vstiq_read(VstiqReader* reader, void* buf, size_t samples);  /* returns the number of samples actually read */
void vstiq_close(VstiqReader* reader);
