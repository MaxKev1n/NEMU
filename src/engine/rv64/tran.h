#ifndef __TRAN_H__
#define __TRAN_H__

#include <common.h>

extern int tran_next_pc;
enum { NEXT_PC_SEQ, NEXT_PC_JMP, NEXT_PC_BRANCH };

#define BBL_MAX_SIZE (16 * 1024)
#define RV64_EXEC_PC BBL_MAX_SIZE // skip bbl

// scratch pad memory, whose address space is [0, 64)
#define spm(op, reg, offset) concat(rv64_, op)(reg, x0, offset)
#define SPM_X86_REG 0    // x86 byte/word register write

enum { x0 = 0 };

#ifdef __ISA_x86__
enum { tmp0 = 30, mask32 = 24, mask16 = 25 };
#endif

#endif
