/*
 * coreMQTT transport for the PC: TCP + TLS (OpenSSL) with mutual authentication. On the station
 * this role would be played by the cellular modem's TLS stack or by mbedTLS.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "emu_net.h"

static void report(const char *what)
{
    unsigned long err = ERR_get_error();
    fprintf(stderr, "net: %s: %s\n", what, err ? ERR_reason_error_string(err) : "failed");
}

static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port_str[8];
    int fd = -1;

    snprintf(port_str, sizeof port_str, "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "net: cannot resolve %s\n", host);
        return -1;
    }
    for (struct addrinfo *a = res; a != NULL && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);

    /* Send small frames immediately. Nagle's algorithm would hold a 30-byte frame back until
     * the previous segment is acknowledged, adding tens of ms to every live publish. */
    int one = 1;
    if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

bool emu_net_connect(NetworkContext_t *n, const char *host, uint16_t port, const char *ca_file,
                     const char *cert_file, const char *key_file)
{
    memset(n, 0, sizeof *n);
    n->fd = -1;

    n->ctx = SSL_CTX_new(TLS_client_method());
    if (n->ctx == NULL || !SSL_CTX_set_min_proto_version(n->ctx, TLS1_2_VERSION) ||
        SSL_CTX_load_verify_locations(n->ctx, ca_file, NULL) != 1 ||
        SSL_CTX_use_certificate_file(n->ctx, cert_file, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(n->ctx, key_file, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(n->ctx) != 1) {
        report("TLS setup (CA, certificate or key)");
        emu_net_close(n);
        return false;
    }
    SSL_CTX_set_verify(n->ctx, SSL_VERIFY_PEER, NULL);

    n->fd = tcp_connect(host, port);
    if (n->fd < 0) {
        fprintf(stderr, "net: cannot connect to %s:%u\n", host, port);
        emu_net_close(n);
        return false;
    }

    n->ssl = SSL_new(n->ctx);
    if (n->ssl == NULL || SSL_set_fd(n->ssl, n->fd) != 1 ||
        SSL_set_tlsext_host_name(n->ssl, host) != 1 || /* SNI */
        SSL_set1_host(n->ssl, host) != 1 ||             /* verify the server's name */
        SSL_connect(n->ssl) != 1) {
        report("TLS handshake");
        emu_net_close(n);
        return false;
    }

    /* After the handshake, reads must not block: coreMQTT polls for incoming data. */
    SSL_set_mode(n->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    fcntl(n->fd, F_SETFL, fcntl(n->fd, F_GETFL) | O_NONBLOCK);
    return true;
}

void emu_net_close(NetworkContext_t *n)
{
    if (n->ssl != NULL) {
        SSL_shutdown(n->ssl);
        SSL_free(n->ssl);
    }
    if (n->fd >= 0) {
        close(n->fd);
    }
    if (n->ctx != NULL) {
        SSL_CTX_free(n->ctx);
    }
    n->ssl = NULL;
    n->ctx = NULL;
    n->fd = -1;
}

/* coreMQTT contract: bytes transferred, 0 for "nothing now, try again", negative on failure. */
static int32_t result(NetworkContext_t *n, int ret)
{
    if (ret > 0) {
        return ret;
    }
    int err = SSL_get_error(n->ssl, ret);
    return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) ? 0 : -1;
}

int32_t emu_net_recv(NetworkContext_t *n, void *buf, size_t len)
{
    int chunk = len > INT32_MAX ? INT32_MAX : (int)len;
    return result(n, SSL_read(n->ssl, buf, chunk));
}

int32_t emu_net_send(NetworkContext_t *n, const void *buf, size_t len)
{
    int chunk = len > INT32_MAX ? INT32_MAX : (int)len;
    return result(n, SSL_write(n->ssl, buf, chunk));
}

uint32_t emu_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
