/*
 * hakuX native core — XBE loader.
 *
 * Maps the title's executable the way the Xbox kernel does: headers at the
 * base address, preload sections at their virtual addresses, the kernel
 * thunk table resolved to the native kernel's exports.
 */
#ifndef NATIVE_CORE_XBE_LOAD_H
#define NATIVE_CORE_XBE_LOAD_H

#include "native/core/vm.h"
#include "native/core/xdvdfs.h"
#include "native/kernel/kernel.h"

#include "hw/xbox/hle/core/xbe.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcXbe {
    HleXbeInfo info;
    NcDiscFile file;
    uint32_t tls_dir;      /* VA of the IMAGE_TLS_DIRECTORY, or 0 */
    uint32_t stack_commit; /* PE stack commit size from the header */
    unsigned thunks;       /* kernel imports resolved */
} NcXbe;

bool nc_xbe_load(NcVm *vm, const NcDisc *disc, const char *path, const NcKernel *k, NcXbe *out);

/* Gives the pages of [va, va+len) that have no memory zeroed pages (an XBE
 * section coming in; pages it shares with a neighbour keep theirs). */
bool nc_xbe_back(NcVm *vm, uint32_t va, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
