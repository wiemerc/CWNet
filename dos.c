/*
 * dos.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *         over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


#include "dos.h"


/*
 * send_internal_packet - send a DOS packet to ourselves
 */
void send_internal_packet(struct StandardPacket *pkt, LONG type, APTR arg)
{
    pkt->sp_Pkt.dp_Type = type;
    pkt->sp_Pkt.dp_Arg1 = (LONG) arg;
    PutMsg(pkt->sp_Pkt.dp_Port, pkt->sp_Pkt.dp_Link);
}


/*
 * return_dos_packet - return a DOS packet to its sender
 */
void return_dos_packet(struct DosPacket *pkt, LONG res1, LONG res2)
{
    struct MsgPort *port = pkt->dp_Port;
    struct Message *msg  = pkt->dp_Link;
    msg->mn_Node.ln_Name = (char *) pkt;
    pkt->dp_Port = g_port;
    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    PutMsg(port, msg);
}


/*
 * get_next_file_from_queue - get next file from queue that is ready for transfer (or NULL)
 */
FileTransfer *get_next_file_from_queue()
{
    FileTransfer *ftx = (FileTransfer *) g_transfers.lh_Head;

    if (IsListEmpty(&g_transfers))
        return NULL;
    for (ftx = (FileTransfer *) g_transfers.lh_Head;
         ftx != (FileTransfer *) &g_transfers.lh_Tail;
         ftx = (FileTransfer *) ftx->ftx_node.ln_Succ) {
        if (ftx->ftx_state == S_READY)
            return ftx;
    }
    return NULL;
}


/*
 * find_lock_in_list - find a lock in the list
 */
LinkedLock *find_lock_in_list(const struct FileLock *flock)
{
    LinkedLock *llock;

    if (IsListEmpty(&g_locks))
        return NULL;
    for (llock = (LinkedLock *) g_locks.lh_Head;
         llock != (LinkedLock *) &g_locks.lh_Tail;
         llock = (LinkedLock *) llock->ll_node.ln_Succ) {
        if (&llock->ll_flock == flock)
            return llock;
    }
    return NULL;
}


/*
 * do_find_output - handle ACTION_FINDOUTPUT packets
 */
void do_find_output(struct DosPacket *inpkt)
{
    struct FileHandle       *fh;
    FileTransfer            *ftx;
    char                     fname[MAX_PATH_LEN], *nameptr;

    fh = (struct FileHandle *) BCPL_TO_C_PTR(inpkt->dp_Arg1);
    BCPL_TO_C_STR(fname, inpkt->dp_Arg3);
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
        ftx->ftx_blknum = 0;                        /* will be set to 1 upon sending the first buffer */
        ftx->ftx_error  = 0;
        ftx->ftx_node.ln_Name = ftx->ftx_fname;     /* so that we can use FindName() */
        strncpy(ftx->ftx_fname, nameptr, MAX_PATH_LEN - 1);
        ftx->ftx_fname[MAX_PATH_LEN - 1] = 0;
        NewList(&ftx->ftx_buffers);
        AddTail(&g_transfers, (struct Node *) ftx);
        fh->fh_Arg1 = (LONG) ftx;
        fh->fh_Port = (struct MsgPort *) DOSFALSE;  /* tells DOS we're not interactive */
        
        LOG("INFO: added file '%s' to queue\n", ftx->ftx_fname);
        return_dos_packet(inpkt, DOSTRUE, 0);
    }
    else {
        LOG("ERROR: could not allocate memory for FileTransfer structure\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
    }

}


/*
 * do_write - handle ACTION_WRITE packets
 */
void do_write(struct DosPacket *inpkt, struct StandardPacket *outpkt)
{
    FileTransfer            *ftx;
    FileBuffer              *fbuf;

    ftx = (FileTransfer *) inpkt->dp_Arg1;
    /* initialize FileBuffer structure and queue it (one buffer for each ACTION_WRITE packet */
    if ((fbuf = (FileBuffer *) AllocVec(sizeof(FileBuffer), 0)) != NULL) {
        /* We need to copy the buffer because we return the packet before the 
        * buffer is sent and the client is free to reuse / free the buffer once the
        * packet has been returned. */
        if ((fbuf->fb_bytes = AllocVec(inpkt->dp_Arg3, 0)) != NULL) {
            memcpy(fbuf->fb_bytes, (APTR) inpkt->dp_Arg2, inpkt->dp_Arg3);
            fbuf->fb_curpos         = fbuf->fb_bytes;
            fbuf->fb_nbytes_to_send = inpkt->dp_Arg3;
            AddTail(&(ftx->ftx_buffers), (struct Node *) fbuf);
            
            LOG("INFO: added buffer of file '%s' to queue\n", ftx->ftx_fname);
            return_dos_packet(inpkt, inpkt->dp_Arg3, 0);
        }
        else {
            LOG("ERROR: could not allocate memory for data buffer\n");
            return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
            ftx->ftx_state = S_ERROR;
            ftx->ftx_error = ERROR_NO_FREE_STORE;
            send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
        }
    }
    else {
        LOG("ERROR: could not allocate memory for FileBuffer structure\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
        ftx->ftx_state = S_ERROR;
        ftx->ftx_error = ERROR_NO_FREE_STORE;
        send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
    }
}


/*
 * do_locate_object - handle ACTION_LOCATE_OBJECT packets
 */
void do_locate_object(struct DosPacket *inpkt)
{
    FileTransfer            *ftx;
    LinkedLock              *llock;
    struct FileLock         *flock;
    char                     fname[MAX_PATH_LEN], *nameptr;

    BCPL_TO_C_STR(fname, inpkt->dp_Arg2);
    LOG("DEBUG: lock = 0x%08lx, name = %s, mode = %ld\n", inpkt->dp_Arg1, fname, inpkt->dp_Arg3);

    /* initialize FileLock structure */
    if ((llock = (LinkedLock *) AllocVec(sizeof(LinkedLock), 0)) != NULL) {
        flock = &(llock->ll_flock);
        flock->fl_Link = 0;
        flock->fl_Access = inpkt->dp_Arg3;
        flock->fl_Task   = g_port;
        /* not completely correct since NET: is a device and not a volume (the structure 
        * pointed to by dnode contains a dol_handler and not a dol_volume entry) */
        flock->fl_Volume = C_TO_BCPL_PTR(g_dnode);
    }
    else {
        LOG("ERROR: could not allocate memory for LinkedLock structure\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
        return;
    }

    /* handle the different combinations of the lock and name arguments */
    if (inpkt->dp_Arg1 == 0) {
        /* name is absolute */
        if (strstr(fname, "//")) {
            /* name is a full URL => lock is being requested for a file transfer 
                * => return error because file does not yet exist */
            LOG("DEBUG: lock for file transfer requested\n");
            return_dos_packet(inpkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
        }
        else if ((strncmp(fname, "net:", 256) == 0)) {
            /* lock is being requested for a queue listing */
            LOG("DEBUG: lock for queue listing requested\n");
            flock->fl_Key = 0;
            return_dos_packet(inpkt, C_TO_BCPL_PTR(flock), 0);
            AddTail(&g_locks, (struct Node *) llock);
        }
        else {
            /* lock is being requested for a single file in the queue */
            /* => search for name *without* the device name in list */
            if (strrchr(fname, ':'))
                nameptr = strrchr(fname, ':') + 1;
            else
                nameptr = fname;
            if ((ftx = (FileTransfer *) FindName(&g_transfers, nameptr)) != NULL) {
                LOG("DEBUG: lock for file '%s' requested\n", fname);
                flock->fl_Key = (LONG) ftx;
                return_dos_packet(inpkt, C_TO_BCPL_PTR(flock), 0);
                AddTail(&g_locks, (struct Node *) llock);
            }
            else {
                LOG("ERROR: lock for file '%s' requested but file not found in queue\n", fname);
                return_dos_packet(inpkt, DOSFALSE,  ERROR_OBJECT_NOT_FOUND);
            }
        }
    }
    else {
        /* name is relative to an existing lock => not supported because we don't support directories */
        LOG("ERROR: new lock relative to an existing lock requested\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_NOT_IMPLEMENTED);
    }
}


/*
 * do_examine_object - handle ACTION_EXAMINE_OBJECT packets
 */
void do_examine_object(struct DosPacket *inpkt)
{
    FileTransfer            *ftx;
    struct FileLock         *flock;
    struct FileInfoBlock    *fib;

    flock = BCPL_TO_C_PTR(inpkt->dp_Arg1);
    LOG("DEBUG: lock = 0x%08lx\n", (ULONG) flock);
    if (!find_lock_in_list(flock)) {
        LOG("ERROR: unknown lock\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
        return;
    }

    fib = (struct FileInfoBlock *) BCPL_TO_C_PTR(inpkt->dp_Arg2);
    if (flock->fl_Key == 0) {
        /* lock refers to root directory => fill FileInfoBlock structure with 
            * the values for the root directory, *not* for the first entry in the 
            * list of g_transfers (this happens in the first ACTION_EXAMINE_NEXT packet) */
        if (!IsListEmpty(&g_transfers)) {
            LOG("DEBUG: entries to examine\n");
            fib->fib_DiskKey      = (LONG) g_transfers.lh_Head;
            fib->fib_DirEntryType = ST_ROOT;
            fib->fib_EntryType    = ST_ROOT;
            fib->fib_Protection   = FIBF_READ | FIBF_WRITE | FIBF_EXECUTE;
            fib->fib_Size         = 0;
            fib->fib_FileName[0]  = 0;    /* BCPL string => first byte contains length */
            fib->fib_Comment[0]   = 0;    /* BCPL string => first byte contains length */
            return_dos_packet(inpkt, DOSTRUE, 0);
        }
        else {
            LOG("DEBUG: no entries to examine\n");
            return_dos_packet(inpkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
        }
    }
    else {
        /* lock refers to a single file => fill FileInfoBlock structure with 
            * the values from the entry in the list of g_transfers for this file, protection
            * bits contain the state and the size contains the error code */
        ftx = (FileTransfer *) flock->fl_Key;
        fib->fib_DiskKey      = (LONG) ftx;
        fib->fib_Protection   = ftx->ftx_state;
        fib->fib_Size         = ftx->ftx_error;
        /* fib_FileName is a BCPL string => first byte contains length, but for some
            * reason it has to be null-terminated as well, => maximum length is MAX_FILENAME_LEN - 2,
            * and we can copy MAX_FILENAME_LEN - 2 characters at most */
        /* TODO: For some reason, the last character of the file name gets lost on its way
         *       to the application calling Examine() or ExNext() */
        fib->fib_FileName[0]  = strlen(ftx->ftx_fname) % MAX_FILENAME_LEN - 1;
        strncpy(fib->fib_FileName + 1, ftx->ftx_fname, MAX_FILENAME_LEN - 2);
        fib->fib_FileName[MAX_FILENAME_LEN - 1] = 0;
        /* TODO: initialize fib_Date */
        return_dos_packet(inpkt, DOSTRUE, 0);
    }
}


/*
 * do_examine_next - handle ACTION_EXAMINE_NEXT packets
 */
void do_examine_next(struct DosPacket *inpkt)
{
    FileTransfer            *ftx;
    struct FileLock         *flock;
    struct FileInfoBlock    *fib;

    flock = BCPL_TO_C_PTR(inpkt->dp_Arg1);
    LOG("DEBUG: lock = 0x%08lx\n", (ULONG) flock);
    if (!find_lock_in_list(flock)) {
        LOG("ERROR: unknown lock\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
        return;
    }
    if (flock->fl_Key != 0) {
        LOG("ERROR: lock does not refer to root directory\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
        return;
    }

    /* fill FileInfoBlock structure with the values from the next entry 
        * in the list of g_transfers and return it, protection bits contain the 
        * state and the size contains the error code. We assume here that the
        * FileInfoBlock passed is the same as in ACTION_EXAMINE_OBJECT. */
    fib = (struct FileInfoBlock *) BCPL_TO_C_PTR(inpkt->dp_Arg2);
    ftx = (FileTransfer *) fib->fib_DiskKey;
    /* The end of the list is not reached when the pointer to the next node
        * (ln_Succ) is NULL (which is stated in the ROM Reference Manual and 
        * would seem logical) but when it points to the lh_Tail field of the
        * list structure. I don't understand why, but this is also what
        * Matt Dillon checks for in his code, so it can't be that wrong... */
    if (ftx != (FileTransfer *) &g_transfers.lh_Tail) {
        LOG("DEBUG: still entries to examine\n");
        fib->fib_DiskKey      = (LONG) ftx->ftx_node.ln_Succ;
        fib->fib_Protection   = ftx->ftx_state;
        fib->fib_Size         = ftx->ftx_error;
        fib->fib_FileName[0]  = strlen(ftx->ftx_fname) % MAX_FILENAME_LEN - 1;
        strncpy(fib->fib_FileName + 1, ftx->ftx_fname, MAX_FILENAME_LEN - 2);
        fib->fib_FileName[MAX_FILENAME_LEN - 1] = 0;
        /* TODO: initialize fib_Date */
        return_dos_packet(inpkt, DOSTRUE, 0);
    }
    else {
        LOG("DEBUG: no more entries to examine\n");
        return_dos_packet(inpkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
    }
}


/*
 * do_write_return - handle (internal) ACTION_WRITE_RETURN packets
 */
void do_write_return(struct DosPacket *inpkt, struct DosPacket *iopkt, struct StandardPacket *outpkt)
{
    FileTransfer            *ftx;
    BYTE                     status;

    ftx = (FileTransfer *) inpkt->dp_Arg1;
        
    /* get status of write command */
    /* There is a race condition here: As the timer is still running when we
     * receive the IO completion message, it could expire before we can stop it */
    netio_stop_timer();
    status = netio_get_status();
    if (status == -1) {
        LOG("CRITICAL: IO operation has not been completed although IO completion message was received\n");
        g_busy = 0;
        g_running = 0;
    }
    else if (status > 0) {
        LOG("ERROR: sending write request / data to server failed with error %ld\n", status);
        ftx->ftx_state = S_ERROR;
        ftx->ftx_error = status;
        g_busy = 0;
        send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
    }

    /* read answer from server */
    /* TODO: answer seems to get lost sometimes */
    LOG("DEBUG: reading answer from server\n");
    iopkt->dp_Type = ACTION_READ_RETURN;
    if (recv_tftp_packet() == DOSFALSE) {
        LOG("ERROR: reading answer from server failed with error %ld\n", g_netio_errno);
        ftx->ftx_state = S_ERROR;
        ftx->ftx_error = g_netio_errno;
        g_busy = 0;
        send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
    }
}


/*
 * do_read_return - handle (internal) ACTION_READ_RETURN packets
 */
void do_read_return(struct DosPacket *inpkt, struct StandardPacket *outpkt, Buffer *tftppkt)
{
    FileTransfer            *ftx;
    FileBuffer              *fbuf;
    BYTE                     status;

    ftx = (FileTransfer *) inpkt->dp_Arg1;
    fbuf = (FileBuffer *) ftx->ftx_buffers.lh_Head;

    /* get status of read command */
    /* There is a race condition here: As the timer is still running when we
     * receive the IO completion message, it could expire before we can stop it */
    netio_stop_timer();
    status = netio_get_status();
    if (status == -1) {
        LOG("CRITICAL: IO operation has not been completed although IO completion message was received\n");
        g_busy = 0;
        g_running = 0;
    }
    else if (status > 0) {
        LOG("ERROR: reading answer from server failed with error %ld\n", status);
        ftx->ftx_state = S_ERROR;
        ftx->ftx_error = status;
        g_busy = 0;
        send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
    }

    /* extract TFTP packet from received data */
    if (extract_tftp_packet(tftppkt) == DOSFALSE) {
        LOG("ERROR: reading answer from server failed with error %ld\n", g_netio_errno);
        ftx->ftx_state = S_ERROR;
        ftx->ftx_error = g_netio_errno;
        g_busy = 0;
        send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
    }

#if DEBUG
    LOG("DEBUG: dump of received packet (%ld bytes):\n", tftppkt->b_size);
    dump_buffer(tftppkt);
#endif
    switch (get_opcode(tftppkt)) {
        case OP_ACK:
            if (ftx->ftx_state == S_WRQ_SENT) {
                LOG("DEBUG: ACK received for sent write request\n");
                send_internal_packet(outpkt, ACTION_SEND_NEXT_BUFFER, ftx);
            }
            else if (ftx->ftx_state == S_DATA_SENT) {
                if (get_blknum(tftppkt) == ftx->ftx_blknum) {
                    LOG("DEBUG: ACK received for sent data packet\n");
                    fbuf->fb_nbytes_to_send -= TFTP_MAX_DATA_SIZE;
                    if (fbuf->fb_nbytes_to_send > 0) {
                        send_internal_packet(outpkt, ACTION_CONTINUE_BUFFER, ftx);
                        LOG("DEBUG: sending next data packet to server\n");
                    }
                    else {
                        LOG("DEBUG: buffer has been completely transfered\n");
                        send_internal_packet(outpkt, ACTION_BUFFER_FINISHED, ftx);
                    }
                }
                else {
                    LOG("ERROR: ACK with unexpected block number %ld received - terminating\n", (ULONG) get_blknum(tftppkt));
                    ftx->ftx_state = S_ERROR;
                    ftx->ftx_error = ERROR_TFTP_WRONG_BLOCK_NUM;
                    g_busy = 0;
                    send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
                }
            }
            else {
                LOG("CRITICAL: file transfer is in wrong state %ld\n", ftx->ftx_state);
                g_busy = 0;
                g_running = 0;
            }
            break;

        case OP_ERROR:
            LOG("ERROR: OP_ERROR received from server\n");
            ftx->ftx_state = S_ERROR;
            /* TODO: map TFTP error codes to AmigaDOS or custom error codes */
            ftx->ftx_error = ERROR_TFTP_GENERIC_ERROR;
            g_busy = 0;
            send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
            break;

        default:
            LOG("ERROR: unknown opcode received from server\n");
            ftx->ftx_state = S_ERROR;
            ftx->ftx_error = ERROR_TFTP_UNKNOWN_OPCODE;
            g_busy = 0;
            send_internal_packet(outpkt, ACTION_FILE_FAILED, ftx);
    } /* end opcode switch */
}
