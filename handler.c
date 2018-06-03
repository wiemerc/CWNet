/*
 * cwnet.c - AmigaDOS handler that allows uploading files to a TFTP server
 *           over a serial link (using SLIP)
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
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>
#include <stdio.h>


#define C_TO_BCPL_PTR(ptr) (((ULONG) (ptr)) >> 2)
#define BCPL_TO_C_PTR(ptr) (((ULONG) (ptr)) << 2)
#define LOG(fmt, ...) {sprintf(logmsg, fmt, ##__VA_ARGS__); log(logmsg);}

/*
 * constants
 */


/*
 * global variables
 */
static struct MsgPort *logport;
static BPTR logfh;
static char logmsg[256];


/*
 * return_packet - return a DOS packet to its sender
 */
static void return_packet(struct DosPacket *pkt, LONG res1, LONG res2)
{
    struct MsgPort *port = pkt->dp_Port;
    struct Message *msg  = pkt->dp_Link;
    msg->mn_Node.ln_Name = (char *) pkt;
    pkt->dp_Port = &(((struct Process *) FindTask(NULL))->pr_MsgPort);
    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    PutMsg(port, msg);
}


static void log(const char *msg)
{
    struct StandardPacket *pkt;
    if ((pkt = (struct StandardPacket *) AllocVec(sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
        pkt->sp_Msg.mn_ReplyPort    = logport;
        pkt->sp_Pkt.dp_Port         = logport;
        pkt->sp_Msg.mn_Node.ln_Name = (char *) &(pkt->sp_Pkt);
        pkt->sp_Pkt.dp_Link         = &(pkt->sp_Msg);
        pkt->sp_Pkt.dp_Type = ACTION_WRITE;
        pkt->sp_Pkt.dp_Arg1 = ((struct FileHandle *) BCPL_TO_C_PTR(logfh))->fh_Arg1;
        pkt->sp_Pkt.dp_Arg2 = (LONG) msg;
        pkt->sp_Pkt.dp_Arg3 = strlen(msg);
        PutMsg((struct MsgPort *) ((struct FileHandle *) BCPL_TO_C_PTR(logfh))->fh_Type, &(pkt->sp_Msg));
        WaitPort(logport);
        GetMsg(logport);
        FreeVec(pkt);
    }
}


/*
 * main function
 */
void entry()
{
    struct Process *proc = (struct Process *) FindTask(NULL);
    struct MsgPort *port = &(proc->pr_MsgPort);
    struct DosPacket *pkt;

    /* wait for startup packet, set up everything and return packet */
    WaitPort(port);
    pkt = (struct DosPacket *) GetMsg(port)->mn_Node.ln_Name;
    ((struct DeviceNode *) BCPL_TO_C_PTR(pkt->dp_Arg3))->dn_Task = port;
    /* initialization of handler goes here... */
    return_packet(pkt, DOSTRUE, pkt->dp_Res2);

//    Alert(AT_DeadEnd | AN_Unknown | AG_BadParm | AO_Unknown);
    if ((logport = CreateMsgPort()) != NULL) {
        if ((logfh = Open("CON:0/0/800/200/CWNET Console", MODE_NEWFILE)) != 0) {
            LOG("DEBUG: received startup packet\n");

            while(1) {
                WaitPort(port);
                pkt = (struct DosPacket *) GetMsg(port)->mn_Node.ln_Name;
                LOG("DEBUG: received DOS packet with type %ld\n", pkt->dp_Type);
                switch (pkt->dp_Type) {
                    case ACTION_FINDOUTPUT:
                        LOG("DEBUG: packet type = ACTION_FINDOUTPUT\n");
                        break;

                    default:
                        LOG("ERROR: packet type is unknown\n");
                        return_packet(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                }
            }
            Delay(250);
            Close(logfh);
        }
        DeleteMsgPort(logport);
    }
}