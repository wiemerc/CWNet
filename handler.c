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
 * file lock that can be put into a list
 * The FileLock structure already contains a field fl_Link that we could use for chaining
 * locks together, but the advantage of the structure below is that we can use the
 * standard list management functions like AddTail(), RemTail(), Remove() and so on.
 */
typedef struct
{
    struct Node     ll_node;
    UWORD           ll_dummy;   /* to keep the FileLock structure longword-aligned */
    struct FileLock ll_flock;
} LinkedLock;


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


/* TODO: move routines to util.c */
/*
 * send_internal_packet - send a DOS packet to ourselves
 */
static void send_internal_packet(struct StandardPacket *pkt, LONG type, APTR arg)
{
    pkt->sp_Pkt.dp_Type = type;
    pkt->sp_Pkt.dp_Arg1 = (LONG) arg;
    PutMsg(pkt->sp_Pkt.dp_Port, pkt->sp_Pkt.dp_Link);
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
 * get_next_file_from_queue - get next file from queue that is ready for transfer (or NULL)
 */
static FileTransfer *get_next_file_from_queue(const struct List *transfers)
{
    FileTransfer *ftx = (FileTransfer *) transfers->lh_Head;

    if (IsListEmpty(transfers))
        return NULL;
    while (ftx && (ftx->ftx_state != S_READY))
        ftx = (FileTransfer *) ftx->ftx_node.ln_Succ;
    return ftx;
}


/*
 * find_file_in_queue - find file in queue with the given name
 */
static FileTransfer *find_file_in_queue(const struct List *transfers, const char *fname)
{
    FileTransfer *ftx = (FileTransfer *) transfers->lh_Head;

    if (IsListEmpty(transfers))
        return NULL;
    while (ftx && (strncmp(ftx->ftx_fname, fname, 256) != 0))
        ftx = (FileTransfer *) ftx->ftx_node.ln_Succ;
    return ftx;
}


/*
 * find_lock_in_list - find a lock in the list
 */
static LinkedLock *find_lock_in_list(const struct List *locks, const struct FileLock *flock)
{
    LinkedLock *llock = (LinkedLock *) locks->lh_Head;

    if (IsListEmpty(locks))
        return NULL;
    while (llock && (&(llock->ll_flock) != flock))
        llock = (LinkedLock *) llock->ll_node.ln_Succ;
    return llock;
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
    struct Message          *msg;
    struct DosPacket        *inpkt, iopkt;
    struct StandardPacket    outpkt;
    struct IOExtSer         *req;
    LinkedLock              *llock;
    struct FileLock         *flock;
    struct FileHandle       *fh;
    struct FileInfoBlock    *fib;
    FileTransfer            *ftx, dummy;
    FileBuffer              *fbuf;
    struct List              transfers;
    struct List              locks;
    Buffer                  *tftppkt;
    ULONG                    running = 1, busy = 0;
    char                     fname[256], *nameptr;


    /* wait for startup packet */
    WaitPort(port);
    inpkt = (struct DosPacket *) GetMsg(port)->mn_Node.ln_Name;

    /* tell DOS not to start a new handler for every request */
    dnode = (struct DeviceNode *) BCPL_TO_C_PTR(inpkt->dp_Arg3);
    dnode->dn_Task = port;

    /* return packet */
    return_dos_packet(inpkt, DOSTRUE, inpkt->dp_Res2);


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
//    if ((logfh = Open("WORK:cwnet.log", MODE_NEWFILE)) == 0)
        goto ENOLOG;

    /* initialize serial device and add DOS packet to IO request so that IO completion
     * messages can be handled as internal packets */
    if ((req = (struct IOExtSer *) CreateExtIO(port, sizeof(struct IOExtSer))) == NULL) {
        LOG("CRITICAL: could not create request for serial device\n");
        goto ENOREQ;
    }
    if (OpenDevice("serial.device", 0l, (struct IORequest *) req, 0l) != 0) {
        LOG("CRITICAL: could not open serial device\n");
        goto ENODEV;
    }
    iopkt.dp_Type = ACTION_IO_COMPLETED;
    req->IOSer.io_Message.mn_Node.ln_Name = (char *) &iopkt;

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

    /* initialize lists of file transfers and locks */
    NewList(&transfers);
    NewList(&locks);
    LOG("INFO: initialization complete - waiting for requests\n");
    
    /* initialize internal DOS packet */
    outpkt.sp_Msg.mn_ReplyPort    = port;
    outpkt.sp_Pkt.dp_Port         = port;
    outpkt.sp_Msg.mn_Node.ln_Name = (char *) &(outpkt.sp_Pkt);
    outpkt.sp_Pkt.dp_Link         = &(outpkt.sp_Msg);

    /*
    dummy.ftx_state = S_QUEUED;
    dummy.ftx_error = 0;
    dummy.ftx_node.ln_Succ = NULL;
    strncpy(dummy.ftx_fname, "xxx", 256);
    AddTail(&transfers, (struct Node *) &dummy);
    */

    /* TODO: reverse conditions to get rid of nested ifs */
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
    while(running) {
        WaitPort(port);
        msg   = GetMsg(port);
        inpkt = (struct DosPacket *) msg->mn_Node.ln_Name;
        LOG("DEBUG: received DOS packet of type %ld\n", inpkt->dp_Type);

        switch (inpkt->dp_Type) {
            /*
             * regular actions
             */
            case ACTION_IS_FILESYSTEM:
                LOG("INFO: packet type = ACTION_IS_FILESYSTEM\n");
                return_dos_packet(inpkt, DOSTRUE, 0);
                break;


            case ACTION_FINDOUTPUT:
                LOG("INFO: packet type = ACTION_FINDOUTPUT\n");
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
                if ((ftx = (FileTransfer *) AllocVec(sizeof(FileTransfer), MEMF_CLEAR)) != NULL) {
                    ftx->ftx_state  = S_QUEUED;
                    ftx->ftx_blknum = 0;    /* will be set to 1 upon sending the first buffer */
                    ftx->ftx_error  = 0;
                    strncpy(ftx->ftx_fname, nameptr, 256);
                    NewList(&(ftx->ftx_buffers));
                    AddTail(&transfers, (struct Node *) ftx);
                    fh->fh_Arg1 = (LONG) ftx;
                    fh->fh_Port = (struct MsgPort *) DOSTRUE;   /* TODO: really necessary? */
                    
                    LOG("INFO: added file '%s' to queue\n", ftx->ftx_fname);
                    return_dos_packet(inpkt, DOSTRUE, 0);
                }
                else {
                    LOG("ERROR: could not allocate memory for FileTransfer structure\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
                }
                break;


            case ACTION_WRITE:
                LOG("INFO: packet type = ACTION_WRITE\n");
                ftx = (FileTransfer *) inpkt->dp_Arg1;
                /* initialize FileBuffer structure and queue it (one buffer for each ACTION_WRITE packet */
                if ((fbuf = (FileBuffer *) AllocVec(sizeof(FileBuffer), MEMF_CLEAR)) != NULL) {
                    /* We need to copy the buffer because we return the packet before the 
                    * buffer is sent and the client is free to reuse / free the buffer once the
                    * packet has been returned. */
                    if ((fbuf->fb_bytes = AllocVec(inpkt->dp_Arg3, MEMF_CLEAR)) != NULL) {
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
                        send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                    }
                }
                else {
                    LOG("ERROR: could not allocate memory for FileBuffer structure\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
                    ftx->ftx_state = S_ERROR;
                    ftx->ftx_error = ERROR_NO_FREE_STORE;
                    send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                }
                break;


            case ACTION_END:
                LOG("INFO: packet type = ACTION_END\n");
                ftx = (FileTransfer *) inpkt->dp_Arg1;
                LOG("INFO: file '%s' is now ready for transfer\n", ftx->ftx_fname);
                return_dos_packet(inpkt, DOSTRUE, 0);
                
                /* We only inform ourselves now that a file has been added and is ready 
                    * for transfer in order to prevent a race condition between buffers being
                    * added and being sent. Otherwise it could happen that we transfer bufffers
                    * faster than we receive them and would therefore assume the file has been
                    * transfered completely somewhere in the middle of the file. */
                ftx->ftx_state = S_READY;
                send_internal_packet(&outpkt, ACTION_SEND_NEXT_FILE, NULL);
                break;


            case ACTION_LOCATE_OBJECT:
                LOG("INFO: packet type = ACTION_LOCATE_OBJECT\n");
                BCPL_TO_C_STR(fname, inpkt->dp_Arg2);
                LOG("DEBUG: lock = 0x%08lx, name = %s, mode = %ld\n", inpkt->dp_Arg1, fname, inpkt->dp_Arg3);

                /* initialize FileLock structure */
                if ((llock = (LinkedLock *) AllocVec(sizeof(LinkedLock), MEMF_CLEAR)) != NULL) {
                    flock = &(llock->ll_flock);
                    flock->fl_Link = 0;
                    flock->fl_Access = inpkt->dp_Arg3;
                    flock->fl_Task   = port;
                    /* not completely correct since NET: is a device and not a volume (the structure 
                    * pointed to by dnode contains a dol_handler and not a dol_volume entry) */
                    flock->fl_Volume = C_TO_BCPL_PTR(dnode);
                }
                else {
                    LOG("ERROR: could not allocate memory for LinkedLock structure\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_NO_FREE_STORE);
                    break;
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
                        AddTail(&locks, (struct Node *) llock);
                    }
                    else {
                        /* lock is being requested for a single file in the queue */
                        if ((ftx = find_file_in_queue(&transfers, fname)) != NULL) {
                            LOG("DEBUG: lock for file '%s' requested\n", fname);
                            flock->fl_Key = (LONG) ftx;
                            return_dos_packet(inpkt, C_TO_BCPL_PTR(flock), 0);
                            AddTail(&locks, (struct Node *) llock);
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
                break;


            case ACTION_FREE_LOCK:
                LOG("INFO: packet type = ACTION_FREE_LOCK\n");
                flock = BCPL_TO_C_PTR(inpkt->dp_Arg1);
                LOG("DEBUG: lock = 0x%08lx\n", (ULONG) flock);
                if (flock == NULL)
                    return_dos_packet(inpkt, DOSTRUE, 0);
                else if ((llock = find_lock_in_list(&locks, flock))) {
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
                flock = BCPL_TO_C_PTR(inpkt->dp_Arg1);
                LOG("DEBUG: lock = 0x%08lx\n", (ULONG) flock);
                if (!find_lock_in_list(&locks, flock)) {
                    LOG("ERROR: unknown lock\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
                    break;
                }

                fib = (struct FileInfoBlock *) BCPL_TO_C_PTR(inpkt->dp_Arg2);
                if (flock->fl_Key == 0) {
                    /* lock refers to root directory => fill FileInfoBlock structure with 
                    * the values for the root directory, *not* for the first entry in the 
                    * list of transfers (this happens in the first ACTION_EXAMINE_NEXT packet) */
                    if (!IsListEmpty(&transfers)) {
                        LOG("DEBUG: entries to examine\n");
                        fib->fib_DiskKey      = (LONG) transfers.lh_Head;
                        fib->fib_DirEntryType = ST_ROOT;
                        fib->fib_EntryType    = ST_ROOT;
                        fib->fib_Protection   = FIBF_READ | FIBF_WRITE | FIBF_EXECUTE;
                        fib->fib_Size         = 0;
                        fib->fib_FileName[0]  = 0;    /* BCPL string */
                        fib->fib_Comment[0]   = 0;    /* BCPL string */
                        return_dos_packet(inpkt, DOSTRUE, 0);
                    }
                    else {
                        LOG("DEBUG: no entries to examine\n");
                        return_dos_packet(inpkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
                    }
                }
                else {
                    /* lock refers to a single file => fill FileInfoBlock structure with 
                    * the values from the entry in the list of transfers for this file, protection
                    * bits contain the state and the size contains the error code */
                    ftx = (FileTransfer *) flock->fl_Key;
                    fib->fib_DiskKey      = (LONG) ftx;
                    fib->fib_Protection   = ftx->ftx_state;
                    fib->fib_Size         = ftx->ftx_error;
                    fib->fib_FileName[0]  = strlen(ftx->ftx_fname);
                    strncpy(fib->fib_FileName + 1, ftx->ftx_fname, 30);     /* BCPL string */
                    /* TODO: initialize fib_Date */
                    return_dos_packet(inpkt, DOSTRUE, 0);
                }
                break;


            case ACTION_EXAMINE_NEXT:
                LOG("INFO: packet type = ACTION_EXAMINE_NEXT\n");
                flock = BCPL_TO_C_PTR(inpkt->dp_Arg1);
                LOG("DEBUG: lock = 0x%08lx\n", (ULONG) flock);
                if (!find_lock_in_list(&locks, flock)) {
                    LOG("ERROR: unknown lock\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
                    break;
                }
                if (flock->fl_Key != 0) {
                    LOG("ERROR: lock does not refer to root directory\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_INVALID_LOCK);
                    break;
                }

                /* fill FileInfoBlock structure with the values from the next entry 
                 * in the list of transfers and return it, protection bits contain the 
                 * state and the size contains the error code. We assume here that the
                 * FileInfoBlock passed is the same as in ACTION_EXAMINE_OBJECT. */
                fib = (struct FileInfoBlock *) BCPL_TO_C_PTR(inpkt->dp_Arg2);
                ftx = (FileTransfer *) fib->fib_DiskKey;
                LOG("DEBUG: current transfer = 0x%08lx, next transfer = 0x%08lx\n", ftx, ftx->ftx_node.ln_Succ);
                if (ftx) {
                    LOG("DEBUG: still entries to examine\n");
                    fib->fib_DiskKey      = (LONG) ftx->ftx_node.ln_Succ;
                    fib->fib_Protection   = ftx->ftx_state;
                    fib->fib_Size         = ftx->ftx_error;
                    fib->fib_FileName[0]  = strlen(ftx->ftx_fname);
                    strncpy(fib->fib_FileName + 1, ftx->ftx_fname, 30);     /* BCPL string */
                    /* TODO: initialize fib_Date */
                    return_dos_packet(inpkt, DOSTRUE, 0);
                }
                else {
                    LOG("DEBUG: no more entries to examine\n");
                    return_dos_packet(inpkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
                }
                break;


            case ACTION_DIE:
                LOG("INFO: packet type = ACTION_DIE\n");
                LOG("INFO: ACTION_DIE packet received - shutting down\n");
                /* TODO: abort ongoing IO operations */
                /* tell DOS not to send us any more packets */
                dnode->dn_Task = NULL;
                running = 0;
                return_dos_packet(inpkt, DOSTRUE, 0);
                break;


            /*
                * internal actions
                */
            case ACTION_SEND_NEXT_FILE:
                LOG("DEBUG: received internal packet of type ACTION_SEND_NEXT_FILE\n");
                if (!busy) {
                    if ((ftx = get_next_file_from_queue(&transfers))) {
                        busy = 1;
                        /* store pointer to current FileTransfer structure in DOS packet so
                            * that we can retrieve it in ACTION_IO_COMPLETED below */
                        iopkt.dp_Arg1 = (LONG) ftx;
                        if (send_tftp_req_packet(req, OP_WRQ, ftx->ftx_fname) == DOSTRUE) {
                            LOG("DEBUG: sent write request for file '%s' to server\n", ftx->ftx_fname);
                            ftx->ftx_state = S_WRQ_SENT;
                        }
                        else {
                            LOG("ERROR: sending write request for file '%s' to server failed\n", ftx->ftx_fname);
                            ftx->ftx_state = S_ERROR;
                            ftx->ftx_error = netio_errno;
                            busy = 0;
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
                    if (send_tftp_data_packet(req, ftx->ftx_blknum, fbuf->fb_curpos, fbuf->fb_nbytes_to_send) == DOSTRUE) {
                        LOG("DEBUG: sent data packet #%ld to server\n", ftx->ftx_blknum);
                        ftx->ftx_state = S_DATA_SENT;
                    }
                    else {
                        LOG("ERROR: sending data packet #%ld to server failed\n", ftx->ftx_blknum);
                        ftx->ftx_state = S_ERROR;
                        ftx->ftx_error = netio_errno;
                        busy = 0;
                        send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                    }
                }
                else {
                    LOG("INFO: file has been completely transfered\n");
                    ftx->ftx_state = S_FINISHED;
                    busy = 0;
                    send_internal_packet(&outpkt, ACTION_FILE_FINISHED, ftx);
                }
                break;


            case ACTION_CONTINUE_BUFFER:
                LOG("DEBUG: received internal packet of type ACTION_CONTINUE_BUFFER\n");
                ftx  = (FileTransfer *) inpkt->dp_Arg1;
                fbuf = (FileBuffer *) ftx->ftx_buffers.lh_Head;
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


            case ACTION_IO_COMPLETED:
                LOG("DEBUG: received internal packet of type ACTION_IO_COMPLETED (IO completion message)\n");
                /* must be a reply message for a write command, but we better check anyway... */
                if (!CheckIO((struct IORequest *) req)) {
                    LOG("CRITICAL: IO operation has not completed although IO completion message was received\n");
                    busy = 0;
                    running = 0;
                }
                    
                ftx = (FileTransfer *) inpkt->dp_Arg1;
                fbuf = (FileBuffer *) ftx->ftx_buffers.lh_Head;

                /* get status of write command */
                if (WaitIO((struct IORequest *) req) != 0) {
                    LOG("ERROR: sending write request / data to server failed with error %ld\n", req->IOSer.io_Error);
                    ftx->ftx_state = S_ERROR;
                    ftx->ftx_error = req->IOSer.io_Error;
                    busy = 0;
                    send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                }

                /* read and handle answer from server */
                /* TODO: answer from server seems to get lost sometimes */
                LOG("DEBUG: reading answer from server\n");
                if (recv_tftp_packet(req, tftppkt) == DOSFALSE) {
                    LOG("ERROR: reading answer from server failed\n");
                    ftx->ftx_state = S_ERROR;
                    ftx->ftx_error = netio_errno;
                    busy = 0;
                    send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                }
    //            LOG("DEBUG: dump of received packet (%ld bytes):\n", tftppkt->b_size);
    //            dump_buffer(tftppkt);
                switch (get_opcode(tftppkt)) {
                    case OP_ACK:
                        if (ftx->ftx_state == S_WRQ_SENT) {
                            LOG("DEBUG: ACK received for sent write request\n");
                            send_internal_packet(&outpkt, ACTION_SEND_NEXT_BUFFER, ftx);
                        }
                        else if (ftx->ftx_state == S_DATA_SENT) {
                            if (get_blknum(tftppkt) == ftx->ftx_blknum) {
                                LOG("DEBUG: ACK received for sent data packet\n");
                                fbuf->fb_nbytes_to_send -= TFTP_MAX_DATA_SIZE;
                                if (fbuf->fb_nbytes_to_send > 0) {
                                    send_internal_packet(&outpkt, ACTION_CONTINUE_BUFFER, ftx);
                                    LOG("DEBUG: sending next data packet to server\n");
                                }
                                else {
                                    LOG("DEBUG: buffer has been completely transfered\n");
                                    send_internal_packet(&outpkt, ACTION_BUFFER_FINISHED, ftx);
                                }
                            }
                            else {
                                LOG("ERROR: ACK with unexpected block number %ld received - terminating\n", (ULONG) get_blknum(tftppkt));
                                ftx->ftx_state = S_ERROR;
                                ftx->ftx_error = ERROR_TFTP_WRONG_BLOCK_NUM;
                                busy = 0;
                                send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
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
                        send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                        break;

                    default:
                        LOG("ERROR: unknown opcode received from server\n");
                        ftx->ftx_state = S_ERROR;
                        ftx->ftx_error = ERROR_TFTP_UNKNOWN_OPCODE;
                        busy = 0;
                        send_internal_packet(&outpkt, ACTION_FILE_FAILED, ftx);
                }
                break;


            default:
                LOG("ERROR: packet type is unknown\n");
                return_dos_packet(inpkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        }   /* end switch */
    }   /* end while */


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
