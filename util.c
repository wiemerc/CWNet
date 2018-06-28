/*
 * util.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *          over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


#include "util.h"


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
 * convert a BCPL to a C string
 */
char *BCPL_TO_C_STR(char *buffer, BSTR str)
{
    memcpy(buffer,
           ((char *) BCPL_TO_C_PTR(str)) + 1,
           ((char *) BCPL_TO_C_PTR(str))[0]);
    buffer[(int) ((char *) BCPL_TO_C_PTR(str))[0]] = 0;
    return buffer;
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


/*
 * create a hexdump of a buffer
 */
void dump_buffer(const Buffer *buffer)
{
    ULONG pos = 0, i, nchars;
    char line[256], *p;

    /* For some reason, we have to use the 'l' modifier for all integers in sprintf(),
     * otherwise only zeros instead of the real values are printed. Maybe this is
     * because sprintf() from amiga.lib defaults to 16-bit integers, but GCC always uses
     * 32-bit integers? Anyway, it works now... */
    while (pos < buffer->b_size) {
        LOG("DEBUG: %04lx: ", pos);
        for (i = pos, p = line, nchars = 0; (i < pos + 16) && (i < buffer->b_size); ++i, ++p, ++nchars) {
            LOG("%02lx ", (ULONG) buffer->b_addr[i]);
            if (buffer->b_addr[i] >= 0x20 && buffer->b_addr[i] <= 0x7e) {
                sprintf(p, "%lc", buffer->b_addr[i]);
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
        *p = '\0';

        LOG("\t%s\n", line);
        pos += 16;
    }
}
