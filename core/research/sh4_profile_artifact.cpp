#include "research/sh4_profile_artifact.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace research
{
namespace
{
constexpr std::array<std::uint8_t, 8> Magic {'F','C','S','H','4','P','R','1'};
constexpr std::uint32_t BlockRecordType = 1;
constexpr std::uint32_t BranchRecordType = 2;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
	for (unsigned index = 0; index < 2; ++index)
		out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
	for (unsigned index = 0; index < 8; ++index)
		out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void appendDigest(std::vector<std::uint8_t>& out, const Sha256Digest& digest)
{
	out.insert(out.end(), digest.begin(), digest.end());
}

void putU32(std::vector<std::uint8_t>& out, std::size_t offset,
		std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		out[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
	std::uint32_t crc = 0xffffffffu;
	for (std::size_t index = 0; index < size; ++index)
	{
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

std::vector<std::uint8_t> blockRecord(const Sh4DynarecBlockExecution& block)
{
	if (block.definition.guestBytes.size()
			> std::numeric_limits<std::uint32_t>::max() - 108u)
		throw std::runtime_error("SH-4 dynarec profile block bytes are too large");
	std::vector<std::uint8_t> out;
	out.reserve(108 + block.definition.guestBytes.size());
	appendU32(out, BlockRecordType);
	appendU32(out, static_cast<std::uint32_t>(108 + block.definition.guestBytes.size()));
	appendU64(out, block.generation);
	appendU32(out, block.definition.virtualAddress);
	appendU32(out, block.definition.physicalAddress);
	appendU32(out, block.definition.fpuConfiguration);
	appendU32(out, block.definition.guestCodeSize);
	appendU32(out, block.definition.guestCycles);
	appendU32(out, block.definition.guestOpcodes);
	out.push_back(static_cast<std::uint8_t>(block.definition.byteStatus));
	out.push_back(static_cast<std::uint8_t>(block.definition.branchKind));
	appendU16(out, block.definition.branchOpcode);
	appendU32(out, block.definition.branchSource);
	appendU32(out, block.definition.branchTarget);
	appendU32(out, block.definition.fallthroughTarget);
	appendU32(out, static_cast<std::uint32_t>(block.definition.guestBytes.size()));
	appendU64(out, block.enteredCount);
	appendU64(out, block.completedCount);
	appendU64(out, block.abortedCount);
	appendU64(out, block.totalCycles);
	appendU64(out, block.firstEntryTick);
	appendU64(out, block.lastExitTick);
	if (out.size() != 108)
		throw std::logic_error("SH-4 dynarec profile block layout is inconsistent");
	out.insert(out.end(), block.definition.guestBytes.begin(),
			block.definition.guestBytes.end());
	return out;
}

std::vector<std::uint8_t> branchRecord(const Sh4DynarecBranchExecution& branch)
{
	std::vector<std::uint8_t> out;
	out.reserve(64);
	appendU32(out, BranchRecordType);
	appendU32(out, 64);
	appendU64(out, branch.sourceGeneration);
	appendU32(out, branch.source);
	appendU32(out, branch.destination);
	appendU16(out, branch.opcode);
	out.push_back(static_cast<std::uint8_t>(branch.kind));
	out.push_back(branch.taken ? 1 : 0);
	appendU32(out, 0);
	appendU64(out, branch.count);
	appendU64(out, branch.firstBoundaryTick);
	appendU64(out, branch.lastBoundaryTick);
	appendU64(out, 0);
	if (out.size() != 64)
		throw std::logic_error("SH-4 dynarec profile branch layout is inconsistent");
	return out;
}

class ExclusiveFile
{
public:
	explicit ExclusiveFile(const std::filesystem::path& path)
	{
		std::error_code error;
		if (path.parent_path().empty()
				|| !std::filesystem::is_directory(path.parent_path(), error) || error)
			throw std::runtime_error("SH-4 profile output directory does not exist");
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::runtime_error("cannot create SH-4 profile output exclusively");
#else
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::runtime_error("cannot create SH-4 profile output exclusively");
#endif
	}
	~ExclusiveFile()
	{
#ifdef _WIN32
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
#else
		if (fd >= 0)
			::close(fd);
#endif
	}
	void write(const std::uint8_t* data, std::size_t size)
	{
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(size,
					std::numeric_limits<DWORD>::max()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written != chunk)
				throw std::runtime_error("cannot write SH-4 profile output");
#else
			const ssize_t written = ::write(fd, data, size);
			if (written <= 0)
				throw std::runtime_error("cannot write SH-4 profile output");
#endif
			data += written;
			size -= static_cast<std::size_t>(written);
		}
	}
	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::runtime_error("cannot flush SH-4 profile output");
#else
		if (::fsync(fd) != 0)
			throw std::runtime_error("cannot flush SH-4 profile output");
#endif
	}
private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};
}

Sh4DynarecProfileArtifactSummary writeSh4DynarecProfileArtifact(
		const std::filesystem::path& path,
		const Sh4DynarecProfileArtifactBinding& binding,
		const Sh4DynarecProfileSnapshot& profile, std::uint64_t maximumBytes)
{
	if (maximumBytes < Sh4DynarecProfileArtifactHeaderSize)
		throw std::runtime_error("SH-4 dynarec profile bound is smaller than the header");
	if (!profile.complete)
		throw std::runtime_error("cannot write an incomplete SH-4 dynarec profile");
	if (profile.blocks.empty())
		throw std::runtime_error("cannot write an empty SH-4 dynarec profile");
	std::vector<std::uint8_t> payload;
	Sh4DynarecProfileArtifactSummary summary;
	summary.binding = binding;
	for (const auto& block : profile.blocks)
	{
		const auto record = blockRecord(block);
		if (record.size() > maximumBytes - Sh4DynarecProfileArtifactHeaderSize
				|| payload.size() > maximumBytes - Sh4DynarecProfileArtifactHeaderSize
						- record.size())
			throw std::runtime_error("SH-4 dynarec profile byte limit exceeded");
		payload.insert(payload.end(), record.begin(), record.end());
		if (block.definition.byteStatus != Sh4DynarecProfileByteStatus::Complete)
			++summary.incompleteByteBlocks;
	}
	for (const auto& branch : profile.branches)
	{
		const auto record = branchRecord(branch);
		if (record.size() > maximumBytes - Sh4DynarecProfileArtifactHeaderSize
				|| payload.size() > maximumBytes - Sh4DynarecProfileArtifactHeaderSize
						- record.size())
			throw std::runtime_error("SH-4 dynarec profile byte limit exceeded");
		payload.insert(payload.end(), record.begin(), record.end());
	}
	summary.blockCount = profile.blocks.size();
	summary.branchCount = profile.branches.size();
	summary.enteredExecutions = profile.enteredExecutions;
	summary.completedExecutions = profile.completedExecutions;
	summary.abortedExecutions = profile.abortedExecutions;
	summary.payloadBytes = payload.size();
	summary.payloadDigest = sha256(payload.data(), payload.size());

	std::vector<std::uint8_t> header;
	header.insert(header.end(), Magic.begin(), Magic.end());
	appendU32(header, Sh4DynarecProfileArtifactSchemaVersion);
	appendU32(header, Sh4DynarecProfileArtifactHeaderSize);
	appendU32(header, 0x01020304);
	appendU32(header, 1);
	appendU64(header, summary.blockCount);
	appendU64(header, summary.branchCount);
	appendU64(header, summary.enteredExecutions);
	appendU64(header, summary.completedExecutions);
	appendU64(header, summary.abortedExecutions);
	appendU64(header, summary.incompleteByteBlocks);
	appendU64(header, summary.payloadBytes);
	appendDigest(header, binding.identityDigest);
	appendDigest(header, binding.replayDigest);
	appendDigest(header, binding.configurationDigest);
	appendDigest(header, summary.payloadDigest);
	header.resize(Sh4DynarecProfileArtifactHeaderSize - 4, 0);
	appendU32(header, 0);
	putU32(header, Sh4DynarecProfileArtifactHeaderSize - 4,
			crc32(header.data(), Sh4DynarecProfileArtifactHeaderSize - 4));

	ExclusiveFile output(path);
	output.write(header.data(), header.size());
	output.write(payload.data(), payload.size());
	output.flush();
	return summary;
}

} // namespace research
