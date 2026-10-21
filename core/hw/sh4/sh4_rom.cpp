#include "types.h"
#include "sh4_rom.h"
#include "fsca-rom-table.h"

f32_x2 fsca_tables[2][0x10000];
f32_x2 *fsca_cur = fsca_tables[1]; // FPSCR resets to 0x00040001: RM = 1, round toward zero

void fsca_select(u32 rm)
{
	// RM 2 and 3 are reserved; they round to nearest here, as setHostRoundingMode rounds them
	fsca_cur = fsca_tables[rm == 1 ? 1 : 0];
}

static void sh4rom_init()
{
	const u32 *sines[2] = { fsca_rom_sin_rn, fsca_rom_sin_rz };
	for (int t = 0; t < 2; t++)
	{
		for (int i = 0x0000; i < 0x10000; i++)
		{
			reinterpret_cast<u32&>(fsca_tables[t][i].u[0]) = sines[t][i];
			reinterpret_cast<u32&>(fsca_tables[t][i].u[1]) = sines[t][(i + 0x4000) & 0xFFFF]; // cos: a quarter turn on
		}
		// +0 at the zero crossings, as the hardware gives it (Ikaruga's bullets need sin(pi) == 0)
		verify(reinterpret_cast<u32&>(fsca_tables[t][0x8000].u[0]) == 0);
	}
}

static OnLoad ol_sh4rom(&sh4rom_init);
