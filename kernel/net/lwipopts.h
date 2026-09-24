#pragma once

#define NO_SYS 1

#define SYS_LIGHTWEIGHT_PROT 0

#define LWIP_TIMERS 1
#define LWIP_TIMERS_CUSTOM 0

#define MEM_LIBC_MALLOC 1

#define MEMP_MEM_MALLOC 1

#define MEM_ALIGNMENT 4

#define mem_clib_malloc(s) kmalloc(s)
#define mem_clib_free(p) kfree(p)
#define mem_clib_calloc(n, s) kcalloc((n), (s))

#define MEM_SIZE (128 * 1024)

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_RAW 1 // fuck ts
#define LWIP_DHCP 1

#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_DNS 1
#define DNS_MAX_SERVERS 2
#define DNS_TABLE_SIZE 8
#define LWIP_SNMP 0
#define LWIP_MDNS_RESPONDER 0

#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0

#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN 16
#define MEMP_NUM_TCP_PCB 16
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_TCP_SEG 64

#define ARP_TABLE_SIZE 8
#define ARP_MAXAGE 300

#define PBUF_LINK_HLEN 14
#define PBUF_POOL_BUFSIZE (TCP_MSS + 60 + PBUF_LINK_HLEN)
#define MEMP_NUM_PBUF 32
#define PBUF_POOL_SIZE 32

#define CHECKSUM_GEN_IP 1
#define CHECKSUM_GEN_UDP 1
#define CHECKSUM_GEN_TCP 1
#define CHECKSUM_CHECK_IP 1
#define CHECKSUM_CHECK_UDP 1
#define CHECKSUM_CHECK_TCP 1
#define LWIP_CHECKSUM_ON_COPY 0

#define LWIP_STATS 0
#define LWIP_DEBUG 0

#define LWIP_NETIF_HOSTNAME 0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0
#define LWIP_HAVE_LOOPIF 0
#define LWIP_LOOPBACK_MAX_PBUFS 0
