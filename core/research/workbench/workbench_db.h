#pragma once

// Thin RAII wrapper over the SQLite C API used by the workbench recorder.
// Errors surface as std::runtime_error carrying the SQLite message.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace research::workbench
{

class Statement
{
public:
	Statement() = default;
	Statement(sqlite3 *db, std::string_view sql);
	~Statement();
	Statement(Statement&& other) noexcept;
	Statement& operator=(Statement&& other) noexcept;
	Statement(const Statement&) = delete;
	Statement& operator=(const Statement&) = delete;

	// Bind helpers. Parameter indexes are 1-based like SQLite's.
	Statement& bindInt(int index, std::int64_t value);
	Statement& bindUInt(int index, std::uint64_t value);
	// UINT32_MAX is the "absent" sentinel used throughout the observation
	// structs; it becomes SQL NULL so joins stay clean.
	Statement& bindOptionalU32(int index, std::uint32_t value);
	// Zero generations mean "not observed" and become NULL.
	Statement& bindGeneration(int index, std::uint64_t generation);
	Statement& bindBool(int index, bool value);
	Statement& bindDouble(int index, double value);
	Statement& bindText(int index, std::string_view value);
	Statement& bindBlob(int index, const void *bytes, std::size_t size);
	Statement& bindNull(int index);

	// Runs an INSERT/UPDATE to completion, then resets and clears bindings.
	void execute();
	// Query iteration: true while a row is available. Call reset() when done.
	bool step();
	void reset();

	std::int64_t columnInt64(int column) const;
	double columnDouble(int column) const;
	std::string columnText(int column) const;
	std::string columnBlob(int column) const;
	bool columnIsNull(int column) const;

	bool valid() const { return statement != nullptr; }

private:
	void check(int rc, const char *what) const;

	sqlite3 *db = nullptr;
	sqlite3_stmt *statement = nullptr;
};

class Database
{
public:
	// Opens (creating if needed) a database in WAL mode with NORMAL sync so a
	// reader process can query while the recorder is still writing.
	explicit Database(const std::filesystem::path& path);
	~Database();
	Database(const Database&) = delete;
	Database& operator=(const Database&) = delete;

	void exec(std::string_view sql);
	Statement prepare(std::string_view sql);
	void begin();
	void commit();
	void rollback() noexcept;
	std::int64_t lastInsertRowId() const;
	// Convenience for small key/value scalar queries in tests and status.
	std::int64_t queryInt64(std::string_view sql);

	sqlite3 *handle() const { return db; }
	const std::filesystem::path& path() const { return databasePath; }

private:
	sqlite3 *db = nullptr;
	std::filesystem::path databasePath;
};

// "YYYY-MM-DDTHH:MM:SSZ" for the current wall clock.
std::string utcNowIso8601();

} // namespace research::workbench
