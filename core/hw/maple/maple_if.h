#pragma once
#include "maple_devs.h"
#include <memory>

extern std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

void maple_Init();
void maple_Reset(bool Manual);
void maple_Term();
void maple_ReconnectDevices();
void maple_ReconnectDevice(int bus, int port);

void maple_vblank();

#ifdef FLYCAST_TEST_FILES
namespace research_test
{
using MapleDmaTickObserver = void (*)(u64 rawTick, u64 executionTick,
		u32 ownerPc, u16 ownerOpcode, bool ownerValid);
void setMapleDmaTickObserver(MapleDmaTickObserver observer);
}
#endif
