#pragma once
#include "types.h"
struct f32_x2
{
	f32 u[2];
};
// FSCA's {sin, cos} for each angle, as the hardware gives them: one table per FPSCR.RM (fsca-rom-table.h).
// fsca_cur is the one the current FPSCR.RM selects (fsca_select, from every FPSCR change).
extern f32_x2 fsca_tables[2][0x10000];
extern f32_x2 *fsca_cur;
void fsca_select(u32 rm);
