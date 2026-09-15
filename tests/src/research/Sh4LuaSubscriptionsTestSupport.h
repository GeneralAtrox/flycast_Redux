#pragma once

#include "research/sh4_lua_subscriptions.h"
#include "hw/sh4/sh4_if.h"
#include "ResearchRuntimeStubs.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

class NativeSubscription
{
public:
	explicit NativeSubscription(research::Sh4ObservationSubscription token = 0)
		: token(token)
	{
	}

	~NativeSubscription()
	{
		if (token != 0)
			research::unsubscribeSh4Observations(token);
	}

	NativeSubscription(const NativeSubscription&) = delete;
	NativeSubscription& operator=(const NativeSubscription&) = delete;

private:
	research::Sh4ObservationSubscription token;
};

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-sh4-lua-subscription-test-"
						+ std::to_string(sequence.fetch_add(1)));
		std::error_code error;
		std::filesystem::remove_all(path, error);
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

inline research::Sh4Observation instruction(std::uint32_t pc)
{
	research::Sh4Observation observation;
	observation.backend = research::Sh4ObservationBackend::Interpreter;
	observation.type = research::Sh4ObservationType::InstructionEnd;
	observation.tick = pc;
	observation.instructionPc = pc;
	observation.opcode = 0x0009;
	return observation;
}

inline research::Sh4Observation memory(research::Sh4ObservationType type,
		std::uint32_t address, std::uint8_t width, std::uint64_t value)
{
	research::Sh4Observation observation = instruction(0x8c010000);
	observation.type = type;
	observation.memoryAddress = address;
	observation.memoryWidth = width;
	observation.memoryValue = value;
	return observation;
}

inline research::Sh4ObservationFilter interpreterInstructions()
{
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(
			research::Sh4ObservationBackend::Interpreter);
	filter.typeMask = research::sh4ObservationTypeBit(
			research::Sh4ObservationType::InstructionEnd);
	return filter;
}
