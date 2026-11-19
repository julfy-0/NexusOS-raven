#include "unlock_animation.h"

/* Clamp v to [0, 1000]. */
static int32_t clamp1000(int32_t v) {
    if (v < 0)    return 0;
    if (v > 1000) return 1000;
    return v;
}

/* Cubic smoothstep: e = t^2*(3 - 2*t), all in fixed-point *1000.
 * Input t: 0..1000.  Output: 0..1000. */
static int32_t smoothstep1000(int32_t t) {
    /* t2 = t*t / 1000  (still in *1000 scale) */
    int32_t t2 = (int32_t)((int64_t)t * t / 1000);
    /* e = t2 * (3000 - 2*t) / 1000 */
    int32_t e  = (int32_t)((int64_t)t2 * (3000 - 2 * t) / 1000000);
    return clamp1000(e);
}

static void apply_progress(nexus_unlock_animation_t *a) {
    int32_t t = clamp1000(a->progress);
    int32_t e = smoothstep1000(t);
    a->eased          = e;
    /* panel_offset_y: 0 -> -100 (representing 0 -> -0.10 screen heights) */
    a->panel_offset_y = -(int32_t)((int64_t)100 * e / 1000);
    /* panel_opacity: 1000 -> 0 */
    a->panel_opacity  = 1000 - e;
    /* wallpaper_dim: 420 -> 0 */
    a->wallpaper_dim  = (int32_t)((int64_t)420 * (1000 - e) / 1000);
    /* content_scale: 985 -> 1000 */
    a->content_scale  = 985 + (int32_t)((int64_t)15 * e / 1000);
}

void nexus_unlock_animation_init(nexus_unlock_animation_t *a) {
    if (!a) return;
    a->state       = NEXUS_UNLOCK_IDLE;
    a->elapsed_ms  = 0;
    a->duration_ms = 360;
    a->progress    = 0;
    apply_progress(a);
}

void nexus_unlock_animation_start(nexus_unlock_animation_t *a, uint32_t duration_ms) {
    if (!a) return;
    a->state       = NEXUS_UNLOCK_RUNNING;
    a->elapsed_ms  = 0;
    a->duration_ms = duration_ms ? duration_ms : 1;
    a->progress    = 0;
    apply_progress(a);
}

int nexus_unlock_animation_update(nexus_unlock_animation_t *a, uint32_t delta_ms) {
    if (!a || a->state != NEXUS_UNLOCK_RUNNING) return 0;
    if (UINT32_MAX - a->elapsed_ms < delta_ms) a->elapsed_ms = a->duration_ms;
    else a->elapsed_ms += delta_ms;
    if (a->elapsed_ms >= a->duration_ms) {
        a->elapsed_ms = a->duration_ms;
        a->progress   = 1000;
        a->state      = NEXUS_UNLOCK_FINISHED;
    } else {
        /* progress = elapsed * 1000 / duration, rounded */
        a->progress = (int32_t)((int64_t)a->elapsed_ms * 1000 / a->duration_ms);
    }
    apply_progress(a);
    return a->state == NEXUS_UNLOCK_RUNNING;
}