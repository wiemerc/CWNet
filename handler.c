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
#include <exec/alerts.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#include "netio.h"


typedef struct 
{
    ULONG dummy;
} FileTransfer;


/*
 * constants
 */
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


char *BCPL_TO_C_STR(char *buffer, BSTR str) {
    memcpy(buffer,
           ((char *) BCPL_TO_C_PTR(str)) + 1,
           ((char *) BCPL_TO_C_PTR(str))[0]);
    buffer[(int) ((char *) BCPL_TO_C_PTR(str))[0]] = 0;
    return buffer;
}


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
    Buffer                  *tftppkt;
    ULONG                    running = 1, error, blknum;
    LONG                     nbytes_to_send;
    char                     fname[256], *p;
    void                    *bytes;


    /* wait for startup packet */
    WaitPort(port);
    dospkt = (struct DosPacket *) GetMsg(port)->mn_Node.ln_Name;

    /* tell DOS not to start a new handler for every request */
    dnode = (struct DeviceNode *) BCPL_TO_C_PTR(dospkt->dp_Arg3);
    dnode->dn_Task = port;

    /* return packet */
    return_dos_packet(dospkt, DOSTRUE, dospkt->dp_Res2);


    /* 
     * initialize logging and the serial device and allocate a buffer for received TFTP packets
     * There is a race condition here: It seems we have to return the startup packet 
     * before we call Open() or DoIO(), but a client could already send a packet to us 
     * before these calls have finished, which would result in undefined behaviour. 
     * However, as we are started when the mount command is issued, this is unlikely.
     */
//    Alert(AT_DeadEnd | AN_Unknown | AG_BadParm | AO_Unknown);
    if ((logport = CreateMsgPort()) == NULL)
        goto ENOPORT;
    if ((logfh = Open("CON:0/0/800/200/CWNET Console", MODE_NEWFILE)) == 0)
        goto ENOLOG;
    if ((req = (struct IOExtSer *) CreateExtIO(port, sizeof(struct IOExtSer))) == NULL) {
        LOG("CRITICAL: could not create request for serial device\n");
        goto ENOREQ;
    }
    if (OpenDevice("serial.device", 0l, (struct IORequest *) req, 0l) != 0) {
        LOG("CRITICAL: could not open serial device\n");
        goto ENODEV;
    }

    /* configure device to terminate read requests on SLIP end-of-frame-markers */
    /* TODO: What about other flags? Flow control? */
    req->IOSer.io_Command = SDCMD_SETPARAMS;
    memset(&req->io_TermArray, SLIP_END, 8);
    if (DoIO((struct IORequest *) req) != 0) {
        LOG("CRITICAL: could not configure serial device\n");
        goto ESERCONF;
    }

    if ((tftppkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("CRITICAL: could not allocate memory for TFTP packet\n");
        goto ENOMEM;
    }
    LOG("INFO: initialization complete - waiting for requests\n");


    /* TODO: use custom error code, e. g. ERROR_WRITE_REQ_FAILED, instead of 999 */
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
                if ((ftx = (FileTransfer *) AllocVec(sizeof(FileTransfer), 0)) != NULL) {
                    /* TODO: initialize FileTransfer structure */
                    fh->fh_Arg1 = (LONG) ftx;
                    fh->fh_Port = (struct MsgPort *) DOSTRUE;   /* TODO: really necessary? */

                    /* send write request to server */
                    /* TODO: pass IP address of server to tftp_send_req_packet() */
                    BCPL_TO_C_STR(fname, dospkt->dp_Arg3);
                    p = fname;
                    p = strrchr(p, ':') + 1;        /* skip device name and colon */
                    ++p; ++p;                       /* skip leading slashes */
                    p = strchr(p, '/') + 1;         /* skip IP address and slash */
                    if (send_tftp_req_packet(req, OP_WRQ, p) == 0) {
                        LOG("DEBUG: sent write request to server\n");
                        if (recv_tftp_packet(req, tftppkt) == 0) {
                            LOG("DEBUG: dump of received packet (%ld bytes):\n", tftppkt->b_size);
                            dump_packet(tftppkt->b_addr, tftppkt->b_size);
                            switch (get_opcode(tftppkt)) {
                                case OP_ACK:
                                    LOG("DEBUG: OP_ACK received from server\n");
                                    return_dos_packet(dospkt, DOSTRUE, 0);
                                    break;
                                case OP_ERROR:
                                    /* TODO: map TFTP error codes to AmigaDOS or custom error codes */
                                    LOG("ERROR: OP_ERROR received from server\n");
                                    return_dos_packet(dospkt, DOSFALSE, 999);
                                    break;
                                default:
                                    LOG("ERROR: unknown opcode received from server\n");
                                    return_dos_packet(dospkt, DOSFALSE, 999);
                            }
                        }
                        else {
                            LOG("ERROR: reading answer from server failed\n");
                            return_dos_packet(dospkt, DOSFALSE, 999);
                        }
                    }
                    else {
                        LOG("ERROR: sending write request to server failed\n");
                        return_dos_packet(dospkt, DOSFALSE, 999);
                    }

                }
                else {
                    LOG("ERROR: could not allocate memory for FileTransfer structure\n");
                    return_dos_packet(dospkt, DOSFALSE, ERROR_NO_FREE_STORE);
                }
                break;


            case ACTION_WRITE:
                LOG("INFO: packet type = ACTION_WRITE\n");
                LOG("DEBUG: buffer size = %ld\n", dospkt->dp_Arg3);
                bytes          = (APTR) dospkt->dp_Arg2;
                nbytes_to_send = dospkt->dp_Arg3;
                blknum         = 1;
                error          = 0;
                while (nbytes_to_send > 0 && !error) {
                    if (send_tftp_data_packet(req, blknum, bytes, nbytes_to_send) == 0) {
                        LOG("DEBUG: sent data packet #%ld to server\n", blknum);
                        if (recv_tftp_packet(req, tftppkt) == 0) {
                            LOG("DEBUG: dump of received packet (%ld bytes):\n", tftppkt->b_size);
                            dump_packet(tftppkt->b_addr, tftppkt->b_size);
                            switch (get_opcode(tftppkt)) {
                                case OP_ACK:
                                    if (get_blknum(tftppkt) == blknum) {
                                        LOG("DEBUG: ACK received for sent packet - sending next packet\n");
                                        bytes = ((UBYTE *) bytes) + TFTP_MAX_DATA_SIZE;
                                        nbytes_to_send -= TFTP_MAX_DATA_SIZE;
                                        ++blknum;
                                    }
                                    else {
                                        LOG("ERROR: ACK with unexpected block number %d received - terminating\n", get_blknum(tftppkt));
                                        return_dos_packet(dospkt, DOSFALSE, 999);
                                        error = 1;
                                    }
                                    break;
                                case OP_ERROR:
                                    /* TODO: map TFTP error codes to AmigaDOS or custom error codes */
                                    LOG("ERROR: OP_ERROR received from server\n");
                                    return_dos_packet(dospkt, DOSFALSE, 999);
                                    error = 1;
                                    break;
                                default:
                                    LOG("ERROR: unknown opcode received from server\n");
                                    return_dos_packet(dospkt, DOSFALSE, 999);
                                    error = 1;
                            }
                        }
                        else {
                            LOG("ERROR: reading answer from server failed\n");
                            return_dos_packet(dospkt, DOSFALSE, 999);
                            error = 1;
                        }
                    }
                    else {
                        LOG("ERROR: sending write request to server failed\n");
                        return_dos_packet(dospkt, DOSFALSE, 999);
                        error = 1;
                    }
                }
                if (!error)
                    LOG("INFO: complete buffer sent to server\n");
                    return_dos_packet(dospkt, dospkt->dp_Arg3, 0);
                break;


            case ACTION_END:
                LOG("INFO: packet type = ACTION_END\n");
                return_dos_packet(dospkt, DOSTRUE, 0);
                /* TODO */
                break;


            case ACTION_DIE:
                LOG("INFO: packet type = ACTION_DIE\n");
                LOG("INFO: ACTION_DIE packet received - shutting down\n");
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
