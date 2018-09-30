/*
 * listq.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *           over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>


static char *state_tbl[] = 
{
    "S_QUEUED",
    "S_READY",
    "S_WRQ_SENT",
    "S_RRQ_SENT",
    "S_DATA_SENT",
    "S_ERROR",
    "S_FINISHED",
};


int main(int argc, char **argv)
{
    BPTR                     lock;
    struct FileInfoBlock    *fib;

    if ((lock = Lock("net:", ACCESS_READ)) == 0) {
        printf("could not obtain lock for root directory\n");
        goto ENOLOCK;
    }
    if ((fib = AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR)) == NULL) {
        printf("could not allocate memory for FileInfoBlock\n");
        goto ENOMEM;
    }
    if (!Examine(lock, fib)) {
        if (IoErr() != ERROR_NO_MORE_ENTRIES)
            printf("error returned by Examine(): %ld\n", IoErr());
        goto ENOEXAM;
    }
    printf("FILE                             STATE        ERROR\n");
    while (ExNext(lock, fib)) {
        printf("%-30s   %-10s   %ld\n", fib->fib_FileName, state_tbl[fib->fib_Protection], fib->fib_Size);
    }
    if (IoErr() != ERROR_NO_MORE_ENTRIES)
        printf("error returned by ExNext(): %ld\n", IoErr());

ENOEXAM:
    FreeVec(fib);
ENOMEM:
    UnLock(lock);
ENOLOCK:
    return RETURN_OK;
}