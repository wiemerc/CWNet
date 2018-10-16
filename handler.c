/*
 * handler.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *             over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "util.h"
#include "dos.h"
#include "netio.h"


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
struct MsgPort      *g_logport;                    /* for the LOG() macro */
BPTR                 g_logfh;
char                 g_logmsg[256];
struct MsgPort      *g_port;
struct DeviceNode   *g_dnode;
struct List          g_transfers;                  /* list of all file transfers */
struct List          g_locks;                      /* list of all open locks */
UBYTE                g_running, g_busy;            /* handler state */


/*
 * main function
 */
/* TODO: switch from GCC to VBCC and declare variable where they're used (C99) */
void entry()
{
    struct Message          *msg;
    struct DosPacket        *inpkt, iopkt1, iopkt2;
    struct StandardPacket    outpkt;
    LinkedLock              *llock;
    struct FileLock         *flock;
    FileTransfer            *ftx;
    FileBuffer              *fbuf;
    Buffer                  *tftppkt;


    /* wait for startup packet */
    g_port = &(((struct Process *) FindTask(NULL))->pr_MsgPort);
    WaitPort(g_port);
    inpkt = (struct DosPacket *) GetMsg(g_port)->mn_Node.ln_Name;

    /* tell DOS not to start a new handler for every request */
    g_dnode = (struct DeviceNode *) BCPL_TO_C_PTR(inpkt->dp_Arg3);
    g_dnode->dn_Task = g_port;

    /* return packet */
    return_dos_packet(inpkt, DOSTRUE, inpkt->dp_Res2);


    /* 
     * There is a race condition here: It seems we have to return the startup packet 
     * before we call Open() or DoIO(), but a client could already send a packet to us 
     * before these calls have finished, which would result in undefined behaviour. 
     * However, as we are started when the mount command is issued, this is unlikely.
     */
    /* initialize logging */
    if ((g_logport = CreateMsgPort()) == NULL)
        goto ERROR_NO_PORT;
//    if ((g_logfh = Open("WORK:cwnet.log", MODE_NEWFILE)) == 0)
    if ((g_logfh = Open("CON:0/0/800/200/CWNET Console", MODE_NEWFILE)) == 0)
        goto ERROR_NO_LOGGING;

    /* initialize the network IO module */
    if (netio_init(&iopkt1, &iopkt2) == DOSFALSE) {
        LOG("CRITICAL: could not initialize the network IO module\n");
        goto ERROR_NO_NETIO;
    }

    /* allocate a buffer for received TFTP packets */
    if ((tftppkt = create_buffer(MAX_BUFFER_SIZE)) == NULL) {
        LOG("CRITICAL: could not allocate memory for TFTP packet\n");
        goto ERROR_NO_MEMORY;
    }

    /* initialize lists of file transfers and locks */
    NewList(&g_transfers);
    NewList(&g_locks);
    LOG("INFO: initialization complete - waiting for requests\n");
    
    /* initialize internal DOS packets */
    outpkt.sp_Msg.mn_ReplyPort    = g_port;
    outpkt.sp_Pkt.dp_Port         = g_port;
    outpkt.sp_Msg.mn_Node.ln_Name = (char *) &(outpkt.sp_Pkt);
    outpkt.sp_Pkt.dp_Link         = &(outpkt.sp_Msg);
    iopkt1.dp_Type                = 0;
    iopkt2.dp_Type                = ACTION_TIMER_EXPIRED;


    /*
     * main message loop
     * We receive and handle 2 types of messages here:
     * - DOS packets coming from the OS
     * - internal DOS packets sent by ourselves for certain events
     *
     * We use internal packets instead of a state variable, because otherwise we
     * could get blocked in WaitPort() forever.
     *
     * state machine of a file transfer:
                                             |---------<--------|  /-- S_FINISHED
     * S_QUEUED --> S_READY --> S_WRQ_SENT --|--> S_DATA_SENT --|--
     *                                                             \-- S_ERROR
     */
    g_running = 1;
    g_busy    = 0;
    while(g_running) {
        WaitPort(g_port);
        msg   = GetMsg(g_port);
        inpkt = (struct DosPacket *) msg->mn_Node.ln_Name;
        LOG("DEBUG: received DOS packet of type %ld\n", inpkt->dp_Type);

        switch (inpkt->dp_Type) {
            /*
             * regular actions
             */
            /* TODO: move actions into separate routines in dos.h */
            case ACTION_IS_FILESYSTEM:
                LOG("INFO: packet type = ACTION_IS_FILESYSTEM\n");
                return_dos_packet(inpkt, DOSTRUE, 0);
                break;


            case ACTION_FINDOUTPUT:
                LOG("INFO: packet type = ACTION_FINDOUTPUT\n");
                do_find_output(inpkt);
                break;


            case ACTION_WRITE:
                LOG("INFO: packet type = ACTION_WRITE\n");
                do_write(inpkt, &outpkt);
                break;


            case ACTION_END:
                LOG("INFO: packet type = ACTION_END\n");
                ftx = (FileTransfer *) inpkt->dp_Arg1;
                LOG("INFO: file '%s' is now ready for transfer\n", ftx->ftx_fname);
                return_dos_packet(inpkt, DOSTRUE, 0);
                
                /* We only now inform ourselves that a file has been added and is ready 
                    * for transfer in order to prevent a race condition between buffers being
                    * added and being sent. Otherwise it could happen that we transfer bufffers
                    * faster than we receive them and would therefore assume the file has been
                    * transfered completely somewhere in the middle of the file. */
                ftx->ftx_state = S_READY;
                send_internal_packet(&outpkt, ACTION_SEND_NEXT_FILE, NULL);
                break;


            case ACTION_LOCATE_OBJECT:
                LOG("INFO: packet type = ACTION_LOCATE_OBJECT\n");
                do_locate_object(inpkt);
                break;


            case ACTION_FREE_LOCK:
                LOG("INFO: packet type = ACTION_FREE_LOCK\n");
                flock = BCPL_TO_C_PTR(inpkt->dp_Arg1);
                LOG("DEBUG: lock = 0x%08lx\n", (ULONG) flock);
                if (flock == NULL)
                    return_dos_packet(inpkt, DOSTRUE, 0);
                else if ((llock = find_lock_in_list(flock))) {
                    Remove((struct Node *) llock);
                    FreeVec(llock);
                    return_dos_packet(inpkt, DOSTRUE, 0);
                }
                else {
                    LOG("ERROR: unknown lock\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
                }
                break;


            case ACTION_EXAMINE_OBJECT:
                LOG("INFO: packet type = ACTION_EXAMINE_OBJECT\n");
                do_examine_object(inpkt);
                break;


            case ACTION_EXAMINE_NEXT:
                LOG("INFO: packet type = ACTION_EXAMINE_NEXT\n");
                do_examine_next(inpkt);
                break;


            case ACTION_DIE:
                LOG("INFO: packet type = ACTION_DIE\n");
                LOG("INFO: ACTION_DIE packet received - shutting down\n");

                /* abort ongoing IO operation */
                netio_abort();

                /* tell DOS not to send us any more packets */
                g_dnode->dn_Task = NULL;

                g_running = 0;
                return_dos_packet(inpkt, DOSTRUE, 0);
                break;


            /*
             * internal actions
             */
            case ACTION_SEND_NEXT_FILE:
                LOG("DEBUG: received internal packet of type ACTION_SEND_NEXT_FILE\n");
                if (!g_busy) {
                    if ((ftx = get_next_file_from_queue())) {
                        g_busy = 1;
                        /* store pointer to current FileTransfer structure in DOS packet so
                            * that we can retrieve it in ACTION_WRITE_RETURN below */
                        iopkt1.dp_Type = ACTION_WRITE_RETURN;
                        iopkt1.dp_Arg1 = (LONG) ftx;
                        if (send_tftp_req_packet(OP_WRQ, ftx->ftx_fname) == DOSTRUE) {
                            LOG("DEBUG: sent write request for file '%s' to server\n", ftx->ftx_fname);
                            ftx->ftx_state = S_WRQ_SENT;
                        }
                        else {
                            LOG("ERROR: sending write request for file '%s' to server failed\n", ftx->ftx_fname);
                            ftx->ftx_state = S_ERROR;
                            ftx->ftx_error = netio_errno;
                            g_busy = 0;
                            send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                        }
                    }
                }
                break;


            case ACTION_SEND_NEXT_BUFFER:
                LOG("DEBUG: received internal packet of type ACTION_SEND_NEXT_BUFFER\n");
                ftx  = (FileTransfer *) inpkt->dp_Arg1;
                if (!IsListEmpty(&(ftx->ftx_buffers))) {
                    fbuf = (FileBuffer *) ftx->ftx_buffers.lh_Head;
                    ++ftx->ftx_blknum;
                    iopkt1.dp_Type = ACTION_WRITE_RETURN;
                    if (send_tftp_data_packet(ftx->ftx_blknum, fbuf->fb_curpos, fbuf->fb_nbytes_to_send) == DOSTRUE) {
                        LOG("DEBUG: sent data packet #%ld to server\n", ftx->ftx_blknum);
                        ftx->ftx_state = S_DATA_SENT;
                    }
                    else {
                        LOG("ERROR: sending data packet #%ld to server failed\n", ftx->ftx_blknum);
                        ftx->ftx_state = S_ERROR;
                        ftx->ftx_error = netio_errno;
                        g_busy = 0;
                        send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                    }
                }
                else {
                    LOG("INFO: file has been completely transfered\n");
                    ftx->ftx_state = S_FINISHED;
                    g_busy = 0;
                    send_internal_packet(&outpkt, ACTION_FILE_FINISHED, ftx);
                }
                break;


            case ACTION_CONTINUE_BUFFER:
                LOG("DEBUG: received internal packet of type ACTION_CONTINUE_BUFFER\n");
                ftx  = (FileTransfer *) inpkt->dp_Arg1;
                fbuf = (FileBuffer *) ftx->ftx_buffers.lh_Head;
                fbuf->fb_curpos = ((UBYTE *) fbuf->fb_curpos) + TFTP_MAX_DATA_SIZE;
                ++ftx->ftx_blknum;
                iopkt1.dp_Type = ACTION_WRITE_RETURN;
                if (send_tftp_data_packet(ftx->ftx_blknum, fbuf->fb_curpos, fbuf->fb_nbytes_to_send) == DOSTRUE) {
                    LOG("DEBUG: sent data packet #%ld to server\n", ftx->ftx_blknum);
                    ftx->ftx_state = S_DATA_SENT;
                }
                else {
                    LOG("ERROR: sending data packet #%ld to server failed\n", ftx->ftx_blknum);
                    ftx->ftx_state = S_ERROR;
                    ftx->ftx_error = netio_errno;
                    g_busy = 0;
                    send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                }
                break;


            case ACTION_FILE_FINISHED:
            case ACTION_FILE_FAILED:
                LOG("DEBUG: received internal packet of type ACTION_FILE_FINISHED / ACTION_FILE_FAILED\n");
                ftx = (FileTransfer *) inpkt->dp_Arg1;
                /* list of buffers is empty in case of a finished file (buffers have
                    * already been freed one by one), so the loop will be skipped */
                while ((fbuf = (FileBuffer *) RemHead(&(ftx->ftx_buffers)))) {
                    FreeVec(fbuf->fb_bytes);
                    FreeVec(fbuf);
                }
                send_internal_packet(&outpkt, ACTION_SEND_NEXT_FILE, NULL);
                break;


            case ACTION_BUFFER_FINISHED:
                LOG("DEBUG: received internal packet of type ACTION_BUFFER_FINISHED\n");
                ftx  = (FileTransfer *) inpkt->dp_Arg1;
                fbuf = (FileBuffer *) RemHead(&(ftx->ftx_buffers));
                FreeVec(fbuf->fb_bytes);
                FreeVec(fbuf);
                send_internal_packet(&outpkt, ACTION_SEND_NEXT_BUFFER, ftx);
                break;


            case ACTION_WRITE_RETURN:
                LOG("DEBUG: received internal packet of type ACTION_WRITE_RETURN (IO completion message)\n");
                do_write_return(inpkt, &iopkt1, &outpkt);
                break;


            case ACTION_READ_RETURN:
                LOG("DEBUG: received internal packet of type ACTION_READ_RETURN (IO completion message)\n");
                do_read_return(inpkt, &outpkt, tftppkt);
                break;


            case ACTION_TIMER_EXPIRED:
                LOG("DEBUG: received internal packet of type ACTION_TIMER_EXPIRED\n");
                LOG("ERROR: timeout occured during IO operation\n");
                
                /* abort current operation (if it's still running) */
                netio_abort();

                ftx->ftx_state = S_ERROR;
                ftx->ftx_error = ERROR_IO_TIMEOUT;
                g_busy = 0;
                send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                break;


            default:
                LOG("ERROR: packet type is unknown\n");
                return_dos_packet(inpkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        }   /* end action switch */
    }   /* end while */


    Delay(150);
    delete_buffer(tftppkt);
ERROR_NO_MEMORY:
    netio_exit();
ERROR_NO_NETIO:
    Close(g_logfh);
ERROR_NO_LOGGING:
    DeleteMsgPort(g_logport);
ERROR_NO_PORT:
}
