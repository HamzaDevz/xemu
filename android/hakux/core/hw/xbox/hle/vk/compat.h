/*
 * x1box: the HLE GPU on Vulkan 1.1 drivers (the Adreno 610's, older Malis').
 *
 * The back end is written for Vulkan 1.3 plus extended dynamic state 1/2 and
 * vertex-input dynamic state. On a device without them, hle_compat_install
 * puts a layer under the device table that gives the rest of the back end
 * those entry points anyway:
 *   - dynamic rendering: render passes (cached by attachment formats, ops and
 *     layouts) and framebuffers (by views and size);
 *   - the extended / vertex-input dynamic states: recorded per command buffer
 *     and baked into pipeline variants, made at the first draw that needs one;
 *   - timeline semaphores: a host-side value advanced by fences on the submits
 *     that signal it (every wait on one is the host's);
 *   - synchronization2, copy_commands2, host query reset, buffer device
 *     address: their KHR/EXT entry points.
 */
#ifndef HW_XBOX_HLE_VK_COMPAT_H
#define HW_XBOX_HLE_VK_COMPAT_H

#include <stdbool.h>

struct VolkDeviceTable;

/* Replaces t's entry points with the layer's; it calls the ones t had. */
void hle_compat_install(struct VolkDeviceTable *t, VkDevice dev);
/* The layer is in use (set by hle_compat_install). */
bool hle_compat_on(void);
/* The layer's table, before anything installed over it (the presenter's: no
 * batching, but the layer). */
const struct VolkDeviceTable *hle_compat_table(void);

#endif
