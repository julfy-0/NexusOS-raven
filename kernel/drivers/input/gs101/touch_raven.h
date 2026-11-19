#ifndef NEXUS_TOUCH_RAVEN_H
#define NEXUS_TOUCH_RAVEN_H

#include <stdint.h>

/* Samsung SEC_TS (S6SY79x) touchscreen driver, Pixel 6 Pro (raven / gs101), SPI11 */

#define TOUCH_RAVEN_VERSION  "0.1"
#define MAX_TOUCH_POINTS     10

struct disp_state;

struct touch_point {
    uint16_t x, y;       /* native panel coordinates, 1440x3120 */
    uint8_t  id;         /* 0..MAX_TOUCH_POINTS-1 */
    uint8_t  pressure;   /* 0..63 */
    uint8_t  pressed;    /* 1 = press/move, 0 = released */
};

struct touch_state {
    struct touch_point points[MAX_TOUCH_POINTS];
    uint8_t  num_points;
    uint8_t  active;     /* 1 if any finger is down */
    uint32_t event_counter;
};

struct touch_debug_info {
    uint8_t  last_rx_header[7];
    uint8_t  last_rx_payload[8];
    uint32_t poll_count;
    uint32_t packet_count;
    uint32_t spi_status;
    uint8_t  hw_active;
    uint16_t last_x, last_y;
};

int         touch_raven_init(void);          /* 0 = ok */
const char *touch_raven_reason(void);        /* why init failed */
int         touch_raven_poll(struct touch_state *st);  /* 1 = state updated */
void        touch_raven_get_debug(struct touch_debug_info *dbg);
void        touch_raven_render_feedback(struct disp_state *s, const struct touch_state *st);

#endif