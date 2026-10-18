/*
 * Native replacement of hot Bink 1 video routines (see bink-hle.c).
 */

#ifndef TARGET_I386_TCG_BINK_HLE_H
#define TARGET_I386_TCG_BINK_HLE_H

#include "exec/translator.h"

#define BINK_HLE_NONE (-1)

bool bink_hle_enabled(void);

/*
 * If the code at @pc (the first instruction of the TB being translated) is
 * a known Bink routine, return its id and fill @bases with the guest
 * addresses it was linked against. Reads the whole routine through the
 * translator, so the TB is invalidated if any of it changes.
 */
int bink_hle_match(CPUX86State *env, DisasContextBase *db, vaddr pc,
                   uint32_t bases[3]);

#endif
