/*
 * rec3ds-launch.3gx: starts the 3DS Recorder module inside a game that rebooted an Old 3DS.
 *
 * A game that wants 80 / 96 MB of memory on an Old 3DS (Smash, Pokemon Sun / Moon, Monster
 * Hunter...) makes the console reboot into that memory mode first, which ends every process, the
 * recorder too. Right before the reboot the recorder leaves Rosalina a one-time order
 * (/luma/plugins/user_param.bin: this file, that game, no screen flash). Rosalina reads it after
 * the reboot and calls _start once from the game's first instruction. The plugin asks pm to start
 * the recorder module and returns; the module then deletes the order.
 *
 * No libctru runtime: no heap, no threads, nothing left running in the game.
 */
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/ipc.h>
#include <3ds/svc.h>

#define REC_TITLE_ID 0x0004013000F1D302ULL
#define MEDIATYPE_NAND 0

Result plg_svcSendSyncRequest(Handle session);
Result plg_svcCloseHandle(Handle handle);
Result plg_svcControlService(u32 op, Handle *out, const char *name);

void launch_main(void)
{
    Handle pm = 0;
    /* Luma: a copy of an open pm:app session (games may not ask srv for pm:app) */
    if (R_FAILED(plg_svcControlService(0 /* steal client session */, &pm, "pm:app")) || !pm)
        return;
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x1, 5, 0);   /* LaunchTitle(FS_ProgramInfo, launchFlags) */
    cmd[1] = (u32)REC_TITLE_ID;
    cmd[2] = (u32)(REC_TITLE_ID >> 32);
    cmd[3] = MEDIATYPE_NAND;
    cmd[4] = 0;
    cmd[5] = 0;
    plg_svcSendSyncRequest(pm);
    plg_svcCloseHandle(pm);
}
