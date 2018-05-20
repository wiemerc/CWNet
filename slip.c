#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>


/* IP and UDP protocol headers, adapted from FreeBSD */
/*
 * IP header
 */
/* TODO: Do we really need to consider byte order? */
struct IPHeader {
#if BYTE_ORDER == LITTLE_ENDIAN
    uint8_t ip_hl:4,        /* header length */
            ip_v:4;         /* version */
#endif
#if BYTE_ORDER == BIG_ENDIAN
    uint8_t ip_v:4,         /* version */
            ip_hl:4;        /* header length */
#endif
    uint8_t  ip_tos;         /* type of service */
    uint16_t ip_len;         /* total length */
    uint16_t ip_id;          /* identification */
    uint16_t ip_off;         /* fragment offset field */
    uint8_t  ip_ttl;         /* time to live */
    uint8_t  ip_p;           /* protocol */
    uint16_t ip_sum;         /* checksum */
    uint8_t  ip_src[4];      /* source address */
    uint8_t  ip_dst[4];      /* destination address */
};

/*
 * UDP header
 */
struct UDPHeader {
    uint16_t uh_sport;       /* source port */
    uint16_t uh_dport;       /* destination port */
    uint16_t uh_ulen;        /* datagram length */
    uint16_t uh_sum;         /* UDP checksum */
};

/* SLIP protocol */
#define SLIP_END                0xc0
#define SLIP_ESCAPED_END        0xdc
#define SLIP_ESC                0xdb
#define SLIP_ESCAPED_ESC        0xdd


#define MAX_PKT_SIZE 65535
#define IP_HDR_LEN (sizeof(struct IPHeader))
#define UDP_HDR_LEN (sizeof(struct UDPHeader))
#define IPPROTO_UDP 17


/*
 * copy data to buffer and SLIP-encode them on the way
 */
int copy_and_encode_data(uint8_t *buffer, const uint8_t *data, int buflen, int datalen)
{
    const uint8_t *src = data;
    uint8_t *dst = buffer;
    int nbytes, nbytes_tot = 0;
    /* The limit for nbytes_tot has to be buflen - 1 because due to the escaping
     * mechanism in SLIP, we can get two bytes in one pass of the loop */
    for (nbytes = 0; nbytes < datalen && nbytes_tot < buflen - 1; 
		nbytes++, nbytes_tot++, src++, dst++) {
        if (*src == SLIP_END) {
            *dst = SLIP_ESC;
            ++dst;
            ++nbytes_tot;
            *dst = SLIP_ESCAPED_END;
        }
        else if (*src == SLIP_ESC) {
            *dst = SLIP_ESC;
            ++dst;
            ++nbytes_tot;
            *dst = SLIP_ESCAPED_ESC;
        }
        else {
            *dst = *src;
        }
    }
    if (nbytes < datalen) {
        printf("could not copy all bytes to the buffer\n");
        return -1;
    }
    else {
        return nbytes_tot;
    }
}


/*
 * calculate IP / ICMP checksum (taken from the code for in_cksum() floating on the net)
 */
static uint16_t calc_checksum(const uint8_t * bytes, uint32_t len)
{
    uint32_t sum, i;
    uint16_t * p;

    sum = 0;
    p = (uint16_t *) bytes;

    for (i = len; i > 1; i -= 2)                /* sum all 16-bit words */
        sum += *p++;

    if (i == 1)                                 /* add an odd byte if necessary */
        sum += (uint16_t) *((uint8_t *) p);

    sum = (sum >> 16) + (sum & 0x0000ffff);     /* fold in upper 16 bits */
    sum += (sum >> 16);                         /* add carry bits */
    return ~((uint16_t) sum);                   /* return 1-complement truncated to 16 bits */
}


/* TODO: return the correct error codes */
static int send_packet(int sockfd, const uint8_t *data, int datalen)
{
    /* build IP header */
    struct IPHeader iphdr;
    memset(&iphdr, 0, IP_HDR_LEN);
    iphdr.ip_v   = 4;                                           /* version */
    iphdr.ip_hl  = IP_HDR_LEN / 4;                              /* header length in 32-bit words */
    iphdr.ip_len = htons(IP_HDR_LEN + UDP_HDR_LEN + datalen);   /* length of datagram in octets */
    iphdr.ip_ttl = 255;                                         /* time-to-live */
    iphdr.ip_p   = IPPROTO_UDP;                                 /* transport layer protocol */
    iphdr.ip_src[0] = 127;                                      /* source address */
    iphdr.ip_src[1] = 0;
    iphdr.ip_src[2] = 0;
    iphdr.ip_src[3] = 1;
    iphdr.ip_dst[0] = 127;                                      /* destination address */
    iphdr.ip_dst[1] = 0;
    iphdr.ip_dst[2] = 0;
    iphdr.ip_dst[3] = 99;
    iphdr.ip_sum = calc_checksum((uint8_t *) &iphdr, IP_HDR_LEN);
    
    /* build UPD header (without checksum) */
    struct UDPHeader udphdr;
    memset(&udphdr, 0, UDP_HDR_LEN);
    udphdr.uh_sport = htons(4711);
    udphdr.uh_dport = htons(69);
    udphdr.uh_ulen  = htons(UDP_HDR_LEN + datalen);

    /* copy headers and user data to packet buffer and SLIP-encode them on the way */
    uint8_t buffer[MAX_PKT_SIZE];
    int nbytes, nbytes_tot = 0;
    if ((nbytes = copy_and_encode_data(buffer + nbytes_tot, 
									   (uint8_t *) &iphdr, 
									   MAX_PKT_SIZE - nbytes_tot, 
									   IP_HDR_LEN)) < IP_HDR_LEN) {
        printf("could not copy all bytes of the IP header to the buffer\n");
        return -1;
    }
    nbytes_tot += nbytes;
    if ((nbytes = copy_and_encode_data(buffer + nbytes_tot, 
									   (uint8_t *) &udphdr, 
									   MAX_PKT_SIZE - nbytes_tot, 
									   UDP_HDR_LEN)) < UDP_HDR_LEN) {
        printf("could not copy all bytes of the UDP header to the buffer\n");
        return -1;
    }
    nbytes_tot += nbytes;
    if ((nbytes = copy_and_encode_data(buffer + nbytes_tot, 
									   data, 
									   MAX_PKT_SIZE - nbytes_tot, 
									   datalen)) < datalen) {
        printf("could not copy all user data to the buffer\n");
        return -1;
    }
    nbytes_tot += nbytes;
    
    /* add SLIP end-of-frame marker */
    if (nbytes_tot < MAX_PKT_SIZE) {
        *(buffer + nbytes_tot) = SLIP_END;
        ++nbytes_tot;
    }
    else {
        printf("could not add SLIP end-of-frame marker\n");
        return -1;
    }

    /* send packet */
    return send(sockfd, buffer, nbytes_tot, 0);
}


/* TODO: use returned error codes instead of errno */
int main(int argc, char **argv)
{
    int error = 0;
    int sockfd;
    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) != -1) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(struct sockaddr_un));
        strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);
        if (connect(sockfd, (struct sockaddr*) &addr, sizeof(struct sockaddr_un)) != -1) {
            printf("connected to TFTP daemon\n");

            /* send a TFTP WRQ packet to the daemon */
            if (send_packet(sockfd, (uint8_t *) "\x00\x02hello.txt\x00NETASCII", 21) != -1) {
                printf("sent WRQ packet to daemon\n");
            }
            else {
                perror("could not send packet to daemon");
                error = 1;
            }
            shutdown(sockfd, SHUT_RDWR);
        }
        else {
            perror("could not connect to TFTP daemon");
            error = 1;
        }
        close(sockfd);
    }
    else {
        perror("could not open socket");
        error = 1;
    }

    return error;
}

