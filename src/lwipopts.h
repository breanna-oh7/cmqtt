#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Base config for polled cyw43 arch (single core, no RTOS).
// LWIP_SOCKET/LWIP_NETCONN both OFF: those layers need OS semaphores/mailboxes
// (NO_SYS=0 + FreeRTOS). We're NO_SYS=1, so main.c drives the raw tcp_pcb API
// directly instead.
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        1
#define MEM_LIBC_MALLOC             1   // fine to use libc malloc in poll mode (single-threaded)

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (16 * 1024)

#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETCONN_SEM_PER_THREAD 0

#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define LWIP_STATS                  0

#ifndef NDEBUG
#define LWIP_DEBUG                  0
#endif

#define LWIP_TIMEVAL_PRIVATE        0

#endif /* _LWIPOPTS_H */