#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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

#include "network.h"
#include "mbedtls_config.h"

// ================================================================
//  Definitions - Define the Wifi Network, Password, Broker, Port, Path, and topic
// ================================================================
#define PKT_QUEUE_LEN   8
#define HOME_WIFI      "Breanna"
#define HOME_PASS      "BreannaOh"

#define BROKER_HOST    "ao27.net"
#define BROKER_PORT    443
#define WS_PATH        "/mqtt"
#define CLIENT_ID      "pico_2w_client"
#define MQTT_USER      "ao27"
#define MQTT_PASS      "ao27passwd"

#define MQTT_TOPIC     "ao27/pico/data"

typedef struct {
    uint8_t  data[NETWORK_PKT_MAX_LEN];
    uint16_t len;
} pkt_slot_t;

static volatile pkt_slot_t pkt_queue[PKT_QUEUE_LEN];
static volatile uint32_t   pkt_write_idx = 0;
static volatile uint32_t   pkt_read_idx  = 0;

// Takes the fully decoded received packets from the ao27.c in the other core and places
// each packet into a small waiting line so the main networking loop can handle
// it later without blocking the interrupt path.
void network_push_packet(const uint8_t *data, uint16_t len) {
    uint32_t next = (pkt_write_idx + 1) % PKT_QUEUE_LEN;
    if (next == pkt_read_idx) {
        return; // queue full - drop rather than stall the caller
    }

    if (len > NETWORK_PKT_MAX_LEN) {
        len = NETWORK_PKT_MAX_LEN;
    }
    memcpy((void *)pkt_queue[pkt_write_idx].data, data, len);
    pkt_queue[pkt_write_idx].len = len;
    __sync_synchronize(); // publish the data write before the index update
    pkt_write_idx = next;
}

// Internal - only ever called from network_task()'s own loop on core1.
// Removes one queued packet from the waiting line and hands it to the
// main loop so it can be sent out to the broker.
static bool pkt_queue_pop(uint8_t *out, uint16_t *out_len) {
    if (pkt_read_idx == pkt_write_idx) {
        return false;
    }
    uint16_t len = pkt_queue[pkt_read_idx].len;
    memcpy(out, (void *)pkt_queue[pkt_read_idx].data, len);
    *out_len = len;
    __sync_synchronize();
    pkt_read_idx = (pkt_read_idx + 1) % PKT_QUEUE_LEN;

    return true;
}

// ================================================================
//  Raw lwIP TCP connection state
// ================================================================
typedef struct {
    struct tcp_pcb *pcb;
    volatile bool   connected;
    volatile bool   error;
    uint8_t         rx_buf[9000];  // must comfortably fit one TLS record (IN_CONTENT_LEN + header)
    volatile size_t rx_len;
    volatile size_t rx_head;
} raw_conn_t;

static raw_conn_t              g_conn;
static mbedtls_ssl_context     g_ssl;
static mbedtls_ssl_config      g_conf;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;

static uint8_t  ws_rx_buf[2048];
static size_t   ws_rx_len = 0;
static size_t   ws_rx_pos = 0;

// Hard-stop fallback - loops until a valid connection found
static void net_die(const char *msg) {
    // Repeat instead of printing once - if the serial monitor gets attached
    // a moment after this fires, a one-shot message would be gone forever.

    while (1) {
        printf("FATAL (net): %s\n", msg);
        cyw43_arch_poll();
        sleep_ms(2000);
    }
}

// Turns a TLS library error into a clearer message and then calls the
// main fatal handler so the network task stops cleanly.
static void mbedtls_die(const char *msg, int ret) {
    printf("FATAL (net): %s (mbedtls error -0x%04x)\n", msg, -ret);
    net_die(msg);
}

// Connects the Pico to Wi-Fi using the Wifi name and definition
static void wifi_connect(void) {
    if (cyw43_arch_init()) {
        net_die("cyw43_arch_init failed");  
    }
    cyw43_arch_enable_sta_mode(); //enables wifi station mode

    printf("Connecting to wifi: %s\n", HOME_WIFI);
    int rc = cyw43_arch_wifi_connect_timeout_ms(HOME_WIFI, HOME_PASS, CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc) {
        net_die("wifi connect failed/timed out");
    }
    printf("Connected. IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
}

// ---------------- DNS (raw API) ----------------
static ip_addr_t        g_resolved_ip;
static volatile bool    g_dns_done = false;

// Callback that runs after DNS finishes looking up a host.
// It saves the address that was found so the main code can use it.
static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    if (ipaddr) g_resolved_ip = *ipaddr;
    g_dns_done = true;
}

// Asks DNS for the broker's address and waits briefly until an answer comes
// since broker is named by text, not number.
static bool resolve_host(const char *host, ip_addr_t *out) {
    g_dns_done = false;
    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(host, out, dns_found_cb, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        return true;
    }
    if (err != ERR_INPROGRESS) {
        return false;
    }

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
// Called whenever new TCP data arrives. 
// copies the incoming bytes into a local buffer so the higher-level TLS and WebSocket code can read them.
static err_t on_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    raw_conn_t *c = (raw_conn_t *)arg;
    if (!p) {
        c->error = true;
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    if (c->rx_head > 0) {
        size_t remaining = c->rx_len - c->rx_head;
        memmove(c->rx_buf, c->rx_buf + c->rx_head, remaining);
        c->rx_len = remaining;
        c->rx_head = 0;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t space = sizeof(c->rx_buf) - c->rx_len;
        size_t n = (q->len < space) ? q->len : space;
        memcpy(c->rx_buf + c->rx_len, q->payload, n);
        c->rx_len += n;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// Marks the TCP connection as broken when the low-level stack reports an error
// Lets the rest of the code stop trying to use a dead connection.
static void on_tcp_err(void *arg, err_t err) {
    (void)err;
    raw_conn_t *c = (raw_conn_t *)arg;
    c->error = true;
}

// Called once the TCP connect attempt finishes. 
// returns a the flag ERR_OK so the code knows the socket is ready to use.
static err_t on_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;
    raw_conn_t *c = (raw_conn_t *)arg;
    c->connected = (err == ERR_OK);
    return ERR_OK;
}

// Opens the raw TCP connection to the broker and waits until it is either ready or clearly failed.
//  TLS steps cannot start without a live socket.
static void tcp_connect_blocking(const char *host, int port) {
    ip_addr_t addr;
    if (!resolve_host(host, &addr)) {
        net_die("DNS lookup failed");
    }

    memset(&g_conn, 0, sizeof(g_conn));

    cyw43_arch_lwip_begin();
    g_conn.pcb = tcp_new();
    if (!g_conn.pcb) {
        cyw43_arch_lwip_end();
        net_die("tcp_new failed");
    }
    tcp_arg(g_conn.pcb, &g_conn);
    tcp_err(g_conn.pcb, on_tcp_err);
    tcp_recv(g_conn.pcb, on_tcp_recv);
    err_t err = tcp_connect(g_conn.pcb, &addr, (u16_t)port, on_tcp_connected);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
        net_die("tcp_connect failed");
    }

    absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!g_conn.connected && !g_conn.error) {
        cyw43_arch_poll();
        sleep_ms(1);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            net_die("tcp connect timed out");
        }
    }
    if (g_conn.error) {
        net_die("tcp connect failed (reset/refused)");
    }
}

// ---------------- mbedtls bio glue (drives the raw tcp_pcb above) ----------------
#define NET_SEND_FAILED  -0x0001

// This is the send side for the TLS layer. It takes bytes from mbedtls and
// pushes them onto the TCP connection, which is how secure traffic leaves the device.
static int net_send(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    cyw43_arch_poll();
    if (g_conn.error) {
        return NET_SEND_FAILED;
    }
    if (tcp_sndbuf(g_conn.pcb) < len) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    cyw43_arch_lwip_begin();
    err_t err = tcp_write(g_conn.pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) tcp_output(g_conn.pcb);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)len;
}

// Receive side for the TLS layer that gives mbedtls bytes that
// were already collected from the TCP connection so it can keep reading safely.
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

// pico has no OS entropy source -> feed the hardware RNG into mbedtls
// Provides random bytes to the TLS library. It is needed because the
// secure connection uses those bytes when creating keys and other crypto material.
static int hw_entropy_source(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    for (size_t i = 0; i < len; i++) {
        output[i] = (uint8_t)get_rand_32();
    }
    *olen = len;
    return 0;
}

// Sets up the TLS layer on top of the raw TCP connection. It prepares
// the crypto settings, performs the handshake, and turns the socket into a
// secure channel for later traffic.
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

    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);

    ret = mbedtls_ssl_setup(&g_ssl, &g_conf);
    if (ret != 0) mbedtls_die("ssl_setup failed", ret);

    ret = mbedtls_ssl_set_hostname(&g_ssl, BROKER_HOST);
    if (ret != 0) mbedtls_die("ssl_set_hostname failed", ret);

    mbedtls_ssl_set_bio(&g_ssl, NULL, net_send, net_recv, NULL);

    printf("Starting TLS handshake...\n");
    while ((ret = mbedtls_ssl_handshake(&g_ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_die("tls handshake failed", ret);
        }
    }
    printf("TLS handshake complete.\n");
}

// Sends bytes until the whole message is handed to the TLS layer.
// It is needed because the secure socket may only accept a chunk at a time.
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

// REads precise number of bytes from the secure stream. It is used when
// the code needs an exact amount of data, such as a fixed-size response header.
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

// Reads one line from the secure stream until it reaches a newline. It is
// used during the WebSocket upgrade step when the broker sends a text response.
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

// ---------------- websocket framing ----------------
// This wraps a chunk of bytes in a WebSocket frame before sending it. The frame
// adds the message headers the broker expects so it can understand the data.
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

// Pulls a requested amount of bytes out of the WebSocket receive buffer.
// It is needed because the broker may send data in frames, and the code must
// reassemble the message piece by piece.
static void ws_read(uint8_t *out, size_t amt) {
    size_t out_pos = 0;

    while (out_pos < amt) {
        size_t available = ws_rx_len - ws_rx_pos;
        if (available == 0) {
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
                net_die("websocket frame too large for rx buffer");
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

// Webscoket handshake - asks the broker to switch the connection from plain TCP/TLS into a WebSocket tunnel that MQTT can ride over.
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

    char line[256];
    for (;;) {
        int n = tls_read_line(line, sizeof(line));
        if (n == 0) break;
        if (strcmp(line, "\r\n") == 0) break;
    }
    printf("WebSocket handshake complete.\n");
}

// ---------------- MQTT packet building ----------------
// This turns a packet length into the compact format MQTT uses in its headers.
// It is needed so the broker can tell how big the next message is.
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

// This builds and sends the MQTT login packet. It tells the broker who the
// client is and proves the connection should be allowed to continue.
static void mqtt_connect(void) {
    uint8_t variable_and_payload[256];
    size_t p = 0;

    static const uint8_t protocol_name[] = {0x00, 0x04, 'M', 'Q', 'T', 'T'};
    memcpy(variable_and_payload + p, protocol_name, sizeof(protocol_name));
    p += sizeof(protocol_name);

    variable_and_payload[p++] = 0x04;
    variable_and_payload[p++] = 0xC2;
    variable_and_payload[p++] = 0x00;
    variable_and_payload[p++] = 0x3C;

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

    uint8_t packet_buf[8 + sizeof(variable_and_payload)];
    size_t idx = 0;
    packet_buf[idx++] = 0x10;
    memcpy(packet_buf + idx, remaining, rl_len);
    idx += rl_len;
    memcpy(packet_buf + idx, variable_and_payload, p);
    idx += p;

    printf("Sending MQTT Login Frame...\n");
    ws_write(packet_buf, idx);

    uint8_t resp[4];
    ws_read(resp, 4);
    printf("Raw bytes received from broker: %02x %02x %02x %02x\n",
           resp[0], resp[1], resp[2], resp[3]);

    if (resp[0] != 0x20 || resp[1] != 0x02) {
        net_die("Broker dropped connection without full CONNACK packet");
    }
    if (resp[3] != 0) {
        printf("Broker rejected login. Connection Return Code: %d\n", resp[3]);
        net_die("MQTT connect refused");
    }
    printf("MQTT connected\n");
}

// Publishes a raw binary payload (not a C string) - fit for arbitrary
// captured packet bytes.
// This packages a captured packet into an MQTT publish message and sends it to
// the broker so it can be seen by other subscribers.
static void mqtt_publish(const char *topic, const uint8_t *payload, size_t payload_len, bool retain) {
    static uint8_t buf[2 + 64 + NETWORK_PKT_MAX_LEN];
    size_t p = 0;

    uint16_t tlen = (uint16_t)strlen(topic);
    buf[p++] = (uint8_t)(tlen >> 8);
    buf[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(buf + p, topic, tlen);
    p += tlen;

    memcpy(buf + p, payload, payload_len);
    p += payload_len;

    uint8_t remaining[4];
    size_t rl_len = encode_remaining_length(remaining, (uint32_t)p);

    static uint8_t packet_buf[8 + sizeof(buf)];
    size_t idx = 0;
    packet_buf[idx++] = 0x30 | (retain ? 0x01 : 0x00);
    memcpy(packet_buf + idx, remaining, rl_len);
    idx += rl_len;
    memcpy(packet_buf + idx, buf, p);
    idx += p;

    ws_write(packet_buf, idx);
}

// This sends a small MQTT keepalive message. It is needed so the broker knows
// the client is still alive during quiet times.
static void mqtt_ping(void) {
    uint8_t packet_buf[2] = {0xC0, 0x00};
    ws_write(packet_buf, sizeof(packet_buf));
}

// Non-blocking: opportunistically pulls and discards any bytes the broker
// sent us (PINGRESP etc). We're publish-only, but incoming bytes still need
// to go through mbedtls_ssl_read so its internal record-parsing state stays
// correct.
// This keeps the connection healthy by reading any reply bytes the broker sends
// back, even though this device is mostly sending data.
static void mqtt_drain_incoming(void) {
    uint8_t scratch[256];
    mbedtls_ssl_read(&g_ssl, scratch, sizeof(scratch));
}


// This is the main networking loop. It brings up Wi-Fi, opens the broker
// connection, sets up TLS and WebSocket, then keeps publishing queued packets
// while also sending occasional keepalive messages.
void network_task(void) {
    printf("core1 alive\n");
    wifi_connect();

    printf("Connecting to broker...\n");
    tcp_connect_blocking(BROKER_HOST, BROKER_PORT);
    tls_wrap();
    ws_handshake();
    mqtt_connect();

    printf("Ready - publishing captured packets to '%s'\n", MQTT_TOPIC);

    static uint8_t pkt_buf[NETWORK_PKT_MAX_LEN];
    uint16_t pkt_len;
    absolute_time_t last_ping = get_absolute_time();

    while (1) {
        cyw43_arch_poll();

        while (pkt_queue_pop(pkt_buf, &pkt_len)) {
            mqtt_publish(MQTT_TOPIC, pkt_buf, pkt_len, false);
            printf("Published packet (%u bytes)\n", pkt_len);
        }

        // keep the broker's 60s keepalive happy during quiet periods
        if (absolute_time_diff_us(last_ping, get_absolute_time()) > 30 * 1000 * 1000) {
            mqtt_ping();
            last_ping = get_absolute_time();
        }

        mqtt_drain_incoming();

        sleep_ms(5);
    }
}