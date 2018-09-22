/*
 * handler.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *             over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <devices/serial.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <exec/io.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "util.h"
#include "netio.h"


/*
 * structures holding all the information of an ongoing file transfer
 */
typedef struct
{
    struct Node fb_node;    /* so that these structures can be put into a list */
    APTR        fb_bytes;
    APTR        fb_curpos;
    LONG        fb_nbytes_to_send;
} FileBuffer;
typedef struct 
{
    struct Node ftx_node;   /* so that these structures can be put into a list */
    char        ftx_fname[256];
    ULONG       ftx_state;
    ULONG       ftx_blknum;
    ULONG       ftx_error;
    struct List ftx_buffers;
} FileTransfer;


/*
 * The declaration below is a workaround for a bug in GCC which causes it to create a
 * corrupt executable (additional null word at the end of HUNK_DATA) if there are no
 * relocations for the data block. By referencing a string constant, which is placed at
 * the beginning of the code block, we make sure there is at least one relocation.
 */
static const char *dummy = "bla";


/*
 * global variables
 */
struct MsgPort *logport;
BPTR logfh;
char logmsg[256];


/*
 * return_dos_packet - return a DOS packet to its sender
 */
static void return_dos_packet(struct DosPacket *pkt, LONG res1, LONG res2)
{
    struct MsgPort *port = pkt->dp_Port;
    struct Message *inmsg  = pkt->dp_Link;
    inmsg->mn_Node.ln_Name = (char *) pkt;
    pkt->dp_Port = &(((struct Process *) FindTask(NULL))->pr_MsgPort);
    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    PutMsg(port, inmsg);
}


/*
 * get_next_file - get next file from queue that is ready for transfer (or NULL)
 */
static FileTransfer *get_next_file(const struct List *transfers)
{
    FileTransfer *ftx = transfers.lh_Head;
    while (ftx && (ftx->ftx_state != S_READY))
        ftx = ftx->ftx_node.ln_Succ;
    return ftx;
}


/*
 * main function
 */
/* TODO: switch from GCC to VBCC and declare variable where they're used (C99) */
void entry()
{
    struct Process          *proc = (struct Process *) FindTask(NULL);
    struct MsgPort          *port = &(proc->pr_MsgPort);
    struct DeviceNode       *dnode;
    struct Message          *inmsg;
    struct Message           outmsg;
    struct DosPacket        *dospkt;
    struct IOExtSer         *req;
    struct FileHandle       *fh;
    FileTransfer            *ftx;
    FileBuffer              *fbuf;
    struct List              transfers;
    Buffer                  *tftppkt;
    ULONG                    running = 1, busy = 0;
    char                     fname[256], *nameptr;
    APTR                     bufptr;


    /* wait for startup packet */
    WaitPort(port);
    dospkt = (struct DosPacket *) GetMsg(port)->mn_Node.ln_Name;

    /* tell DOS not to start a new handler for every request */
    dnode = (struct DeviceNode *) BCPL_TO_C_PTR(dospkt->dp_Arg3);
    dnode->dn_Task = port;

    /* return packet */
    return_dos_packet(dospkt, DOSTRUE, dospkt->dp_Res2);


    /* 
     * There is a race condition here: It seems we have to return the startup packet 
     * before we call Open() or DoIO(), but a client could already send a packet to us 
     * before these calls have finished, which would result in undefined behaviour. 
     * However, as we are started when the mount command is issued, this is unlikely.
     */
    /* initialize logging */
    if ((logport = CreateMsgPort()) == NULL)
        goto ENOPORT;
    if ((logfh = Open("CON:0/0/800/200/CWNET Console", MODE_NEWFILE)) == 0)
        goto ENOLOG;

    /* initialize serial device */
    if ((req = (struct IOExtSer *) CreateExtIO(port, sizeof(struct IOExtSer))) == NULL) {
        LOG("CRITICAL: could not create request for serial device\n");
        goto ENOREQ;
    }
    if (OpenDevice("serial.device", 0l, (struct IORequest *) req, 0l) != 0) {
        LOG("CRITICAL: could not open serial device\n");
        goto ENODEV;
    }

    /* configure device to terminate read requests on SLIP end-of-frame-markers and disable flow control */
    /* TODO: configure device for maximum speed */
//    req->io_SerFlags     |= SERF_XDISABLED | SERF_RAD_BOOGIE;
    req->io_SerFlags     |= SERF_XDISABLED;
//    req->io_Baud          = 292000l;
    req->IOSer.io_Command = SDCMD_SETPARAMS;
    memset(&req->io_TermArray, SLIP_END, 8);
    if (DoIO((struct IORequest *) req) != 0) {
        LOG("CRITICAL: could not configure serial device\n");
        goto ESERCONF;
    }

    /* allocate a buffer for received TFTP packets */
    if ((tftppkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("CRITICAL: could not allocate memory for TFTP packet\n");
        goto ENOMEM;
    }

    /* initialize list of file transfers */
    NewList(&transfers);
    LOG("INFO: initialization complete - waiting for requests\n");
    
    /* initialize internal message */
    outmsg.mn_ReplyPort    = port;
    outmsg.mn_Length       = sizeof(struct Message);
    outmsg.mn_Node.ln_Type = NT_MESSAGE;


    /* TODO: reverse conditions to get rid of nested ifs */
    /*
     * main message loop
     * We receive and handle 3 types of messages here:
     * - IO completion messages from the serial device
     * - DOS packets
     * - internal messages sent by ourselves for certain events
     *
     * We use internal messages instead of a state variable, because otherwise we
     * could get blocked in WaitPort() forever.
     *
     * state machine of a file transfer:
                                             |---------<--------|  /-- S_FINISHED
     * S_QUEUED --> S_READY --> S_REQ_SENT --|--> S_DATA_SENT --|--
     *                                                             \-- S_ERROR
     */
    while(running) {
        LOG("DEBUG: waiting for message...\n");
        WaitPort(port);
        inmsg = GetMsg(port);

        /*
         * internal message
         */
        if (inmsg == &outmsg) {
            /* ln_Pri is used as message type because ln_Type is already used by Exec (always NT_MESSAGE) */
            switch (inmsg->mn_Node.ln_Pri) {
                case MSG_NEW_FILE:
                    LOG("DEBUG: received internal message of type MSG_NEW_FILE\n");
                    if (!busy) {
                        busy = 1;
                        ftx = get_next_file(&transfers);
                        req->IOSer.io_Message.mn_Node.ln_Name = ftx;    /* TODO: Can we overwrite ln_Name? */
                        if (send_tftp_req_packet(req, OP_WRQ, ftx->ftx_name) == DOSTRUE) {
                            LOG("DEBUG: sent write request for file '%s' to server\n", ftx->ftx_name);
                            ftx->ftx_state = S_REQ_SENT;
                        }
                        else {
                            LOG("ERROR: sending write request for file '%s' to server failed\n", ftx->ftx_name);
                            ftx->ftx_state = S_ERROR;
                            ftx->ftx_error = netio_errno;
                            busy = 0;
                            SEND_MSG(MSG_FILE_FAILED, ftx);
                        }
                    }
                    break;
                    
                case MSG_NEW_BUFFER:
                    LOG("DEBUG: received internal message of type MSG_NEW_BUFFER\n");
                    ftx  = inmsg->mn_Node.ln_Name;
                    fbuf = ftx->ftx_buffers.lh_Head;
                    if (fbuf) {
                        if (send_tftp_data_packet(req, ftx->ftx_blknum, fbuf->fb_curpos, fbuf->fb_nbytes_to_send) == DOSTRUE) {
                            LOG("DEBUG: sent data packet #%ld to server\n", ftx->ftx_blknum);
                            ftx->ftx_state = S_DATA_SENT;
                        }
                        else {
                            LOG("ERROR: sending data packet #%ld to server failed\n", ftx->ftx_blknum);
                            ftx->ftx_state = S_ERROR;
                            ftx->ftx_error = netio_errno;
                            busy = 0;
                            SEND_MSG(MSG_FILE_FAILED, ftx);
                        }
                    }
                    else {
                        LOG("INFO: file has been completely transfered\n");
                        ftx->ftx_state = S_FINISHED;
                        busy = 0;
                        SEND_MSG(MSG_FILE_FINISHED, ftx);
                    }
                    break;
                    
                case MSG_CONTINUE_BUFFER:
                    ftx  = inmsg->mn_Node.ln_Name;
                    fbuf = ftx->ftx_buffers.lh_Head;
                    fbuf->fb_curpos = ((UBYTE *) fbuf->fb_curpos) + TFTP_MAX_DATA_SIZE;
                    ++ftx->ftx_blknum;
                    if (send_tftp_data_packet(req, ftx->ftx_blknum, fbuf->fb_curpos, fbuf->fb_nbytes_to_send) == DOSTRUE) {
                        LOG("DEBUG: sent data packet #%ld to server\n", ftx->ftx_blknum);
                        ftx->ftx_state = S_DATA_SENT;
                    }
                    else {
                        LOG("ERROR: sending data packet #%ld to server failed\n", ftx->ftx_blknum);
                        ftx->ftx_state = S_ERROR;
                        ftx->ftx_error = netio_errno;
                        busy = 0;
                        SEND_MSG(MSG_FILE_FAILED, ftx);
                    }
                    break;
                    
                case MSG_FILE_FINISHED:
                case MSG_FILE_FAILED:
                    LOG("DEBUG: received internal message of type MSG_FILE_FINISHED / MSG_FILE_FAILED\n");
                    ftx = inmsg->mn_Node.ln_Name;
                    /* list of buffers is empty in case of a finished file (buffers have
                     * already been freed one by one), so the loop will be skipped */
                    while ((fbuf = RemHead(ftx->ftx_buffers))) {
                        FreeVec(fbuf->fb_bytes);
                        FreeVec(fbuf);
                    }
                    SEND_MSG(MSG_NEW_FILE, NULL);
                    break;
                    
                case MSG_BUFFER_FINISHED:
                    LOG("DEBUG: received internal message of type MSG_BUFFER_FINISHED\n");
                    ftx  = inmsg->mn_Node.ln_Name;
                    fbuf = RemHead(ftx->ftx_buffers);
                    FreeVec(fbuf->fb_bytes);
                    FreeVec(fbuf);
                    SEND_MSG(MSG_NEW_BUFFER);
                    break;
                    
                default:
                    LOG("CRITICAL: internal message of unknown type %ld received\n", inmsg->mn_Node.ln_Pri);
                    busy = 0;
                    running = 0;
            }
        }


        /*
         * replay message from serial device
         */
        else if (inmsg == &(req->IOSer.io_Message)) {
            LOG("DEBUG: received IO completion message\n");
            /* must be a reply message for a write command, but we better check anyway... */
            if (!CheckIO((struct IORequest *) req)) {
                LOG("CRITICAL: IO operation has not completed although IO completion message was received\n");
                busy = 0;
                running = 0;
            }
                
            ftx = req->IOSer.io_Message.mn_Node.ln_Name;
            fbuf = ftx->ftx_buffers.lh_Head;

            /* get status of write command */
            if (WaitIO((struct IORequest *) req) != 0) {
                LOG("ERROR: sending write request / data to server failed with error %ld\n", req->IOSer.io_Error);
                ftx->ftx_state = S_ERROR;
                ftx->ftx_error = req->IOSer.io_Error;
                busy = 0;
                SEND_MSG(MSG_FILE_FAILED, ftx);
            }

            /* read and handle answer from server */
            if (recv_tftp_packet(req, tftppkt) == DOSFALSE) {
                LOG("ERROR: reading answer from server failed\n");
                ftx->ftx_state = S_ERROR;
                ftx->ftx_error = netio_errno;
                busy = 0;
                SEND_MSG(MSG_FILE_FAILED, ftx);
            }
//            LOG("DEBUG: dump of received packet (%ld bytes):\n", tftppkt->b_size);
//            dump_buffer(tftppkt);
            switch (get_opcode(tftppkt)) {
                case OP_ACK:
                    if (ftx->ftx_state == S_REQ_SENT) {
                        SEND_MSG(MSG_NEW_BUFFER, ftx);
                    }
                    else if (ftx->ftx_state == S_DATA_SENT) {
                        if (get_blknum(tftppkt) == ftx->ftx_blknum) {
                            LOG("DEBUG: ACK received for sent data packet - sending next data packet if there is data left\n");
                            fbuf->fb_nbytes_to_send -= TFTP_MAX_DATA_SIZE;
                            if (fbuf->fb_nbytes_to_send > 0) {
                                SEND_MSG(MSG_CONTINUE_BUFFER, ftx);
                                LOG("DEBUG: sending next data packet to server\n");
                            }
                            else {
                                LOG("DEBUG: complete buffer sent to server - sending next buffer if there is one left\n");
                                SEND_MSG(MSG_BUFFER_FINISHED, ftx);
                            }
                        }
                        else {
                            LOG("ERROR: ACK with unexpected block number %ld received - terminating\n", (ULONG) get_blknum(tftppkt));
                            ftx->ftx_state = S_ERROR;
                            ftx->ftx_error = ERROR_TFTP_WRONG_BLOCK_NUM;
                            busy = 0;
                            SEND_MSG(MSG_FILE_FAILED, ftx);
                        }
                    }
                    else {
                        LOG("CRITICAL: file transfer is in wrong state %ld\n", ftx->ftx_state);
                        busy = 0;
                        running = 0;
                    }
                    break;
                    
                case OP_ERROR:
                    LOG("ERROR: OP_ERROR received from server\n");
                    ftx->ftx_state = S_ERROR;
                    /* TODO: map TFTP error codes to AmigaDOS or custom error codes */
                    ftx->ftx_error = ERROR_TFTP_GENERIC_ERROR;
                    busy = 0;
                    SEND_MSG(MSG_FILE_FAILED, ftx);
                    break;
                    
                default:
                    LOG("ERROR: unknown opcode received from server\n");
                    ftx->ftx_state = S_ERROR;
                    ftx->ftx_error = ERROR_TFTP_UNKNOWN_OPCODE;
                    busy = 0;
                    SEND_MSG(MSG_FILE_FAILED, ftx);
            }
        }
        
        
        /*
         * DOS packet
         */
        else {
            dospkt = (struct DosPacket *) inmsg->mn_Node.ln_Name;
            LOG("DEBUG: received DOS packet of type %ld\n", dospkt->dp_Type);
            switch (dospkt->dp_Type) {
                case ACTION_IS_FILESYSTEM:
                    LOG("INFO: packet type = ACTION_IS_FILESYSTEM\n");
                    return_dos_packet(dospkt, DOSFALSE, 0);
                    break;


                case ACTION_FINDOUTPUT:
                    LOG("INFO: packet type = ACTION_FINDOUTPUT\n");
                    fh = (struct FileHandle *) BCPL_TO_C_PTR(dospkt->dp_Arg1);
                    BCPL_TO_C_STR(fname, dospkt->dp_Arg3);
                    nameptr = fname;
                    nameptr = strrchr(nameptr, ':') + 1;        /* skip device name and colon */
                    ++nameptr; ++nameptr;                       /* skip leading slashes */
                    nameptr = strchr(nameptr, '/') + 1;         /* skip IP address and slash */

                    /* initialize FileTransfer structure, queue it and save a pointer in the file handle
                     * We need to reset the block number once *per file* here, and not for every 
                     * ACTION_WRITE packet, otherwise the last packet of a buffer doesn't get 
                     * saved by the server because the block number would be reset to 1 in the 
                     * middle of a transfer and the server would assume a duplicate packet. */
                    if ((ftx = (FileTransfer *) AllocVec(sizeof(FileTransfer), 0)) != NULL) {
                        ftx->ftx_state  = S_QUEUED;
                        ftx->ftx_blknum = 1;
                        ftx->ftx_error  = 0;
                        strncpy(ftx->ftx_name, nameptr, 256);                        
                        NewList(&(ftx->ftx_buffers));
                        AddTail(&transfers, (struct Node *) ftx);
                        fh->fh_Arg1 = (LONG) ftx;
                        fh->fh_Port = (struct MsgPort *) DOSTRUE;   /* TODO: really necessary? */
                        
                        return_dos_packet(dospkt, DOSTRUE, 0);
                    }
                    else {
                        LOG("ERROR: could not allocate memory for FileTransfer structure\n");
                        return_dos_packet(dospkt, DOSFALSE, ERROR_NO_FREE_STORE);
                    }
                    break;


                case ACTION_WRITE:
                    LOG("INFO: packet type = ACTION_WRITE\n");
    //                LOG("DEBUG: buffer size = %ld\n", dospkt->dp_Arg3);
                    ftx = (FileTransfer *) dospkt->dp_Arg1;
                    /* initialize FileBuffer structure and queue it (one buffer for each ACTION_WRITE packet */
                    if ((fbuf = (FileBuffer *) AllocVec(sizeof(FileBuffer), 0)) != NULL) {
                        /* We need to copy the buffer because we return the packet before the 
                        * buffer is sent and the client is free to reuse / free the buffer once the
                        * packet has been returned. */
                        if ((fbuf->fb_bytes = AllocVec(dospkt->dp_Arg3, 0)) != NULL) {
                            memcpy(fbuf->fb_bytes, dospkt->dp_Arg2, dospkt->dp_Arg3);
                            fbuf->fb_curpos         = fbuf->fb_bytes;
                            fbuf->fb_nbytes_to_send = dospkt->dp_Arg3;
                            AddTail(&(ftx->ftx_buffers), (struct Node *) fbuf);
                            
                            return_dos_packet(dospkt, DOSTRUE, 0);
                        }
                        else {
                            LOG("ERROR: could not allocate memory for data buffer\n");
                            return_dos_packet(dospkt, DOSFALSE, ERROR_NO_FREE_STORE);
                            ftx->ftx_state = S_ERROR;
                            ftx->ftx_error = ERROR_NO_FREE_STORE;
                            SEND_MSG(MSG_FILE_FAILED);
                        }
                    }
                    else {
                        LOG("ERROR: could not allocate memory for FileBuffer structure\n");
                        return_dos_packet(dospkt, DOSFALSE, ERROR_NO_FREE_STORE);
                        ftx->ftx_state = S_ERROR;
                        ftx->ftx_error = ERROR_NO_FREE_STORE;
                        SEND_MSG(MSG_FILE_FAILED);
                    }
                    break;


                case ACTION_END:
                    LOG("INFO: packet type = ACTION_END\n");
                    ftx = (FileTransfer *) dospkt->dp_Arg1;
                    return_dos_packet(dospkt, DOSTRUE, 0);
                    
                    /* We only inform ourselves now that a file has been added and is ready 
                     * for transfer in order to prevent a race condition between buffers being
                     * added and being sent. Otherwise it could happen that we transfer bufffers
                     * faster than we receive them and would therefore assume the file has been
                     * transfered completely somewhere in the middle of the file. */
                    ftx->ftx_state = S_READY;
                    SEND_MSG(MSG_NEW_FILE, NULL);
                    break;


                case ACTION_LOCATE_OBJECT:
                case ACTION_EXAMINE_OBJECT:
                case ACTION_EXAMINE_NEXT:
                case ACTION_DELETE_OBJECT:
                    /* TODO: implement queue handling (list / remove entries) via these actions */
                    break;

                case ACTION_DIE:
                    LOG("INFO: packet type = ACTION_DIE\n");
                    LOG("INFO: ACTION_DIE packet received - shutting down\n");
                    /* TODO: abort ongoing IO operations and remove file transfers from list */
                    /* tell DOS not to send us any more packets */
                    dnode->dn_Task = NULL;
                    running = 0;
                    return_dos_packet(dospkt, DOSTRUE, 0);
                    break;


                default:
                    LOG("ERROR: packet type is unknown\n");
                    return_dos_packet(dospkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
            }
        }
    }


    Delay(150);
    delete_buffer(tftppkt);
ENOMEM:
ESERCONF:
    CloseDevice((struct IORequest *) req);
ENODEV:
    DeleteExtIO((struct IORequest *) req);
ENOREQ:
    Close(logfh);
ENOLOG:
    DeleteMsgPort(logport);
ENOPORT:
}
