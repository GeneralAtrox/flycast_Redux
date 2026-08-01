#pragma once

#include "research/pvr_presentation_observation.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace research
{

struct DecodedPvrFramebuffer
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> rgba;
};

struct ExactPvrFrameDifference
{
	bool exact = false;
	std::uint64_t comparedPixels = 0;
	std::uint64_t differingPixels = 0;
	std::uint8_t maximumChannelDifference = 0;
	std::uint64_t absoluteChannelDifference = 0;
	std::uint32_t minimumX = UINT32_MAX;
	std::uint32_t minimumY = UINT32_MAX;
	std::uint32_t maximumX = UINT32_MAX;
	std::uint32_t maximumY = UINT32_MAX;
};

DecodedPvrFramebuffer decodePvrFramebuffer(
		const PvrFramebufferConfig& config, std::uint32_t capturedWidth,
		std::uint32_t capturedHeight, std::uint32_t rowBytes,
		const std::uint8_t* bytes, std::size_t size,
		PvrFramebufferKind kind = PvrFramebufferKind::DreamcastVram);

ExactPvrFrameDifference comparePvrFramebuffersExact(
		const DecodedPvrFramebuffer& reference,
		const DecodedPvrFramebuffer& candidate);

} // namespace research
