/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <string_view>

namespace debugger {

// exception thrown in response to trap
struct Stop { };

	static const int DEFAULT_PORT = 3263;

	// The research GDB endpoint is intentionally limited to coherent read-only
	// snapshots plus the stop/resume lifecycle needed to take them.
	inline bool isReadOnlyCommandAllowed(std::string_view packet)
	{
		if (packet.empty())
			return false;
		if (packet.size() == 1)
		{
			switch (packet.front())
			{
			case '\x03': // interrupt
			case '!':    // extended-mode negotiation
			case '?':    // halt reason
			case 'c':    // resume without changing PC
			case 'D':    // detach and resume
			case 'g':    // read all registers
				return true;
			default:
				break;
			}
		}
		if (packet.front() == 'H' || packet.front() == 'm'
				|| packet.front() == 'p' || packet.front() == 'T')
			return true;
		if (packet.front() == 'q')
		{
			constexpr std::string_view remoteCommand = "qRcmd,";
			if (packet.substr(0, remoteCommand.size()) != remoteCommand)
				return true;
			// Hex-encoded "stack" is the only read-only monitor command.
			return packet == "qRcmd,737461636b";
		}
		return packet == "vCont?" || packet == "vCont;c"
				|| packet == "vMustReplyEmpty";
	}

#ifdef GDB_SERVER

	void init(int port);
	void term();
	void run();
	void debugTrap(u32 event);
	void subroutineCall();
	void subroutineReturn();

#else
	static inline void init(int port) {}
	static inline void term() {}
	static inline void run() {}
	static inline void debugTrap(u32 event) {}
	static inline void subroutineCall() {}
	static inline void subroutineReturn() {}
#endif
}
