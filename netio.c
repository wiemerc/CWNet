/*
 * netio.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *           over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


#include "netio.h"


ULONG netio_errno = 0;
static struct IOExtSer *sreq;
static struct IOExtTime *treq;


/*
 * initialize this module
 */
LONG netio_init(const struct DosPacket *iopkt1, const struct DosPacket *iopkt2)
{
    /* initialize serial device */
    if ((sreq = (struct IOExtSer *) CreateExtIO(g_port, sizeof(struct IOExtSer))) != NULL) {
        /* add DOS packet to IO request so that IO completion messages can be handled as internal packets */
        sreq->IOSer.io_Message.mn_Node.ln_Name = (char *) iopkt1;
        if ((treq = (struct IOExtTime *) CreateExtIO(g_port, sizeof(struct IOExtTime)))) {
            /* add DOS packet to IO request so that IO completion messages can be handled as internal packets */
            treq->tr_node.io_Message.mn_Node.ln_Name = (char *) iopkt2;
            if (OpenDevice("serial.device", 0l, (struct IORequest *) sreq, 0l) == 0) {
                /* configure device to terminate read requests on SLIP end-of-frame-markers and disable flow control */
                /* 
                 * TODO: configure device for maximum speed:
                sreq->io_SerFlags     |= SERF_XDISABLED | SERF_RAD_BOOGIE;
                sreq->io_Baud          = 292000l;
                 */
                sreq->io_SerFlags     |= SERF_XDISABLED;
                sreq->IOSer.io_Command = SDCMD_SETPARAMS;
                memset(&sreq->io_TermArray, SLIP_END, 8);
                if (DoIO((struct IORequest *) sreq) == 0) {
                    if (OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) treq, 0l) == 0) {
                        LOG("INFO: network IO module initialized\n");
                        return DOSTRUE;
                    }
                    else {
                        LOG("CRITICAL: could not open timer device\n");
                        CloseDevice((struct IORequest *) sreq);
                        DeleteExtIO((struct IORequest *) treq);
                        DeleteExtIO((struct IORequest *) sreq);
                        return DOSFALSE;
                    }
                }
                else {
                    LOG("CRITICAL: could not configure serial device\n");
                    CloseDevice((struct IORequest *) sreq);
                    DeleteExtIO((struct IORequest *) treq);
                    DeleteExtIO((struct IORequest *) sreq);
                    return DOSFALSE;
                }
            }
            else {
                LOG("CRITICAL: could not open serial device\n");
                DeleteExtIO((struct IORequest *) treq);
                DeleteExtIO((struct IORequest *) sreq);
                return DOSFALSE;
            }
        }
        else {
            LOG("CRITICAL: could not create request for timer device\n");
            DeleteExtIO((struct IORequest *) sreq);
            return DOSFALSE;
        }
    }
    else {
        LOG("CRITICAL: could not create request for serial device\n");
        return DOSFALSE;
    }
}


/*
 * free all ressources
 */
void netio_exit()
{
    CloseDevice((struct IORequest *) treq);
    CloseDevice((struct IORequest *) sreq);
    DeleteExtIO((struct IORequest *) treq);
    DeleteExtIO((struct IORequest *) sreq);
}


/*
 * get status of last IO operation 
 * 
 * returns:
 * 0 if last operation was successful
 * the error code from the serial device (values > 0) if an error occurred
 * -1 if the status could not be determined, netio_errno is set in this case
 */
BYTE netio_get_status()
{
    /* check if IO operation has actually finished */
    if (!CheckIO((struct IORequest *) sreq)) {
        LOG("ERROR: IO operation has not yet finished\n");
        netio_errno = ERROR_IO_NOT_FINISHED;
        return -1;
    }
    return WaitIO((struct IORequest *) sreq);
}


/*
 * stop the running IO timer
 */
void netio_stop_timer()
{
    /* We ignore any errors that might occur */
    AbortIO((struct IORequest *) treq);
    WaitIO((struct IORequest *) treq);
}


/*
 * abort the current IO operation (called when a timeout occurs)
 */
void netio_abort()
{
    /* We ignore any errors that might occur */
    AbortIO((struct IORequest *) sreq);
    WaitIO((struct IORequest *) sreq);
}


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
        netio_errno = ERROR_BUFFER_OVERFLOW;
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


static LONG send_slip_frame(const Buffer *frame, BOOL async)
{
    BYTE error;

    sreq->io_SerFlags     &= ~SERF_EOFMODE;      /* clear EOF mode */
    sreq->IOSer.io_Command = CMD_WRITE;
    sreq->IOSer.io_Length  = frame->b_size;
    sreq->IOSer.io_Data    = (APTR) frame->b_addr;
    if (async) {
        SendIO((struct IORequest *) sreq);
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs    = NETIO_TIMEOUT;
        treq->tr_time.tv_micro   = 0;
        SendIO((struct IORequest *) treq);
        netio_errno = 0;
        return DOSTRUE;
    }
    else {
        error = DoIO((struct IORequest *) sreq);
        netio_errno = error;
        if (error == 0)
            return DOSTRUE;
        else
            return DOSFALSE;
    }
}


static LONG recv_slip_frame(Buffer *frame, BOOL async)
{
    BYTE error;

    /* TODO: It would be better if the whole network stack ran in its own task,
     *       then we could handle timeouts internally instead of in the main loop */
    sreq->io_SerFlags     |= SERF_EOFMODE;       /* set EOF mode */
    sreq->IOSer.io_Command = CMD_READ;
    sreq->IOSer.io_Length  = MAX_BUFFER_SIZE;
    sreq->IOSer.io_Data    = (APTR) frame->b_addr;
    if (async) {
        SendIO((struct IORequest *) sreq);
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs    = NETIO_TIMEOUT;
        treq->tr_time.tv_micro   = 0;
        SendIO((struct IORequest *) treq);
        netio_errno = 0;
        return DOSTRUE;
    }
    else {
        error = DoIO((struct IORequest *) sreq);
        netio_errno = error;
        if (error == 0) {
#if DEBUG
            LOG("DEBUG: dump of received SLIP frame (%ld bytes):\n", frame->b_size);
            dump_buffer(frame);
#endif
            frame->b_size = sreq->IOSer.io_Actual;
            return DOSTRUE;
        }
        else
            return DOSFALSE;
    }
}


/*
 * TFTP routines
 */
static LONG send_tftp_packet(Buffer *pkt, BOOL async)
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
    if (send_slip_frame(curbuf, async) == DOSFALSE) {
        LOG("ERROR: error occurred while sending SLIP frame: %ld\n", netio_errno);
    }
    delete_buffer(curbuf);
    if (netio_errno == 0)
        return DOSTRUE;
    else
        return DOSFALSE;

}


LONG send_tftp_req_packet(USHORT opcode, const char *fname)
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
    
    return send_tftp_packet(pkt, 1);     /* send asynchronously */
}


LONG send_tftp_data_packet(USHORT blknum, const UBYTE *bytes, LONG nbytes)
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

    return send_tftp_packet(pkt, 1);     /* send asynchronously */
}


LONG recv_tftp_packet()
{
    Buffer *buf;

    if ((buf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for SLIP frame\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return DOSFALSE;
    }
    return recv_slip_frame(buf, 1 /* receive asynchronously */);
}


LONG extract_tftp_packet(Buffer *pkt)
{
    Buffer data, *prevbuf, *curbuf;

    /* reconstruct the buffer that was allocated in recv_tftp_packet(), of course this
     * only works when the function is called after a server reply has been read 
     * (the IO operation initiated by recv_tftp_packet() has completed) */
    data.b_addr = sreq->IOSer.io_Data;      /* data read from the server */
    data.b_size = sreq->IOSer.io_Actual;    /* number of bytes read */
    prevbuf = &data;
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
    /* We call FreeVec() directly instead of delete_buffer() because of the reconstructed buffer */
    FreeVec(prevbuf->b_addr - sizeof(Buffer));
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
