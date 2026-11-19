import sys
p=sys.argv[1] if len(sys.argv)>1 else 'gui/core/gui.c'
TASKBAR=r'''static void draw_taskbar(void) {
    int top = tb_top(), h = tb_h(), W = (int)fb->width;
    if (g_glass) {
        blur_rect(0, top, W, h, g_blur_r[g_blur_lvl]);
        rect_alpha(0, top, W, h, C_TASKBAR, 170);
    } else rect(0, top, W, h, C_TASKBAR);
    rect(0, top, W, 1, 0x3C3F49);

    /* Start button */
    rc_t s = r_start();
    if (g_menu_open) tb_hi(s, 1); else if (g_hover == 1) tb_hi(s, 0);
    icon_nexus(s.x + s.w / 2, s.y + s.h / 2, C_ACCENT);

    /* Search box (hidden on narrow screens) */
    rc_t q = r_search();
    if (q.w) {
        rounded_rect_alpha(q.x, q.y, q.w, q.h, 6, C_FIELD, gl(g_hover == 2 ? 230 : 190));
        icon_search(q.x + 18, q.y + q.h / 2, C_TEXT_DIM);
        text_at("Search", q.x + 36, q.y + (q.h - 16) / 2, 1, C_TEXT_DIM);
    }

    /* Window buttons */
    for (int i = 0; i < tb_count; ++i) {
        gui_window_t *w = gui_window_get((gui_window_id_t)tb_order[i]);
        if (!w) continue;
        rc_t r = r_task(i);
        int active = !w->minimized && gui_window_active_id() == (int)w->id;
        if (active) tb_hi(r, 1); else if (g_hover == 10 + i) tb_hi(r, 0);
        app_icon(app_of_id(w->id), r.x + 8, r.y + (r.h - 16) / 2, 16, active ? C_TB_ACTIVE : C_TASKBAR);
        if (w->title && r.w > 60) text_clip(w->title, r.x + 32, r.y + (r.h - 16) / 2, r.w - 36, w->minimized ? C_TEXT_DIM : C_TEXT);
        rect(r.x + 6, r.y + r.h - 3, r.w - 12, 2, active ? C_ACCENT : (w->minimized ? C_TEXT_DIM : C_WIN_BORDER));
    }

    /* Tray + clock */
    rc_t t = r_tray(), c = r_clock();
    if (g_hover == 4) tb_hi(t, 0);
    icon_net(t.x + 10, t.y + (t.h - 14) / 2, C_TEXT);
    icon_speaker(t.x + 32, t.y + (t.h - 14) / 2 - 1, C_TEXT);
    if (g_hover == 5) tb_hi(c, 0);
    char tm[8];
    fmt_2d(tm, g_clock_h); tm[2] = ':'; fmt_2d(tm + 3, g_clock_m);
    text_at(tm, c.x + c.w - 8 - text_width(tm, 1), c.y + c.h / 2 - 16, 1, C_TEXT);
    char d[16]; int n = 0;
    n += fmt_2d(d + n, g_date_d); d[n++] = '.'; n += fmt_2d(d + n, g_date_mo); d[n++] = '.'; n += fmt_u(d + n, (uint32_t)g_date_y); d[n] = 0;
    text_at(d, c.x + c.w - 8 - text_width(d, 1), c.y + c.h / 2, 1, C_TEXT_DIM);

    /* Show-desktop sliver */
    rc_t sd = r_desktop_btn();
    if (g_hover == 3) rect_alpha(sd.x, sd.y, sd.w, sd.h, 0xFFFFFF, 40);
    rect(sd.x, sd.y + 6, 1, sd.h - 12, 0x4A4E5C);
}

'''
L=open(p,encoding='utf-8').read().split('\n')
i=lambda n:n-1
assert len(L)>1700 and L[i(1610)].startswith('static void draw_desktop_icons')
assert L[i(1625)].startswith('static void tb_hi')
assert L[i(1652)].startswith('static void draw_start_menu')
assert L[i(1655)].strip()=='if (g_glass) {' and 'C_MENU, 195' in L[i(1699)]
tb_hi=L[i(1624):i(1628)+1]
taskbar=TASKBAR.split('\n')
out=L[:i(1609)]+['/* ---- Drawing: taskbar, menu -------------------------------------------- */']+tb_hi+['']+taskbar
out+=L[i(1652):i(1656)]+['        blur_rect(m.x, m.y, m.w, m.h, g_blur_r[g_blur_lvl]);']+L[i(1699):]
import shutil; shutil.copy(p, p+'.bak')
open(p,'w',encoding='utf-8').write('\n'.join(out))
