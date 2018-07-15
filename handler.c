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
 * structure holding all the information of an ongoing file transfer
 */
typedef struct 
{
    struct Node ftx_node;   /* so that these structures can be put into a list */
    ULONG       ftx_state;
    ULONG       ftx_blknum;
    APTR        ftx_bytes;
    LONG        ftx_nbytes_to_send;
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
    struct Message *msg  = pkt->dp_Link;
    msg->mn_Node.ln_Name = (char *) pkt;
    pkt->dp_Port = &(((struct Process *) FindTask(NULL))->pr_MsgPort);
    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    PutMsg(port, msg);
}


/*
 * start transfer of a file
 */
static LONG start_file_transfer(struct IOExtSer *req, const char *fname)
{
    Buffer *pkt;

    if ((pkt = create_buffer(MAX_BUFFER_SIZE)) != NULL) {
        if (send_tftp_req_packet(req, OP_WRQ, fname) == DOSTRUE) {
            LOG("DEBUG: sent write request to server\n");
            if (recv_tftp_packet(req, pkt) == DOSTRUE) {
    //            LOG("DEBUG: dump of received packet (%ld bytes):\n", pkt->b_size);
    //            dump_buffer(pkt);
                switch (get_opcode(pkt)) {
                    case OP_ACK:
                        LOG("DEBUG: OP_ACK received from server\n");
                        netio_errno = 0;
                        return DOSTRUE;
                    case OP_ERROR:
                        /* TODO: map TFTP error codes to AmigaDOS or custom error codes and set netio_errno */
                        LOG("ERROR: OP_ERROR received from server\n");
                        netio_errno = ERROR_TFTP_GENERIC_ERROR;
                        return DOSFALSE;
                    default:
                        LOG("ERROR: unknown opcode received from server\n");
                        netio_errno = ERROR_TFTP_UNKNOWN_OPCODE;
                        return DOSFALSE;
                }
            }
            else {
                LOG("ERROR: reading answer from server failed\n");
                /* netio_errno has already been set by recv_tftp_packet() */
                return DOSFALSE;
            }
        }
        else {
            LOG("ERROR: sending write request to server failed\n");
            /* netio_errno has already been set by send_tftp_req_packet() */
            return DOSFALSE;
        }
    }
    else {
        LOG("CRITICAL: could not allocate memory for TFTP packet\n");
        netio_errno = ERROR_NO_FREE_STORE;
        return DOSFALSE;
    }
}


/*
 * main function
 */
void entry()
{
    struct Process          *proc = (struct Process *) FindTask(NULL);
    struct MsgPort          *port = &(proc->pr_MsgPort);
    struct DeviceNode       *dnode;
    struct DosPacket        *dospkt;
    struct IOExtSer         *req;
    struct FileHandle       *fh;
    FileTransfer            *ftx;
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


    while(running) {
        WaitPort(port);
        dospkt = (struct DosPacket *) GetMsg(port)->mn_Node.ln_Name;
        LOG("DEBUG: received DOS packet with type %ld\n", dospkt->dp_Type);
        switch (dospkt->dp_Type) {
            case ACTION_IS_FILESYSTEM:
                LOG("INFO: packet type = ACTION_IS_FILESYSTEM\n");
                return_dos_packet(dospkt, DOSFALSE, 0);
                break;


            case ACTION_FINDOUTPUT:
                LOG("INFO: packet type = ACTION_FINDOUTPUT\n");
                fh = (struct FileHandle *) BCPL_TO_C_PTR(dospkt->dp_Arg1);

                /* initialize FileTransfer structure, queue it and save a pointer in the file handle */
                if ((ftx = (FileTransfer *) AllocVec(sizeof(FileTransfer), 0)) != NULL) {
                    ftx->ftx_state = S_IDLE;
                    AddTail(&transfers, (struct Node *) ftx);
                    fh->fh_Arg1 = (LONG) ftx;
                    fh->fh_Port = (struct MsgPort *) DOSTRUE;   /* TODO: really necessary? */

                    /* start file transfer immediately if no other transfer is ongoing */
                    if (!busy) {
                        busy = 1;

                        /* TODO: pass IP address of server to send_tftp_req_packet() */
                        BCPL_TO_C_STR(fname, dospkt->dp_Arg3);
                        nameptr = fname;
                        nameptr = strrchr(nameptr, ':') + 1;        /* skip device name and colon */
                        ++nameptr; ++nameptr;                       /* skip leading slashes */
                        nameptr = strchr(nameptr, '/') + 1;         /* skip IP address and slash */

                        if (start_file_transfer(req, nameptr) == DOSTRUE) {
                            ftx->ftx_state = S_WRQ_ACKED;
                            /* We need to reset the block number once *per file* here,
                                * and not for every ACTION_WRITE packet, otherwise the last
                                * packet of a buffer doesn't get saved by the server because
                                * the block number would be reset to 1 in the middle of a
                                * transfer and the server would assume a duplicate packet. */
                            ftx->ftx_blknum = 1;
                            return_dos_packet(dospkt, DOSTRUE, 0);
                        }
                        else {
                            LOG("ERROR: starting transfer of file '%s' failed (error code %ld)\n", nameptr, netio_errno);
                            return_dos_packet(dospkt, DOSFALSE, netio_errno);
                        }
                    }
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
                /* We need to copy the buffer because we return the packet before the complete
                 * buffer is sent and the client is free to reuse / free the buffer once the
                 * packet has been returned. */
                if ((ftx->ftx_bytes = AllocVec(dospkt->dp_Arg3, 0)) != NULL) {
                    bufptr = (APTR) dospkt->dp_Arg2;
                    memcpy(ftx->ftx_bytes, bufptr, dospkt->dp_Arg3);
                    ftx->ftx_nbytes_to_send = dospkt->dp_Arg3;
                    ftx->ftx_state          = S_IDLE;
                    while (ftx->ftx_nbytes_to_send > 0 && ftx->ftx_state != S_ERROR) {
                        if (send_tftp_data_packet(req, ftx->ftx_blknum, bufptr, ftx->ftx_nbytes_to_send) == DOSTRUE) {
                            LOG("DEBUG: sent data packet #%ld to server\n", ftx->ftx_blknum);
                            if (recv_tftp_packet(req, tftppkt) == DOSTRUE) {
    //                            LOG("DEBUG: dump of received packet (%ld bytes):\n", tftppkt->b_size);
    //                            dump_buffer(tftppkt);
                                switch (get_opcode(tftppkt)) {
                                    case OP_ACK:
                                        if (get_blknum(tftppkt) == ftx->ftx_blknum) {
                                            LOG("DEBUG: ACK received for sent packet - sending next packet\n");
                                            bufptr = ((UBYTE *) bufptr) + TFTP_MAX_DATA_SIZE;
                                            ftx->ftx_nbytes_to_send -= TFTP_MAX_DATA_SIZE;
                                            ++ftx->ftx_blknum;
                                        }
                                        else {
                                            LOG("ERROR: ACK with unexpected block number %ld received - terminating\n", (ULONG) get_blknum(tftppkt));
                                            return_dos_packet(dospkt, DOSFALSE, ERROR_TFTP_WRONG_BLOCK_NUM);
                                            ftx->ftx_state = S_ERROR;
                                        }
                                        break;
                                    case OP_ERROR:
                                        /* TODO: map TFTP error codes to AmigaDOS or custom error codes */
                                        LOG("ERROR: OP_ERROR received from server\n");
                                        return_dos_packet(dospkt, DOSFALSE, ERROR_TFTP_GENERIC_ERROR);
                                        ftx->ftx_state = S_ERROR;
                                        break;
                                    default:
                                        LOG("ERROR: unknown opcode received from server\n");
                                        return_dos_packet(dospkt, DOSFALSE, ERROR_TFTP_UNKNOWN_OPCODE);
                                        ftx->ftx_state = S_ERROR;
                                }
                            }
                            else {
                                LOG("ERROR: reading answer from server failed\n");
                                return_dos_packet(dospkt, DOSFALSE, netio_errno);
                                ftx->ftx_state = S_ERROR;
                            }
                        }
                        else {
                            LOG("ERROR: sending write request to server failed\n");
                            return_dos_packet(dospkt, DOSFALSE, netio_errno);
                            ftx->ftx_state = S_ERROR;
                        }
                    }
                    if (ftx->ftx_state != S_ERROR) {
                        LOG("INFO: complete buffer sent to server\n");
                        return_dos_packet(dospkt, dospkt->dp_Arg3, 0);
                    }
                }
                else {
                    LOG("ERROR: could not allocate memory for data buffer\n");
                    return_dos_packet(dospkt, DOSFALSE, ERROR_NO_FREE_STORE);
                }
                break;


            case ACTION_END:
                LOG("INFO: packet type = ACTION_END\n");
                return_dos_packet(dospkt, DOSTRUE, 0);
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
