#ifndef CWNET_NETIO_H
#define CWNET_NETIO_H
/*
 * netio.h - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *           over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <devices/serial.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <exec/io.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/exec.h>

#include "util.h"
#include "dos.h"


/*
 * SLIP protocol
 */
#define SLIP_END                0xc0
#define SLIP_ESCAPED_END        0xdc
#define SLIP_ESC                0xdb
#define SLIP_ESCAPED_ESC        0xdd

/* IP and UDP protocol headers, adapted from FreeBSD */
/*
 * IP
 */
/* TODO: Do we really need to consider byte order? */
typedef struct {
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
} IPHeader;
#define IP_HDR_LEN (sizeof(IPHeader))

/*
 * UDP
 */
typedef struct {
    USHORT uh_sport;       /* source port */
    USHORT uh_dport;       /* destination port */
    USHORT uh_ulen;        /* datagram length */
    USHORT uh_sum;         /* UDP checksum */
} UDPHeader;
#define IPPROTO_UDP 17
#define UDP_HDR_LEN (sizeof(UDPHeader))

/*
 * TFTP
 */
#define TFTP_MAX_DATA_SIZE 512
#define TFTP_MAX_BLK_NUM 65535

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


/*
 * states
 */
#define S_QUEUED       0
#define S_READY        1
#define S_WRQ_SENT     2
#define S_RRQ_SENT     3
#define S_DATA_SENT    4
#define S_ERROR        5
#define S_FINISHED     6


#define IOExtTime timerequest   /* just to make the code look a bit nicer... */
#define NETIO_TIMEOUT 10        /* timeout for reads and writes in seconds */


/*
 * function prototypes
 */
LONG netio_init(const struct DosPacket *iopkt1, const struct DosPacket *iopkt2);
void netio_exit();
BYTE netio_get_status();
void netio_stop_timer();
void netio_abort();
LONG send_tftp_req_packet(USHORT opcode, const char *fname);
LONG send_tftp_data_packet(USHORT blknum, const UBYTE *bytes, LONG nbytes);
LONG recv_tftp_packet();
LONG extract_tftp_packet(Buffer *pkt);
USHORT get_opcode(const Buffer *pkt);
USHORT get_blknum(const Buffer *pkt);


/*
 * external references
 */
extern struct MsgPort   *g_port;
extern ULONG             g_netio_errno;    /* network IO error code */

#endif /* CWNET_NETIO_H */
