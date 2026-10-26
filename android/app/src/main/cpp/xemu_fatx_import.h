#ifndef XEMU_FATX_IMPORT_H
#define XEMU_FATX_IMPORT_H

#include "block/block-common.h"
#include "qapi/error.h"

#include <stdbool.h>

typedef struct XemuFatxDashboardStatus {
    bool xboxdash_xbe_present;
    bool msdash_xbe_present;
    bool xbox_xtf_present;
    bool xodash_dir_present;
    bool audio_dir_present;
    bool fonts_dir_present;
    bool xboxdashdata_dir_present;
} XemuFatxDashboardStatus;

bool xemu_fatx_import_dashboard(BlockBackend *blk,
                                const char *source_root,
                                const char *backup_root,
                                Error **errp);

bool xemu_fatx_inspect_retail_dashboard(BlockBackend *blk,
                                        XemuFatxDashboardStatus *status,
                                        Error **errp);

/*
 * Handle-based access to one retail partition, used by the save manager.
 * Paths are absolute FATX paths ("/UDATA/4d530004"). The handle borrows blk;
 * close it before unreferencing the backend.
 */
typedef struct XemuFatxVolume XemuFatxVolume;

typedef struct XemuFatxEntryInfo {
    const char *name;
    bool is_dir;
    /* File size, or the total size of every file below a directory. */
    uint64_t size;
    /* Last-modified time as local-time seconds since the epoch, or 0. */
    int64_t modified;
} XemuFatxEntryInfo;

typedef void (*XemuFatxEntryCallback)(const XemuFatxEntryInfo *info, void *opaque);

XemuFatxVolume *xemu_fatx_volume_open(BlockBackend *blk, char letter, Error **errp);
/* Flushes pending FAT writes. Returns false if that flush failed. */
bool xemu_fatx_volume_close(XemuFatxVolume *vol, Error **errp);

bool xemu_fatx_volume_space(XemuFatxVolume *vol,
                            uint64_t *total_bytes,
                            uint64_t *free_bytes,
                            Error **errp);
/* A missing path is not an error: it succeeds with *found = false. */
bool xemu_fatx_volume_list(XemuFatxVolume *vol,
                           const char *path,
                           bool dir_sizes,
                           XemuFatxEntryCallback callback,
                           void *opaque,
                           bool *found,
                           Error **errp);
bool xemu_fatx_volume_read_file(XemuFatxVolume *vol,
                                const char *path,
                                uint8_t **data,
                                size_t *size,
                                Error **errp);
bool xemu_fatx_volume_export(XemuFatxVolume *vol,
                             const char *fatx_path,
                             const char *host_path,
                             Error **errp);
/*
 * Merges the contents of host_dir into fatx_dir, replacing files that already
 * exist. Checks names and free space first so a doomed import writes nothing.
 */
bool xemu_fatx_volume_import(XemuFatxVolume *vol,
                             const char *host_dir,
                             const char *fatx_dir,
                             Error **errp);
bool xemu_fatx_volume_remove(XemuFatxVolume *vol, const char *path, Error **errp);

#endif
