/*
 * netio.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *           over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


#include "netio.h"


ULONG netio_errno = 0;


/*
 * copy data between two buffers and SLIP-encode them on the way
 */
static LONG slip_encode_buffer(Buffer *dbuf, const Buffer *sbuf)
{
    const UBYTE *src = sbuf->b_addr;
    UBYTE *dst       = dbuf->b_addr;
    int nbytes, nbytes_tot = 0;
    /*
     * The limit for nbytes_tot has to be length of the destination buffer - 1
     * because due to the escaping mechanism in SLIP, we can get two bytes in
     * one pass of the loop.
     */
    netio_errno = ERROR_BUFFER_OVERFLOW;
    for (nbytes = 0;
        nbytes < sbuf->b_size && nbytes_tot < MAX_BUFFER_SIZE - 1; 
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
    dbuf->b_size = nbytes_tot;
    if (nbytes < sbuf->b_size) {
        LOG("ERROR: could not copy all bytes to the destination\n");
        return DOSFALSE;
    }
    netio_errno = 0;
    return DOSTRUE;
}


/*
 * copy data between two buffers and SLIP-decode them on the way
 */
static LONG slip_decode_buffer(Buffer *dbuf, const Buffer *sbuf)
{
    const UBYTE *src = sbuf->b_addr;
    UBYTE *dst       = dbuf->b_addr;
    int nbytes;
    netio_errno = ERROR_BUFFER_OVERFLOW;
    for (nbytes = 0;
        nbytes < sbuf->b_size && nbytes < MAX_BUFFER_SIZE; 
		nbytes++, src++, dst++) {
        if (*src == SLIP_ESC) {
            ++src;
            if (*src == SLIP_ESCAPED_END)
                *dst = SLIP_END;
            else if (*src == SLIP_ESCAPED_ESC)
                *dst = SLIP_ESC;
            else {
                LOG("ERROR: invalid escape sequence found in SLIP frame: 0x%02lx\n", (ULONG) *src);
                netio_errno = ERROR_BAD_NUMBER;
                break;
            }
        }
        else
            *dst = *src;
    }
    dbuf->b_size = nbytes;
    if (nbytes < sbuf->b_size) {
        LOG("ERROR: could not copy all bytes to the destination\n");
        return DOSFALSE;
    }
    netio_errno = 0;
    return DOSTRUE;
}


/*
 * calculate IP / ICMP checksum (taken from the code for in_cksum() floating on the net)
 */
static USHORT calc_checksum(const UBYTE * bytes, ULONG len)
{
    ULONG sum, i;
    USHORT * p;

    sum = 0;
    p = (USHORT *) bytes;

    for (i = len; i > 1; i -= 2)                /* sum all 16-bit words */
        sum += *p++;

    if (i == 1)                                 /* add an odd byte if necessary */
        sum += (USHORT) *((UBYTE *) p);

    sum = (sum >> 16) + (sum & 0x0000ffff);     /* fold in upper 16 bits */
    sum += (sum >> 16);                         /* add carry bits */
    return ~((USHORT) sum);                     /* return 1-complement truncated to 16 bits */
}


/*
 * UDP routines
 */
static Buffer *create_udp_packet(const Buffer *data)
{
    Buffer *pkt;
    UDPHeader hdr;

    if ((UDP_HDR_LEN + data->b_size) > MAX_BUFFER_SIZE) {
        LOG("ERROR: UDP packet would exceed maximum buffer size\n");
        netio_errno = ERROR_BUFFER_OVERFLOW;
        return NULL;
    }

    /* create buffer large enough to hold the UDP header and the data */
    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for UDP packet\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return NULL;
    }

    /* build UPD header (without checksum) and copy it to buffer */
    memset(&hdr, 0, UDP_HDR_LEN);
    hdr.uh_sport = htons(4711);
    hdr.uh_dport = htons(69);
    hdr.uh_ulen  = htons(UDP_HDR_LEN + data->b_size);
    memcpy(pkt->b_addr, &hdr, sizeof(UDPHeader));

    /* copy data */
    memcpy(pkt->b_addr + sizeof(UDPHeader), data->b_addr, data->b_size);
    pkt->b_size = UDP_HDR_LEN + data->b_size;
    netio_errno = 0;
    return pkt;
}


static Buffer *get_data_from_udp_packet(const Buffer *pkt)
{
    Buffer *data;

    if ((data = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for UDP data\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return NULL;
    }
    memcpy(data->b_addr, pkt->b_addr + sizeof(UDPHeader), pkt->b_size - sizeof(UDPHeader));
    data->b_size = pkt->b_size - sizeof(UDPHeader);
    netio_errno = 0;
    return data;

}


/*
 * IP routines
 */
static Buffer *create_ip_packet(const Buffer *data)
{
    Buffer *pkt;
    IPHeader hdr;

    if ((IP_HDR_LEN + data->b_size) > MAX_BUFFER_SIZE) {
        LOG("ERROR: IP packet would exceed maximum buffer size\n");
        netio_errno = ERROR_BUFFER_OVERFLOW;
        return NULL;
    }

    /* create buffer large enough to hold the IP header and the data */
    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for IP packet\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return NULL;
    }

    /* build IP header and copy it to buffer */
    /* TODO: supply destination IP address as argument */
    memset(&hdr, 0, IP_HDR_LEN);
    hdr.ip_v   = 4;                                   /* version */
    hdr.ip_hl  = IP_HDR_LEN / 4;                      /* header length in 32-bit words */
    hdr.ip_len = htons(IP_HDR_LEN + data->b_size);    /* length of datagram in octets */
    hdr.ip_ttl = 255;                                 /* time-to-live */
    hdr.ip_p   = IPPROTO_UDP;                         /* transport layer protocol */
    hdr.ip_src[0] = 127;                              /* source address */
    hdr.ip_src[1] = 0;
    hdr.ip_src[2] = 0;
    hdr.ip_src[3] = 1;
    hdr.ip_dst[0] = 127;                              /* destination address */
    hdr.ip_dst[1] = 0;
    hdr.ip_dst[2] = 0;
    hdr.ip_dst[3] = 99;
    hdr.ip_sum = calc_checksum((UBYTE *) &hdr, IP_HDR_LEN);
    memcpy(pkt->b_addr, &hdr, sizeof(IPHeader));
    
    /* copy data */
    memcpy(pkt->b_addr + sizeof(IPHeader), data->b_addr, data->b_size);
    pkt->b_size = IP_HDR_LEN + data->b_size;
    netio_errno = 0;
    return pkt;
}


static Buffer *get_data_from_ip_packet(const Buffer *pkt)
{
    Buffer *data;

    if ((data = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for IP data\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return NULL;
    }
    memcpy(data->b_addr, pkt->b_addr + sizeof(IPHeader), pkt->b_size - sizeof(IPHeader));
    data->b_size = pkt->b_size - sizeof(IPHeader);
    netio_errno = 0;
    return data;

}


/*
 * SLIP routines
 */
static Buffer *create_slip_frame(const Buffer *data)
{
    Buffer *frame;

    /* create buffer large enough to hold the IP header and the data */
    if ((frame = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for SLIP frame\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return NULL;
    }

    if (slip_encode_buffer(frame, data) == DOSFALSE) {
        LOG("ERROR: could not copy all data to the SLIP frame\n");
        /* netio_errno has already been set by slip_encode_buffer() */
        return NULL;
    }
    
    /* add SLIP end-of-frame marker */
    if (frame->b_size < MAX_BUFFER_SIZE) {
        *(frame->b_addr + frame->b_size) = SLIP_END;
        ++frame->b_size;
    }
    else {
        LOG("ERROR: could not add SLIP end-of-frame marker\n");
        netio_errno = ERROR_BUFFER_OVERFLOW;
        return NULL;
    }
    netio_errno = 0;
    return frame;
}


static LONG send_slip_frame(struct IOExtSer *req, const Buffer *frame, BOOL async)
{
    BYTE error;

    req->io_SerFlags     &= ~SERF_EOFMODE;      /* clear EOF mode */
    req->IOSer.io_Command = CMD_WRITE;
    req->IOSer.io_Length  = frame->b_size;
    req->IOSer.io_Data    = (APTR) frame->b_addr;
    if (async) {
        SendIO((struct IORequest *) req);
        error = 0;
    }
    else
        error = DoIO((struct IORequest *) req);
    netio_errno = error;
    if (error == 0)
        return DOSTRUE;
    else
        return DOSFALSE;
}


static LONG recv_slip_frame(struct IOExtSer *req, Buffer *frame)
{
    BYTE error;

    /* TODO: make read command time out */
    req->io_SerFlags     |= SERF_EOFMODE;       /* set EOF mode */
    req->IOSer.io_Command = CMD_READ;
    req->IOSer.io_Length  = MAX_BUFFER_SIZE;
    req->IOSer.io_Data    = (APTR) frame->b_addr;
    error = DoIO((struct IORequest *) req);
    netio_errno = error;
    if (error == 0) {
//        LOG("DEBUG: dump of received SLIP frame (%ld bytes):\n", frame->b_size);
//        dump_buffer(frame);
        frame->b_size = req->IOSer.io_Actual;
        return DOSTRUE;
    }
    else
        return DOSFALSE;
}


/*
 * TFTP routines
 */
static LONG send_tftp_packet(struct IOExtSer *req, Buffer *pkt, BOOL async)
{
    Buffer *curbuf, *prevbuf;

    /*
     * Creating a new buffer for each protocol layer (UDP, IP, SLIP) and copying the buffer
     * from the layer above each time would probably not have been very efficient back in the
     * days of the Amiga, but nowadays it doesn't matter and allows for a cleaner design.
     * As each function creates a new buffer and returns a pointer to it, we can delete the
     * previous buffer once the new has been created (and ignore the memory leak when an error occurs).
     */
    curbuf  = pkt;
    prevbuf = curbuf;
    if ((curbuf = create_udp_packet(prevbuf)) == NULL) {
        LOG("ERROR: could not create UDP packet\n");
        /* netio_errno has already been set by create_udp_packet() */
        return DOSFALSE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = create_ip_packet(prevbuf)) == NULL) {
        LOG("ERROR: could not create IP packet\n");
        /* netio_errno has already been set by create_ip_packet() */
        return DOSFALSE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = create_slip_frame(prevbuf)) == NULL) {
        LOG("ERROR: could not create SLIP frame\n");
        /* netio_errno has already been set by create_slip_frame() */
        return DOSFALSE;
    }
    delete_buffer(prevbuf);
    if (send_slip_frame(req, curbuf, async) == DOSFALSE) {
        LOG("ERROR: error occurred while sending SLIP frame: %ld\n", netio_errno);
    }
    delete_buffer(curbuf);
    if (netio_errno == 0)
        return DOSTRUE;
    else
        return DOSFALSE;

}


LONG send_tftp_req_packet(struct IOExtSer *req, USHORT opcode, const char *fname)
{
    Buffer *pkt;
    UBYTE *pos;

    /*
     * length of packet = 2 bytes for the opcode
     *                  + length of the file name
     *                  + terminating NUL byte
     *                  + 8 bytes for the mode "NETASCII"
     *                  + terminating NUL byte
     */
    if ((strlen(fname) + 12) > MAX_BUFFER_SIZE) {
        LOG("ERROR: TFTP packet would exceed maximum buffer size\n");
        netio_errno = ERROR_BUFFER_OVERFLOW;
        return DOSFALSE;
    }

    /* create buffer large enough to hold the TFTP packet */
    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for TFTP packet\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return DOSFALSE;
    }

    pos = pkt->b_addr;
    *((USHORT *) pos) = htons(opcode);        /* opcode */
    pos += 2;
    strcpy((char *) pos, fname);              /* file name */
    pos += strlen(fname) + 1;
    strcpy((char *) pos, "NETASCII");         /* mode */
    pkt->b_size = strlen(fname) + 12;
    
    return send_tftp_packet(req, pkt, 1);     /* send asynchronously */
}


LONG send_tftp_data_packet(struct IOExtSer *req, USHORT blknum, const UBYTE *bytes, LONG nbytes)
{
    Buffer *pkt;
    UBYTE *pos;

    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for TFTP packet\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return DOSFALSE;
    }
    pos = pkt->b_addr;
    *((USHORT *) pos) = htons(OP_DATA);       /* opcode */
    pos += 2;
    *((USHORT *) pos) = htons(blknum);        /* block number */
    pos += 2;
    /* As we are called in a loop, if there are still more than TFTP_MAX_DATA_SIZE bytes
     * in the buffer, we only send TFTP_MAX_DATA_SIZE bytes, otherwise the complete buffer */
    if (nbytes > TFTP_MAX_DATA_SIZE)
        nbytes = TFTP_MAX_DATA_SIZE;
    memcpy(pos, bytes, nbytes);
    pkt->b_size = nbytes + 4;
    LOG("DEBUG: size of TFTP packet = %ld\n", pkt->b_size);

    return send_tftp_packet(req, pkt, 1);     /* send asynchronously */
}


LONG recv_tftp_packet(struct IOExtSer *req, Buffer *pkt)
{
    Buffer *curbuf, *prevbuf;

    if ((curbuf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for SLIP frame\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return DOSFALSE;
    }
    if (recv_slip_frame(req, curbuf) == DOSFALSE) {
        LOG("ERROR: error occurred while receiving SLIP frame: %ld\n", netio_errno);
        /* netio_errno has already been set by recv_slip_frame() */
        return DOSFALSE;
    }

    prevbuf = curbuf;
    if ((curbuf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for IP packet\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return DOSFALSE;
    }
    if (slip_decode_buffer(curbuf, prevbuf) == DOSFALSE) {
        LOG("ERROR: error occured while decoding SLIP frame\n");
        /* netio_errno has already been set by slip_decode_buffer() */
        return DOSFALSE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = get_data_from_ip_packet(prevbuf)) == NULL) {
        LOG("ERROR: error occurred while extracting data from IP packet\n");
        /* netio_errno has already been set by get_data_from_ip_packet() */
        return DOSFALSE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = get_data_from_udp_packet(prevbuf)) == NULL) {
        LOG("ERROR: error occurred while extracting data from UDP packet\n");
        /* netio_errno has already been set by get_data_from_udp_packet() */
        return DOSFALSE;
    }

    memcpy(pkt->b_addr, curbuf->b_addr, curbuf->b_size);
    pkt->b_size = curbuf->b_size;
    delete_buffer(prevbuf);
    delete_buffer(curbuf);
    netio_errno = 0;
    return DOSTRUE;
}


USHORT get_opcode(const Buffer *pkt)
{
    return ntohs(*((USHORT *) pkt->b_addr));
}


USHORT get_blknum(const Buffer *pkt)
{
    return ntohs(*((USHORT *) (pkt->b_addr + 2)));
}
