/* Minimal TDMS writer interface. File layout matches the NI example:
 *   group "vst" / channel "iq" / u32 (one u32 = one complex sample, packed I16+Q16).
 * Write direction only; reading uses the VSTIQ container (see vst_file.h). */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE*    fp;
    uint64_t samples;       /* number of u32 samples written */
    uint64_t count_pos;     /* file position of numberOfValues, fixed up on close */
    uint64_t next_seg_pos;  /* file position of nextSegmentOffset */
} TdmsWriter;

/* expected_samples only seeds the metadata; the actual count wins on close. */
int  tdms_create(const char* path, TdmsWriter* writer, uint64_t expected_samples);
int  tdms_write(TdmsWriter* writer, const void* iq_u32, uint32_t samples);
int  tdms_finish(TdmsWriter* writer);
void tdms_abort(TdmsWriter* writer);

/* Reader side: supports single-segment, single-u32-channel, property-free TDMS (this repo and the NI example).
 * On success the caller frees *out_bytes; *out_samples is the complex sample count (4 bytes each). */
int  tdms_read_all(const char* path, uint8_t** out_bytes, uint64_t* out_samples);
