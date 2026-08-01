#pragma once

#include "types.h"

namespace research { struct PvrTaRenderSelectionTranscript; }

int getTAContextAddresses(u32 *addresses,
		research::PvrTaRenderSelectionTranscript *transcript = nullptr);
