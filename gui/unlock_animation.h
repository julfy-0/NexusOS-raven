#ifndef NEXUSOS_UNLOCK_ANIMATION_H
#define NEXUSOS_UNLOCK_ANIMATION_H
#include <stdint.h>

/* Frame-driven unlock transition controller.  Call update() once per frame and
 * feed the returned values into the GUI renderer.  Time is supplied by caller.
 *
 * All values use a fixed-point 0-1000 scale (i.e. 1000 == 1.0, 500 == 0.5).
 * No floating-point arithmetic is used so the file compiles with -mno-sse. */
typedef enum {
    NEXUS_UNLOCK_IDLE = 0,
    NEXUS_UNLOCK_AUTHENTICATED,
    NEXUS_UNLOCK_RUNNING,
    NEXUS_UNLOCK_FINISHED
} nexus_unlock_state_t;

typedef struct {
    nexus_unlock_state_t state;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    /* All fields below: fixed-point, 0-1000 represents 0.000-1.000 */
    int32_t progress;       /* linear 0..1000 */
    int32_t eased;          /* smoothstep 0..1000 */
    int32_t panel_offset_y; /* normalized screen heights *1000: 0 -> -100 */
    int32_t panel_opacity;  /* 1000 -> 0 */
    int32_t wallpaper_dim;  /* 0 -> 420 -> 0 */
    int32_t content_scale;  /* 985 -> 1000 */
} nexus_unlock_animation_t;

void nexus_unlock_animation_init(nexus_unlock_animation_t *animation);
void nexus_unlock_animation_start(nexus_unlock_animation_t *animation, uint32_t duration_ms);
/* Returns 1 while animation is active, 0 when idle or finished. */
int nexus_unlock_animation_update(nexus_unlock_animation_t *animation, uint32_t delta_ms);
#endif