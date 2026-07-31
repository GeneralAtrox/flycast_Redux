#include "types.h"
#include "debug/gdb_server.h"
#include "gtest/gtest.h"

TEST(GdbServerPolicyTest, AllowsOnlyReadAndSnapshotLifecycleCommands)
{
	const char interrupt[] = { 3, 0 };
	for (const char *packet : {
			"!", "?", "c", "D", "g", "Hc0", "m8c000000,20", "p10",
			"qSupported", "qRcmd,737461636b", "T0", "vCont?", "vCont;c",
			"vMustReplyEmpty", interrupt })
		EXPECT_TRUE(debugger::isReadOnlyCommandAllowed(packet)) << packet;

	for (const char *packet : {
			"", "C05", "G00", "M8c000000,1:ff", "P0=00000000",
			"R", "s", "S05", "X8c000000,1:x", "Z0,8c000000,2",
			"z0,8c000000,2", "k", "QStartNoAckMode", "c8c010000",
			"qRcmd,7265736574", "vCont;s", "vCont;r8c010000,8c010010",
			"vKill", "vRun;" })
		EXPECT_FALSE(debugger::isReadOnlyCommandAllowed(packet)) << packet;
}
