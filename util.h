#ifndef CWNET_UTIL_H
#define CWNET_UTIL_H
/*
 * util.h - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *          over a serial link (using SLIP)
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>


/*
 * generic buffer for packets and payloads
 */
typedef struct {
    UBYTE *b_addr;
    ULONG  b_size;
} Buffer;


/*
 * function prototypes
 */
void log(const char *msg);
char *BCPL_TO_C_STR(char *buffer, BSTR str);
Buffer *create_buffer(ULONG size);
void delete_buffer(const Buffer *buffer);
void dump_buffer(const Buffer *buffer);


/*
 * external references
 */
extern struct MsgPort *logport;
extern BPTR logfh;
extern char logmsg[256];


/*
 * constants / macros
 */
#define MAX_BUFFER_SIZE 1024
#define LOG(fmt, ...) {sprintf(logmsg, fmt, ##__VA_ARGS__); log(logmsg);}
#define C_TO_BCPL_PTR(ptr) ((BPTR) (((ULONG) (ptr)) >> 2))
#define BCPL_TO_C_PTR(ptr) ((APTR) (((ULONG) (ptr)) << 2))

#endif /* CWNET_UTIL_H */
