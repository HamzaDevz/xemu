/*
 * Sega Chihiro widescreen camera patches
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_XBOX_CHIHIRO_WIDESCREEN_H
#define HW_XBOX_CHIHIRO_WIDESCREEN_H

/* Guest memory by virtual address; false if any of it is unmapped. */
typedef bool (*ChihiroGuestRW)(uint32_t va, void *buf, uint32_t len,
                               bool write);

/* Patch the running game (its .xbe name, e.g. "vsg.xbe") to a 16:9 camera if
 * it is a known revision. True if it was patched. */
bool chihiro_widescreen_apply(const char *executable, ChihiroGuestRW rw);

#endif
