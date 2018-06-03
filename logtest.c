#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>


#define C_TO_BCPL_PTR(ptr) (((ULONG) (ptr)) >> 2)
#define BCPL_TO_C_PTR(ptr) (((ULONG) (ptr)) << 2)


int main()
{
    struct MsgPort *sport, *dport;
    struct Message *msg;
    struct StandardPacket *pkt;
    struct FileHandle *fh;
    char * fname;

    if ((sport = CreateMsgPort()) != NULL) {
        printf("DEBUG: created message port\n");
        if ((dport = DeviceProc("CON:")) != NULL) {
            printf("DEBUG: found process for CON:\n");
            if ((pkt = (struct StandardPacket *) AllocVec(sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
                pkt->sp_Msg.mn_ReplyPort    = sport;
                pkt->sp_Pkt.dp_Port         = sport;
                pkt->sp_Msg.mn_Node.ln_Name = (char *) &(pkt->sp_Pkt);
                pkt->sp_Pkt.dp_Link         = &(pkt->sp_Msg);
                if ((fh = (struct FileHandle *) AllocVec(sizeof(struct FileHandle), MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
                    fh->fh_Pos = -1;
                    fh->fh_End = -1;
                    if ((fname = (char *) AllocVec(256, MEMF_PUBLIC)) != NULL) {
                        fname[0] = 0x1c;
                        memcpy(fname + 1, "CON:0/0/640/50/CWNET Console", 0x1c);
                        pkt->sp_Pkt.dp_Type = ACTION_FINDOUTPUT;
                        pkt->sp_Pkt.dp_Arg1 = C_TO_BCPL_PTR(fh);
                        pkt->sp_Pkt.dp_Arg2 = NULL;
                        pkt->sp_Pkt.dp_Arg3 = C_TO_BCPL_PTR(fname);
                        PutMsg(dport, &(pkt->sp_Msg));
                        printf("DEBUG: sent packet with ACTION_FINDOUTPUT to handler\n");
                        WaitPort(sport);
                        msg = GetMsg(sport);
                        printf("DEBUG: answer received from handler with result code %ld\n", ((struct DosPacket *) msg->mn_Node.ln_Name)->dp_Res1);
                        pkt->sp_Pkt.dp_Type = ACTION_WRITE;
                        pkt->sp_Pkt.dp_Arg1 = ((struct FileHandle *) BCPL_TO_C_PTR(fh))->fh_Arg1;
                        pkt->sp_Pkt.dp_Arg2 = (LONG) "So a scheener Dog\n";
                        pkt->sp_Pkt.dp_Arg3 = 18;
                        PutMsg(dport, &(pkt->sp_Msg));
                        printf("DEBUG: sent packet with ACTION_WRITE to handler\n");
                        WaitPort(sport);
                        msg = GetMsg(sport);
                        printf("DEBUG: answer received from handler with result code %ld\n", ((struct DosPacket *) msg->mn_Node.ln_Name)->dp_Res1);

                        FreeVec(fname);
                    }
                    else {
                        printf("ERROR: could not allocate memory for file name\n");
                    }
                    FreeVec(fh);
                }
                else {
                    printf("ERROR: could not allocate memory for file handle\n");
                }
                FreeVec(pkt);
            }
            else {
                printf("ERROR: could not allocate memory for DOS packet\n");
            }
        }
        else {
            printf("ERROR: could not find process for CON:\n");
        }
        DeleteMsgPort(sport);
    }
    else {
        printf("ERROR: could not create message port\n");
    }
}