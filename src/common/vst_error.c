#include "vst_error.h"

#include "niRFSA.h"
#include "niRFSG.h"

void vst_status_text(int32_t status, char* out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (status == 0) {
        return;
    }
    /* Try RFSA first, then RFSG: different error domains, same code may have two meanings. */
    if (niRFSA_error_message(VI_NULL, status, out) == VI_SUCCESS && out[0] != '\0') {
        return;
    }
    if (niRFSG_error_message(VI_NULL, status, out) == VI_SUCCESS && out[0] != '\0') {
        return;
    }
    snprintf(out, out_size, "(no error text, 0x%08X)", (unsigned)status);
}

int32_t vst_check(int32_t status, const char* what)
{
    if (status == 0) {
        return 0;
    }
    char text[512];
    vst_status_text(status, text, sizeof text);
    fprintf(stderr, "error: %s failed, status 0x%08X (%d): %s\n",
            what ? what : "(unnamed)", (unsigned)status, (int)status, text);
    return status;
}
