#ifndef EMU_NET_H
#define EMU_NET_H

#include <stdbool.h>
#include <stdint.h>

#include <openssl/ssl.h>

#include "transport_interface.h"

/* coreMQTT leaves the network context to the platform. */
struct NetworkContext {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
};

/* TCP + TLS with a client certificate. Verifies the server against ca_file and its hostname. */
bool emu_net_connect(NetworkContext_t *n, const char *host, uint16_t port, const char *ca_file,
                     const char *cert_file, const char *key_file);
void emu_net_close(NetworkContext_t *n);

int32_t emu_net_recv(NetworkContext_t *n, void *buf, size_t len);
int32_t emu_net_send(NetworkContext_t *n, const void *buf, size_t len);

/* MQTT bytes handed to TLS since start (before TLS framing). */
uint64_t emu_net_bytes_sent(void);

/* Monotonic milliseconds, for coreMQTT timeouts and keep-alive. */
uint32_t emu_now_ms(void);

#endif
