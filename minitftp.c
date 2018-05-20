/*
 * minitftp.c - simple TFTP client which can just send a file
 *
 * Copyright(C) 2018 Constantin Wiemer
 */



/*
 * included files
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>



/*
 * global constants
 */
#define MAX_PKT_SIZE 1024

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


/*
 * functions
 */
char *get_basename(const char *fname)
{
    char *pos = strrchr(fname, '/');

    if (pos)
        /* slash found - return pointer to character after the slash */
        return pos + 1;
    else
        /* no slash found - return the file name */
        return (char *) fname;
}


int send_req_packet(int sockfd, const struct addrinfo *addr, int opcode, const char *fname)
{
    uint8_t pktbuf[MAX_PKT_SIZE], *pos;
    int pktsize;

    pos = pktbuf;
    *((uint16_t *) pos) = htons(opcode);        /* opcode */
    pos += 2;
    /* length of the file name has been checked before, so the packet buffer is
     * guaranteed to be large enough */
    /* TODO: Maybe it's better to check the length here */
    strcpy((char *) pos, fname);                /* file name */
    pos += strlen(fname) + 1;
    strcpy((char *) pos, "NETASCII");           /* mode */
    pktsize = strlen(fname) + 12;
    return sendto(sockfd, pktbuf, pktsize, 0, addr->ai_addr, addr->ai_addrlen);
}


int send_data_packet(int sockfd, const struct addrinfo *addr, const uint8_t *data, uint16_t blknum, uint16_t datalen)
{
    uint8_t pktbuf[MAX_PKT_SIZE], *pos;
    int pktsize;

    pos = pktbuf;
    *((uint16_t *) pos) = htons(OP_DATA);       /* opcode */
    pos += 2;
    *((uint16_t *) pos) = htons(blknum);        /* block number */
    pos += 2;
    if (datalen > TFTP_MAX_DATA_SIZE)
        /* TODO: set errno accordingly or use some other mechanism to report errors */
        return -1;
    memcpy(pos, data, datalen);
    pktsize = datalen + 4;
    return sendto(sockfd, pktbuf, pktsize, 0, addr->ai_addr, addr->ai_addrlen);
}


int recv_packet(int sockfd, struct addrinfo *addr, uint8_t *pkt)
{
    return recvfrom(sockfd, pkt, MAX_PKT_SIZE, 0, addr->ai_addr, &(addr->ai_addrlen));
}


int get_opcode(uint8_t *pkt)
{
    return ntohs(*((uint16_t *) pkt));
}


int get_blknum(uint8_t *pkt)
{
    return ntohs(*((uint16_t *) (pkt + 2)));
}


/*
 * main function
 */
int main(int argc, char **argv)
{
    int infd, sockfd, error = 0;
    struct addrinfo addr_hints, *addr;
    char *fname, *bname;
    uint8_t pkt[MAX_PKT_SIZE];

    fname = argv[3];
    bname = get_basename(argv[3]);
    if (strlen(bname) > MAX_PKT_SIZE - 12) {
        /* 12 bytes are needed for the rest of the header */
        printf("ERROR: file name too long\n");
        return 1;
    }

    if ((infd = open(fname, O_RDONLY | O_CLOEXEC)) != -1) {
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) != -1) {
            memset(&addr_hints, 0, sizeof(struct addrinfo));
            addr_hints.ai_family   = AF_INET;
            addr_hints.ai_socktype = SOCK_DGRAM;
            addr_hints.ai_protocol = IPPROTO_UDP;
            if (getaddrinfo(argv[1], argv[2], &addr_hints, &addr) == 0) {
                /* send WRQ packet,
                   we use the first address from the list returned by getaddrinfo() */
                if (send_req_packet(sockfd, addr, OP_WRQ, bname) != -1) {
                    printf("DEBUG: sent packet to server\n");
                }
                else {
                    perror("ERROR: failed to send packet to server");
                    error = 1;
                }

                /* wait for ACK / ERROR packet */
                if (recv_packet(sockfd, addr, pkt) != -1) {
                    printf("DEBUG: received packet from server\n");
                    switch (get_opcode(pkt)) {
                        case OP_ACK:
                            printf("DEBUG: received OP_ACK from server - starting file transfer\n");
                            /* send file block by block */
                            uint16_t blknum = 0;
                            int buflen = 0;
                            char buffer[TFTP_MAX_DATA_SIZE];
                            do {
                                ++blknum;
                                if ((buflen = read(infd, buffer, TFTP_MAX_DATA_SIZE)) == -1) {
                                    /* TODO: send error packet to server */
                                    perror("ERROR: error occurred while reading from file");
                                    error = 1;
                                }
                                if (send_data_packet(sockfd, addr, (uint8_t *) buffer, blknum, buflen) == -1) {
                                    printf("ERROR: error occurred while sending file to server\n");
                                    error = 1;
                                }
                                if (recv_packet(sockfd, addr, pkt) == -1) {
                                    perror("ERROR: failed to receive packet from server");
                                    error = 1;
                                }
                                switch (get_opcode(pkt)) {
                                    case OP_ACK:
                                        if (get_blknum(pkt) == blknum) {
                                            printf("DEBUG: ACK received for sent packet - sending next packet\n");
                                        }
                                        else {
                                            printf("ERROR: ACK with unexpected block number - terminating\n");
                                            error = 1;
                                        }
                                        break;
                                    case OP_ERROR:
                                        printf("ERROR: received OP_ERROR from server\n");
                                        error = 1;
                                        break;
                                    default:
                                        printf("DEBUG: received unknown opcode from server\n");
                                        error = 1;
                                        break;
                                }
                            } while ((buflen == TFTP_MAX_DATA_SIZE) && !error);
                            printf("DEBUG: transmitted file successfully\n");
                            break;

                        case OP_ERROR:
                            printf("ERROR: received OP_ERROR from server - terminating\n");
                            error = 1;
                            break;

                        default:
                            printf("DEBUG: received unknown opcode from server - terminating\n");
                            error = 1;
                            break;
                    }
                }
                else {
                    perror("ERROR: failed to receive packet from server");
                    error = 1;
                }
                freeaddrinfo(addr);
            }
            else {
                perror("ERROR: failed to retrieve address information");
                error = 1;
            }
            close(sockfd);
        }
        else {
            perror("ERROR: failed to create socket");
            error = 1;
        }
        close(infd);
    }
    else {
        perror("ERROR: failed to open file for reading");
        error = 1;
    }

    return error;
}

