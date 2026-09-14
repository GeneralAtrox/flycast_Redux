#include "research/workbench/workbench_db.h"

#include "sqlite3.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>

namespace research::workbench
{
namespace
{

std::string pathUtf8(const std::filesystem::path& path)
{
#ifdef __cpp_char8_t
	const std::u8string utf8 = path.u8string();
	return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
#else
	return path.u8string();
#endif
}

[[noreturn]] void fail(sqlite3 *db, const char *what)
{
	const char *message = db != nullptr ? sqlite3_errmsg(db) : "unknown SQLite error";
	throw std::runtime_error(std::string(what) + ": " + message);
}

} // namespace

// ---------------------------------------------------------------- Statement

Statement::Statement(sqlite3 *db, std::string_view sql)
	: db(db)
{
	const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
			&statement, nullptr);
	if (rc != SQLITE_OK)
		throw std::runtime_error(std::string("SQLite prepare failed: ")
				+ sqlite3_errmsg(db) + " in: " + std::string(sql));
}

Statement::~Statement()
{
	if (statement != nullptr)
		sqlite3_finalize(statement);
}

Statement::Statement(Statement&& other) noexcept
	: db(other.db), statement(other.statement)
{
	other.statement = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept
{
	if (this != &other)
	{
		if (statement != nullptr)
			sqlite3_finalize(statement);
		db = other.db;
		statement = other.statement;
		other.statement = nullptr;
	}
	return *this;
}

void Statement::check(int rc, const char *what) const
{
	if (rc != SQLITE_OK)
		fail(db, what);
}

Statement& Statement::bindInt(int index, std::int64_t value)
{
	check(sqlite3_bind_int64(statement, index, value), "bind int");
	return *this;
}

Statement& Statement::bindUInt(int index, std::uint64_t value)
{
	// Ticks and ordinals never approach 2^63 in practice; store as signed.
	return bindInt(index, static_cast<std::int64_t>(value));
}

Statement& Statement::bindOptionalU32(int index, std::uint32_t value)
{
	if (value == std::numeric_limits<std::uint32_t>::max())
		return bindNull(index);
	return bindInt(index, value);
}

Statement& Statement::bindGeneration(int index, std::uint64_t generation)
{
	if (generation == 0)
		return bindNull(index);
	return bindUInt(index, generation);
}

Statement& Statement::bindBool(int index, bool value)
{
	return bindInt(index, value ? 1 : 0);
}

Statement& Statement::bindDouble(int index, double value)
{
	check(sqlite3_bind_double(statement, index, value), "bind double");
	return *this;
}

Statement& Statement::bindText(int index, std::string_view value)
{
	check(sqlite3_bind_text(statement, index, value.data(),
			static_cast<int>(value.size()), SQLITE_TRANSIENT), "bind text");
	return *this;
}

Statement& Statement::bindBlob(int index, const void *bytes, std::size_t size)
{
	if (size == 0)
		return bindNull(index);
	check(sqlite3_bind_blob64(statement, index, bytes, size, SQLITE_TRANSIENT), "bind blob");
	return *this;
}

Statement& Statement::bindNull(int index)
{
	check(sqlite3_bind_null(statement, index), "bind null");
	return *this;
}

void Statement::execute()
{
	const int rc = sqlite3_step(statement);
	if (rc != SQLITE_DONE && rc != SQLITE_ROW)
	{
		sqlite3_reset(statement);
		fail(db, "SQLite step failed");
	}
	sqlite3_reset(statement);
	sqlite3_clear_bindings(statement);
}

bool Statement::step()
{
	const int rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW)
		return true;
	if (rc == SQLITE_DONE)
		return false;
	fail(db, "SQLite step failed");
}

void Statement::reset()
{
	sqlite3_reset(statement);
	sqlite3_clear_bindings(statement);
}

std::int64_t Statement::columnInt64(int column) const
{
	return sqlite3_column_int64(statement, column);
}

double Statement::columnDouble(int column) const
{
	return sqlite3_column_double(statement, column);
}

std::string Statement::columnText(int column) const
{
	const unsigned char *text = sqlite3_column_text(statement, column);
	if (text == nullptr)
		return {};
	return std::string(reinterpret_cast<const char *>(text),
			static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

std::string Statement::columnBlob(int column) const
{
	const void *bytes = sqlite3_column_blob(statement, column);
	if (bytes == nullptr)
		return {};
	return std::string(static_cast<const char *>(bytes),
			static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

bool Statement::columnIsNull(int column) const
{
	return sqlite3_column_type(statement, column) == SQLITE_NULL;
}

// ----------------------------------------------------------------- Database

Database::Database(const std::filesystem::path& path)
	: databasePath(path)
{
	const int rc = sqlite3_open_v2(pathUtf8(path).c_str(), &db,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
	if (rc != SQLITE_OK)
	{
		const std::string message = db != nullptr ? sqlite3_errmsg(db) : "cannot allocate";
		if (db != nullptr)
			sqlite3_close(db);
		db = nullptr;
		throw std::runtime_error("cannot open workbench database " + path.string()
				+ ": " + message);
	}
	sqlite3_busy_timeout(db, 5000);
	exec("PRAGMA journal_mode=WAL");
	exec("PRAGMA synchronous=NORMAL");
	exec("PRAGMA temp_store=MEMORY");
}

Database::~Database()
{
	if (db != nullptr)
		sqlite3_close(db);
}

void Database::exec(std::string_view sql)
{
	char *error = nullptr;
	const std::string text(sql);
	const int rc = sqlite3_exec(db, text.c_str(), nullptr, nullptr, &error);
	if (rc != SQLITE_OK)
	{
		const std::string message = error != nullptr ? error : sqlite3_errmsg(db);
		sqlite3_free(error);
		throw std::runtime_error("SQLite exec failed: " + message + " in: " + text);
	}
}

Statement Database::prepare(std::string_view sql)
{
	return Statement(db, sql);
}

void Database::begin()
{
	exec("BEGIN");
}

void Database::commit()
{
	exec("COMMIT");
}

void Database::rollback() noexcept
{
	sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
}

std::int64_t Database::lastInsertRowId() const
{
	return sqlite3_last_insert_rowid(db);
}

std::int64_t Database::queryInt64(std::string_view sql)
{
	Statement statement = prepare(sql);
	if (!statement.step())
		throw std::runtime_error("query returned no rows: " + std::string(sql));
	const std::int64_t value = statement.columnInt64(0);
	statement.reset();
	return value;
}

std::string utcNowIso8601()
{
	const std::time_t now = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now());
	std::tm parts {};
#ifdef _WIN32
	gmtime_s(&parts, &now);
#else
	gmtime_r(&now, &parts);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
	return buffer;
}

} // namespace research::workbench
