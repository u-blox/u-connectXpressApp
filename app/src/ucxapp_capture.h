#ifndef UCXAPP_CAPTURE_H
#define UCXAPP_CAPTURE_H

/* Packet captures run by an external tool (nrfutil ble-sniffer for sniff.*, ssh + tcpdump on a WLAN Pi for
 * wlanpi.*) into a pcap file, optionally shown live in Wireshark. PC builds only. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucxapp_capture ucxapp_capture_t;

/** Starts cmdline. stdoutToPcap: the tool writes the capture to stdout (tcpdump -w -), else it writes pcap itself.
 *  live: Wireshark shows the file as it grows. NULL (with a message printed) if the tool does not start. */
ucxapp_capture_t *ucxapp_capture_start(const char *cmdline, const char *pcap, bool stdoutToPcap, bool live);

/** Reads what the tool printed since the last call and returns everything so far (the last 8 KB). */
const char *ucxapp_capture_log(ucxapp_capture_t *c);

/** True while the tool runs; when it has ended its exit code goes to *code. */
bool ucxapp_capture_running(ucxapp_capture_t *c, int *code);

/** Stops the tool (interrupt, then kill after graceMs), lets Wireshark go on alone, prints the result; frees c.
 *  Returns the tool's exit code. */
int ucxapp_capture_stop(ucxapp_capture_t *c, uint32_t graceMs);

const char *ucxapp_capture_file(const ucxapp_capture_t *c);
uint32_t ucxapp_capture_elapsed_ms(const ucxapp_capture_t *c);

/** Packets and size of a pcap or pcapng file; false if it is neither. */
bool ucxapp_capture_count(const char *path, uint32_t *packets, uint64_t *bytes);

/** Wireshark's path (UCXAPP_WIRESHARK, PATH, the usual install folders); false if not found. */
bool ucxapp_capture_wireshark(char *out, size_t size);

/** Opens a file in Wireshark without waiting for it. */
bool ucxapp_capture_view(const char *path);

/** <prefix>-YYYYMMDD-HHMMSS.pcap */
void ucxapp_capture_default_name(const char *prefix, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
