#include "research/identity_manifest.h"
#include "research/maple_trace.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

std::string configValue(const std::vector<std::string>& arguments, const std::string& name)
{
	const std::string prefix = name + "=";
	for (std::size_t i = 0; i + 1 < arguments.size(); ++i)
	{
		if (arguments[i] != "-config" && arguments[i] != "--config")
			continue;
		const std::string& config = arguments[i + 1];
		const std::size_t start = config.find(prefix);
		if (start == std::string::npos)
			continue;
		const std::size_t quotePosition = start + prefix.size();
		if (quotePosition >= config.size()
				|| (config[quotePosition] != '\'' && config[quotePosition] != '"'))
			throw std::runtime_error("malformed fixture configuration quote: " + name);
		const char quote = config[quotePosition];
		const std::size_t valueStart = quotePosition + 1;
		const std::size_t end = config.find(quote, valueStart);
		if (end == std::string::npos || end == valueStart)
			throw std::runtime_error("malformed fixture configuration value: " + name);
		return config.substr(valueStart, end - valueStart);
	}
	throw std::runtime_error("missing fixture configuration value: " + name);
}

int currentProcessId()
{
#ifdef _WIN32
	return _getpid();
#else
	return static_cast<int>(getpid());
#endif
}

std::vector<std::uint8_t> littleEndianWords(std::initializer_list<std::uint32_t> words)
{
	std::vector<std::uint8_t> bytes;
	for (const std::uint32_t word : words)
	{
		bytes.push_back(static_cast<std::uint8_t>(word));
		bytes.push_back(static_cast<std::uint8_t>(word >> 8));
		bytes.push_back(static_cast<std::uint8_t>(word >> 16));
		bytes.push_back(static_cast<std::uint8_t>(word >> 24));
	}
	return bytes;
}

research::MapleDmaBeginEvent beginEvent()
{
	research::MapleDmaBeginEvent event;
	event.tick = 100;
	event.descriptorAddress = 0x0c001000;
	event.mden = 1;
	event.mdst = 1;
	event.mmsel = 1;
	event.trigger = research::MapleDmaTrigger::Software;
	return event;
}

void writeProductionTrace(const std::string& identityPath, const std::string& tracePath)
{
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	research::MapleTraceWriter writer(tracePath, identity.digest);
	writer.beginDma(beginEvent());

	research::MapleTransactionEvent transaction;
	transaction.dmaOrdinal = 0;
	transaction.tick = 100;
	transaction.descriptorAddress = 0x0c001000;
	transaction.destinationAddress = 0x0c002000;
	transaction.descriptorHeader1 = 0x80000001;
	transaction.descriptorHeader2 = 0x0c002000;
	transaction.deviceType = 0;
	transaction.bus = 0;
	transaction.port = 5;
	transaction.command = 0x09;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = littleEndianWords({0x01002009, 0x01000000});
	transaction.response = littleEndianWords({0x02002008, 0x01000000, 0xffff0000});
	writer.writeTransaction(transaction);

	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = 0;
	schedule.tick = 100;
	schedule.inputWireBytes = 11;
	schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000;
	schedule.responseCount = 1;
	writer.scheduleDma(schedule);

	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = 0;
	commit.tick = 1100;
	commit.callbackCycles = 1000;
	commit.responseCount = 1;
	commit.flags = research::MapleCommitInterruptRaised;
	writer.commitDma(commit);
	writer.finalize();
}

void writeAll(std::FILE *stream, const std::vector<std::uint8_t>& bytes)
{
	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const std::size_t written = std::fwrite(bytes.data() + offset, 1,
				bytes.size() - offset, stream);
		if (written == 0)
			throw std::runtime_error("cannot write tool-output fixture bytes");
		offset += written;
	}
	if (std::fflush(stream) != 0)
		throw std::runtime_error("cannot flush tool-output fixture bytes");
}

void writeToolOutput()
{
	constexpr std::size_t OutputBytes = 1024 * 1024 + 8193;
	std::vector<std::uint8_t> bytes(OutputBytes, static_cast<std::uint8_t>('x'));
	bytes[8191] = 0xf0;
	bytes[8192] = 0x9f;
	bytes[8193] = 0x98;
	bytes[8194] = 0x80;
	writeAll(stdout, bytes);
	writeAll(stderr, bytes);
}

void writePidMarker(const std::filesystem::path& path)
{
	std::ofstream marker(path, std::ios::binary | std::ios::out);
	if (!marker)
		throw std::runtime_error("cannot create process-fixture PID marker");
	marker << currentProcessId() << '\n';
	marker.close();
	if (!marker)
		throw std::runtime_error("cannot write process-fixture PID marker");
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "capture process fixture requires a mode\n");
		return 2;
	}
	try
	{
		const std::string mode = argv[1];
		if (mode == "sentinel")
		{
			const int milliseconds = argc >= 3 ? std::stoi(argv[2]) : 120000;
			std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
			return 0;
		}
		if (mode == "tool-hold")
		{
			if (argc < 3)
				throw std::runtime_error("tool-hold requires a PID marker path");
			writePidMarker(argv[2]);
			std::this_thread::sleep_for(std::chrono::minutes(2));
			return 0;
		}
		if (mode == "--package")
		{
			if (argc < 3)
				throw std::runtime_error("capture-validator fixture requires a package path");
			writePidMarker(std::filesystem::path(argv[2]) / "capture-validator.pid");
			std::this_thread::sleep_for(std::chrono::minutes(2));
			return 0;
		}
		if (mode == "instant")
			return 0;
		if (mode == "tool-output")
		{
			writeToolOutput();
			return 0;
		}

		std::vector<std::string> arguments;
		for (int i = 2; i < argc; ++i)
			arguments.emplace_back(argv[i]);
		const std::string identityPath = configValue(arguments, "research:IdentityManifest");
		const std::string tracePath = configValue(arguments, "research:MapleRecord");
		const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);

		if (mode == "hold")
		{
			research::MapleTraceWriter writer(tracePath, identity.digest);
			writer.beginDma(beginEvent());
			std::this_thread::sleep_for(std::chrono::minutes(2));
			return 0;
		}
		if (mode == "nonzero")
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			return 7;
		}
		if (mode == "invalid")
		{
			std::ofstream output(tracePath, std::ios::binary | std::ios::out);
			if (!output)
				throw std::runtime_error("cannot create invalid fixture trace");
			output << "not-a-maple-trace";
			output.close();
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			return 0;
		}
		if (mode != "record")
			throw std::runtime_error("unsupported capture process fixture mode");
		writeProductionTrace(identityPath, tracePath);
		// Keep the short-lived fixture available long enough for the driver to
		// acquire the OS command line and complete ownership tuple.
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "%s\n", exception.what());
		return 1;
	}
}
