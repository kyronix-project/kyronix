#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <bits/hwcap.h>
#include <elf.h>

unsigned long getauxval(unsigned long);

#ifdef __cplusplus
}
#endif

#endif
