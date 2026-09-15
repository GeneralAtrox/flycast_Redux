// Sh4LuaSubscriptionQueue: GD-ROM, AICA and CD-DA subscribe overloads.
#include "research/sh4_lua_subscriptions_internal.h"

namespace research
{

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const GdromFilter& filter, GdromCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua GD-ROM subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x1fu) != 0
			|| (filter.hasFadRange
					&& (filter.fadEndExclusive <= filter.fadStart
							|| filter.fadEndExclusive > 0x100000000ull)))
		throw std::invalid_argument("Lua GD-ROM filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::Gdrom,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.gdromCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribeGdromObservations(
						[weakState, weakEntry, filter](const GdromObservation& observation) {
							const auto bit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							if ((filter.typeMask & bit) == 0)
								return;
							if (filter.hasFadRange)
							{
								if (observation.type != GdromObservationType::TransferChunk)
									return;
								const std::uint64_t end = std::uint64_t(observation.fad)
										+ observation.sectorCount;
								if (observation.fad >= filter.fadEndExclusive
										|| end <= filter.fadStart)
									return;
							}
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribeGdromObservations(token); });
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const AicaFilter& filter, AicaCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua AICA subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x1fffu) != 0
			|| (filter.hasWriter
					&& (filter.writerMask == 0 || (filter.writerMask & ~0x7fu) != 0))
			|| (filter.hasAddressRange
					&& (filter.addressEndExclusive <= filter.addressStart
							|| filter.addressEndExclusive > 0x100000000ull))
			|| (filter.hasChannel && filter.channel >= 64)
			|| (filter.requireNonzeroCddaContribution
					&& filter.typeMask != (std::uint32_t {1}
							<< (static_cast<unsigned>(AicaObservationType::SampleFrame) - 1u))))
		throw std::invalid_argument("Lua AICA filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::Aica,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.aicaCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribeAicaObservations(
						[weakState, weakEntry, filter](const AicaObservation& observation) {
							const auto typeBit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							const auto writer = static_cast<unsigned>(observation.owner.writer);
							const auto writerBit = writer == 0 ? 0u
									: std::uint32_t {1} << (writer - 1u);
							if ((filter.typeMask & typeBit) == 0
									|| (filter.hasWriter
											&& (writerBit == 0
													|| (filter.writerMask & writerBit) == 0)))
								return;
							if (filter.hasAddressRange)
							{
								if (observation.type != AicaObservationType::RegisterWrite
										&& observation.type != AicaObservationType::RamWrite)
									return;
								const std::uint64_t size = observation.type
										== AicaObservationType::RegisterWrite
										? observation.width : observation.bytes.size();
								const std::uint64_t end = std::uint64_t(observation.address) + size;
								if (observation.address >= filter.addressEndExclusive
										|| end <= filter.addressStart)
									return;
							}
							if (filter.hasChannel)
							{
								if ((observation.type == AicaObservationType::KeyOn
										|| observation.type == AicaObservationType::KeyOff)
										&& observation.channel != filter.channel)
									return;
								if (observation.type == AicaObservationType::KeyBatchComplete)
								{
									const std::uint64_t bit = std::uint64_t {1} << filter.channel;
									if (((observation.keyOnMask | observation.keyOffMask) & bit) == 0)
										return;
								}
								else if (observation.type != AicaObservationType::KeyOn
										&& observation.type != AicaObservationType::KeyOff)
									return;
							}
							if (filter.requireNonzeroCddaContribution
									&& observation.cddaContributionLeft == 0
									&& observation.cddaContributionRight == 0)
								return;
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribeAicaObservations(token); });
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const CddaFilter& filter, CddaCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua CD-DA subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x0fu) != 0
			|| (filter.hasCommand && !isCddaControlCommand(filter.command))
			|| (filter.hasSuccessful
					&& filter.typeMask != (std::uint32_t {1}
								<< (static_cast<unsigned>(CddaObservationType::ControlApplied) - 1u))
					&& filter.typeMask != (std::uint32_t {1}
								<< (static_cast<unsigned>(CddaObservationType::Sector) - 1u)))
			|| (filter.hasFadRange
					&& (filter.fadEndExclusive <= filter.fadStart
							|| filter.fadEndExclusive > 0x100000000ull)))
		throw std::invalid_argument("Lua CD-DA filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::Cdda,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.cddaCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribeCddaObservations(
						[weakState, weakEntry, filter](const CddaObservation& observation) {
							const auto bit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							if ((filter.typeMask & bit) == 0)
								return;
							if (filter.hasCommand
									&& ((observation.type != CddaObservationType::ControlAccepted
												&& observation.type != CddaObservationType::ControlApplied)
											|| observation.command != filter.command))
								return;
							if (filter.hasSuccessful)
							{
								const bool successful = observation.type
										== CddaObservationType::ControlApplied
										? observation.appliedSuccessfully
										: observation.readSuccessful;
								if (successful != filter.successful)
									return;
							}
							if (filter.hasFadRange
									&& (observation.type != CddaObservationType::Sector
											|| observation.fad < filter.fadStart
											|| observation.fad >= filter.fadEndExclusive))
								return;
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribeCddaObservations(token); });
}

} // namespace research
