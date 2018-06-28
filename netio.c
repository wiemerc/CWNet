#include "netio.h"


/* TODO: unify return values indicating an error and report the exact error via netio_errno (with custom error codes) */
void dump_packet(const UBYTE *buffer, ULONG length)
{
    ULONG pos = 0, i, nchars;
    char line[256], *p;

    while (pos < length) {
        LOG("DEBUG: %04lx: ", pos);
        for (i = pos, p = line, nchars = 0; (i < pos + 16) && (i < length); ++i, ++p, ++nchars) {
            LOG("%02x ", buffer[i]);
            if (buffer[i] >= 0x20 && buffer[i] <= 0x7e) {
                sprintf(p, "%c", buffer[i]);
            }
            else {
                sprintf(p, ".");
            }
        }
        if (nchars < 16) {
            for (i = 1; i <= (3 * (16 - nchars)); ++i, ++p, ++nchars) {
                sprintf(p, " ");
            }
        }

        LOG("\t%s\n", line);
        pos += 16;
    }
}


/*
 * copy data between two buffers and SLIP-encode them on the way
 */
static LONG copy_and_encode_buffer(Buffer *dbuf, const Buffer *sbuf)
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
    }
    return nbytes;
}


/*
 * copy data between two buffers and SLIP-decode them on the way
 */
static LONG copy_and_decode_buffer(Buffer *dbuf, const Buffer *sbuf)
{
    const UBYTE *src = sbuf->b_addr;
    UBYTE *dst       = dbuf->b_addr;
    int nbytes;
    /*
     * The limit for nbytes_tot has to be length of the destination buffer - 1
     * because due to the escaping mechanism in SLIP, we can get two bytes in
     * one pass of the loop.
     */
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
                LOG("ERROR: invalid escape sequence found in SLIP frame: 0x%02x\n", *src);
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
 * log a message to the console
 */
void log(const char *msg)
{
    struct StandardPacket *pkt;
    if ((pkt = (struct StandardPacket *) AllocVec(sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
        pkt->sp_Msg.mn_ReplyPort    = logport;
        pkt->sp_Pkt.dp_Port         = logport;
        pkt->sp_Msg.mn_Node.ln_Name = (char *) &(pkt->sp_Pkt);
        pkt->sp_Pkt.dp_Link         = &(pkt->sp_Msg);
        pkt->sp_Pkt.dp_Type = ACTION_WRITE;
        pkt->sp_Pkt.dp_Arg1 = ((struct FileHandle *) BCPL_TO_C_PTR(logfh))->fh_Arg1;
        pkt->sp_Pkt.dp_Arg2 = (LONG) msg;
        pkt->sp_Pkt.dp_Arg3 = strlen(msg);
        PutMsg((struct MsgPort *) ((struct FileHandle *) BCPL_TO_C_PTR(logfh))->fh_Type, &(pkt->sp_Msg));
        WaitPort(logport);
        GetMsg(logport);
        FreeVec(pkt);
    }
}


/*
 * create / delete a buffer for packets or payloads
 */
/* TODO: Do we need to store the capacity? */
Buffer *create_buffer(ULONG size)
{
    Buffer *buffer;

    /* allocate a memory block large enough for the Buffer structure and buffer itself */
    if ((buffer = AllocVec(size + sizeof(Buffer), 0)) != NULL) {
        buffer->b_addr = ((UBYTE *) buffer) + sizeof(Buffer);
        buffer->b_size = 0;
        return buffer;
    }
    else
        return NULL;
}


void delete_buffer(const Buffer *buffer)
{
    FreeVec((APTR) buffer);
}


static Buffer *udp_create_packet(const Buffer *data)
{
    Buffer *pkt;
    UDPHeader hdr;

    if ((UDP_HDR_LEN + data->b_size) > MAX_BUFFER_SIZE) {
        LOG("ERROR: UDP packet would exceed maximum buffer size\n");
        return NULL;
    }

    /* create buffer large enough to hold the UDP header and the data */
    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for UDP packet\n");
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
    return pkt;
}


static Buffer *get_data_from_udp_packet(const Buffer *pkt)
{
    Buffer *data;

    if ((data = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for UDP data\n");
        return NULL;
    }
    memcpy(data->b_addr, pkt->b_addr + sizeof(UDPHeader), pkt->b_size - sizeof(UDPHeader));
    data->b_size = pkt->b_size - sizeof(UDPHeader);
    return data;

}


static Buffer *ip_create_packet(const Buffer *data)
{
    Buffer *pkt;
    IPHeader hdr;

    if ((IP_HDR_LEN + data->b_size) > MAX_BUFFER_SIZE) {
        LOG("ERROR: IP packet would exceed maximum buffer size\n");
        return NULL;
    }

    /* create buffer large enough to hold the IP header and the data */
    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for IP packet\n");
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
    return pkt;
}


static Buffer *get_data_from_ip_packet(const Buffer *pkt)
{
    Buffer *data;

    if ((data = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for IP data\n");
        return NULL;
    }
    memcpy(data->b_addr, pkt->b_addr + sizeof(IPHeader), pkt->b_size - sizeof(IPHeader));
    data->b_size = pkt->b_size - sizeof(IPHeader);
    return data;

}


static Buffer *slip_create_frame(const Buffer *data)
{
    Buffer *frame;

    /* create buffer large enough to hold the IP header and the data */
    if ((frame = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for SLIP frame\n");
        return NULL;
    }

    if (copy_and_encode_buffer(frame, data) < data->b_size) {
        LOG("ERROR: could not copy all data to the SLIP frame\n");
        return NULL;
    }
    
    /* add SLIP end-of-frame marker */
    if (frame->b_size < MAX_BUFFER_SIZE) {
        *(frame->b_addr + frame->b_size) = SLIP_END;
        ++frame->b_size;
    }
    else {
        LOG("ERROR: could not add SLIP end-of-frame marker\n");
        return NULL;
    }
    return frame;
}


static BYTE send_slip_frame(struct IOExtSer *req, const Buffer *frame)
{
    req->io_SerFlags      = 0;
    req->IOSer.io_Command = CMD_WRITE;
    req->IOSer.io_Length  = frame->b_size;
    req->IOSer.io_Data    = (APTR) frame->b_addr;
    return DoIO((struct IORequest *) req);
}


static BYTE recv_slip_frame(struct IOExtSer *req, Buffer *frame)
{
    BYTE error;

    req->io_SerFlags     |= SERF_EOFMODE;
    req->IOSer.io_Command = CMD_READ;
    req->IOSer.io_Length  = MAX_BUFFER_SIZE;
    req->IOSer.io_Data    = (APTR) frame->b_addr;
    error = DoIO((struct IORequest *) req);
    if (error == 0)
        frame->b_size = req->IOSer.io_Actual;
    LOG("DEBUG: dump of received SLIP frame:\n");
    dump_packet(frame->b_addr, frame->b_size);
    return error;
}


LONG send_tftp_req_packet(struct IOExtSer *req, USHORT opcode, const char *fname)
{
    Buffer *curbuf, *prevbuf;
    UBYTE *pos;
    LONG error;

    /*
     * length of packet = 2 bytes for the opcode
     *                  + length of the file name
     *                  + terminating NUL byte
     *                  + 8 bytes for the mode "NETASCII"
     *                  + terminating NUL byte
     */
    if ((strlen(fname) + 12) > MAX_BUFFER_SIZE) {
        LOG("ERROR: TFTP packet would exceed maximum buffer size\n");
        return ERROR_BUFFER_OVERFLOW;
    }

    /* create buffer large enough to hold the TFTP packet */
    if ((curbuf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for TFTP packet\n");
        return ERROR_NO_FREE_STORE;
    }

    pos = curbuf->b_addr;
    *((USHORT *) pos) = htons(opcode);        /* opcode */
    pos += 2;
    strcpy((char *) pos, fname);              /* file name */
    pos += strlen(fname) + 1;
    strcpy((char *) pos, "NETASCII");         /* mode */
    curbuf->b_size = strlen(fname) + 12;

    /*
     * Creating a new buffer for each protocol layer (UDP, IP, SLIP) and copying the buffer
     * from the layer above each time would probably not have been very efficient back in the
     * days of the Amiga, but nowadays it doesn't matter and allows for a cleaner design.
     * As each function creates a new buffer and returns a pointer to it, we can delete the
     * previous buffer once the new has been created (and ignore the memory leak when an error occurs).
     */
    prevbuf = curbuf;
    if ((curbuf = udp_create_packet(prevbuf)) == NULL) {
        LOG("ERROR: could not create UDP packet\n");
        return ERROR_NO_FREE_STORE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = ip_create_packet(prevbuf)) == NULL) {
        LOG("ERROR: could not create IP packet\n");
        return ERROR_NO_FREE_STORE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    /* TODO: inline creation of SLIP frame */
    if ((curbuf = slip_create_frame(prevbuf)) == NULL) {
        LOG("ERROR: could not create SLIP frame\n");
        return ERROR_NO_FREE_STORE;
    }
    delete_buffer(prevbuf);
    error = send_slip_frame(req, curbuf);
    if (error != 0) {
        LOG("ERROR: error occurred while sending SLIP frame: %ld\n", error);
    }
    delete_buffer(curbuf);
    return error;
}


LONG recv_tftp_packet(struct IOExtSer *req, Buffer *pkt)
{
    Buffer *curbuf, *prevbuf;
    LONG error;

    if ((curbuf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for SLIP frame\n");
        return ERROR_NO_FREE_STORE;
    }
    error = recv_slip_frame(req, curbuf);
    if (error != 0) {
        LOG("ERROR: error occurred while receiving SLIP frame: %ld\n", error);
        return error;
    }

    prevbuf = curbuf;
    if ((curbuf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for IP packet\n");
        return ERROR_NO_FREE_STORE;
    }
    if (copy_and_decode_buffer(curbuf, prevbuf) == DOSFALSE) {
        LOG("ERROR: error occured while decoding SLIP frame\n");
        return 999;
    }

    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = get_data_from_ip_packet(prevbuf)) == NULL) {
        LOG("ERROR: error occurred while extracting data from IP packet\n");
        return 999;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = get_data_from_udp_packet(prevbuf)) == NULL) {
        LOG("ERROR: error occurred while extracting data from UDP packet\n");
        return 999;
    }

    memcpy(pkt->b_addr, curbuf->b_addr, curbuf->b_size);
    pkt->b_size = curbuf->b_size;
    delete_buffer(prevbuf);
    delete_buffer(curbuf);
    return 0;
}


LONG send_tftp_data_packet(struct IOExtSer *req, USHORT blknum, const UBYTE *bytes, LONG nbytes)
{
    Buffer *curbuf, *prevbuf;
    UBYTE *pos;
    LONG error;

    /*
     * create TFTP packet
     * length of packet = 2 bytes for the opcode
     *                  + 2 bytes for the block number
     *                  + length of the data
     */
    if ((curbuf = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("ERROR: could not create buffer for TFTP packet\n");
        return ERROR_NO_FREE_STORE;
    }
    pos = curbuf->b_addr;
    *((USHORT *) pos) = htons(OP_DATA);       /* opcode */
    pos += 2;
    *((USHORT *) pos) = htons(blknum);        /* block number */
    pos += 2;
    /* As we are called in a loop, if there are still more than TFTP_MAX_DATA_SIZE bytes
     * in the buffer, we only send TFTP_MAX_DATA_SIZE bytes, otherwise the complete buffer */
    if (nbytes > TFTP_MAX_DATA_SIZE)
        nbytes = TFTP_MAX_DATA_SIZE;
    memcpy(pos, bytes, nbytes);
    curbuf->b_size = nbytes + 4;
    LOG("DEBUG: size of TFTP packet = %ld\n", curbuf->b_size);

    prevbuf = curbuf;
    if ((curbuf = udp_create_packet(prevbuf)) == NULL) {
        LOG("ERROR: could not create UDP packet\n");
        return ERROR_NO_FREE_STORE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    if ((curbuf = ip_create_packet(prevbuf)) == NULL) {
        LOG("ERROR: could not create IP packet\n");
        return ERROR_NO_FREE_STORE;
    }
    delete_buffer(prevbuf);
    prevbuf = curbuf;
    /* TODO: inline creation of SLIP frame */
    if ((curbuf = slip_create_frame(prevbuf)) == NULL) {
        LOG("ERROR: could not create SLIP frame\n");
        return ERROR_NO_FREE_STORE;
    }
    delete_buffer(prevbuf);
    error = send_slip_frame(req, curbuf);
    if (error != 0) {
        LOG("ERROR: error occurred while sending SLIP frame: %ld\n", error);
    }
//    LOG("DEBUG: bytes sent = %ld\n", req->IOSer.io_Actual);
    delete_buffer(curbuf);
    return error;
}


USHORT get_opcode(const Buffer *pkt)
{
    return ntohs(*((USHORT *) pkt->b_addr));
}


USHORT get_blknum(const Buffer *pkt)
{
    return ntohs(*((USHORT *) (pkt->b_addr + 2)));
}
