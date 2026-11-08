@ Entry point of the 3GX: Rosalina calls it once from the game's first instruction
@ (sysmodules/rosalina/source/plugin/pluginLoader.s, after saving the game's registers),
@ before the game runs. It returns, and the game starts as usual.
.section .crt0, "ax", %progbits
.arm
.align 2
.global _start
_start:
    push    {r4-r11, lr}
    bl      launch_main
    pop     {r4-r11, pc}

@ The three system calls the plugin uses. svcControlService is Luma's (0xB0): the kernel
@ extension does not check permissions for its own calls.
.section .text.svc, "ax", %progbits
.global plg_svcSendSyncRequest
plg_svcSendSyncRequest:
    svc     0x32
    bx      lr
.global plg_svcCloseHandle
plg_svcCloseHandle:
    svc     0x23
    bx      lr
.global plg_svcControlService
plg_svcControlService:
    svc     0xB0
    bx      lr
