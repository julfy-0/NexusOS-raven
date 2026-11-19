#include "a64.h"
#include "wdt.h"
#include "gs101_disp.h"
#include "disp_draw.h"

static struct disp_state *g_exc_ds;
static int g_exc_busy;

void exc_set_display(struct disp_state *s) { g_exc_ds = s; }

static const char *vec_name(uint64_t type) {
    static const char *grp[4] = { "cur EL SP0", "cur EL SPx", "lower EL a64", "lower EL a32" };
    (void)grp;
    switch (type & 3) {
    case 0: return "SYNC";
    case 1: return "IRQ";
    case 2: return "FIQ";
    default: return "SERROR";
    }
}

static const char *grp_name(uint64_t type) {
    switch ((type >> 2) & 3) {
    case 0: return "cur EL SP0";
    case 1: return "cur EL SPx";
    case 2: return "lower EL a64";
    default: return "lower EL a32";
    }
}

static const char *ec_name(uint64_t ec) {
    switch (ec) {
    case 0x15: return "SVC a64";
    case 0x20: return "instr abort lower EL";
    case 0x21: return "instr abort same EL";
    case 0x22: return "PC align";
    case 0x24: return "data abort lower EL";
    case 0x25: return "data abort same EL";
    case 0x26: return "SP align";
    case 0x2F: return "SError";
    default:   return "other";
    }
}

static void line(struct disp_state *s, int y, const char *lbl, uint64_t v) {
    int x = 40, sc = 3;
    disp_draw_text(s, lbl, x, y, sc, 0xFFFFFF);
    disp_draw_hex(s, v, x + disp_text_width(lbl, sc), y, sc, 0xFFFF00);
}

void exc_report(uint64_t type, uint64_t esr, uint64_t elr, uint64_t far) {
    ramoops_puts("\n[nexus] EXCEPTION vec="); ramoops_put_hex(type);
    ramoops_puts(" esr=");  ramoops_put_hex(esr);
    ramoops_puts(" elr=");  ramoops_put_hex(elr);
    ramoops_puts(" far=");  ramoops_put_hex(far);
    ramoops_puts("\n");
    ramoops_flush();

    /* Защита от повторного исключения внутри самого вывода на экран */
    if (g_exc_ds && g_exc_ds->fb && !g_exc_busy) {
        struct disp_state *s = g_exc_ds;
        uint64_t ec = (esr >> 26) & 0x3F;
        g_exc_busy = 1;

        disp_fill_rect(s, 0, 0, (int)s->width, (int)s->height, 0x7F0000);
        disp_draw_text(s, "NEXUS EXCEPTION", 40, 120, 4, 0xFFFFFF);
        disp_draw_text(s, vec_name(type), 40, 220, 3, 0xFFFFFF);
        disp_draw_text(s, grp_name(type), 40 + disp_text_width(vec_name(type), 3) + 24, 220, 3, 0xFFFFFF);
        line(s, 300, "VEC=", type);
        line(s, 360, "ESR=", esr);
        line(s, 420, "EC =", ec);
        disp_draw_text(s, ec_name(ec), 40, 480, 3, 0xFFFFFF);
        line(s, 560, "ELR=", elr);
        line(s, 620, "FAR=", far);
        disp_draw_text(s, "halted, no reboot", 40, 720, 3, 0xFFFFFF);
        disp_present(s);
    }

    ramoops_puts("[nexus] EXCEPTION: halted without reboot\n");
    ramoops_flush();
    for (;;) {
        wdt_pet();
        delay_ms(1000);
    }
}