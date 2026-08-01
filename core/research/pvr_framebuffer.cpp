#include "research/pvr_framebuffer.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace research
{
namespace
{

constexpr std::uint32_t FbDepthShift = 2;
constexpr std::uint32_t FbDepthMask = 0x3;
constexpr std::uint32_t FbConcatShift = 4;
constexpr std::uint32_t FbConcatMask = 0x7;

[[noreturn]] void invalid(const char *reason)
{
	throw std::runtime_error(std::string("invalid raw PowerVR framebuffer: ") + reason);
}

void appendPixel(std::vector<std::uint8_t>& rgba, std::uint8_t red,
		std::uint8_t green, std::uint8_t blue)
{
	rgba.push_back(red);
	rgba.push_back(green);
	rgba.push_back(blue);
	rgba.push_back(0xff);
}

} // namespace

DecodedPvrFramebuffer decodePvrFramebuffer(
		const PvrFramebufferConfig& config, std::uint32_t capturedWidth,
		std::uint32_t capturedHeight, std::uint32_t rowBytes,
		const std::uint8_t* bytes, std::size_t size, PvrFramebufferKind kind)
{
	if (bytes == nullptr || capturedWidth == 0 || capturedHeight == 0
			|| rowBytes == 0)
		invalid("empty capture");
	if (static_cast<std::uint64_t>(rowBytes) * capturedHeight != size)
		invalid("byte count differs from rows");
	if (static_cast<std::uint64_t>(capturedWidth) * capturedHeight
			> std::numeric_limits<std::size_t>::max() / 4)
		invalid("decoded size overflows");
	if (kind == PvrFramebufferKind::PresentedRgb24)
	{
		if (static_cast<std::uint64_t>(capturedWidth) * 3 != rowBytes)
			invalid("presented RGB row size is invalid");
		DecodedPvrFramebuffer result;
		result.width = capturedWidth;
		result.height = capturedHeight;
		result.rgba.reserve(static_cast<std::size_t>(capturedWidth)
				* capturedHeight * 4);
		for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(capturedWidth)
				* capturedHeight; ++pixel)
			appendPixel(result.rgba, bytes[pixel * 3], bytes[pixel * 3 + 1],
					bytes[pixel * 3 + 2]);
		return result;
	}
	if (kind != PvrFramebufferKind::DreamcastVram)
		invalid("framebuffer kind is invalid");

	const std::uint32_t depth =
			(config.fbReadControl >> FbDepthShift) & FbDepthMask;
	const std::uint32_t bytesPerPixel = depth <= 1 ? 2 : depth == 2 ? 3 : 4;
	if (static_cast<std::uint64_t>(capturedWidth) * bytesPerPixel != rowBytes)
		invalid("row byte count differs from format and width");
	const std::uint8_t concat = static_cast<std::uint8_t>(
			(config.fbReadControl >> FbConcatShift) & FbConcatMask);

	DecodedPvrFramebuffer result;
	result.width = capturedWidth;
	result.height = capturedHeight;
	result.rgba.reserve(static_cast<std::size_t>(capturedWidth)
			* capturedHeight * 4);
	for (std::uint32_t y = 0; y < capturedHeight; ++y)
	{
		const std::uint8_t* row = bytes + static_cast<std::size_t>(y) * rowBytes;
		for (std::uint32_t x = 0; x < capturedWidth; ++x)
		{
			const std::uint8_t* pixel = row + x * bytesPerPixel;
			switch (depth)
			{
			case 0:
				{
					const std::uint16_t value = static_cast<std::uint16_t>(
							pixel[0] | (pixel[1] << 8));
					appendPixel(result.rgba,
							static_cast<std::uint8_t>((((value >> 10) & 0x1f) << 3) | concat),
							static_cast<std::uint8_t>((((value >> 5) & 0x1f) << 3) | concat),
							static_cast<std::uint8_t>(((value & 0x1f) << 3) | concat));
				}
				break;
			case 1:
				{
					const std::uint16_t value = static_cast<std::uint16_t>(
							pixel[0] | (pixel[1] << 8));
					appendPixel(result.rgba,
							static_cast<std::uint8_t>((((value >> 11) & 0x1f) << 3) | concat),
							static_cast<std::uint8_t>((((value >> 5) & 0x3f) << 2) | (concat & 3)),
							static_cast<std::uint8_t>(((value & 0x1f) << 3) | concat));
				}
				break;
			case 2:
				appendPixel(result.rgba, pixel[2], pixel[1], pixel[0]);
				break;
			case 3:
				appendPixel(result.rgba, pixel[2], pixel[1], pixel[0]);
				break;
			default:
				invalid("unsupported depth");
			}
		}
	}
	return result;
}

ExactPvrFrameDifference comparePvrFramebuffersExact(
		const DecodedPvrFramebuffer& reference,
		const DecodedPvrFramebuffer& candidate)
{
	if (reference.width == 0 || reference.height == 0
			|| reference.width != candidate.width
			|| reference.height != candidate.height)
		throw std::invalid_argument("PowerVR frame dimensions differ");
	const std::size_t expected = static_cast<std::size_t>(reference.width)
			* reference.height * 4;
	if (reference.rgba.size() != expected || candidate.rgba.size() != expected)
		throw std::invalid_argument("PowerVR RGBA byte count is invalid");

	ExactPvrFrameDifference result;
	result.comparedPixels = static_cast<std::uint64_t>(reference.width)
			* reference.height;
	for (std::uint32_t y = 0; y < reference.height; ++y)
	{
		for (std::uint32_t x = 0; x < reference.width; ++x)
		{
			const std::size_t offset = (static_cast<std::size_t>(y)
					* reference.width + x) * 4;
			bool pixelDiffers = false;
			for (std::size_t channel = 0; channel < 4; ++channel)
			{
				const unsigned difference = static_cast<unsigned>(std::abs(
						static_cast<int>(reference.rgba[offset + channel])
						- static_cast<int>(candidate.rgba[offset + channel])));
				result.maximumChannelDifference = std::max(
						result.maximumChannelDifference,
						static_cast<std::uint8_t>(difference));
				result.absoluteChannelDifference += difference;
				pixelDiffers = pixelDiffers || difference != 0;
			}
			if (pixelDiffers)
			{
				++result.differingPixels;
				if (result.minimumX == UINT32_MAX)
				{
					result.minimumX = result.maximumX = x;
					result.minimumY = result.maximumY = y;
				}
				else
				{
					result.minimumX = std::min(result.minimumX, x);
					result.minimumY = std::min(result.minimumY, y);
					result.maximumX = std::max(result.maximumX, x);
					result.maximumY = std::max(result.maximumY, y);
				}
			}
		}
	}
	result.exact = result.differingPixels == 0;
	return result;
}

} // namespace research
