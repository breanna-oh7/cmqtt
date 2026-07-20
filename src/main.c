#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"

#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"

// ---------------- config (same values as config.py) ----------------
#define HOME_WIFI      "NSA_VAN"
#define HOME_PASS      "Test!234"

#define BROKER_HOST    "ao27.net"
#define BROKER_PORT    443
#define WS_PATH        "/mqtt"
#define CLIENT_ID      "pico_2w_client"
#define MQTT_USER      "ao27"
#define MQTT_PASS      "ao27passwd"

#define MQTT_TOPIC     "ao27/pico/data"
#define MQTT_PAYLOAD   "\"payload\""   // ujson.dumps("payload") -> quoted string

// ---------------- globals ----------------
// raw lwIP TCP connection state - BSD sockets need NO_SYS=0/FreeRTOS, which
// we're not using, so we drive tcp_pcb directly with a small buffered shim
// that presents mbedtls with a "would block -> WANT_READ/WANT_WRITE" bio,
// which is exactly the contract mbedtls's non-blocking bio API expects.
typedef struct {
    struct tcp_pcb *pcb;
    volatile bool   connected;
    volatile bool   error;
    uint8_t         rx_buf[9000];  // must comfortably fit one TLS record (IN_CONTENT_LEN + header)
    volatile size_t rx_len;   // total valid bytes in rx_buf
    volatile size_t rx_head;  // read cursor into rx_buf
} raw_conn_t;

static raw_conn_t              g_conn;
static mbedtls_ssl_context     g_ssl;
static mbedtls_ssl_config      g_conf;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;

// small re-assembly buffer for the websocket unwrapper (mirrors the python bytearray buffer)
static uint8_t  ws_rx_buf[2048];
static size_t   ws_rx_len = 0;
static size_t   ws_rx_pos = 0;

static void die(const char *msg) {
    // Repeat instead of printing once - if you attach the serial monitor a
    // moment after this fires, a one-shot message would be gone forever.
    while (1) {
        printf("FATAL: %s\n", msg);
        cyw43_arch_poll();
        sleep_ms(2000);
    }
}

static void mbedtls_die(const char *msg, int ret) {
    // mbedtls_strerror() requires MBEDTLS_ERROR_C, which we don't enable
    // (its string table costs several KB of flash we don't need just for a
    // human-readable message) - the hex code is enough to look up if needed.
    printf("FATAL: %s (mbedtls error -0x%04x)\n", msg, -ret);
    die(msg);
}

// ---------------- wifi ----------------
static void wifi_connect(void) {
    if (cyw43_arch_init()) {
        die("cyw43_arch_init failed");
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to wifi: %s\n", HOME_WIFI);
    int rc = cyw43_arch_wifi_connect_timeout_ms(
        HOME_WIFI, HOME_PASS, CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc) {
        die("wifi connect failed/timed out");
    }
    printf("Connected. IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
}

// ---------------- DNS (raw API) ----------------
static ip_addr_t        g_resolved_ip;
static volatile bool    g_dns_done = false;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    if (ipaddr) g_resolved_ip = *ipaddr;
    g_dns_done = true;
}

static bool resolve_host(const char *host, ip_addr_t *out) {
    g_dns_done = false;
    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(host, out, dns_found_cb, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) return true; // was already cached, *out filled synchronously
    if (err != ERR_INPROGRESS) return false;

    absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!g_dns_done) {
        cyw43_arch_poll();
        sleep_ms(1);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
    }
    *out = g_resolved_ip;
    return true;
}

// ---------------- raw lwIP TCP connect ----------------
static err_t on_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    raw_conn_t *c = (raw_conn_t *)arg;
    if (!p) { // remote closed the connection
        c->error = true;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    // compact any already-consumed bytes out of the way before appending
    if (c->rx_head > 0) {
        size_t remaining = c->rx_len - c->rx_head;
        memmove(c->rx_buf, c->rx_buf + c->rx_head, remaining);
        c->rx_len = remaining;
        c->rx_head = 0;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t space = sizeof(c->rx_buf) - c->rx_len;
        size_t n = (q->len < space) ? q->len : space; // drop bytes if our rx_buf is full
        memcpy(c->rx_buf + c->rx_len, q->payload, n);
        c->rx_len += n;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_tcp_err(void *arg, err_t err) {
    (void)err;
    raw_conn_t *c = (raw_conn_t *)arg;
    c->error = true;
}

static err_t on_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;
    raw_conn_t *c = (raw_conn_t *)arg;
    c->connected = (err == ERR_OK);
    return ERR_OK;
}

static void tcp_connect_blocking(const char *host, int port) {
    ip_addr_t addr;
    if (!resolve_host(host, &addr)) die("DNS lookup failed");

    memset(&g_conn, 0, sizeof(g_conn));

    cyw43_arch_lwip_begin();
    g_conn.pcb = tcp_new();
    if (!g_conn.pcb) {
        cyw43_arch_lwip_end();
        die("tcp_new failed");
    }
    tcp_arg(g_conn.pcb, &g_conn);
    tcp_err(g_conn.pcb, on_tcp_err);
    tcp_recv(g_conn.pcb, on_tcp_recv);
    err_t err = tcp_connect(g_conn.pcb, &addr, (u16_t)port, on_tcp_connected);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) die("tcp_connect failed");

    absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!g_conn.connected && !g_conn.error) {
        cyw43_arch_poll();
        sleep_ms(1);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            die("tcp connect timed out");
        }
    }
    if (g_conn.error) die("tcp connect failed (reset/refused)");
}

// ---------------- mbedtls bio glue (drives the raw tcp_pcb above) ----------------
// mbedtls's bio contract already supports non-blocking use: returning
// MBEDTLS_ERR_SSL_WANT_READ / WANT_WRITE tells mbedtls_ssl_read/write to be
// called again later, which is a perfect fit for lwIP's raw (callback-driven)
// API in NO_SYS=1 poll mode - no FreeRTOS/blocking sockets required.
#define NET_SEND_FAILED  -0x0001

static int net_send(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    cyw43_arch_poll();
    if (g_conn.error) return NET_SEND_FAILED;
    if (tcp_sndbuf(g_conn.pcb) < len) return MBEDTLS_ERR_SSL_WANT_WRITE;

    cyw43_arch_lwip_begin();
    err_t err = tcp_write(g_conn.pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) tcp_output(g_conn.pcb);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)len;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    cyw43_arch_poll();
    if (g_conn.error) return MBEDTLS_ERR_SSL_CONN_EOF;

    size_t avail = g_conn.rx_len - g_conn.rx_head;
    if (avail == 0) return MBEDTLS_ERR_SSL_WANT_READ;

    size_t n = (len < avail) ? len : avail;
    memcpy(buf, g_conn.rx_buf + g_conn.rx_head, n);
    g_conn.rx_head += n;
    return (int)n;
}

// pico has no OS entropy source -> feed the hardware RNG (rp2 ROSC/ring-osc based) into mbedtls
static int hw_entropy_source(void *data, unsigned char *output, size_t len,
                              size_t *olen) {
    (void)data;
    for (size_t i = 0; i < len; i++) {
        output[i] = (uint8_t)get_rand_32();
    }
    *olen = len;
    return 0;
}

// temporary: prints mbedtls's internal handshake trace, including the exact
// alert description when the server rejects the connection
static void mbedtls_debug_cb(void *ctx, int level, const char *file, int line, const char *str) {
    (void)ctx; (void)level; (void)file; (void)line;
    // these fire constantly and add nothing (pure I/O bookkeeping) - skip them
    if (strstr(str, "flush output") || strstr(str, "fetch input") || strstr(str, "in_left:")) {
        return;
    }
    printf("mbedtls: %s", str); // str already ends with '\n'
}

static void tls_wrap(void) {
    mbedtls_ssl_init(&g_ssl);
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);

    mbedtls_entropy_add_source(&g_entropy, hw_entropy_source, NULL, 32,
                                MBEDTLS_ENTROPY_SOURCE_STRONG);

    int ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
                                     &g_entropy, NULL, 0);
    if (ret != 0) mbedtls_die("ctr_drbg_seed failed", ret);

    ret = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) mbedtls_die("ssl_config_defaults failed", ret);

    // python used ssl.CERT_NONE -> same here, no cert verification
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);

    ret = mbedtls_ssl_setup(&g_ssl, &g_conf);
    if (ret != 0) mbedtls_die("ssl_setup failed", ret);

    ret = mbedtls_ssl_set_hostname(&g_ssl, BROKER_HOST); // SNI
    if (ret != 0) mbedtls_die("ssl_set_hostname failed", ret);

    mbedtls_ssl_set_bio(&g_ssl, NULL, net_send, net_recv, NULL);

    mbedtls_ssl_conf_dbg(&g_conf, mbedtls_debug_cb, NULL);
    // mbedtls_debug_set_threshold(3); // 1=basic, 3=shows handshake/alert detail    debug 

    printf("Starting TLS handshake...\n");
    while ((ret = mbedtls_ssl_handshake(&g_ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_die("tls handshake failed", ret);
        }
    }
    printf("TLS handshake complete.\n");
}

static int tls_write_all(const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int ret = mbedtls_ssl_write(&g_ssl, data + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret < 0) mbedtls_die("tls write failed", ret);
        sent += ret;
    }
    return (int)sent;
}

static int tls_read_exact(uint8_t *out, size_t len) {
    size_t got = 0;
    while (got < len) {
        int ret = mbedtls_ssl_read(&g_ssl, out + got, len - got);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) mbedtls_die("tls read failed", ret);
        got += ret;
    }
    return (int)got;
}

// reads one line (up to and including '\n') off the TLS stream, used only
// during the plaintext HTTP upgrade response - mirrors readline() in micropython
static int tls_read_line(char *out, size_t max) {
    size_t i = 0;
    while (i < max - 1) {
        uint8_t c;
        int ret = mbedtls_ssl_read(&g_ssl, &c, 1);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) mbedtls_die("tls read (line) failed", ret);
        out[i++] = (char)c;
        if (c == '\n') break;
    }
    out[i] = '\0';
    return (int)i;
}

// ---------------- websocket framing (matches WSClientWrapper in config.py) ----------------
static void ws_write(const uint8_t *data, size_t length) {
    uint8_t header[14];
    size_t hlen = 0;

    header[hlen++] = 0x82; // FIN + binary opcode

    if (length < 126) {
        header[hlen++] = (uint8_t)(length | 0x80);
    } else if (length < 65536) {
        header[hlen++] = 126 | 0x80;
        header[hlen++] = (uint8_t)((length >> 8) & 0xFF);
        header[hlen++] = (uint8_t)(length & 0xFF);
    } else {
        header[hlen++] = 127 | 0x80;
        for (int shift = 56; shift >= 0; shift -= 8) {
            header[hlen++] = (uint8_t)((length >> shift) & 0xFF);
        }
    }

    static const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    tls_write_all(header, hlen);

    // mask and send the payload in chunks to avoid a giant stack buffer
    uint8_t chunk[256];
    size_t sent = 0;
    while (sent < length) {
        size_t n = length - sent;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = data[sent + i] ^ mask[(sent + i) % 4];
        }
        tls_write_all(chunk, n);
        sent += n;
    }
}

// pulls `amt` decoded MQTT bytes out of the websocket stream, buffering
// leftovers exactly like the python bytearray buffer did
static void ws_read(uint8_t *out, size_t amt) {
    size_t out_pos = 0;

    while (out_pos < amt) {
        size_t available = ws_rx_len - ws_rx_pos;
        if (available == 0) {
            // no buffered data - pull one more websocket frame
            uint8_t hdr[2];
            tls_read_exact(hdr, 2);

            uint64_t payload_len = hdr[1] & 0x7F;
            if (payload_len == 126) {
                uint8_t ext[2];
                tls_read_exact(ext, 2);
                payload_len = ((uint64_t)ext[0] << 8) | ext[1];
            } else if (payload_len == 127) {
                uint8_t ext[8];
                tls_read_exact(ext, 8);
                payload_len = 0;
                for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
            }

            if (payload_len > sizeof(ws_rx_buf)) {
                die("websocket frame too large for rx buffer");
            }

            tls_read_exact(ws_rx_buf, (size_t)payload_len);
            ws_rx_len = (size_t)payload_len;
            ws_rx_pos = 0;
            available = ws_rx_len;
        }

        size_t want = amt - out_pos;
        size_t take = (want < available) ? want : available;
        memcpy(out + out_pos, ws_rx_buf + ws_rx_pos, take);
        ws_rx_pos += take;
        out_pos += take;
    }
}

static void ws_handshake(void) {
    char req[256];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: mqtt\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        WS_PATH, BROKER_HOST);

    tls_write_all((const uint8_t *)req, strlen(req));

    // drain the HTTP upgrade response headers until the blank line
    char line[256];
    for (;;) {
        int n = tls_read_line(line, sizeof(line));
        if (n == 0) break;
        if (strcmp(line, "\r\n") == 0) break;
    }
    printf("WebSocket handshake complete.\n");
}

// ---------------- MQTT packet building (mirrors the manual packing in config.py) ----------------
static size_t encode_remaining_length(uint8_t *out, uint32_t len) {
    size_t n = 0;
    do {
        uint8_t b = len & 0x7F;
        len >>= 7;
        if (len > 0) b |= 0x80;
        out[n++] = b;
    } while (len > 0);
    return n;
}

static void mqtt_connect(void) {
    uint8_t variable_and_payload[256];
    size_t p = 0;

    // protocol name "MQTT" + level + connect flags + keepalive
    static const uint8_t protocol_name[] = {0x00, 0x04, 'M', 'Q', 'T', 'T'};
    memcpy(variable_and_payload + p, protocol_name, sizeof(protocol_name));
    p += sizeof(protocol_name);

    variable_and_payload[p++] = 0x04; // protocol level (MQTT 3.1.1)
    variable_and_payload[p++] = 0xC2; // clean session + username + password flags
    variable_and_payload[p++] = 0x00; // keepalive high byte
    variable_and_payload[p++] = 0x3C; // keepalive = 60s

    #define APPEND_STR(field) do {                                  \
        uint16_t l = (uint16_t)strlen(field);                       \
        variable_and_payload[p++] = (uint8_t)(l >> 8);               \
        variable_and_payload[p++] = (uint8_t)(l & 0xFF);             \
        memcpy(variable_and_payload + p, field, l);                  \
        p += l;                                                      \
    } while (0)

    APPEND_STR(CLIENT_ID);
    APPEND_STR(MQTT_USER);
    APPEND_STR(MQTT_PASS);
    #undef APPEND_STR

    uint8_t remaining[4];
    size_t rl_len = encode_remaining_length(remaining, (uint32_t)p);

    uint8_t packet[8 + sizeof(variable_and_payload)];
    size_t idx = 0;
    packet[idx++] = 0x10; // CONNECT
    memcpy(packet + idx, remaining, rl_len);
    idx += rl_len;
    memcpy(packet + idx, variable_and_payload, p);
    idx += p;

    printf("Sending MQTT Login Frame...\n");
    ws_write(packet, idx);

    uint8_t resp[4];
    ws_read(resp, 4);
    printf("Raw bytes received from broker: %02x %02x %02x %02x\n",
           resp[0], resp[1], resp[2], resp[3]);

    if (resp[0] != 0x20 || resp[1] != 0x02) {
        die("Broker dropped connection without full CONNACK packet");
    }
    if (resp[3] != 0) {
        printf("Broker rejected login. Connection Return Code: %d\n", resp[3]);
        die("MQTT connect refused");
    }
    printf("Connected\n");
}

static void mqtt_publish(const char *topic, const char *payload, bool retain) {
    uint8_t buf[256];
    size_t p = 0;

    uint16_t tlen = (uint16_t)strlen(topic);
    buf[p++] = (uint8_t)(tlen >> 8);
    buf[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(buf + p, topic, tlen);
    p += tlen;

    size_t plen = strlen(payload);
    memcpy(buf + p, payload, plen);
    p += plen;

    uint8_t remaining[4];
    size_t rl_len = encode_remaining_length(remaining, (uint32_t)p);

    uint8_t packet[8 + sizeof(buf)];
    size_t idx = 0;
    packet[idx++] = 0x30 | (retain ? 0x01 : 0x00); // PUBLISH, QoS0, retain flag
    memcpy(packet + idx, remaining, rl_len);
    idx += rl_len;
    memcpy(packet + idx, buf, p);
    idx += p;

    ws_write(packet, idx);
    printf("Message published successfully!\n");
}

static void mqtt_disconnect(void) {
    uint8_t packet[2] = {0xE0, 0x00};
    ws_write(packet, sizeof(packet));
    printf("Disconnected\n");
}

// ---------------- main ----------------
int main(void) {
    stdio_init_all();

    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(500); // small settle delay right after the host opens the port
    printf("\n--- USB serial connected, starting ---\n");

    wifi_connect();

    printf("Connecting...\n");
    tcp_connect_blocking(BROKER_HOST, BROKER_PORT);
    tls_wrap();

    ws_handshake();

    mqtt_connect();
    
    mqtt_publish(MQTT_TOPIC, MQTT_PAYLOAD, true);
    mqtt_disconnect();

    mbedtls_ssl_close_notify(&g_ssl);
    mbedtls_ssl_free(&g_ssl);
    // mbedtls_ssl_config_free(&g_conf);
    mbedtls_ctr_drbg_free(&g_ctr_drbg);
    mbedtls_entropy_free(&g_entropy);

    cyw43_arch_lwip_begin();
    tcp_close(g_conn.pcb);
    cyw43_arch_lwip_end();

    cyw43_arch_deinit();

    while (1) tight_loop_contents();
}