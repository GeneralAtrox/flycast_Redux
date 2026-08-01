#include "research/pvr_framebuffer.h"

#include <gtest/gtest.h>

#include <array>

namespace
{

research::PvrFramebufferConfig configForDepth(std::uint32_t depth,
		std::uint32_t concat = 0)
{
	research::PvrFramebufferConfig config;
	config.fbReadControl = (depth << 2) | (concat << 4);
	return config;
}

} // namespace

TEST(ResearchPvrFramebuffer, Decodes0555And565Exactly)
{
	const std::array<std::uint8_t, 4> raw0555 {{0x22, 0x7c, 0x00, 0x00}};
	const auto decoded0555 = research::decodePvrFramebuffer(
			configForDepth(0, 3), 2, 1, 4, raw0555.data(), raw0555.size());
	ASSERT_EQ(8u, decoded0555.rgba.size());
	EXPECT_EQ((std::array<std::uint8_t, 4> {251, 11, 19, 255}),
			(std::array<std::uint8_t, 4> {decoded0555.rgba[0], decoded0555.rgba[1],
					decoded0555.rgba[2], decoded0555.rgba[3]}));

	const std::array<std::uint8_t, 2> raw565 {{0xe0, 0x07}};
	const auto decoded565 = research::decodePvrFramebuffer(
			configForDepth(1), 1, 1, 2, raw565.data(), raw565.size());
	EXPECT_EQ((std::vector<std::uint8_t> {0, 252, 0, 255}), decoded565.rgba);
}

TEST(ResearchPvrFramebuffer, DecodesPacked888AndC888Exactly)
{
	const std::array<std::uint8_t, 6> raw888 {{3, 2, 1, 6, 5, 4}};
	const auto decoded888 = research::decodePvrFramebuffer(
			configForDepth(2), 2, 1, 6, raw888.data(), raw888.size());
	EXPECT_EQ((std::vector<std::uint8_t> {
			1, 2, 3, 255, 4, 5, 6, 255}), decoded888.rgba);

	const std::array<std::uint8_t, 4> rawC888 {{9, 8, 7, 0xaa}};
	const auto decodedC888 = research::decodePvrFramebuffer(
			configForDepth(3), 1, 1, 4, rawC888.data(), rawC888.size());
	EXPECT_EQ((std::vector<std::uint8_t> {7, 8, 9, 255}), decodedC888.rgba);
}

TEST(ResearchPvrFramebuffer, PreservesPresentedRgbExactly)
{
	const std::array<std::uint8_t, 6> rgb {{1, 2, 3, 4, 5, 6}};
	const auto decoded = research::decodePvrFramebuffer({}, 2, 1, 6,
			rgb.data(), rgb.size(), research::PvrFramebufferKind::PresentedRgb24);
	EXPECT_EQ((std::vector<std::uint8_t> {
			1, 2, 3, 255, 4, 5, 6, 255}), decoded.rgba);
	EXPECT_THROW(research::decodePvrFramebuffer({}, 2, 1, 5, rgb.data(), 5,
			research::PvrFramebufferKind::PresentedRgb24), std::runtime_error);
}

TEST(ResearchPvrFramebuffer, ExactDifferenceReportsOneChangedPixel)
{
	research::DecodedPvrFramebuffer reference;
	reference.width = 2;
	reference.height = 1;
	reference.rgba = {1, 2, 3, 255, 4, 5, 6, 255};
	auto candidate = reference;
	candidate.rgba[5] = 9;

	const auto difference = research::comparePvrFramebuffersExact(reference,
			candidate);
	EXPECT_FALSE(difference.exact);
	EXPECT_EQ(2u, difference.comparedPixels);
	EXPECT_EQ(1u, difference.differingPixels);
	EXPECT_EQ(4u, difference.maximumChannelDifference);
	EXPECT_EQ(4u, difference.absoluteChannelDifference);
	EXPECT_EQ(1u, difference.minimumX);
	EXPECT_EQ(0u, difference.minimumY);
	EXPECT_EQ(1u, difference.maximumX);
	EXPECT_EQ(0u, difference.maximumY);
}

TEST(ResearchPvrFramebuffer, RejectsImplicitResizeOrMalformedRows)
{
	const std::array<std::uint8_t, 4> raw {{0, 0, 0, 0}};
	EXPECT_THROW(research::decodePvrFramebuffer(configForDepth(1), 1, 1, 4,
			raw.data(), raw.size()), std::runtime_error);

	research::DecodedPvrFramebuffer lhs {1, 1, {0, 0, 0, 255}};
	research::DecodedPvrFramebuffer rhs {2, 1, {0, 0, 0, 255, 0, 0, 0, 255}};
	EXPECT_THROW(research::comparePvrFramebuffersExact(lhs, rhs),
			std::invalid_argument);
}
