#pragma once

#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/dyna/shil.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

class ObservationSubscription
{
public:
	ObservationSubscription() = default;
	explicit ObservationSubscription(research::Sh4ObservationSubscription id)
		: id(id) {}
	~ObservationSubscription()
	{
		research::unsubscribeSh4Observations(id);
	}

	ObservationSubscription(const ObservationSubscription&) = delete;
	ObservationSubscription& operator=(const ObservationSubscription&) = delete;

	research::Sh4ObservationSubscription get() const { return id; }

private:
	research::Sh4ObservationSubscription id = 0;
};

inline research::Sh4Observation memoryObservation(research::Sh4ObservationType type,
		std::uint32_t address, std::uint8_t width, std::uint64_t value)
{
	research::Sh4Observation observation;
	observation.type = type;
	observation.instructionPc = 0x8c010000;
	observation.memoryAddress = address;
	observation.memoryWidth = width;
	observation.memoryValue = value;
	return observation;
}

inline void invokeDynarecMarker(Sh4Context& context, const shil_opcode& marker)
{
	const auto function = research::sh4DynarecObservationMarkerFor(marker.op);
	ASSERT_NE(nullptr, function);
	ASSERT_LE(marker.size, 0xffffu);
	function(&context, marker.rs1._imm,
			marker.rs2._imm | (marker.size << 16), marker.rs3._imm);
}
