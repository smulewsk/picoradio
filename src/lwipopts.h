#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* lwIP running on FreeRTOS with the BSD sockets API */
#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_SO_RCVTIMEO            1
#define LWIP_TIMEVAL_PRIVATE        0
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_ERRNO_STDINCLUDE       1   /* sockets set newlib errno */

#define TCPIP_THREAD_STACKSIZE      2048
#define TCPIP_THREAD_PRIO           4
#define DEFAULT_THREAD_STACKSIZE    1024
#define TCPIP_MBOX_SIZE             16
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   32
#define DEFAULT_ACCEPTMBOX_SIZE     8

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    16000
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_NETBUF             8
#define MEMP_NUM_NETCONN            8    /* web server keeps up to 3 waiting connections */
#define MEMP_NUM_TCP_PCB            10   /* + closed web connections in TIME_WAIT */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 10)
#define PBUF_POOL_SIZE              24   /* 36 KB, still > TCP_WND */

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define LWIP_TCP_KEEPALIVE          1

/* mDNS responder: http://picoradio.local */
#define LWIP_MDNS_RESPONDER         1
#define LWIP_IGMP                   1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define MDNS_MAX_SERVICES           1
#define MEMP_NUM_UDP_PCB            6

#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_CHKSUM_ALGORITHM       3

#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

#endif
