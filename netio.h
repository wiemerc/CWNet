#include <devices/serial.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>


/*
 * generic buffer for packets and payloads
 */
typedef struct {
    UBYTE *b_addr;
    ULONG  b_size;
} Buffer;


/* SLIP protocol */
#define SLIP_END                0xc0
#define SLIP_ESCAPED_END        0xdc
#define SLIP_ESC                0xdb
#define SLIP_ESCAPED_ESC        0xdd

/* IP and UDP protocol headers, adapted from FreeBSD */
/*
 * IP header
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

/*
 * UDP header
 */
typedef struct {
    USHORT uh_sport;       /* source port */
    USHORT uh_dport;       /* destination port */
    USHORT uh_ulen;        /* datagram length */
    USHORT uh_sum;         /* UDP checksum */
} UDPHeader;

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

#define MAX_BUFFER_SIZE 1024
#define IP_HDR_LEN (sizeof(IPHeader))
#define UDP_HDR_LEN (sizeof(UDPHeader))
#define IPPROTO_UDP 17


/* TODO: move macros / constants used by several modules to another header */
#define C_TO_BCPL_PTR(ptr) ((BPTR) (((ULONG) (ptr)) >> 2))
#define BCPL_TO_C_PTR(ptr) ((APTR) (((ULONG) (ptr)) << 2))


extern struct MsgPort *logport;
extern BPTR logfh;
extern char logmsg[256];


/*
 * function prototypes
 */
void log(const char *msg);
#define LOG(fmt, ...) {sprintf(logmsg, fmt, ##__VA_ARGS__); log(logmsg);}
void dump_packet(const UBYTE *buffer, ULONG length);
Buffer *create_buffer(ULONG size);
void delete_buffer(const Buffer *buffer);
LONG send_tftp_req_packet(struct IOExtSer *req, USHORT opcode, const char *fname);
LONG send_tftp_data_packet(struct IOExtSer *req, USHORT blknum, const UBYTE *bytes, LONG nbytes);
LONG recv_tftp_packet(struct IOExtSer *req, Buffer *pkt);
USHORT get_opcode(const Buffer *pkt);
USHORT get_blknum(const Buffer *pkt);
