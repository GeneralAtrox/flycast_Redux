#include "research/memory_ranges_capture.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{

MemoryRangesCapture::MemoryRangesCapture(const std::filesystem::path& outputPath,
		const IdentityManifest& identity, const MemoryRangesManifest& manifest,
		std::uint64_t maximumBytes)
	: manifest(manifest)
{
	requireMemoryRangesIdentity(this->manifest, identity);
	writer = std::make_unique<MemoryRangesArtifactWriter>(outputPath, identity.digest,
			this->manifest, maximumBytes);
}

MemoryRangesCapture::~MemoryRangesCapture()
{
	if (!finished)
		abandon();
}

bool MemoryRangesCapture::observeInstruction(std::uint32_t executedPc, std::uint64_t tick,
		const GuestMemoryReader& reader)
{
	if (finished)
		throw std::logic_error("memory-ranges capture is already finished");
	if (captured || executedPc < manifest.triggerStart
			|| executedPc >= manifest.triggerEndExclusive)
		return false;
	if (!reader)
		throw std::invalid_argument("memory-ranges guest-memory reader is empty");

	std::vector<MemoryRangeView> views;
	views.reserve(manifest.ranges.size());
	for (const MemoryRangeDefinition& range : manifest.ranges)
	{
		const std::uint8_t *data = reader(range.address, range.length);
		if (data == nullptr)
			throw std::runtime_error("memory-ranges request is not backed by contiguous guest RAM: "
					+ range.id);
		views.push_back(MemoryRangeView {data, range.length});
	}
	writer->capture(executedPc, tick, views);
	captured = true;
	return true;
}

MemoryRangesArtifactSummary MemoryRangesCapture::finish()
{
	if (finished)
		throw std::logic_error("memory-ranges capture is already finished");
	MemoryRangesArtifactSummary summary = writer->finalize();
	finished = true;
	return summary;
}

void MemoryRangesCapture::abandon() noexcept
{
	if (finished)
		return;
	finished = true;
	if (writer != nullptr)
		writer->abandon();
}

} // namespace research
