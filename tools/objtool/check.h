/*
 * Copyright (C) 2017 Josh Poimboeuf <jpoimboe@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _CHECK_H
#define _CHECK_H

#include <stdbool.h>
#include "elf.h"
#include "cfi.h"
#include "arch.h"
#include "orc.h"
#include <linux/hashtable.h>

struct insn_state {
	struct cfi_reg cfa;
	struct cfi_reg regs[CFI_NUM_REGS];
	int stack_size;
	unsigned char type;
	bool bp_scratch;
	bool drap, end, uaccess, df;
	unsigned int uaccess_stack;
	int drap_reg, drap_offset;
	struct cfi_reg vals[CFI_NUM_REGS];
};

struct instruction {
	struct list_head list;
	struct hlist_node hash;
	struct list_head call_node;
	struct section *sec;
	unsigned long offset;
	unsigned int len;
	enum insn_type type;
	unsigned long immediate;
	bool alt_group, visited, dead_end, ignore, hint, save, restore, ignore_alts;
	bool retpoline_safe;
	struct symbol *call_dest;
	struct instruction *jump_dest;
	struct instruction *first_jump_src;
	struct rela *jump_table;
	struct list_head alts;
	struct symbol *func;
	struct list_head stack_ops;
	struct insn_state state;
	struct orc_entry orc;
};

struct objtool_file {
	struct elf *elf;
	struct list_head insn_list;
	DECLARE_HASHTABLE(insn_hash, 16);
	struct list_head return_thunk_list;
	bool ignore_unreachables, c_file, hints, rodata;
};

int check(const char *objname, bool orc);

struct instruction *find_insn(struct objtool_file *file,
			      struct section *sec, unsigned long offset);

#define for_each_insn(file, insn)					\
	list_for_each_entry(insn, &file->insn_list, list)

#define sec_for_each_insn(file, sec, insn)				\
	for (insn = find_insn(file, sec, 0);				\
	     insn && &insn->list != &file->insn_list &&			\
			insn->sec == sec;				\
	     insn = list_next_entry(insn, list))


#endif /* _CHECK_H */
