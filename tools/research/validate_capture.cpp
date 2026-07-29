#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sha256.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

using json = nlohmann::json;
constexpr std::uint64_t MaxCaptureJsonBytes = 16ull * 1024 * 1024;

struct Blob
{
	std::string pathText;
	std::uint64_t size = 0;
	std::string sha256;
};

struct SourceIdentity
{
	std::string role;
	Blob blob;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid research capture package: " + reason);
}

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --package <capture-directory> [--receipt <output.json>]\n",
			executable);
}

void requireAllowedKeys(const json& value, const std::set<std::string>& allowed,
		const std::string& field)
{
	if (!value.is_object())
		invalid(field + " must be an object");
	for (const auto& item : value.items())
		if (allowed.find(item.key()) == allowed.end())
			invalid(field + " has unknown field '" + item.key() + "'");
}

const json& requiredObject(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_object())
		invalid(field + "." + name + " must be an object");
	return parent.at(name);
}

const json& requiredArray(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_array())
		invalid(field + "." + name + " must be an array");
	return parent.at(name);
}

const json& requiredStringArray(const json& parent, const char *name,
		const std::string& field, std::size_t maximum)
{
	const json& values = requiredArray(parent, name, field);
	if (values.size() > maximum)
		invalid(field + "." + name + " has too many entries");
	for (std::size_t i = 0; i < values.size(); ++i)
		if (!values.at(i).is_string())
			invalid(field + "." + name + " must contain only strings");
	return values;
}

bool isResearchSection(const std::string& section)
{
	static constexpr char Expected[] = "research";
	if (section.size() != sizeof(Expected) - 1)
		return false;
	for (std::size_t i = 0; i < section.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(section[i])) != Expected[i])
			return false;
	return true;
}

bool readUtf8CodePoint(const std::string& text, std::size_t& offset,
		std::uint32_t& codePoint)
{
	if (offset >= text.size())
		return false;
	const auto first = static_cast<unsigned char>(text[offset++]);
	if (first <= 0x7f)
	{
		codePoint = first;
		return true;
	}
	std::size_t continuationCount = 0;
	std::uint32_t minimum = 0;
	if (first >= 0xc2 && first <= 0xdf)
	{
		continuationCount = 1;
		minimum = 0x80;
		codePoint = first & 0x1f;
	}
	else if (first >= 0xe0 && first <= 0xef)
	{
		continuationCount = 2;
		minimum = 0x800;
		codePoint = first & 0x0f;
	}
	else if (first >= 0xf0 && first <= 0xf4)
	{
		continuationCount = 3;
		minimum = 0x10000;
		codePoint = first & 0x07;
	}
	else
	{
		return false;
	}
	if (continuationCount > text.size() - offset)
		return false;
	for (std::size_t i = 0; i < continuationCount; ++i)
	{
		const auto continuation = static_cast<unsigned char>(text[offset++]);
		if ((continuation & 0xc0) != 0x80)
			return false;
		codePoint = (codePoint << 6) | (continuation & 0x3f);
	}
	return codePoint >= minimum && codePoint <= 0x10ffff
			&& !(codePoint >= 0xd800 && codePoint <= 0xdfff);
}

bool isDotNetWhitespace(std::uint32_t codePoint)
{
	return (codePoint >= 0x0009 && codePoint <= 0x000d)
			|| codePoint == 0x0020 || codePoint == 0x0085 || codePoint == 0x00a0
			|| codePoint == 0x1680 || (codePoint >= 0x2000 && codePoint <= 0x200a)
			|| codePoint == 0x2028 || codePoint == 0x2029 || codePoint == 0x202f
			|| codePoint == 0x205f || codePoint == 0x3000;
}

bool isEmptyOrDotNetWhitespaceOnly(const std::string& text)
{
	if (text.empty())
		return true;
	std::size_t offset = 0;
	while (offset < text.size())
	{
		std::uint32_t codePoint = 0;
		if (!readUtf8CodePoint(text, offset, codePoint) || !isDotNetWhitespace(codePoint))
			return false;
	}
	return true;
}

bool containsResearchConfigSection(const std::string& value)
{
	// Keep the parser state aligned with core/cfg/cl.cpp::parseConfigOption.
	// Section comparison is deliberately case-insensitive and therefore more
	// conservative than Flycast's case-sensitive configuration map.
	int step = 0; // section, key, value
	char inQuote = '\0';
	std::string section;
	bool keyEmpty = true;
	for (const char character : value)
	{
		if (inQuote != '\0' && character == inQuote)
		{
			inQuote = '\0';
			step = 0;
			section.clear();
			keyEmpty = true;
			continue;
		}
		switch (character)
		{
		case ':':
			if (step == 0)
			{
				if (section.empty())
					return false;
				if (isResearchSection(section))
					return true;
				step = 1;
				keyEmpty = true;
			}
			else if (step == 1)
				keyEmpty = false;
			break;
		case '=':
			if (step == 0)
				return false;
			if (step == 1)
			{
				if (keyEmpty)
					return false;
				step = 2;
			}
			break;
		case '\'':
		case '"':
			if (step == 0)
				section += character;
			else if (step == 1)
				keyEmpty = false;
			else if (step == 2 && inQuote == '\0')
				inQuote = character;
			break;
		case ',':
			if (step == 2 && inQuote == '\0')
			{
				step = 0;
				section.clear();
				keyEmpty = true;
			}
			else if (step == 1)
				keyEmpty = false;
			break;
		case ' ':
			break;
		default:
			if (step == 0)
				section += character;
			else if (step == 1)
				keyEmpty = false;
			break;
		}
	}
	return false;
}

std::string requiredString(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_string()
			|| parent.at(name).get<std::string>().empty())
		invalid(field + "." + name + " must be a non-empty string");
	return parent.at(name).get<std::string>();
}

bool requiredBool(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_boolean())
		invalid(field + "." + name + " must be a boolean");
	return parent.at(name).get<bool>();
}

std::uint64_t requiredUnsigned(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_number_unsigned())
		invalid(field + "." + name + " must be an unsigned integer");
	return parent.at(name).get<std::uint64_t>();
}

std::filesystem::path pathFromUtf8(const std::string& value)
{
	return std::filesystem::u8path(value);
}

std::string pathToUtf8(const std::filesystem::path& value)
{
	return value.u8string();
}

std::int64_t requiredSigned(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_number_integer())
		invalid(field + "." + name + " must be an integer");
	return parent.at(name).get<std::int64_t>();
}

bool isLowerSha256(const std::string& value)
{
	research::Sha256Digest digest {};
	return research::sha256FromHex(value, digest);
}

Blob parseBlob(const json& value, const std::string& field, bool requireAbsolute)
{
	if (!value.is_object())
		invalid(field + " must be an object");
	Blob blob;
	blob.pathText = requiredString(value, "path", field);
	blob.size = requiredUnsigned(value, "size", field);
	blob.sha256 = requiredString(value, "sha256", field);
	if (!isLowerSha256(blob.sha256))
		invalid(field + ".sha256 must be lowercase SHA-256");
	if (requireAbsolute && !pathFromUtf8(blob.pathText).is_absolute())
		invalid(field + ".path must be absolute");
	return blob;
}

std::filesystem::path normalizedPath(const std::filesystem::path& value)
{
	std::error_code error;
	std::filesystem::path path = std::filesystem::weakly_canonical(value, error);
	if (error)
		path = std::filesystem::absolute(value, error).lexically_normal();
	if (error)
		invalid("cannot normalize path '" + value.string() + "'");
	return path;
}

#ifdef _WIN32
bool equalPathText(const std::wstring& lhs, const std::wstring& rhs)
{
	if (lhs.size() > INT_MAX || rhs.size() > INT_MAX)
		return false;
	return CompareStringOrdinal(lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
			static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

bool pathTextStartsWith(const std::wstring& value, const std::wstring& prefix)
{
	return value.size() >= prefix.size()
			&& equalPathText(value.substr(0, prefix.size()), prefix);
}

bool isPathSeparator(wchar_t value)
{
	return value == L'\\' || value == L'/';
}
#else
bool equalPathText(const std::string& lhs, const std::string& rhs)
{
	return lhs == rhs;
}

bool pathTextStartsWith(const std::string& value, const std::string& prefix)
{
	return value.compare(0, prefix.size(), prefix) == 0;
}

bool isPathSeparator(char value)
{
	return value == '/';
}
#endif

bool samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
#ifdef _WIN32
	return equalPathText(normalizedPath(lhs).native(), normalizedPath(rhs).native());
#else
	return equalPathText(normalizedPath(lhs).generic_string(),
			normalizedPath(rhs).generic_string());
#endif
}

bool samePath(const std::string& lhs, const std::filesystem::path& rhs)
{
	return samePath(pathFromUtf8(lhs), rhs);
}

bool samePath(const std::filesystem::path& lhs, const std::string& rhs)
{
	return samePath(lhs, pathFromUtf8(rhs));
}

bool samePath(const std::string& lhs, const std::string& rhs)
{
	return samePath(pathFromUtf8(lhs), pathFromUtf8(rhs));
}

bool isDescendantPath(const std::filesystem::path& parent,
		const std::filesystem::path& descendant)
{
#ifdef _WIN32
	std::wstring parentText = normalizedPath(parent).native();
	const std::wstring descendantText = normalizedPath(descendant).native();
	const std::wstring rootText = normalizedPath(parent).root_path().native();
#else
	std::string parentText = normalizedPath(parent).generic_string();
	const std::string descendantText = normalizedPath(descendant).generic_string();
	const std::string rootText = normalizedPath(parent).root_path().generic_string();
#endif
	while (parentText.size() > rootText.size() && !parentText.empty()
			&& isPathSeparator(parentText.back()))
		parentText.pop_back();
	if (descendantText.size() <= parentText.size()
			|| !pathTextStartsWith(descendantText, parentText))
		return false;
	if (!parentText.empty() && isPathSeparator(parentText.back()))
		return true;
	return isPathSeparator(descendantText[parentText.size()]);
}

std::filesystem::path canonicalAbsolutePath(const std::filesystem::path& value,
		const std::string& field)
{
	if (!value.is_absolute())
		invalid(field + " must be absolute");
	std::error_code error;
	std::filesystem::path result = std::filesystem::absolute(value, error).lexically_normal();
	if (error)
		invalid("cannot canonicalize " + field + ": " + error.message());
	while (result != result.root_path() && result.filename().empty())
		result = result.parent_path();
	return result;
}

std::filesystem::path resolvePackagePath(const std::filesystem::path& package,
		const std::string& relative, const std::string& field)
{
	const std::filesystem::path supplied = pathFromUtf8(relative);
	if (supplied.empty() || supplied.is_absolute())
		invalid(field + " must be a package-relative path");
	const std::filesystem::path normalized = supplied.lexically_normal();
	for (const auto& component : normalized)
		if (component == "..")
			invalid(field + " escapes the package");
	const std::filesystem::path result = (package / normalized).lexically_normal();
	if (!isDescendantPath(package, result))
		invalid(field + " escapes the package");
	return result;
}

bool isCaptureId(const std::string& value)
{
	if (value.size() != 36)
		return false;
	for (std::size_t i = 0; i < value.size(); ++i)
	{
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			if (value[i] != '-')
				return false;
		}
		else if (!((value[i] >= '0' && value[i] <= '9')
				|| (value[i] >= 'a' && value[i] <= 'f')))
		{
			return false;
		}
	}
	return true;
}

bool isAsciiAlphaNumeric(char value)
{
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
			|| (value >= '0' && value <= '9');
}

bool isSafeArtifactName(const std::string& value)
{
	constexpr const char *suffix = ".fcmr";
	if (value.empty() || value.size() > 128 || !isAsciiAlphaNumeric(value.front())
			|| value.size() <= 5 || value.compare(value.size() - 5, 5, suffix) != 0)
		return false;
	for (const char character : value)
		if (!isAsciiAlphaNumeric(character) && character != '.' && character != '_'
				&& character != '-')
			return false;

	std::string stem = value.substr(0, value.size() - 5);
	std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	for (const std::string& reserved : {"con", "prn", "aux", "nul", "com1", "com2",
			"com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1",
			"lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"})
		if (stem == reserved || (stem.size() > reserved.size()
				&& stem.compare(0, reserved.size(), reserved) == 0
				&& stem[reserved.size()] == '.'))
			return false;
	return true;
}

std::string quoteFlycastConfigValue(const std::string& value)
{
	if (value.find('\0') != std::string::npos)
		invalid("Flycast configuration value contains NUL");
	if (value.find('\'') == std::string::npos)
		return "'" + value + "'";
	if (value.find('"') == std::string::npos)
		return "\"" + value + "\"";
	invalid("Flycast configuration path contains both quote characters");
}

std::vector<std::string> parseWindowsCommandLine(const std::string& commandLine)
{
	std::vector<std::string> arguments;
	std::size_t position = 0;
	while (position < commandLine.size())
	{
		while (position < commandLine.size()
				&& (commandLine[position] == ' ' || commandLine[position] == '\t'))
			++position;
		if (position == commandLine.size())
			break;

		std::string argument;
		bool quoted = false;
		while (position < commandLine.size())
		{
			const char character = commandLine[position];
			if (!quoted && (character == ' ' || character == '\t'))
				break;
			if (character == '\\')
			{
				const std::size_t slashStart = position;
				while (position < commandLine.size() && commandLine[position] == '\\')
					++position;
				const std::size_t slashCount = position - slashStart;
				if (position < commandLine.size() && commandLine[position] == '"')
				{
					argument.append(slashCount / 2, '\\');
					if (slashCount % 2 != 0)
					{
						argument.push_back('"');
						++position;
					}
					else if (quoted && position + 1 < commandLine.size()
							&& commandLine[position + 1] == '"')
					{
						argument.push_back('"');
						position += 2;
					}
					else
					{
						quoted = !quoted;
						++position;
					}
				}
				else
				{
					argument.append(slashCount, '\\');
				}
				continue;
			}
			if (character == '"')
			{
				if (quoted && position + 1 < commandLine.size()
						&& commandLine[position + 1] == '"')
				{
					argument.push_back('"');
					position += 2;
				}
				else
				{
					quoted = !quoted;
					++position;
				}
				continue;
			}
			argument.push_back(character);
			++position;
		}
		if (quoted)
			invalid("recorded process command line has an unterminated quote");
		arguments.push_back(std::move(argument));
	}
	return arguments;
}

void requireExactPackageEntries(const std::filesystem::path& package,
		const std::set<std::string>& expected)
{
	std::set<std::string> found;
	std::error_code error;
	std::filesystem::directory_iterator iterator(package, error);
	if (error)
		invalid("cannot enumerate package directory: " + error.message());
	const std::filesystem::directory_iterator end;
	for (; iterator != end; iterator.increment(error))
	{
		if (error)
			invalid("cannot enumerate package directory: " + error.message());
		const std::filesystem::directory_entry& entry = *iterator;
		const std::filesystem::file_status status = entry.symlink_status(error);
		if (error || std::filesystem::is_symlink(status)
				|| !std::filesystem::is_regular_file(status))
			invalid("package contains a link or non-regular entry: " + entry.path().string());
		const std::string name = entry.path().filename().string();
		if (expected.find(name) == expected.end())
			invalid("package contains unexpected entry: " + name);
		if (!found.insert(name).second)
			invalid("package contains a duplicate entry: " + name);
	}
	if (error)
		invalid("cannot enumerate package directory: " + error.message());
	if (found != expected)
		invalid("package file set is incomplete");
}

Blob fileIdentity(const std::filesystem::path& path, const std::string& pathText,
		std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max())
{
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error) || error)
		invalid("required regular file is missing: " + path.string());
	const std::uintmax_t rawSize = std::filesystem::file_size(path, error);
	if (error || rawSize > std::numeric_limits<std::uint64_t>::max())
		invalid("cannot determine file size: " + path.string());
	const std::uint64_t size = static_cast<std::uint64_t>(rawSize);
	if (size > maximumBytes)
		invalid("file exceeds declared validation limit: " + path.string());
	return Blob {pathText, size,
			research::sha256ToHex(research::hashFileExact(path, maximumBytes))};
}

void requireBlobEqual(const Blob& actual, const Blob& expected, const std::string& field,
		bool comparePath = true)
{
	if (comparePath && !samePath(actual.pathText, expected.pathText))
		invalid(field + " path mismatch");
	if (actual.size != expected.size || actual.sha256 != expected.sha256)
		invalid(field + " identity mismatch");
}

void verifyExternalBlob(const Blob& blob, const std::string& field)
{
	const Blob actual = fileIdentity(pathFromUtf8(blob.pathText), blob.pathText, blob.size);
	requireBlobEqual(actual, blob, field);
}

json parseJsonFile(const std::filesystem::path& path, const std::string& field)
{
	const std::vector<std::uint8_t> bytes = research::readFileExact(path, MaxCaptureJsonBytes);
	if (bytes.empty())
		invalid(field + " is empty");
	try
	{
		return json::parse(bytes.begin(), bytes.end());
	}
	catch (const nlohmann::json::exception& exception)
	{
		invalid(field + " JSON parse/type error: " + exception.what());
	}
}

void addSource(std::vector<SourceIdentity>& sources, const std::string& role,
		const json& blob, const std::string& field)
{
	sources.push_back(SourceIdentity {role, parseBlob(blob, field, true)});
}

std::vector<SourceIdentity> parseSourceArray(const json& value, const std::string& field)
{
	if (!value.is_array())
		invalid(field + " must be an array");
	std::vector<SourceIdentity> result;
	for (std::size_t i = 0; i < value.size(); ++i)
	{
		const json& entry = value.at(i);
		const std::string itemField = field + "[" + std::to_string(i) + "]";
		requireAllowedKeys(entry, {"role", "path", "size", "sha256"}, itemField);
		SourceIdentity source;
		source.role = requiredString(entry, "role", itemField);
		source.blob = parseBlob(json {
			{"path", entry.at("path")},
			{"size", entry.at("size")},
			{"sha256", entry.at("sha256")},
		}, itemField, true);
		result.push_back(std::move(source));
	}
	return result;
}

void requireSourceSets(const std::vector<SourceIdentity>& before,
		const std::vector<SourceIdentity>& after,
		const std::vector<SourceIdentity>& expected)
{
	if (before.size() != after.size() || before.size() != expected.size())
		invalid("source authentication set size mismatch");
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		if (before[i].role != expected[i].role || after[i].role != expected[i].role)
			invalid("source authentication role/order mismatch at index " + std::to_string(i));
		requireBlobEqual(before[i].blob, expected[i].blob,
				"source authentication before " + expected[i].role);
		requireBlobEqual(after[i].blob, expected[i].blob,
				"source authentication after " + expected[i].role);
		verifyExternalBlob(expected[i].blob, "current source " + expected[i].role);
	}
}

std::vector<SourceIdentity> gatherExpectedSources(const json& job, const json& identity,
		const Blob& internalJob, const std::vector<SourceIdentity>& declaredBefore)
{
	std::vector<SourceIdentity> sources;
	if (declaredBefore.empty() || declaredBefore.front().role != "capture_job")
		invalid("source authentication must begin with capture_job");
	if (declaredBefore.front().blob.size != internalJob.size
			|| declaredBefore.front().blob.sha256 != internalJob.sha256)
		invalid("external capture job does not match packaged job bytes");
	sources.push_back(declaredBefore.front());

	addSource(sources, "driver", job.at("driver"), "job.driver");
	addSource(sources, "identity_manifest", job.at("identity_manifest"),
			"job.identity_manifest");

	const json& media = requiredObject(identity, "media", "identity");
	if (requiredString(media, "kind", "identity.media") != "gdi")
		invalid("capture-v1 requires identity.media.kind to be 'gdi'");
	addSource(sources, "media.source", media.at("source"), "identity.media.source");
	addSource(sources, "media.ip_bin", media.at("ip_bin"), "identity.media.ip_bin");
	addSource(sources, "media.boot_executable", media.at("boot_executable"),
			"identity.media.boot_executable");
	const json& tracks = requiredArray(media, "tracks", "identity.media");
	if (tracks.empty())
		invalid("capture-v1 requires at least one identity.media track");
	for (std::size_t i = 0; i < tracks.size(); ++i)
	{
		const json& track = tracks.at(i);
		const std::uint64_t number = requiredUnsigned(track, "track",
				"identity.media.tracks[" + std::to_string(i) + "]");
		addSource(sources, "media.track." + std::to_string(number), track,
				"identity.media.tracks[" + std::to_string(i) + "]");
	}

	const json& firmware = requiredObject(identity, "firmware", "identity");
	if (firmware.contains("bios") && !firmware.at("bios").is_null())
		addSource(sources, "firmware.bios", firmware.at("bios"), "identity.firmware.bios");
	addSource(sources, "firmware.flash_initial", firmware.at("flash_initial"),
			"identity.firmware.flash_initial");

	const json& devices = requiredArray(identity, "persistent_devices", "identity");
	for (std::size_t i = 0; i < devices.size(); ++i)
	{
		const json& device = devices.at(i);
		const std::uint64_t bus = requiredUnsigned(device, "bus",
				"identity.persistent_devices[" + std::to_string(i) + "]");
		const std::uint64_t port = requiredUnsigned(device, "port",
				"identity.persistent_devices[" + std::to_string(i) + "]");
		addSource(sources,
				"persistent_device." + std::to_string(bus) + "." + std::to_string(port),
				device, "identity.persistent_devices[" + std::to_string(i) + "]");
	}

	const json& emulatorIdentity = requiredObject(identity, "emulator", "identity");
	addSource(sources, "emulator.executable", emulatorIdentity.at("executable"),
			"identity.emulator.executable");

	const json& emulator = requiredObject(job, "emulator", "job");
	const json& emulatorInputs = requiredArray(emulator, "authenticated_inputs", "job.emulator");
	for (std::size_t i = 0; i < emulatorInputs.size(); ++i)
		addSource(sources, "emulator.authenticated_input." + std::to_string(i),
				emulatorInputs.at(i), "job.emulator.authenticated_inputs[" + std::to_string(i) + "]");
	const json& runtimeFiles = requiredArray(emulator, "runtime_files", "job.emulator");
	for (std::size_t i = 0; i < runtimeFiles.size(); ++i)
	{
		const json& runtimeFile = runtimeFiles.at(i);
		addSource(sources, "emulator.runtime_file." + std::to_string(i),
				runtimeFile.at("source"), "job.emulator.runtime_files[" + std::to_string(i) + "].source");
	}

	const json& validator = requiredObject(job, "validator", "job");
	addSource(sources, "validator.executable", validator.at("executable"),
			"job.validator.executable");
	const json& validatorInputs = requiredArray(validator, "authenticated_inputs", "job.validator");
	for (std::size_t i = 0; i < validatorInputs.size(); ++i)
		addSource(sources, "validator.authenticated_input." + std::to_string(i),
				validatorInputs.at(i), "job.validator.authenticated_inputs[" + std::to_string(i) + "]");

	const json& captureValidator = requiredObject(job, "capture_validator", "job");
	addSource(sources, "capture_validator.executable", captureValidator.at("executable"),
			"job.capture_validator.executable");
	return sources;
}

std::string vmuFileName(std::uint64_t bus, std::uint64_t port)
{
	const char busName = static_cast<char>('A' + bus);
	const char portName = port == 5 ? 'x' : static_cast<char>('1' + port);
	return std::string("vmu_save_") + busName + portName + ".bin";
}

void validateRuntimeState(const json& runtimeState, const json& identity,
		const std::filesystem::path& expectedCandidate)
{
	requireAllowedKeys(runtimeState, {"initial", "terminal", "removed_before_publication"},
			"transcript.runtime_state");
	if (!requiredBool(runtimeState, "removed_before_publication", "transcript.runtime_state"))
		invalid("private runtime was not removed before publication");
	const std::vector<SourceIdentity> initial = parseSourceArray(runtimeState.at("initial"),
			"transcript.runtime_state.initial");
	const std::vector<SourceIdentity> terminal = parseSourceArray(runtimeState.at("terminal"),
			"transcript.runtime_state.terminal");

	struct ExpectedRuntimeFile
	{
		std::string role;
		std::filesystem::path path;
		Blob source;
	};
	std::vector<ExpectedRuntimeFile> expected;
	const std::filesystem::path data = expectedCandidate / ".runtime" / "data";
	const json& firmware = requiredObject(identity, "firmware", "identity");
	const Blob flash = parseBlob(firmware.at("flash_initial"),
			"identity.firmware.flash_initial", true);
	if (pathFromUtf8(flash.pathText).filename() != "dc_nvmem.bin")
		invalid("identity firmware flash does not use dc_nvmem.bin");
	expected.push_back(ExpectedRuntimeFile {"firmware.flash", data / "dc_nvmem.bin", flash});

	std::set<std::string> roles {"firmware.flash"};
	const json& devices = requiredArray(identity, "persistent_devices", "identity");
	for (std::size_t i = 0; i < devices.size(); ++i)
	{
		const json& device = devices.at(i);
		const std::string field = "identity.persistent_devices[" + std::to_string(i) + "]";
		if (requiredString(device, "kind", field) != "vmu")
			invalid(field + ".kind is not supported by capture transaction v1");
		const std::uint64_t bus = requiredUnsigned(device, "bus", field);
		const std::uint64_t port = requiredUnsigned(device, "port", field);
		if (bus > 3 || port > 5)
			invalid(field + " bus/port is out of range");
		const std::string role = "persistent_device." + std::to_string(bus)
				+ "." + std::to_string(port);
		if (!roles.insert(role).second)
			invalid("duplicate runtime role: " + role);
		expected.push_back(ExpectedRuntimeFile {role, data / vmuFileName(bus, port),
				parseBlob(device, field, true)});
	}

	if (initial.size() != expected.size() || terminal.size() != expected.size())
		invalid("runtime state file count mismatch");
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		if (initial[i].role != expected[i].role || terminal[i].role != expected[i].role)
			invalid("runtime state role/order mismatch at index " + std::to_string(i));
		if (!samePath(initial[i].blob.pathText, expected[i].path)
				|| !samePath(terminal[i].blob.pathText, expected[i].path))
			invalid("runtime state path mismatch for " + expected[i].role);
		requireBlobEqual(initial[i].blob, expected[i].source,
				"runtime initial " + expected[i].role, false);
		if (terminal[i].blob.size != initial[i].blob.size)
			invalid("runtime terminal size changed for " + expected[i].role);
	}
}

void validateJobShape(const json& job)
{
	requireAllowedKeys(job, {"schema", "schema_version", "driver", "identity_manifest",
			"emulator", "validator", "capture_validator", "output", "limits", "metadata"},
			"job");
	if (requiredString(job, "schema", "job") != "flycast-research-capture-job"
			|| requiredUnsigned(job, "schema_version", "job") != 1)
		invalid("unsupported capture job schema");
	parseBlob(job.at("driver"), "job.driver", true);
	parseBlob(job.at("identity_manifest"), "job.identity_manifest", true);

	const json& emulator = requiredObject(job, "emulator", "job");
	requireAllowedKeys(emulator, {"stage_executable", "interactive", "arguments_prefix",
			"authenticated_inputs", "runtime_files"}, "job.emulator");
	if (!requiredBool(emulator, "stage_executable", "job.emulator"))
		invalid("job.emulator.stage_executable must be true");
	requiredBool(emulator, "interactive", "job.emulator");
	const json& emulatorPrefix = requiredStringArray(emulator, "arguments_prefix",
			"job.emulator", 64);
	for (const json& argument : emulatorPrefix)
		if (containsResearchConfigSection(argument.get<std::string>()))
			invalid("job.emulator.arguments_prefix must not supply research settings");
	if (!emulatorPrefix.empty())
	{
		const std::string lastArgument = emulatorPrefix.back().get<std::string>();
		if (lastArgument == "-config" || lastArgument == "--config")
			invalid("job.emulator.arguments_prefix must not end with an incomplete -config/--config option");
	}
	const json& emulatorInputs = requiredArray(emulator, "authenticated_inputs", "job.emulator");
	if (emulatorInputs.size() > 128)
		invalid("job.emulator.authenticated_inputs has too many entries");
	const json& runtimeFiles = requiredArray(emulator, "runtime_files", "job.emulator");
	if (runtimeFiles.size() > 128)
		invalid("job.emulator.runtime_files has too many entries");
	for (std::size_t i = 0; i < runtimeFiles.size(); ++i)
	{
		const std::string field = "job.emulator.runtime_files[" + std::to_string(i) + "]";
		requireAllowedKeys(runtimeFiles.at(i), {"source", "destination"}, field);
		parseBlob(runtimeFiles.at(i).at("source"), field + ".source", true);
		requiredString(runtimeFiles.at(i), "destination", field);
	}

	const json& validator = requiredObject(job, "validator", "job");
	requireAllowedKeys(validator, {"executable", "arguments_prefix", "authenticated_inputs"},
			"job.validator");
	parseBlob(validator.at("executable"), "job.validator.executable", true);
	requiredStringArray(validator, "arguments_prefix", "job.validator", 64);
	const json& validatorInputs = requiredArray(validator, "authenticated_inputs", "job.validator");
	if (validatorInputs.size() > 128)
		invalid("job.validator.authenticated_inputs has too many entries");

	const json& captureValidator = requiredObject(job, "capture_validator", "job");
	requireAllowedKeys(captureValidator, {"executable"}, "job.capture_validator");
	parseBlob(captureValidator.at("executable"), "job.capture_validator.executable", true);

	const json& output = requiredObject(job, "output", "job");
	requireAllowedKeys(output, {"accepted_directory", "artifact_name"}, "job.output");
	if (!pathFromUtf8(requiredString(output, "accepted_directory", "job.output")).is_absolute())
		invalid("job.output.accepted_directory must be absolute");
	const std::string artifactName = requiredString(output, "artifact_name", "job.output");
	const std::filesystem::path artifact(artifactName);
	if (artifact.is_absolute() || artifact.has_parent_path() || !isSafeArtifactName(artifactName))
		invalid("job.output.artifact_name must be a safe ASCII .fcmr file name");

	const json& limits = requiredObject(job, "limits", "job");
	requireAllowedKeys(limits, {"trace_max_bytes", "emulator_timeout_seconds",
			"validator_timeout_seconds", "stability_interval_ms"}, "job.limits");
	const std::uint64_t maximum = requiredUnsigned(limits, "trace_max_bytes", "job.limits");
	if (maximum < research::MapleTraceHeaderSize || maximum > 1099511627776ull)
		invalid("job.limits.trace_max_bytes is out of range");
	const std::uint64_t emulatorTimeout = requiredUnsigned(limits,
			"emulator_timeout_seconds", "job.limits");
	if (emulatorTimeout < 1 || emulatorTimeout > 86400)
		invalid("job.limits.emulator_timeout_seconds is out of range");
	const std::uint64_t validatorTimeout = requiredUnsigned(limits,
			"validator_timeout_seconds", "job.limits");
	if (validatorTimeout < 1 || validatorTimeout > 3600)
		invalid("job.limits.validator_timeout_seconds is out of range");
	const std::uint64_t stabilityInterval = requiredUnsigned(limits,
			"stability_interval_ms", "job.limits");
	if (stabilityInterval < 50 || stabilityInterval > 5000)
		invalid("job.limits.stability_interval_ms is out of range");
}

void validateIdentityResearchSettingOwnership(const json& identity)
{
	const json& configuration = requiredObject(identity, "configuration", "identity");
	const json& values = requiredObject(configuration, "values", "identity.configuration");
	if (!values.contains("flycast_transient"))
		return;
	const json& transient = values.at("flycast_transient");
	if (!transient.is_array() || transient.size() > 128)
		invalid("identity.configuration.values.flycast_transient must be a bounded array");
	for (const json& setting : transient)
	{
		if (!setting.is_string())
			invalid("identity.configuration.values.flycast_transient contains a non-string setting");
		const std::string settingText = setting.get<std::string>();
		if (isEmptyOrDotNetWhitespaceOnly(settingText))
			invalid("identity.configuration.values.flycast_transient contains an empty or whitespace-only setting");
		if (containsResearchConfigSection(settingText))
			invalid("identity.configuration.values.flycast_transient must not supply research settings");
	}
}

void validateReceipt(const std::filesystem::path& package, const Blob& transcript,
		const Blob& artifact, const Blob& expectedValidator)
{
	const std::filesystem::path receiptPath = package / "capture-validation.json";
	const json receipt = parseJsonFile(receiptPath, "capture validation receipt");
	requireAllowedKeys(receipt, {"schema", "schema_version", "accepted", "validated_utc",
			"transcript", "artifact", "validator", "claim_limit"}, "receipt");
	if (requiredString(receipt, "schema", "receipt") != "flycast-research-capture-validation"
			|| requiredUnsigned(receipt, "schema_version", "receipt") != 1
			|| !requiredBool(receipt, "accepted", "receipt"))
		invalid("capture validation receipt is not an accepted v1 receipt");
	requiredString(receipt, "validated_utc", "receipt");
	requiredString(receipt, "claim_limit", "receipt");
	const Blob receiptTranscript = parseBlob(receipt.at("transcript"), "receipt.transcript", false);
	const Blob receiptArtifact = parseBlob(receipt.at("artifact"), "receipt.artifact", false);
	requireBlobEqual(receiptTranscript, transcript, "receipt transcript");
	requireBlobEqual(receiptArtifact, artifact, "receipt artifact");
	const Blob receiptValidator = parseBlob(receipt.at("validator"), "receipt.validator", true);
	requireBlobEqual(receiptValidator, expectedValidator, "receipt validator");
}

std::string utcNow()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t time = std::chrono::system_clock::to_time_t(now);
	std::tm utc {};
#ifdef _WIN32
	gmtime_s(&utc, &time);
#else
	gmtime_r(&time, &utc);
#endif
	std::ostringstream output;
	output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return output.str();
}

void writeReceipt(const std::filesystem::path& package, const std::filesystem::path& receiptPath,
		const Blob& transcript, const Blob& artifact, const Blob& validator)
{
	if (!samePath(receiptPath, package / "capture-validation.json"))
		invalid("receipt path must be <package>/capture-validation.json");
	if (std::filesystem::exists(receiptPath))
		invalid("receipt output already exists");
	const json receipt {
		{"schema", "flycast-research-capture-validation"},
		{"schema_version", 1},
		{"accepted", true},
		{"validated_utc", utcNow()},
		{"transcript", {
			{"path", transcript.pathText}, {"size", transcript.size}, {"sha256", transcript.sha256},
		}},
		{"artifact", {
			{"path", artifact.pathText}, {"size", artifact.size}, {"sha256", artifact.sha256},
		}},
		{"validator", {
			{"path", validator.pathText}, {"size", validator.size}, {"sha256", validator.sha256},
		}},
		{"claim_limit", "Self-contained consistency receipt for the exact v1 transcript, source identities, and Maple artifact; not a signature or hostile-writer attestation."},
	};
	std::filesystem::path temporary = receiptPath;
	temporary += ".tmp";
	if (std::filesystem::exists(temporary))
		invalid("receipt temporary output already exists");
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::out);
		if (!output)
			invalid("cannot create receipt temporary output");
		const std::string bytes = receipt.dump(2) + "\n";
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if (!output)
			invalid("cannot write receipt temporary output");
	}
	std::error_code error;
	std::filesystem::rename(temporary, receiptPath, error);
	if (error)
	{
		std::filesystem::remove(temporary);
		invalid("cannot atomically publish validation receipt: " + error.message());
	}
}

struct ValidationResult
{
	Blob transcript;
	Blob artifact;
	Blob captureValidator;
};

ValidationResult validatePackage(const std::filesystem::path& package, bool issuingReceipt)
{
	std::error_code error;
	if (!std::filesystem::is_directory(package, error) || error)
		invalid("package directory does not exist");
	if (std::filesystem::exists(package / ".runtime"))
		invalid("private runtime is present in an accepted candidate");

	const std::filesystem::path transcriptPath = package / "transcript.json";
	const json transcript = parseJsonFile(transcriptPath, "transcript");
	requireAllowedKeys(transcript, {"schema", "schema_version", "accepted", "capture_id",
			"started_utc", "ended_utc", "elapsed_ms", "job", "identity_manifest",
			"source_authentication", "process", "terminal_stability", "artifact", "validator",
			"capture_validator", "runtime_state", "publication", "retained_processes",
			"records", "failure",
			"cleanup_failure", "claim_limit"}, "transcript");
	if (requiredString(transcript, "schema", "transcript")
				!= "flycast-research-capture-transcript"
			|| requiredUnsigned(transcript, "schema_version", "transcript") != 1)
		invalid("unsupported capture transcript schema");
	if (!requiredBool(transcript, "accepted", "transcript"))
		invalid("transcript is rejected");
	const std::string captureId = requiredString(transcript, "capture_id", "transcript");
	if (!isCaptureId(captureId))
		invalid("transcript.capture_id is not a lowercase UUID");
	requiredString(transcript, "started_utc", "transcript");
	requiredString(transcript, "ended_utc", "transcript");
	requiredUnsigned(transcript, "elapsed_ms", "transcript");
	requiredString(transcript, "claim_limit", "transcript");
	requiredArray(transcript, "records", "transcript");
	if (!requiredArray(transcript, "retained_processes", "transcript").empty())
		invalid("accepted transcript retains a live process");
	if (!transcript.contains("failure") || !transcript.at("failure").is_null()
			|| !transcript.contains("cleanup_failure") || !transcript.at("cleanup_failure").is_null())
		invalid("accepted transcript contains a failure");

	const Blob declaredJob = parseBlob(transcript.at("job"), "transcript.job", false);
	const std::filesystem::path jobPath = resolvePackagePath(package, declaredJob.pathText,
			"transcript.job.path");
	const Blob actualJob = fileIdentity(jobPath, declaredJob.pathText, MaxCaptureJsonBytes);
	requireBlobEqual(actualJob, declaredJob, "packaged job");
	const json job = parseJsonFile(jobPath, "job");
	validateJobShape(job);
	const std::string artifactName = job.at("output").at("artifact_name").get<std::string>();
	const std::filesystem::path acceptedDirectory = canonicalAbsolutePath(
			pathFromUtf8(job.at("output").at("accepted_directory").get<std::string>()),
			"job.output.accepted_directory");
	const std::string expectedCandidateName = ".flycast-research-candidate-" + captureId;
	const std::filesystem::path expectedCandidate =
			acceptedDirectory.parent_path() / expectedCandidateName;
	std::set<std::string> expectedEntries {
		"job.json", "identity.json", artifactName, "transcript.json",
	};
	if (!issuingReceipt)
		expectedEntries.insert("capture-validation.json");
	requireExactPackageEntries(package, expectedEntries);

	const Blob declaredIdentity = parseBlob(transcript.at("identity_manifest"),
			"transcript.identity_manifest", false);
	const std::filesystem::path identityPath = resolvePackagePath(package,
			declaredIdentity.pathText, "transcript.identity_manifest.path");
	const Blob actualIdentity = fileIdentity(identityPath, declaredIdentity.pathText,
			MaxCaptureJsonBytes);
	requireBlobEqual(actualIdentity, declaredIdentity, "packaged identity");
	const Blob jobIdentity = parseBlob(job.at("identity_manifest"), "job.identity_manifest", true);
	if (declaredIdentity.size != jobIdentity.size || declaredIdentity.sha256 != jobIdentity.sha256)
		invalid("packaged identity does not match job identity");
	const research::IdentityManifest loadedIdentity = research::loadIdentityManifest(identityPath);
	research::requireCaptureV1Identity(loadedIdentity);
	const json identity = parseJsonFile(identityPath, "identity");
	validateIdentityResearchSettingOwnership(identity);

	const json& authentication = requiredObject(transcript, "source_authentication", "transcript");
	if (!requiredBool(authentication, "matched", "transcript.source_authentication"))
		invalid("source authentication did not match");
	const std::vector<SourceIdentity> before = parseSourceArray(authentication.at("before"),
			"transcript.source_authentication.before");
	const std::vector<SourceIdentity> after = parseSourceArray(authentication.at("after"),
			"transcript.source_authentication.after");
	const std::vector<SourceIdentity> expected = gatherExpectedSources(job, identity, actualJob, before);
	requireSourceSets(before, after, expected);

	const json& process = requiredObject(transcript, "process", "transcript");
	requireAllowedKeys(process, {"source_executable", "staged_executable", "pid",
			"start_utc_ticks", "command_line", "command_line_sha256", "arguments",
			"stdio_policy", "natural_exit", "termination_requested", "termination_verified",
			"exit_code"}, "transcript.process");
	const Blob sourceExecutable = parseBlob(process.at("source_executable"),
			"transcript.process.source_executable", true);
	const Blob identityExecutable = parseBlob(identity.at("emulator").at("executable"),
			"identity.emulator.executable", true);
	requireBlobEqual(sourceExecutable, identityExecutable, "process source executable");
	const Blob stagedExecutable = parseBlob(process.at("staged_executable"),
			"transcript.process.staged_executable", true);
	if (stagedExecutable.size != sourceExecutable.size
			|| stagedExecutable.sha256 != sourceExecutable.sha256)
		invalid("staged executable bytes do not match the authenticated source");
	const std::filesystem::path expectedStagedExecutable = expectedCandidate / ".runtime"
			/ pathFromUtf8(sourceExecutable.pathText).filename();
	if (!samePath(stagedExecutable.pathText, expectedStagedExecutable))
		invalid("staged executable path is outside the declared private runtime");
	const std::filesystem::path recordedCandidate =
			pathFromUtf8(stagedExecutable.pathText).parent_path().parent_path();
	if (!samePath(recordedCandidate, expectedCandidate))
		invalid("staged executable does not identify the declared private candidate");
	if (requiredUnsigned(process, "pid", "transcript.process") == 0
			|| requiredUnsigned(process, "start_utc_ticks", "transcript.process") == 0)
		invalid("process ownership tuple is incomplete");
	const std::string commandLine = requiredString(process, "command_line", "transcript.process");
	const std::string commandDigest = requiredString(process, "command_line_sha256",
			"transcript.process");
	if (research::sha256ToHex(research::sha256(commandLine.data(), commandLine.size()))
			!= commandDigest)
		invalid("process command-line digest mismatch");
	if (requiredString(process, "stdio_policy", "transcript.process")
			!= "inherited_not_evidence")
		invalid("process stdio policy is not the v1 bounded-memory policy");

	std::vector<std::string> expectedEmulatorArguments;
	const json& emulatorPrefix = requiredStringArray(job.at("emulator"), "arguments_prefix",
			"job.emulator", 64);
	for (const json& argument : emulatorPrefix)
		expectedEmulatorArguments.push_back(argument.get<std::string>());
	// Windows path identity is case-insensitive, but the argument vector and the
	// operating-system command line preserve the spelling used at launch.
	const std::string expectedIdentityPath = pathToUtf8(recordedCandidate / "identity.json");
	const std::string expectedArtifactPath = pathToUtf8(recordedCandidate / artifactName);
	const std::string researchConfiguration = "research:IdentityManifest="
			+ quoteFlycastConfigValue(expectedIdentityPath) + ",research:MapleRecord="
			+ quoteFlycastConfigValue(expectedArtifactPath) + ",research:MapleTraceMaxBytes="
			+ std::to_string(job.at("limits").at("trace_max_bytes").get<std::uint64_t>());
	expectedEmulatorArguments.push_back("-config");
	expectedEmulatorArguments.push_back(researchConfiguration);
	const json& configuration = requiredObject(identity, "configuration", "identity");
	const json& configurationValues = requiredObject(configuration, "values",
			"identity.configuration");
	if (configurationValues.contains("flycast_transient"))
	{
		if (!configurationValues.at("flycast_transient").is_array()
				|| configurationValues.at("flycast_transient").size() > 128)
			invalid("identity.configuration.values.flycast_transient must be a bounded array");
		for (const json& setting : configurationValues.at("flycast_transient"))
		{
			if (!setting.is_string())
				invalid("identity.configuration.values.flycast_transient contains a non-string setting");
			if (isEmptyOrDotNetWhitespaceOnly(setting.get<std::string>()))
				invalid("identity.configuration.values.flycast_transient contains an empty or whitespace-only setting");
			expectedEmulatorArguments.push_back("-config");
			expectedEmulatorArguments.push_back(setting.get<std::string>());
		}
	}
	const Blob mediaSource = parseBlob(identity.at("media").at("source"),
			"identity.media.source", true);
	expectedEmulatorArguments.push_back(mediaSource.pathText);

	const json& recordedArguments = requiredStringArray(process, "arguments",
			"transcript.process", 512);
	if (recordedArguments.size() != expectedEmulatorArguments.size())
		invalid("process argument count does not match the job and candidate");
	for (std::size_t i = 0; i < expectedEmulatorArguments.size(); ++i)
		if (recordedArguments.at(i).get<std::string>() != expectedEmulatorArguments[i])
			invalid("process argument mismatch at index " + std::to_string(i));

	const std::vector<std::string> parsedCommandLine = parseWindowsCommandLine(commandLine);
	if (parsedCommandLine.empty()
			|| !samePath(parsedCommandLine.front(), stagedExecutable.pathText))
		invalid("operating-system command line does not identify the staged executable");
	if (parsedCommandLine.size() != expectedEmulatorArguments.size() + 1)
		invalid("operating-system command-line argument count mismatch");
	for (std::size_t i = 0; i < expectedEmulatorArguments.size(); ++i)
		if (parsedCommandLine.at(i + 1) != expectedEmulatorArguments[i])
			invalid("operating-system command-line argument mismatch at index "
					+ std::to_string(i));
	if (!requiredBool(process, "natural_exit", "transcript.process")
			|| requiredBool(process, "termination_requested", "transcript.process")
			|| !requiredBool(process, "termination_verified", "transcript.process")
			|| requiredSigned(process, "exit_code", "transcript.process") != 0)
		invalid("process did not reach an accepted natural terminal state");

	const std::uint64_t maximumBytes = job.at("limits").at("trace_max_bytes").get<std::uint64_t>();
	const json& stability = requiredObject(transcript, "terminal_stability", "transcript");
	if (!requiredBool(stability, "matched", "transcript.terminal_stability"))
		invalid("terminal samples did not match");
	if (requiredUnsigned(stability, "interval_ms", "transcript.terminal_stability")
			!= job.at("limits").at("stability_interval_ms").get<std::uint64_t>())
		invalid("terminal stability interval differs from the job");
	const json& first = requiredObject(stability, "first", "transcript.terminal_stability");
	const json& second = requiredObject(stability, "second", "transcript.terminal_stability");
	for (const auto& sample : {std::pair<const json *, std::string>(&first, "first"),
			std::pair<const json *, std::string>(&second, "second")})
	{
		if (!requiredBool(*sample.first, "exists", sample.second)
				|| !requiredBool(*sample.first, "process_exited", sample.second)
				|| requiredSigned(*sample.first, "exit_code", sample.second) != 0
				|| requiredString(*sample.first, "path", sample.second) != artifactName)
			invalid("terminal " + sample.second + " sample is not accepted");
	}
	for (const char *field : {"size", "last_write_utc_ticks", "exit_code"})
		if (first.at(field) != second.at(field))
			invalid(std::string("terminal sample field changed: ") + field);
	if (first.at("sha256") != second.at("sha256"))
		invalid("terminal artifact digest changed");

	const Blob declaredArtifact = parseBlob(transcript.at("artifact"),
			"transcript.artifact", false);
	if (declaredArtifact.pathText != artifactName
			|| declaredArtifact.size != first.at("size").get<std::uint64_t>()
			|| declaredArtifact.sha256 != first.at("sha256").get<std::string>())
		invalid("terminal samples do not bind the declared artifact");
	const std::filesystem::path artifactPath = resolvePackagePath(package,
			declaredArtifact.pathText, "transcript.artifact.path");
	const Blob actualArtifact = fileIdentity(artifactPath, declaredArtifact.pathText, maximumBytes);
	requireBlobEqual(actualArtifact, declaredArtifact, "packaged artifact");

	const json& validator = requiredObject(transcript, "validator", "transcript");
	requireAllowedKeys(validator, {"executable", "arguments", "pid", "start_utc_ticks",
			"exit_code", "stdout_size", "stdout_sha256", "stdout_total_size",
			"stdout_truncated", "stderr_size", "stderr_sha256", "stderr_total_size",
			"stderr_truncated"}, "transcript.validator");
	const Blob validatorExecutable = parseBlob(validator.at("executable"),
			"transcript.validator.executable", true);
	const Blob jobValidator = parseBlob(job.at("validator").at("executable"),
			"job.validator.executable", true);
	requireBlobEqual(validatorExecutable, jobValidator, "Maple validator executable");
	if (requiredSigned(validator, "exit_code", "transcript.validator") != 0)
		invalid("Maple validator rejected the candidate");
	if (requiredUnsigned(validator, "pid", "transcript.validator") == 0
			|| requiredUnsigned(validator, "start_utc_ticks", "transcript.validator") == 0)
		invalid("Maple validator ownership tuple is incomplete");
	for (const char *stream : {"stdout", "stderr"})
	{
		const std::string sizeField = std::string(stream) + "_size";
		const std::string totalField = std::string(stream) + "_total_size";
		const std::string digestField = std::string(stream) + "_sha256";
		const std::string truncatedField = std::string(stream) + "_truncated";
		const std::uint64_t captured = requiredUnsigned(validator, sizeField.c_str(),
				"transcript.validator");
		const std::uint64_t total = requiredUnsigned(validator, totalField.c_str(),
				"transcript.validator");
		if (captured > 1048576 || total < captured
				|| requiredBool(validator, truncatedField.c_str(), "transcript.validator")
						!= (total > captured))
			invalid("Maple validator bounded " + std::string(stream) + " accounting is inconsistent");
		if (!isLowerSha256(requiredString(validator, digestField.c_str(),
				"transcript.validator")))
			invalid("Maple validator " + std::string(stream) + " digest is invalid");
	}
	const json& validatorArguments = requiredArray(validator, "arguments", "transcript.validator");
	const json& prefix = job.at("validator").at("arguments_prefix");
	if (validatorArguments.size() != prefix.size() + 6)
		invalid("Maple validator argument count mismatch");
	for (std::size_t i = 0; i < prefix.size(); ++i)
		if (validatorArguments.at(i) != prefix.at(i))
			invalid("Maple validator argument prefix mismatch");
	const std::size_t offset = prefix.size();
	if (validatorArguments.at(offset) != "--trace"
			|| !validatorArguments.at(offset + 1).is_string()
			|| !samePath(validatorArguments.at(offset + 1).get<std::string>(),
					expectedCandidate / artifactName)
			|| validatorArguments.at(offset + 2) != "--identity"
			|| !validatorArguments.at(offset + 3).is_string()
			|| !samePath(validatorArguments.at(offset + 3).get<std::string>(),
					expectedCandidate / "identity.json")
			|| validatorArguments.at(offset + 4) != "--max-bytes"
			|| !validatorArguments.at(offset + 5).is_string()
			|| validatorArguments.at(offset + 5).get<std::string>() != std::to_string(maximumBytes))
		invalid("Maple validator arguments do not bind the candidate");

	const Blob captureValidator = parseBlob(transcript.at("capture_validator"),
			"transcript.capture_validator", true);
	const Blob jobCaptureValidator = parseBlob(job.at("capture_validator").at("executable"),
			"job.capture_validator.executable", true);
	requireBlobEqual(captureValidator, jobCaptureValidator, "capture validator executable");

	const json& runtimeState = requiredObject(transcript, "runtime_state", "transcript");
	validateRuntimeState(runtimeState, identity, expectedCandidate);

	const json& publication = requiredObject(transcript, "publication", "transcript");
	const std::string candidateName = requiredString(publication,
			"candidate_directory_name", "transcript.publication");
	if (candidateName != expectedCandidateName)
		invalid("publication candidate name does not match the capture ID");
	if (!publication.contains("quarantine_directory")
			|| !publication.at("quarantine_directory").is_null())
		invalid("accepted publication has a quarantine directory");
	const std::filesystem::path transcriptAcceptedDirectory = pathFromUtf8(
			requiredString(publication, "accepted_directory", "transcript.publication"));
	if (!requiredBool(publication, "same_volume", "transcript.publication")
			|| requiredString(publication, "state", "transcript.publication")
					!= "ready_for_atomic_publication"
			|| !samePath(transcriptAcceptedDirectory, acceptedDirectory))
		invalid("publication contract mismatch");
	if (issuingReceipt)
	{
		if (!samePath(package, expectedCandidate))
			invalid("receipt may only be issued inside the declared private candidate");
		if (std::filesystem::exists(acceptedDirectory))
			invalid("accepted destination appeared before receipt issuance");
	}
	else if (!samePath(package, acceptedDirectory))
	{
		invalid("published package is not at its declared accepted directory");
	}

	research::validateProductionMapleTraceFile(artifactPath, loadedIdentity.digest, maximumBytes);
	const Blob actualTranscript = fileIdentity(transcriptPath, "transcript.json", MaxCaptureJsonBytes);
	if (!issuingReceipt)
		validateReceipt(package, actualTranscript, actualArtifact, captureValidator);
	return ValidationResult {actualTranscript, actualArtifact, captureValidator};
}

} // namespace

int main(int argc, char **argv)
{
	std::filesystem::path package;
	std::filesystem::path receipt;
	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if (argument == "--package" && i + 1 < argc)
			package = argv[++i];
		else if (argument == "--receipt" && i + 1 < argc)
			receipt = argv[++i];
		else
		{
			usage(argv[0]);
			return 2;
		}
	}
	if (package.empty())
	{
		usage(argv[0]);
		return 2;
	}
	try
	{
		package = std::filesystem::weakly_canonical(package);
		const bool issuingReceipt = !receipt.empty();
		const ValidationResult result = validatePackage(package, issuingReceipt);
		if (!receipt.empty())
		{
			const std::filesystem::path executable = std::filesystem::weakly_canonical(
					std::filesystem::absolute(argv[0]));
			const Blob runningValidator = fileIdentity(executable, pathToUtf8(executable));
			requireBlobEqual(runningValidator, result.captureValidator,
					"running capture validator");
			writeReceipt(package, std::filesystem::absolute(receipt), result.transcript,
					result.artifact, runningValidator);
		}
		std::printf("accepted flycast research capture: %s\n", pathToUtf8(package).c_str());
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "%s\n", exception.what());
		return 1;
	}
}
