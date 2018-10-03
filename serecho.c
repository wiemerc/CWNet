/*
 * serecho.c - simple test program for the serial interface that just echos 
 *             everything it receives, but using asynchronous IO requests
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


#include <devices/serial.h>
#include <devices/timer.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>


#define BUF_SIZE 256
#define IOExtTime timerequest


int main()
{
    struct MsgPort *port;
    struct Message *msg;
    struct IOExtSer *sreq;
    struct IOExtTime *treq;
    struct IOTArray termchars = {0x00000000, 0x00000000};
    char rxbuf[BUF_SIZE], txbuf[BUF_SIZE];
    ULONG signals, running = 1;

    if ((port = CreateMsgPort())) {
        if ((sreq = (struct IOExtSer *) CreateExtIO(port, sizeof(struct IOExtSer)))) {
            if ((treq = (struct IOExtTime *) CreateExtIO(port, sizeof(struct IOExtTime)))) {
                if (OpenDevice("serial.device", 0l, (struct IORequest *) sreq, 0l) == 0) {
                    if (OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) treq, 0l) == 0) {
                        /* configure device to terminate read requests on NUL bytes */
                        sreq->IOSer.io_Command = SDCMD_SETPARAMS;
                        sreq->io_SerFlags     |= SERF_EOFMODE;
                        sreq->io_TermArray     = termchars;
                        if (DoIO((struct IORequest *) sreq) == 0) {
                            /* try to read first message, time out after 5 seconds */
                            sreq->IOSer.io_Command = CMD_READ;
                            sreq->IOSer.io_Length  = BUF_SIZE;
                            sreq->IOSer.io_Data    = (APTR) rxbuf;
                            SendIO((struct IORequest *) sreq);
                            treq->tr_node.io_Command = TR_ADDREQUEST;
                            treq->tr_time.tv_secs    = 5;
                            treq->tr_time.tv_micro   = 0;
                            SendIO((struct IORequest *) treq);
                            while(running) {
                                signals = Wait(SIGBREAKF_CTRL_C | 1l << port->mp_SigBit);
                                if (signals & SIGBREAKF_CTRL_C) {
                                    printf("received Ctrl-C\n");
                                    break;
                                }
                                else {
                                    msg = GetMsg(port);
                                    /* check type of reply message */
                                    if (msg == &treq->tr_node.io_Message) {
                                        /* must be a reply message, but we better check anyway... */
                                        if (!CheckIO((struct IORequest *) treq))
                                            continue;
                                        WaitIO((struct IORequest *) treq);
                                        printf("timeout occured while waiting for message\n");
                                        break;
                                    }
                                    else if (msg == &sreq->IOSer.io_Message) {
                                        /* must be a reply message, but we better check anyway... */
                                        if (!CheckIO((struct IORequest *) sreq))
                                            continue;
                                        if (WaitIO((struct IORequest *) sreq) != 0) {
                                            printf("reading from / writing to serial device failed: error = %d\n", sreq->IOSer.io_Error);
                                            break;
                                        }
                                        switch (sreq->IOSer.io_Command) {
                                            case CMD_READ:
                                                printf("read request finished => sending answer\n");

                                                /* stop timer first */
                                                AbortIO((struct IORequest *) treq);
                                                WaitIO((struct IORequest *) treq);

                                                if (rxbuf[0] == '.') {
                                                    printf("client wants us to terminate\n");
                                                    running = 0;
                                                    break;
                                                }
                                                strncpy(txbuf, "ECHO: ", BUF_SIZE);
                                                strncat (txbuf, rxbuf, BUF_SIZE - 7);
                                                sreq->IOSer.io_Command = CMD_WRITE;
                                                sreq->IOSer.io_Length  = -1;
                                                sreq->IOSer.io_Data    = (APTR) txbuf;
                                                SendIO((struct IORequest *) sreq);
                                                break;

                                            case CMD_WRITE:
                                                printf("write request finished => reading next message\n");
                                                sreq->IOSer.io_Command = CMD_READ;
                                                sreq->IOSer.io_Length  = BUF_SIZE;
                                                sreq->IOSer.io_Data    = (APTR) rxbuf;
                                                SendIO((struct IORequest *) sreq);
                                                treq->tr_node.io_Command = TR_ADDREQUEST;
                                                treq->tr_time.tv_secs    = 5;
                                                treq->tr_time.tv_micro   = 0;
                                                SendIO((struct IORequest *) treq);
                                                break;

                                            default:
                                                printf("unknown command type in reply message: %d\n", sreq->IOSer.io_Command);
                                        }
                                    }
                                }
                            } /* end while */
                            printf("terminating...\n");
                            AbortIO((struct IORequest *) sreq);
                            WaitIO((struct IORequest *) sreq);
                        }
                        else {
                            printf("configuring serial device failed: error = %d\n", sreq->IOSer.io_Error);
                        }
                        CloseDevice((struct IORequest *) treq);
                    }
                    else {
                        printf("could not open timer device\n");
                    }
                    CloseDevice((struct IORequest *) sreq);
                }
                else {
                    printf("could not open serial device\n");
                }
                DeleteExtIO((struct IORequest *) treq);
            }
            else {
                printf("could not request for timer device\n");
            }
            DeleteExtIO((struct IORequest *) sreq);
        }
        else {
            printf("could not create request for serial device\n");
        }
        DeleteMsgPort(port);
    }
    else {
        printf("could not open message port\n");
    }

    return 0;
}
