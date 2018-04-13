/*
 * sertftp.c - simple TFTP client (which can just send a file) which uses
 *             SLIP / IP / UDP over a (virtual) serial line
 *
 * Copyright(C) 2018 Constantin Wiemer
 */



/*
 * included files
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>



/*
 * global constants
 */
#define MAX_PKT_SIZE 1024



/*
 * definition of network packets (SLIP, IP, UDP, TFTP), taken from FreeBSD
 */

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



/*
 * functions
 */
char *GetBaseName(const char *fname)
{
    char *pos = strrchr(fname, '/');

    if (pos)
        /* slash found - return pointer to character after the slash */
        return pos + 1;
    else
        /* no slash found - return the file name */
        return fname;
}


int SendReqPacket(int sockfd, int opcode, const char *fname)
{
    uint8_t pktbuf[MAX_PKT_SIZE], *pos;
    int pktsize;

    pos = pktbuf;
    *pos = 0;                                   /* high byte of opcode */
    ++pos;
    *pos = opcode & 0xff;                       /* low byte of opcode */
    ++pos;
    strcpy((char *) pos, fname);                /* file name */
    pos += strlen(fname) + 1;
    strcpy((char *) pos, "octet");              /* mode */
    pktsize = strlen(fname) + 9;
    return send(sockfd, pktbuf, pktsize, 0);
}


int ReceivePacket(int sockfd, uint8_t *pkt)
{
    return recv(sockfd, pkt, MAX_PKT_SIZE, 0);
}


int GetOpCode(uint8_t *pkt)
{
    return *(pkt + 1);
}


/*
 * main function
 */
int main(int argc, char **argv)
{
    int sockfd;
    struct addrinfo addr_hints, *addr;
    char *fname, *bname;
    uint8_t pkt[MAX_PKT_SIZE];

    fname = argv[3];
    bname = GetBaseName(argv[3]);
    if (strlen(bname) > MAX_PKT_SIZE - 9) {
        /* 9 bytes are needed for the rest of the header */
        printf("ERROR: file name too long\n");
        return 1;
    }

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) != -1) {
        memset(&addr_hints, 0, sizeof(struct addrinfo));
        addr_hints.ai_family   = AF_INET;
        addr_hints.ai_socktype = SOCK_DGRAM;
        addr_hints.ai_protocol = IPPROTO_UDP;
        if (getaddrinfo(argv[1], argv[2], &addr_hints, &addr) == 0) {
            /* "connect" to server so we can use send() and recv(),
               we use the first address from the list returned by getaddrinfo() */
            if (connect(sockfd, addr->ai_addr, addr->ai_addrlen) != -1) {
                /* send WRQ packet */
                if (SendReqPacket(sockfd, OP_WRQ, bname) != -1) {
                    printf("DEBUG: sent packet to server\n");
                }
                else {
                    perror("ERROR: failed to send packet to server");
                    return 1;
                }

                /* wait for ACK / ERROR packet */
                if (ReceivePacket(sockfd, pkt) != -1) {
                    printf("DEBUG: received packet from server\n");
                    switch (GetOpCode(pkt)) {
                        case OP_ACK:
                            printf("DEBUG: received ACK from server\n");
                            break;

                        case OP_ERROR:
                            printf("DEBUG: received ERROR from server\n");
                            break;

                        default:
                            printf("DEBUG: received unknown opcode from server\n");
                            break;
                    }
                }
                else {
                    perror("ERROR: failed to receive packet from server");
                    return 1;
                }
            }
            else {
                perror("ERROR: failed to connect to server");
                return 1;
            }
            freeaddrinfo(addr);
        }
        else {
            perror("ERROR: failed to retrieve address information");
            return 1;
        }
    }
    else {
        perror("ERROR: failed to create socket");
        return 1;
    }

    return 0;
}

