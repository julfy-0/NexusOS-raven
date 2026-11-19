#include "a64.h"

void ramoops_flush(void) { __asm__ volatile("dsb sy" ::: "memory"); }
