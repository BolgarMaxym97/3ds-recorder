#ifndef CSVC_H
#define CSVC_H
/* Luma3DS custom SVCs used by the spike (implemented in csvc.s, see Luma csvc.h). */
#include <3ds.h>

#define PROCESSOP_GET_PA_FROM_VA 4
#define SERVICEOP_STEAL_CLIENT_SESSION 0

u32    svcConvertVAToPA(const void *VA, bool writeCheck);
Result svcControlMemoryUnsafe(u32 *out, u32 addr0, u32 size, MemOp op, MemPerm perm);
Result svcControlProcess(Handle process, u32 op, u32 varg2, u32 varg3);
Result svcControlService(u32 op, ...);

#endif
