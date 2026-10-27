/* Embedded targets (STM32, Zephyr): the features below are not built, so their hooks are no-ops. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ucxapp_web_on_incoming(int32_t sock, int32_t listenSock) { (void)sock; (void)listenSock; }
void ucxapp_web_on_data(int32_t sock, int32_t bytes) { (void)sock; (void)bytes; }
void ucxapp_web_on_closed(int32_t sock) { (void)sock; }
void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock) { (void)sock; (void)listenSock; }
void ucxapp_stream_on_closed(int32_t sock) { (void)sock; }
void ucxapp_stream_on_data(int32_t sock) { (void)sock; }
void ucxapp_stream_on_sps_data(void) { }
void ucxapp_stream_on_ble_rx(const uint8_t *data, size_t len) { (void)data; (void)len; }
size_t ucxapp_gnss_summary(char *out, size_t size) { (void)size; out[0] = '\0'; return 0; }
size_t ucxapp_imu_summary(char *out, size_t size) { (void)size; out[0] = '\0'; return 0; }
void ucxapp_ui_event(const char *text) { (void)text; }
void ucxapp_ui_wifi_set(bool up) { (void)up; }
void ucxapp_ui_prompt(const char *text, const char *prefill, const char *cancel) { (void)text; (void)prefill; (void)cancel; }
bool ucxapp_hid_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_hid_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_ft_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_ft_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_ancs_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_notify_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
void ucxapp_ft_on_sock_data(int32_t sock) { (void)sock; }
void ucxapp_ft_on_sock_closed(int32_t sock) { (void)sock; }
void ucxapp_ft_on_sock_incoming(int32_t sock, int32_t listenSock) { (void)sock; (void)listenSock; }
bool ucxapp_qr_print_wifi(const char *ssid, const char *password, const char *security, bool hidden)
{
    (void)ssid; (void)password; (void)security; (void)hidden;
    return false;
}
