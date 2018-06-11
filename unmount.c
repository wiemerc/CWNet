/*
 * unmount.c - part of CWNet, an AmigaDOS handler that allows uploading files to a TFTP server
 *             over a serial link (using SLIP)
 *             "unmounts" a running handler, that is it sends ACTION_DIE and removes it from
 *             the device list. This program is adapted from the example in the Amiga Guru
 *             book by Ralph Babel.
 *
 * Copyright(C) 2017, 2018 Constantin Wiemer
 */


/*
 * included files
 */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>


#define C_TO_BCPL_PTR(ptr) ((BPTR) (((ULONG) (ptr)) >> 2))
#define BCPL_TO_C_PTR(ptr) ((APTR) (((ULONG) (ptr)) << 2))


char *BCPL_TO_C_STR(char *buffer, BSTR str) {
    memcpy(buffer,
           ((char *) BCPL_TO_C_PTR(str)) + 1,
           ((char *) BCPL_TO_C_PTR(str))[0]);
    buffer[(int) ((char *) BCPL_TO_C_PTR(str))[0]] = 0;
    return buffer;
}


static char *dol_types[] = {"device", "assign", "volume"};


int main(int argc, char **argv)
{
    struct DosInfo        *di;                      /* pointer to DosInfo structure */
    struct DosList        *dln;                     /* pointer to one DosList node */
    BPTR                  *dlnptr;                  /* pointer to this pointer, so we can change the value */
    struct DeviceNode     *dn = NULL;               /* pointer to the device node we are looking for */
    struct StandardPacket *pkt;
    struct MsgPort        *sport, *dport;
    struct Message        *msg;
    char                   name[256], *devname;
    int                    devnlen;

    if (argc != 2) {
        printf("ERROR: usage: unmount list|<device>\n");
        return RETURN_ERROR;
    }

    di = (struct DosInfo *) BCPL_TO_C_PTR(((struct RootNode *) DOSBase->dl_Root)->rn_Info);
    if (strcasecmp(argv[1], "list") == 0) {
        /* just list all volumes, devices and assigns
         * This is not completely safe because calling printf() may break Forbid()
         * (depending on the internal buffer size) and another task might be scheduled,
         * which could change the list underneath us while we walk it. */
        Forbid();
        for (dlnptr = &di->di_DevInfo;
            (dln = (struct DosList *) BCPL_TO_C_PTR(*dlnptr)) != NULL;
            dlnptr = &dln->dol_Next) {
            printf("%-30s\t%s\n", BCPL_TO_C_STR(name, dln->dol_Name), dol_types[dln->dol_Type]);
        }
        Permit();
        return RETURN_OK;
    }
    else {
        /* unmount the specified handler */
        devname = argv[1];
        devnlen  = strlen(argv[1]) - 1;
        Forbid();
        for (dlnptr = &di->di_DevInfo;
            (dln = (struct DosList *) BCPL_TO_C_PTR(*dlnptr)) != NULL;
            dlnptr = &dln->dol_Next) {
            /* check if names are of same length and match */
            if ((((char *) BCPL_TO_C_PTR(dln->dol_Name))[0] == devnlen)
                && (memcmp(((char *) BCPL_TO_C_PTR(dln->dol_Name)) + 1, devname, devnlen) == 0)) {
                /* remove device node from list, but remember it so we can send ACTION_DIE later
                 * dlnptr still points to the previous node's dol_Next field (or di_DevInfo),
                 * so we can just set the pointer's content to dol_Next of the current node
                 * in order to remove it from the list. Tricky stuff... */
                dn = (struct DeviceNode *) dln;
                *dlnptr = dln->dol_Next;
                break;
            }
        }
        Permit();
    }
    if (dn) {
        printf("DEBUG: found handler with name %s\n", devname);
        /* send ACTION_DIE */
        if ((sport = CreateMsgPort()) != NULL) {
            dport = dn->dn_Task;
            if ((pkt = (struct StandardPacket *) AllocVec(sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
                pkt->sp_Msg.mn_ReplyPort    = sport;
                pkt->sp_Pkt.dp_Port         = sport;
                pkt->sp_Msg.mn_Node.ln_Name = (char *) &(pkt->sp_Pkt);
                pkt->sp_Pkt.dp_Link         = &(pkt->sp_Msg);
                pkt->sp_Pkt.dp_Type         = ACTION_DIE;
                PutMsg(dport, &(pkt->sp_Msg));
                printf("DEBUG: sent packet with ACTION_DIE to handler\n");
                WaitPort(sport);
                msg = GetMsg(sport);
                printf("DEBUG: answer received from handler with result code %ld\n", ((struct DosPacket *) msg->mn_Node.ln_Name)->dp_Res1);
                FreeVec(pkt);
            }
            DeleteMsgPort(sport);
        }

        /* free all resources, but give handler time to terminate */
        Delay(250);
        if (dn->dn_Task != NULL) {
            printf("ERROR: handler seems to be still alive after ACTION_DIE - aborting\n");
            return RETURN_FAIL;
        }
        if ((dn->dn_SegList != 0) && (TypeOfMem(BCPL_TO_C_PTR(dn->dn_SegList)) != 0)) {
            /* unload executable if it does not live in ROM */
            UnLoadSeg(dn->dn_SegList);
        }
        if ((dn->dn_Handler != 0) && (TypeOfMem(BCPL_TO_C_PTR(dn->dn_Handler)) != 0)) {
            /* free memory for file name if it does not live in ROM */
            FreeVec(BCPL_TO_C_PTR(dn->dn_Handler));
        }
        FreeVec(BCPL_TO_C_PTR(dn->dn_Name));
        FreeVec(dn);
        printf("DEBUG: all resources freed\n");
        return RETURN_OK;
    }
    else {
        printf("ERROR: no handler found with name %s\n", devname);
        return RETURN_ERROR;
    }
}