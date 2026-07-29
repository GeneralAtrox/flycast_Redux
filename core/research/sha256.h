#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace research
{

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256
{
public:
	Sha256();

	void update(const void *data, std::size_t size);
	Sha256Digest finalize();

private:
	void transform(const std::uint8_t block[64]);

	std::array<std::uint32_t, 8> state {};
	std::array<std::uint8_t, 64> buffer {};
	std::uint64_t totalBytes = 0;
	std::size_t bufferSize = 0;
	bool finalized = false;
};

Sha256Digest sha256(const void *data, std::size_t size);
std::string sha256ToHex(const Sha256Digest& digest);
bool sha256FromHex(const std::string& text, Sha256Digest& digest);
bool sha256Equal(const Sha256Digest& lhs, const Sha256Digest& rhs);

} // namespace research
