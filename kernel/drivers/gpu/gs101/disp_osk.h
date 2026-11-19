#ifndef NEXUS_GS101_DISP_OSK_H
#define NEXUS_GS101_DISP_OSK_H

#include "gs101_disp.h"

#define OSK_BUFFER_MAX 128

struct osk_state {
    int  visible;
    int  shift;
    int  sym_mode;
    int  pressed_key_idx;
    char buffer[OSK_BUFFER_MAX];
    int  buffer_len;
    int  last_touch_x;
    int  last_touch_y;
};

void disp_osk_init(void);
void disp_osk_set_visible(int visible);
int  disp_osk_is_visible(void);
void disp_osk_toggle(void);
void disp_osk_draw(struct disp_state *s);
int  disp_osk_handle_touch(struct disp_state *s, int tx, int ty, int pressed);
const char *disp_osk_get_buffer(void);
void disp_osk_clear_buffer(void);
int  disp_osk_was_entered(void);
void disp_osk_clear_enter_flag(void);

#endif
