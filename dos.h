#ifndef CWNET_DOS_H
#define CWNET_DOS_H
/*
 * dos.h - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *         over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/lists.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "util.h"
#include "netio.h"


/*
 * constants
 */
#define MAX_PATH_LEN 256        /* for all file names */
#define MAX_FILENAME_LEN 108    /* for file names in the FileInfoBlock structure */


/*
 * custom DOS error codes
 */
#define ERROR_TFTP_GENERIC_ERROR    1000
#define ERROR_TFTP_UNKNOWN_OPCODE   1001
#define ERROR_TFTP_WRONG_BLOCK_NUM  1002
#define ERROR_IO_NOT_FINISHED       1003
#define ERROR_IO_TIMEOUT            1004


/*
 * internal actions
 */
#define ACTION_SEND_NEXT_FILE       5000
#define ACTION_SEND_NEXT_BUFFER     5001
#define ACTION_CONTINUE_BUFFER      5002
#define ACTION_FILE_FINISHED        5003
#define ACTION_FILE_FAILED          5004
#define ACTION_BUFFER_FINISHED      5005
#define ACTION_TIMER_EXPIRED        5006


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
    char        ftx_fname[MAX_PATH_LEN];
    ULONG       ftx_state;
    ULONG       ftx_blknum;
    ULONG       ftx_error;
    struct List ftx_buffers;
} FileTransfer;


/*
 * file lock that can be put into a list
 * The FileLock structure already contains a field fl_Link that we could use for chaining
 * locks together, but the advantage of the structure below is that we can use the
 * standard list management functions like AddTail(), RemHead(), FindName() and so on.
 */
typedef struct
{
    struct Node     ll_node;
    UWORD           ll_dummy;   /* to keep the FileLock structure longword-aligned */
    struct FileLock ll_flock;
} LinkedLock;


/*
 * function prototypes
 */
void send_internal_packet(struct StandardPacket *pkt, LONG type, APTR arg);
void return_dos_packet(struct DosPacket *pkt, LONG res1, LONG res2);
FileTransfer *get_next_file_from_queue();
LinkedLock *find_lock_in_list(const struct FileLock *flock);
void do_find_output(struct DosPacket *inpkt);
void do_write(struct DosPacket *inpkt, struct StandardPacket *outpkt);
void do_locate_object(struct DosPacket *inpkt);
void do_examine_object(struct DosPacket *inpkt);
void do_examine_next(struct DosPacket *inpkt);
void do_write_return(struct DosPacket *inpkt, struct DosPacket *iopkt, struct StandardPacket *outpkt);
void do_read_return(struct DosPacket *inpkt, struct StandardPacket *outpkt, Buffer *tftppkt);


/*
 * external references
 */
extern struct MsgPort      *g_port;
extern struct DeviceNode   *g_dnode;
extern struct List          g_transfers;
extern struct List          g_locks;
extern UBYTE                g_running, g_busy;

#endif /* CWNET_DOS_H */
