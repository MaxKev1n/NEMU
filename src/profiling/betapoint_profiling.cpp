#include "profiling/betapoint_profiling.h"

#include <common.h>
#include <debug.h>
#include <lz4.h>
#include <zstd.h>
#include <compress/zstd_compress_internal.h>
#include <cstdlib>

extern uint64_t g_nr_guest_instr;

namespace BetaPointNS {

CompressProfiler::CompressProfiler()
    : cctx(ZSTD_createCCtx()) {
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 1);
    ZSTD_compressBegin(cctx, 1);
    outPtr = outputBuf;
}


ControlProfiler::ControlProfiler()
    : CompressProfiler(),
    brHist(brHistLen, 0) {
    info = reinterpret_cast<ControlInfo *>(inputBuf);
}

unsigned BetaPointNS::CompressProfiler::compressBlock(size_t input_size, char *in_ptr) {
    unsigned c_size = ZSTD_compressBlock(cctx, outPtr, contiguousBytes, in_ptr, input_size);
    return c_size;
}

void ControlProfiler::controlProfile(vaddr_t pc, vaddr_t target, bool taken) {
    // Logbeta("hist size: %lu", brHist.size());
    // fflush(stdout);
    Logbeta("pc: 0x%010lx, target: 0x%010lx, hist:0x%016lx,taken: %i", pc, target, brHist.to_ulong(), taken);
    fflush(stdout);
    info->pc = pc;
    info->target = target;
    info->taken = taken;
    info->hist = brHist.to_ulong();
    if (false) {
#define CMPBUFSIZE (LZ4_COMPRESSBOUND(_tokenBytes))
        char cmpBuf[CMPBUFSIZE];
        const int cmpBytes = LZ4_compress_fast_continue(lz4Stream, (char *)info, cmpBuf, tokenBytes(), CMPBUFSIZE, 0);
        Logbeta("lz cmp bytes: %i", cmpBytes);
        fflush(stdout);
    }
    if (false) {
        unsigned c_size = compressBlock(tokenBytes(), (char *)info);
        Logbeta("Br info zstd cSize: %u", c_size);
        fflush(stdout);
    }

    info++;
    if ((void*) info >= (void*) inputEnd) {
        info = reinterpret_cast<ControlInfo *>(inputBuf);
        Logbeta("flush br history dict");
    }

    // ppm
    if (false) {
        bool pred = ppm.lookup(pc, brHist);
        ppm.update(pc, brHist, taken, pred);
    }


    brHist = brHist << 1;
    brHist[0] = taken;
}

void ControlProfiler::onExit() {
    Log("PPM correct: %lu, PPM mispred: %lu", ppm.correct, ppm.mispred);
    Log("MPKI: %f", (double)ppm.mispred / (double) ::g_nr_guest_instr * 1000);
}

MemProfiler::MemProfiler()
    : CompressProfiler() {
    info = reinterpret_cast<MemInfo *>(inputBuf);
    bitMap = roaring_bitmap_create_with_capacity(CONFIG_MSIZE/64);
    std::map<int64_t, int> vecGlobalMap;
    globalStride.push_back(vecGlobalMap);
    globalStride.push_back(vecGlobalMap);
    std::map<vaddr_t, paddr_t> vecLocalMap;
    localMap.push_back(vecLocalMap);
    localMap.push_back(vecLocalMap);
    std::map<vaddr_t, std::vector<std::pair<int64_t, int> > > veclocalStride;
    localStride.push_back(veclocalStride);
    localStride.push_back(veclocalStride);
}

void MemProfiler::compressProfile(vaddr_t pc, vaddr_t vaddr, paddr_t paddr) {
    info->vaddr = vaddr;
    info->paddr = paddr;
    info->padding0 = 0;
    info->padding1 = 0;
    {
        unsigned c_size = compressBlock(tokenBytes(), (char *)info);
        Logbeta("Mem info zstd cSize: %u", c_size);
    }
    info++;
    if ((void*) info >= (void*) inputEnd) {
        info = reinterpret_cast<MemInfo *>(inputBuf);
        Logbeta("flush mem history dict");
    }
}

void MemProfiler::memProfile(vaddr_t pc, vaddr_t vaddr, paddr_t paddr) {
    Logbeta("vaddr: 0x%010lx, paddr: 0x%010lx", vaddr, paddr);
    roaring_bitmap_add(bitMap, paddr / CacheBlockSize);
}

void MemProfiler::globalStrideProfile(paddr_t paddr, paddr_t pre_addr, int mem_type){
    int64_t stride = int64_t(paddr - pre_addr);
    if (globalStride[mem_type].find(stride) == globalStride[mem_type].end()) {
        globalStride[mem_type][stride] = 1;
    } else {
        globalStride[mem_type][stride] = globalStride[mem_type][stride] + 1;
    }
}

void MemProfiler::localStrideProfile(vaddr_t pc, paddr_t paddr, int mem_type){
    if (localMap[mem_type].find(pc) == localMap[mem_type].end()) {
        localMap[mem_type][pc] = paddr;
    } else {
        paddr_t pre_addr = localMap[mem_type][pc];
        localMap[mem_type][pc] = paddr;
        int64_t stride = int64_t(paddr - pre_addr);
        if (localStride[mem_type].find(pc) == localStride[mem_type].end()) {
            std::vector<std::pair<int64_t, int> > vec;
            vec.push_back(std::make_pair(stride, 1));
            localStride[mem_type].insert(std::pair<vaddr_t, std::vector<std::pair<int64_t, int> > >(pc, vec));
        } else {
            for(std::vector<std::pair<int64_t, int> >::iterator iter = localStride[mem_type][pc].begin();iter != localStride[mem_type][pc].end();++iter){
                if (iter->first == stride) {
                    iter->second++;
                    break;
                }
            }
        }
    }
}

void MemProfiler::dumpStride(int bucketSize){
    std::ofstream ofs;
    ofs.open("strideHistogram.txt");
    std::stringstream ss;

    for(int mem_type = 0;mem_type < 2;mem_type++){
        if (mem_type) {
            ss << "global read stride:" << std::endl;
        } else {
            ss << "global write stride:" << std::endl;
        }
        for(std::map<int64_t, int>::iterator iter = globalStride[mem_type].begin();iter != globalStride[mem_type].end();++iter){
            if (iter->first < 0) {
                ss << std::hex << "-" << -iter->first << " " << std::oct << iter->second << std::endl;
            } else {
                ss << std::hex << iter->first << " " << std::oct << iter->second << std::endl;
            }
        }
    }

    for(int mem_type = 0;mem_type < 2;mem_type++){
        if (mem_type) {
            ss << "local read stride:" << std::endl;
        } else {
            ss << "local write stride:" << std::endl;
        }
        std::map<int64_t, int> local;
        for(std::map<vaddr_t, std::vector<std::pair<int64_t, int> > >::iterator iter = localStride[mem_type].begin();iter != localStride[mem_type].end();++iter){
            std::vector<std::pair<int64_t, int> > vec = iter->second;
            for(std::vector<std::pair<int64_t, int> >::iterator iter1 = vec.begin();iter1 != vec.end();++iter1){
                int64_t index = iter->first < 0 ? iter1->first / bucketSize - 1 : iter1->first / bucketSize;
                if (local.find(index) == local.end()) {
                    local[index] = iter1->second;
                } else {
                    local[index] = local[index] + iter1->second;
                }
            }
        }
        for(std::map<int64_t, int>::iterator iter = local.begin();iter != local.end();++iter){
            if (iter->first >= 0) {
                ss << std::hex << iter->first * bucketSize << "~" << (iter->first + 1) * bucketSize - 1 << " " << std::oct << iter->second << std::endl;
            } else {
                ss << std::hex << "-" << -iter->first * bucketSize << "~-" << (-iter->first - 1) * bucketSize + 1 << " " << std::oct << iter->second << std::endl;
            }
        }
    }

    ofs << ss.rdbuf();
    ofs.close();
}

void MemProfiler::onExit() {
    uint32_t cardinality = roaring_bitmap_get_cardinality(bitMap);
    Log("Footprint: %u cacheblocks\n", cardinality);
    Log("dump stride\n");
    dumpStride(6);
}

ControlProfiler ctrlProfiler;
MemProfiler memProfiler;

}


extern "C" {

void control_profile(vaddr_t pc, vaddr_t target, bool taken) {
    BetaPointNS::ctrlProfiler.controlProfile(pc, target, taken);
}

void global_stride_profile(paddr_t paddr, paddr_t pre_addr, int mem_type){
    BetaPointNS::memProfiler.globalStrideProfile(paddr, pre_addr, mem_type);
}

void local_stride_profile(vaddr_t pc, paddr_t paddr, int mem_type){
    BetaPointNS::memProfiler.localStrideProfile(pc, paddr, mem_type);
}

void beta_on_exit() {
    BetaPointNS::ctrlProfiler.onExit();
    BetaPointNS::memProfiler.onExit();
}

}  // extern C

