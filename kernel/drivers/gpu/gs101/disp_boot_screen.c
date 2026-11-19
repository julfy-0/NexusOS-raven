/*
 * disp_boot_screen.c — экран загрузки NexusOS (Pixel 6 Pro, 1440x3120)
 * Шапка с логотипом, прогресс, карточки служб со статус-плашкой OK/FAIL,
 * тусклые строки-заметки, итоговая сводка. Строки до старта дисплея буферизуются.
 */
#include "a64.h"
#include "gs101_disp.h"
#include "disp_draw.h"
#include "disp_ui.h"
#include "nexus_version.h"
#include "disp_boot_screen.h"

#define C_BG      0x07090F
#define C_CARD    0x0F1420
#define C_LINE    0x1C2333
#define C_TRACK   0x151B2B
#define C_ACCENT  0x38BDF8
#define C_INDIGO  0x818CF8
#define C_OK      0x34D399
#define C_FAIL    0xF43F5E
#define C_TEXT    0xE6EAF2
#define C_DIM     0x8B93A7
#define C_PILLTXT 0x04120C

#define M            72     /* боковое поле (скругления экрана) */
#define TOP          150    /* отступ под вырез камеры */
#define SC           2      /* масштаб текста списка: 16x32 px на символ */
#define CHW          (8 * SC)
#define SVC_H        56
#define NOTE_H       40
#define GAP          10
#define FOOT_H       300
#define EXPECTED_SVC 11

#define BOOT_MAX 64
enum { E_SVC = 0, E_NOTE = 1 };

struct ent {
    uint8_t kind, ok;
    char    text[96];
    char    ver[16];
};

static struct ent        g_e[BOOT_MAX];
static int               g_n;
static struct disp_state *g_ds;
static int               g_svc, g_okc, g_failc, g_done;

/* ── строки ──────────────────────────────────────────────────────────── */
static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void scopy(char *d, int cap, const char *s) {
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void scat(char *d, int cap, const char *s) {
    int n = slen(d);
    if (s) for (int i = 0; s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
}
static void sdec(char *d, int cap, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char o[12]; for (int i = 0; i < n; i++) o[i] = t[n - 1 - i]; o[n] = 0;
    scat(d, cap, o);
}
static void shex(char *d, int cap, uint64_t v) {
    char o[19]; int n = 0;
    o[n++] = '0'; o[n++] = 'x';
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        uint32_t nb = (uint32_t)(v >> (i * 4)) & 0xF;
        if (nb || started || i == 0) { o[n++] = "0123456789ABCDEF"[nb]; started = 1; }
    }
    o[n] = 0;
    scat(d, cap, o);
}

/* ── отрисовка ───────────────────────────────────────────────────────── */
static void draw_pill(struct disp_state *d, int x, int y, int w, int h, uint32_t col, const char *t) {
    disp_rounded_rect(d, x, y, w, h, 12, col, 255);
    int tw = disp_text_width(t, SC);
    disp_draw_text(d, t, x + (w - tw) / 2, y + (h - 16 * SC) / 2, SC, C_PILLTXT);
}

static int entry_h(const struct ent *e) { return (e->kind == E_SVC ? SVC_H : NOTE_H) + GAP; }

static void draw_all(void) {
    struct disp_state *d = g_ds;
    int w = (int)d->width, h = (int)d->height;
    char buf[112];

    disp_clear(d, C_BG);

    /* шапка */
    int lw = disp_text_width("Nexus", 6);
    disp_draw_text(d, "Nexus", M, TOP, 6, C_ACCENT);
    disp_draw_text(d, "OS", M + lw, TOP, 6, C_INDIGO);
    disp_draw_text(d, "v" NEXUS_VERSION_STRING, M, TOP + 104, SC, C_TEXT);
    disp_draw_text(d, "Pixel 6 Pro  |  Google Tensor gs101  |  aarch64", M, TOP + 148, SC, C_DIM);
    disp_fill_rect(d, M, TOP + 210, w - 2 * M, 3, C_LINE);

    /* прогресс */
    int py = TOP + 250;
    int pw = w - 2 * M;
    int shown = g_svc > EXPECTED_SVC ? EXPECTED_SVC : g_svc;
    disp_rounded_rect(d, M, py, pw, 14, 7, C_TRACK, 255);
    int fw = g_done ? pw : pw * shown / EXPECTED_SVC;
    if (fw > 14) disp_rounded_rect(d, M, py, fw, 14, 7, C_ACCENT, 255);

    buf[0] = 0;
    scat(buf, sizeof buf, g_done ? "Boot complete  " : "Starting services  ");
    sdec(buf, sizeof buf, (uint32_t)g_svc);
    if (!g_done) { scat(buf, sizeof buf, " / "); sdec(buf, sizeof buf, EXPECTED_SVC); }
    disp_draw_text(d, buf, M, py + 30, SC, C_DIM);

    /* список: рисуем только то, что влезает; старые строки уходят вверх */
    int y0 = py + 90;
    int avail = h - FOOT_H - y0;
    int first = g_n, used = 0;
    while (first > 0 && used + entry_h(&g_e[first - 1]) <= avail) {
        first--; used += entry_h(&g_e[first]);
    }

    int y = y0;
    for (int i = first; i < g_n; i++) {
        const struct ent *e = &g_e[i];
        if (e->kind == E_SVC) {
            disp_rounded_rect(d, M, y, w - 2 * M, SVC_H, 16, C_CARD, 255);
            draw_pill(d, M + 12, y + 8, 96, 40, e->ok ? C_OK : C_FAIL, e->ok ? "OK" : "FAIL");

            int vw = disp_text_width(e->ver, SC);
            int nx = M + 12 + 96 + 20;
            int maxc = (w - 2 * M - 12 - 96 - 20 - vw - 48) / CHW;
            char nm[96]; scopy(nm, sizeof nm, e->text);
            if (slen(nm) > maxc && maxc > 3) { nm[maxc - 2] = '.'; nm[maxc - 1] = '.'; nm[maxc] = 0; }
            disp_draw_text(d, nm, nx, y + (SVC_H - 16 * SC) / 2, SC, e->ok ? C_TEXT : 0xFFB4C0);
            disp_draw_text(d, e->ver, w - M - 20 - vw, y + (SVC_H - 16 * SC) / 2, SC, C_DIM);
        } else {
            const char *t = e->text;
            while (*t == ' ') t++;
            int maxc = (w - 2 * M - 40 - 24) / CHW;
            char nt[100]; nt[0] = 0;
            scat(nt, sizeof nt, "- ");
            scat(nt, sizeof nt, t);
            if (slen(nt) > maxc && maxc > 3) { nt[maxc - 2] = '.'; nt[maxc - 1] = '.'; nt[maxc] = 0; }
            disp_draw_text(d, nt, M + 40, y + (NOTE_H - 16 * SC) / 2, SC, C_DIM);
        }
        y += entry_h(e);
    }

    /* подвал: сводка */
    if (g_done) {
        int fy = h - FOOT_H + 40;
        buf[0] = 0;
        sdec(buf, sizeof buf, (uint32_t)g_okc); scat(buf, sizeof buf, " OK");
        if (g_failc) { scat(buf, sizeof buf, "   "); sdec(buf, sizeof buf, (uint32_t)g_failc); scat(buf, sizeof buf, " FAILED"); }
        int tw = disp_text_width(buf, 3);
        disp_rounded_rect(d, (w - tw - 64) / 2, fy, tw + 64, 72, 36, C_CARD, 255);
        disp_rect_outline(d, (w - tw - 64) / 2, fy, tw + 64, 72, 3, g_failc ? C_FAIL : C_OK);
        disp_draw_text(d, buf, (w - tw) / 2, fy + 12, 3, g_failc ? 0xFFB4C0 : C_OK);
        disp_draw_text_center(d, "Starting desktop...", fy + 110, SC, C_DIM);
    }
}

static void refresh(void) {
    if (!g_ds) return;
    draw_all();
    disp_present(g_ds);
}

static struct ent *add(int kind, const char *text) {
    if (g_n >= BOOT_MAX) {
        memmove(&g_e[0], &g_e[1], sizeof(struct ent) * (BOOT_MAX - 1));
        g_n = BOOT_MAX - 1;
    }
    struct ent *e = &g_e[g_n++];
    e->kind = (uint8_t)kind; e->ok = 1; e->ver[0] = 0;
    scopy(e->text, sizeof e->text, text);
    return e;
}

/* ── API ─────────────────────────────────────────────────────────────── */
void boot_svc(const char *name, const char *ver, int ok) {
    struct ent *e = add(E_SVC, name);
    e->ok = ok ? 1 : 0;
    scopy(e->ver, sizeof e->ver, ver ? ver : "");
    g_svc++;
    if (ok) g_okc++; else g_failc++;

    ramoops_puts(ok ? "[boot] OK   " : "[boot] FAIL ");
    ramoops_puts(name); ramoops_puts(" "); ramoops_puts(ver ? ver : ""); ramoops_puts("\n");
    refresh();
}

void boot_note(const char *s) {
    add(E_NOTE, s);
    ramoops_puts("[boot] "); ramoops_puts(s); ramoops_puts("\n");
    refresh();
}

void boot_note_cat(const char *a, const char *b) {
    char t[96]; scopy(t, sizeof t, a); scat(t, sizeof t, b);
    boot_note(t);
}

void boot_note_hex(const char *a, uint64_t v) {
    char t[96]; scopy(t, sizeof t, a); shex(t, sizeof t, v);
    boot_note(t);
}

void boot_note_dec2(const char *a, uint32_t x, const char *mid, uint32_t y) {
    char t[96]; scopy(t, sizeof t, a);
    sdec(t, sizeof t, x); scat(t, sizeof t, mid); sdec(t, sizeof t, y);
    boot_note(t);
}

void boot_console_start(struct disp_state *ds) {
    g_ds = ds;
    g_done = 0;
    refresh();
}

void boot_console_finish(void) {
    g_done = 1;
    refresh();
}