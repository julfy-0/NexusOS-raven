/* NexusOS – framebuffer helper layer
 *
 * Tracks whether a hardware GPU driver has taken over the linear
 * framebuffer. The rest of the GUI still writes through the same
 * physical address provided by the UEFI GOP; this module simply
 * records the hw-ready flag so other subsystems can query it.
 */

#include "fb.h"
#include <stdint.h>

static int g_hw_ready = 0;   /* 1 = GPU driver programmed the buffer */

/* Called by the GPU driver once the linear framebuffer is active */
void fb_mark_hw_ready(int ready)
{
    g_hw_ready = ready ? 1 : 0;
}

/* Query whether a GPU driver provided the framebuffer */
int fb_is_hw_ready(void)
{
    return g_hw_ready;
}
