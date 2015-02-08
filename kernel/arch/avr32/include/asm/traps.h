
#ifndef __ASM_AVR32_TRAPS_H
#define __ASM_AVR32_TRAPS_H

#include <linux/list.h>

struct undef_hook {
	struct list_head node;
	u32 insn_mask;
	u32 insn_val;
	int (*fn)(struct pt_regs *regs, u32 insn);
};

void register_undef_hook(struct undef_hook *hook);
void unregister_undef_hook(struct undef_hook *hook);

#endif /* __ASM_AVR32_TRAPS_H */
