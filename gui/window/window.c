#include "window.h"

static gui_window_t windows[GUI_WINDOW_MAX];
static gui_window_id_t zorder[GUI_WINDOW_MAX];   /* bottom .. top, zcount entries */
static int zcount;
static int screen_w, work_h;
static int active_id;
static int cascade;

static gui_window_t *slot(gui_window_id_t id) {
    if (id <= GUI_WINDOW_NONE || id >= GUI_WINDOW_MAX) return 0;
    return &windows[id];
}

static void z_remove(gui_window_id_t id) {
    int j = 0;
    for (int i = 0; i < zcount; ++i) if (zorder[i] != id) zorder[j++] = zorder[i];
    zcount = j;
}

static void z_raise(gui_window_id_t id) {
    z_remove(id);
    if (zcount < GUI_WINDOW_MAX) zorder[zcount++] = id;
}

static void refresh_focus(void) {
    for (int i = 0; i < GUI_WINDOW_MAX; ++i)
        windows[i].focused = (i == active_id && windows[i].visible && !windows[i].minimized);
}

/* Give focus to the topmost visible, non-minimized window (or nobody). */
static void activate_top(void) {
    active_id = GUI_WINDOW_NONE;
    for (int i = zcount - 1; i >= 0; --i) {
        gui_window_t *w = slot(zorder[i]);
        if (w && w->visible && !w->minimized) { active_id = (int)w->id; break; }
    }
    refresh_focus();
}

void gui_window_manager_init(int width, int height) {
    screen_w = width;
    work_h = height;
    active_id = GUI_WINDOW_NONE;
    zcount = 0;
    cascade = 0;
    for (int i = 0; i < GUI_WINDOW_MAX; ++i) {
        windows[i].id = GUI_WINDOW_NONE;
        windows[i].visible = windows[i].focused = windows[i].minimized = windows[i].maximized = 0;
    }
}

gui_window_t *gui_window_open(gui_window_id_t id, const char *title, int width, int height) {
    gui_window_t *w = slot(id);
    if (!w) return 0;
    if (width < 260) width = 260;
    if (height < 160) height = 160;
    if (width > screen_w - 16) width = screen_w - 16;
    if (height > work_h - 16) height = work_h - 16;
    w->title = title;
    if (!w->visible) {
        int step = 28 * (cascade++ % 6);
        w->width = width;
        w->height = height;
        w->x = (screen_w - width) / 2 - 60 + step;
        w->y = (work_h - height) / 2 - 30 + step;
        if (w->x < 4) w->x = 4;
        if (w->y < 4) w->y = 4;
        if (w->x + w->width > screen_w) w->x = screen_w - w->width;
        if (w->y + w->height > work_h) w->y = work_h - w->height;
        w->maximized = 0;
        w->minimized = 0;
        w->id = id;
        w->visible = 1;
    }
    gui_window_focus(id);
    return w;
}

void gui_window_close(gui_window_id_t id) {
    gui_window_t *w = slot(id);
    if (!w || !w->visible) return;
    w->visible = 0;
    w->focused = 0;
    w->minimized = 0;
    w->maximized = 0;
    z_remove(id);
    if (active_id == (int)id) activate_top();
}

void gui_window_focus(gui_window_id_t id) {
    gui_window_t *w = slot(id);
    if (!w || !w->visible) return;
    w->minimized = 0;
    z_raise(id);
    active_id = (int)id;
    refresh_focus();
}

void gui_window_minimize(gui_window_id_t id) {
    gui_window_t *w = slot(id);
    if (!w || !w->visible) return;
    w->minimized = 1;
    if (active_id == (int)id) activate_top();
    else refresh_focus();
}

void gui_window_minimize_all(void) {
    for (int i = 0; i < GUI_WINDOW_MAX; ++i)
        if (windows[i].visible) windows[i].minimized = 1;
    activate_top();
}

void gui_window_toggle_maximize(gui_window_id_t id) {
    gui_window_t *w = slot(id);
    if (!w || !w->visible) return;
    if (w->maximized) {
        w->x = w->rx; w->y = w->ry; w->width = w->rw; w->height = w->rh;
        w->maximized = 0;
    } else {
        w->rx = w->x; w->ry = w->y; w->rw = w->width; w->rh = w->height;
        w->x = 0; w->y = 0; w->width = screen_w; w->height = work_h;
        w->maximized = 1;
    }
}

gui_window_t *gui_window_get(gui_window_id_t id) {
    gui_window_t *w = slot(id);
    return (w && w->visible) ? w : 0;
}

int gui_window_active_id(void) { return active_id; }

int gui_window_stack(gui_window_id_t out[GUI_WINDOW_MAX]) {
    int n = 0;
    for (int i = 0; i < zcount; ++i) {
        gui_window_t *w = slot(zorder[i]);
        if (w && w->visible && !w->minimized) out[n++] = zorder[i];
    }
    return n;
}

gui_window_id_t gui_window_at(int x, int y) {
    for (int i = zcount - 1; i >= 0; --i) {
        gui_window_t *w = slot(zorder[i]);
        if (!w || !w->visible || w->minimized) continue;
        if (x >= w->x && y >= w->y && x < w->x + w->width && y < w->y + w->height) return w->id;
    }
    return GUI_WINDOW_NONE;
}

void gui_window_move(gui_window_id_t id, int x, int y) {
    gui_window_t *w = slot(id);
    if (!w || !w->visible || w->maximized) return;
    if (x < -(w->width - 80)) x = -(w->width - 80);
    if (x > screen_w - 80) x = screen_w - 80;
    if (y < 0) y = 0;
    if (y > work_h - 32) y = work_h - 32;
    w->x = x; w->y = y;
}
