#ifndef __BETAPOINT_EXT_H__
#define __BETAPOINT_EXT_H__

#include<common.h>

void control_profile(vaddr_t pc, vaddr_t target, bool taken);
void beta_on_exit();
void global_stride_profile(paddr_t paddr, paddr_t pre_addr, int mem_type);
void local_stride_profile(vaddr_t pc, paddr_t paddr, int mem_type);

#endif //__BETAPOINT_EXT_H__