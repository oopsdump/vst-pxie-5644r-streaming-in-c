/* NI status codes to readable text. The common layer does not include driver headers; status is int32. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Fetch the official RFSA or RFSG error text; fall back to hex when both fail. */
void vst_status_text(int32_t status, char* out, size_t out_size);

/* Print and return the status unchanged, e.g. if (vst_check(rc, "configure IQ rate")) goto fail; */
int32_t vst_check(int32_t status, const char* what);
