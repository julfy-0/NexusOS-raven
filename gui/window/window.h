#ifndef NEXUSOS_GUI_WINDOW_H
#define NEXUSOS_GUI_WINDOW_H

#include <stdint.h>

#define GUI_WINDOW_MAX 8

typedef enum {
    GUI_WINDOW_NONE = 0,
    GUI_WINDOW_TERMINAL = 1,
    GUI_WINDOW_SETTINGS = 2,
    GUI_WINDOW_FILES = 3,
    GUI_WINDOW_SEARCH = 4,
    GUI_WINDOW_CALC = 5,
    GUI_WINDOW_NOTES = 6
} gui_window_id_t;

typedef struct {
    gui_window_id_t id;
    int x, y, width, height;
    int visible;      /* the window exists (open), minimized or not */
    int focused;
    int minimized;
    int maximized;
    int rx, ry, rw, rh;   /* geometry to restore after un-maximize */
    const char *title;
} gui_window_t;

/* Window manager 3.0: several windows at once, z-order, minimize/maximize.
 * `work_height` is the screen height without the taskbar. */
void gui_window_manager_init(int screen_width, int work_height);
gui_window_t *gui_window_open(gui_window_id_t id, const char *title, int width, int height);
void gui_window_close(gui_window_id_t id);
void gui_window_focus(gui_window_id_t id);            /* raises + activates (restores if minimized) */
void gui_window_minimize(gui_window_id_t id);
void gui_window_toggle_maximize(gui_window_id_t id);
void gui_window_minimize_all(void);
gui_window_t *gui_window_get(gui_window_id_t id);      /* open window or NULL */
int gui_window_active_id(void);
/* Visible (not minimized) windows from bottom to top. Returns the count. */
int gui_window_stack(gui_window_id_t out[GUI_WINDOW_MAX]);
/* Topmost visible window under the point, or GUI_WINDOW_NONE. */
gui_window_id_t gui_window_at(int x, int y);
void gui_window_move(gui_window_id_t id, int x, int y);

#endif
