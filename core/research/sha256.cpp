#include "research/sha256.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace research
{
namespace
{

constexpr std::array<std::uint32_t, 64> RoundConstants {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotateRight(std::uint32_t value, unsigned amount)
{
	return (value >> amount) | (value << (32 - amount));
}

constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return (x & y) ^ (~x & z);
}

constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t bigSigma0(std::uint32_t value)
{
	return rotateRight(value, 2) ^ rotateRight(value, 13) ^ rotateRight(value, 22);
}

constexpr std::uint32_t bigSigma1(std::uint32_t value)
{
	return rotateRight(value, 6) ^ rotateRight(value, 11) ^ rotateRight(value, 25);
}

constexpr std::uint32_t smallSigma0(std::uint32_t value)
{
	return rotateRight(value, 7) ^ rotateRight(value, 18) ^ (value >> 3);
}

constexpr std::uint32_t smallSigma1(std::uint32_t value)
{
	return rotateRight(value, 17) ^ rotateRight(value, 19) ^ (value >> 10);
}

int hexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

} // namespace

Sha256::Sha256()
	: state {
		0x6a09e667,
		0xbb67ae85,
		0x3c6ef372,
		0xa54ff53a,
		0x510e527f,
		0x9b05688c,
		0x1f83d9ab,
		0x5be0cd19,
	}
{
}

void Sha256::update(const void *data, std::size_t size)
{
	if (finalized)
		throw std::logic_error("SHA-256 update after finalize");
	if (size == 0)
		return;
	if (data == nullptr)
		throw std::invalid_argument("SHA-256 data is null");
	if (size > UINT64_MAX - totalBytes)
		throw std::overflow_error("SHA-256 input is too large");

	const auto *bytes = static_cast<const std::uint8_t *>(data);
	totalBytes += size;

	if (bufferSize != 0)
	{
		const std::size_t copied = std::min(size, buffer.size() - bufferSize);
		std::memcpy(buffer.data() + bufferSize, bytes, copied);
		bufferSize += copied;
		bytes += copied;
		size -= copied;
		if (bufferSize == buffer.size())
		{
			transform(buffer.data());
			bufferSize = 0;
		}
	}

	while (size >= buffer.size())
	{
		transform(bytes);
		bytes += buffer.size();
		size -= buffer.size();
	}

	if (size != 0)
	{
		std::memcpy(buffer.data(), bytes, size);
		bufferSize = size;
	}
}

Sha256Digest Sha256::finalize()
{
	if (finalized)
		throw std::logic_error("SHA-256 finalized more than once");
	finalized = true;

	const std::uint64_t totalBits = totalBytes * 8;
	buffer[bufferSize++] = 0x80;
	if (bufferSize > 56)
	{
		std::fill(buffer.begin() + bufferSize, buffer.end(), std::uint8_t {0});
		transform(buffer.data());
		bufferSize = 0;
	}
	std::fill(buffer.begin() + bufferSize, buffer.begin() + 56, std::uint8_t {0});
	for (unsigned i = 0; i < 8; ++i)
		buffer[63 - i] = static_cast<std::uint8_t>(totalBits >> (i * 8));
	transform(buffer.data());

	Sha256Digest digest {};
	for (std::size_t i = 0; i < state.size(); ++i)
	{
		digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
		digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
		digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
		digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
	}
	return digest;
}

void Sha256::transform(const std::uint8_t block[64])
{
	std::array<std::uint32_t, 64> words {};
	for (std::size_t i = 0; i < 16; ++i)
	{
		const std::size_t offset = i * 4;
		words[i] = (static_cast<std::uint32_t>(block[offset]) << 24)
				| (static_cast<std::uint32_t>(block[offset + 1]) << 16)
				| (static_cast<std::uint32_t>(block[offset + 2]) << 8)
				| static_cast<std::uint32_t>(block[offset + 3]);
	}
	for (std::size_t i = 16; i < words.size(); ++i)
		words[i] = smallSigma1(words[i - 2]) + words[i - 7]
				+ smallSigma0(words[i - 15]) + words[i - 16];

	std::uint32_t a = state[0];
	std::uint32_t b = state[1];
	std::uint32_t c = state[2];
	std::uint32_t d = state[3];
	std::uint32_t e = state[4];
	std::uint32_t f = state[5];
	std::uint32_t g = state[6];
	std::uint32_t h = state[7];

	for (std::size_t i = 0; i < words.size(); ++i)
	{
		const std::uint32_t temp1 = h + bigSigma1(e) + choose(e, f, g)
				+ RoundConstants[i] + words[i];
		const std::uint32_t temp2 = bigSigma0(a) + majority(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

Sha256Digest sha256(const void *data, std::size_t size)
{
	Sha256 hasher;
	hasher.update(data, size);
	return hasher.finalize();
}

std::string sha256ToHex(const Sha256Digest& digest)
{
	constexpr char Hex[] = "0123456789abcdef";
	std::string result(digest.size() * 2, '0');
	for (std::size_t i = 0; i < digest.size(); ++i)
	{
		result[i * 2] = Hex[digest[i] >> 4];
		result[i * 2 + 1] = Hex[digest[i] & 0x0f];
	}
	return result;
}

bool sha256FromHex(const std::string& text, Sha256Digest& digest)
{
	if (text.size() != digest.size() * 2)
		return false;
	Sha256Digest parsed {};
	for (std::size_t i = 0; i < parsed.size(); ++i)
	{
		const int high = hexValue(text[i * 2]);
		const int low = hexValue(text[i * 2 + 1]);
		if (high < 0 || low < 0)
			return false;
		parsed[i] = static_cast<std::uint8_t>((high << 4) | low);
	}
	digest = parsed;
	return true;
}

bool sha256Equal(const Sha256Digest& lhs, const Sha256Digest& rhs)
{
	std::uint8_t difference = 0;
	for (std::size_t i = 0; i < lhs.size(); ++i)
		difference |= lhs[i] ^ rhs[i];
	return difference == 0;
}

} // namespace research
