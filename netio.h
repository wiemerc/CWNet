#include <exec/types.h>

/* SLIP protocol */
#define SLIP_END                0xc0
#define SLIP_ESCAPED_END        0xdc
#define SLIP_ESC                0xdb
#define SLIP_ESCAPED_ESC        0xdd

#define MAX_PKT_SIZE 65535
#define IP_HDR_LEN (sizeof(struct IPHeader))
#define UDP_HDR_LEN (sizeof(struct UDPHeader))
#define IPPROTO_UDP 17

/* IP and UDP protocol headers, adapted from FreeBSD */
/*
 * IP header
 */
/* TODO: Do we really need to consider byte order? */
struct IPHeader {
#if BYTE_ORDER == LITTLE_ENDIAN
    UBYTE  ip_hl:4,        /* header length */
           ip_v:4;         /* version */
#endif
#if BYTE_ORDER == BIG_ENDIAN
    UBYTE  ip_v:4,         /* version */
           ip_hl:4;        /* header length */
#endif
    UBYTE  ip_tos;         /* type of service */
    USHORT ip_len;         /* total length */
    USHORT ip_id;          /* identification */
    USHORT ip_off;         /* fragment offset field */
    UBYTE  ip_ttl;         /* time to live */
    UBYTE  ip_p;           /* protocol */
    USHORT ip_sum;         /* checksum */
    UBYTE  ip_src[4];      /* source address */
    UBYTE  ip_dst[4];      /* destination address */
};

/*
 * UDP header
 */
struct UDPHeader {
    USHORT uh_sport;       /* source port */
    USHORT uh_dport;       /* destination port */
    USHORT uh_ulen;        /* datagram length */
    USHORT uh_sum;         /* UDP checksum */
};

/*
 * TFTP
 */
/* packet types */
#define    OP_RRQ    1            /* read request */
#define    OP_WRQ    2            /* write request */
#define    OP_DATA   3            /* data packet */
#define    OP_ACK    4            /* acknowledgement */
#define    OP_ERROR  5            /* error code */

/* error codes */
#define    EUNDEF      0        /* not defined */
#define    ENOTFOUND   1        /* file not found */
#define    EACCESS     2        /* access violation */
#define    ENOSPACE    3        /* disk full or allocation exceeded */
#define    EBADOP      4        /* illegal TFTP operation */
#define    EBADID      5        /* unknown transfer ID */
#define    EEXISTS     6        /* file already exists */
#define    ENOUSER     7        /* no such user */
#define    EOPTNEG     8        /* option negotiation failed */

#define TFTP_MAX_DATA_SIZE 512
#define TFTP_MAX_BLK_NUM 65535
