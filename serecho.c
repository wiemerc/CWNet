/*
 * serecho.c - simple test program for the serial interface that just echos 
 *             everything it receives, but using asynchronous IO requests
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


#include <devices/serial.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>


#define BUF_SIZE 256


int main()
{
    struct MsgPort *port;
    struct IOExtSer *req;
    struct IOTArray termchars = {0x00000000, 0x00000000};
    char rxbuf[BUF_SIZE], txbuf[BUF_SIZE];
    ULONG signals, running = 1;

    if ((port = CreateMsgPort())) {
        if ((req = (struct IOExtSer *) CreateExtIO(port, sizeof(struct IOExtSer)))) {
            if (OpenDevice("serial.device", 0l, (struct IORequest *) req, 0l) == 0) {
                /* configure device to terminate read requests on NUL bytes */
                req->IOSer.io_Command = SDCMD_SETPARAMS;
                req->io_SerFlags     |= SERF_EOFMODE;
                req->io_TermArray     = termchars;
                if (DoIO((struct IORequest *) req) == 0) {
                    /* read first message */
                    req->IOSer.io_Command = CMD_READ;
                    req->IOSer.io_Length  = BUF_SIZE;
                    req->IOSer.io_Data    = (APTR) rxbuf;
                    SendIO((struct IORequest *) req);
                    while(running) {
                        signals = Wait(SIGBREAKF_CTRL_C | 1l << port->mp_SigBit);
                        if (signals & SIGBREAKF_CTRL_C) {
                            printf("received Ctrl-C\n");
                            break;
                        }
                        else {
                            /* must be a reply message, but we better check anyway... */
                            if (!CheckIO((struct IORequest *) req))
                                continue;
                            if (WaitIO((struct IORequest *) req) != 0) {
                                printf("reading from / writing to serial device failed: error = %d\n", req->IOSer.io_Error);
                                break;
                            }
                            switch (req->IOSer.io_Command) {
                                case CMD_READ:
                                    printf("read request finished => sending answer\n");
                                    if (rxbuf[0] == '.') {
                                        printf("client wants us to terminate\n");
                                        running = 0;
                                        break;
                                    }
                                    strncpy(txbuf, "ECHO: ", BUF_SIZE);
                                    strncat (txbuf, rxbuf, BUF_SIZE - 7);
                                    req->IOSer.io_Command = CMD_WRITE;
                                    req->IOSer.io_Length  = -1;
                                    req->IOSer.io_Data    = (APTR) txbuf;
                                    SendIO((struct IORequest *) req);
                                    break;

                                case CMD_WRITE:
                                    printf("write request finished => reading next message\n");
                                    req->IOSer.io_Command = CMD_READ;
                                    req->IOSer.io_Length  = BUF_SIZE;
                                    req->IOSer.io_Data    = (APTR) rxbuf;
                                    SendIO((struct IORequest *) req);
                                    break;

                                default:
                                    printf("unknown command type in reply message: %d\n", req->IOSer.io_Command);
                            }
                        }
                    }
                    printf("terminating...\n");
                    AbortIO((struct IORequest *) req);
                    WaitIO((struct IORequest *) req);
                }
                else {
                    printf("configuring serial device failed: error = %d\n", req->IOSer.io_Error);
                }
                CloseDevice((struct IORequest *) req);
            }
            else {
                printf("could not open serial device\n");
            }
            DeleteExtIO((struct IORequest *) req);
        }
        else {
            printf("could not create IOExtSer request\n");
        }
        DeleteMsgPort(port);
    }
    else {
        printf("could not open message port\n");
    }

    return 0;
}
