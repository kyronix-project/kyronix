#include "kallsyms.h"

/*
 * A first-pass kernel only needs definitions that satisfy kallsyms.c.  The
 * build then extracts global symbols from that image and links the real table.
 */
const sym_entry_t kallsyms_table[] = {};
const int kallsyms_num = 0;
