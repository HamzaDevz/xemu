/*
 * hakuX HLE — on-demand debug properties (debug.hakux.*) that act on a new
 * value. The value seen at the first poll is the baseline, an empty one
 * included (a property reset to '' before a run); afterwards each change to
 * a non-empty value triggers once.
 */
#ifndef HW_XBOX_HLE_PROP_H
#define HW_XBOX_HLE_PROP_H

#ifdef __ANDROID__
#include <stdbool.h>
#include <string.h>
#include <sys/system_properties.h>

typedef struct HleProp {
    char last[PROP_VALUE_MAX];
    bool polled;
} HleProp;

/* True when name holds a new, non-empty value (returned in v). */
static inline bool hle_prop_new(HleProp *p, const char *name, char v[PROP_VALUE_MAX])
{
    v[0] = 0;
    __system_property_get(name, v);
    bool first = !p->polled;
    p->polled = true;
    if (!strcmp(v, p->last)) {
        return false;
    }
    strcpy(p->last, v);
    return !first && v[0];
}
#endif

#endif
