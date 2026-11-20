/*
 * link_transport: what carries the link's packets between two boards.
 *
 * ESP-IDF only. One of ESP-NOW and a UART is compiled in, chosen in
 * menuconfig (MOCK_LINK); both offer these functions.
 */
#ifndef LINK_TRANSPORT_H
#define LINK_TRANSPORT_H

#include "esp_err.h"

#include "link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The send function to give link_init(). Safe before link_transport_start()
 * and if that failed: it then sends nothing and returns false. */
bool link_transport_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len);

/* Start the transport and deliver what it receives to l. For ESP-NOW this
 * starts Wi-Fi if nothing else has. */
esp_err_t link_transport_start(link_t *l);

/* "ESP-NOW" or "UART", for the log. */
const char *link_transport_name(void);

#ifdef __cplusplus
}
#endif

#endif /* LINK_TRANSPORT_H */
