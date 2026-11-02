/* Correction stream checker: counts RTCM3, SPARTN and UBX frames with good and bad CRCs, like ubxlib's uSpartnValidate. */

#ifndef UCXAPP_CORR_H
#define UCXAPP_CORR_H

#include <stddef.h>
#include <stdint.h>

/* Longest frame: SPARTN 10 header + 1023 payload + 64 authentication + 4 CRC bytes. */
#define UCXAPP_CORR_BUF 1104u

enum { UCXAPP_CORR_RTCM, UCXAPP_CORR_SPARTN, UCXAPP_CORR_UBX, UCXAPP_CORR_TYPES };

typedef struct {
    uint32_t good[UCXAPP_CORR_TYPES];
    uint32_t bad[UCXAPP_CORR_TYPES];   /* corrupt frames; one per error, not per byte skipped after it */
    uint32_t junk;                     /* bytes outside any frame */
    int resync;                        /* after a bad frame until the next good one */
    size_t len;
    uint8_t buf[UCXAPP_CORR_BUF];
} ucxapp_corr_t;

void ucxapp_corr_reset(ucxapp_corr_t *c);
void ucxapp_corr_feed(ucxapp_corr_t *c, const uint8_t *data, size_t len);
/* "RTCM 12, SPARTN 40 (1 bad), 37 bytes outside frames"; "no frames yet" before the first one. */
void ucxapp_corr_describe(const ucxapp_corr_t *c, char *out, size_t size);

#endif
