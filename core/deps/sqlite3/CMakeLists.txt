# SQLite amalgamation (public domain), used by the research workbench writer.
add_library(sqlite3 STATIC sqlite3.c sqlite3.h)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
	SQLITE_THREADSAFE=1
	SQLITE_OMIT_LOAD_EXTENSION
	SQLITE_OMIT_DEPRECATED
	SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
	SQLITE_ENABLE_JSON1)
if(MSVC)
	target_compile_options(sqlite3 PRIVATE /w)
else()
	target_compile_options(sqlite3 PRIVATE -w)
endif()
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(SQLite::SQLite3 ALIAS sqlite3)
