// The kernSQL shell: a SQL REPL over one database file (DD-008, "Shell").
//
// Input is buffered until it holds a complete statement — a `;` token, so a `;` inside a string
// or a comment does not count — and every complete statement is run in turn, so several can share
// a line and one can span many. Lines starting with `.` are shell commands, as in sqlite3, and
// only when no statement is half-typed.
//
// Everything starts from Database (DD-006): DiskManager -> BufferPoolManager -> Catalog, owned
// and torn down in that order. The executor holds no pin between statements, so the rule from
// DD-003 that a pin never spans a client round-trip holds by construction: nothing is pinned
// while the prompt waits.
//
// DURABILITY: every statement that may have changed data is followed by Database::Checkpoint()
// (flush + fsync) BEFORE its result is printed, so what the screen says happened is on disk. The
// engine itself is durable only at Shutdown (DD-002); this is the shell's autocommit, as sqlite3
// commits each statement. Ctrl-C cancels the statement being typed rather than killing the
// process, as in psql — a kill without Shutdown used to drop everything since the last flush.
//
// The old hand-driving REPL (raw page reads and writes) is gone: through SQL there is no way to
// write a table's heap without its index, which is the point.

#include <signal.h>

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

#include "buffer/pool_stats.hpp"
#include "catalog/catalog.hpp"
#include "catalog/database.hpp"
#include "catalog/schema.hpp"
#include "catalog/table.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "sql/executor/executor.hpp"

using namespace kernsql;

namespace {

// Set by SIGINT. The handler only sets it: everything else happens back in the loop.
volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void OnInterrupt(int /*signal*/) {
	g_interrupted = 1;
}

// Ctrl-C interrupts the blocking read instead of ending the process. No SA_RESTART, so read()
// returns EINTR and std::getline fails — the loop then tells that apart from EOF by the flag.
void InstallInterruptHandler() {
	struct sigaction action{};
	action.sa_handler = OnInterrupt;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, nullptr);
}

std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

std::string_view ErrorCodeName(ErrorCode code) {
	switch (code) {
		case ErrorCode::kNotFound:
			return "not found";
		case ErrorCode::kInvalidArgument:
			return "error";
		case ErrorCode::kDuplicateKey:
			return "duplicate";
		case ErrorCode::kCorruption:
			return "CORRUPTION";
		case ErrorCode::kIOError:
			return "I/O error";
		default:
			return "error";
	}
}

// SELECT and EXPLAIN only read. Everything else may have written pages — even a failed INSERT
// can have stored some rows before a storage error (DD-008, "Atomicity").
bool IsReadOnly(const Result<QueryResult>& result) {
	return result.has_value() &&
	       (result->tag.starts_with("SELECT") || result->tag.starts_with("EXPLAIN"));
}

void RunStatement(Database& db, Executor& executor, std::string_view sql) {
	Result<QueryResult> result = executor.Execute(sql);
	if (!IsReadOnly(result)) {
		// Before printing: "CREATE TABLE" on screen must mean it is on disk.
		if (Status st = db.Checkpoint(); !st.ok()) {
			std::println("checkpoint failed — the last statement may not be durable: {}",
			             st.message());
		}
	}
	if (!result.has_value()) {
		std::println("{}: {}", ErrorCodeName(result.error().code()), result.error().message());
		return;
	}
	std::println("{}", FormatResult(*result));
}

void PrintHelp() {
	std::println("SQL — end each statement with ';' (it may span lines):");
	std::println("  CREATE TABLE t (id INT64 PRIMARY KEY, name VARCHAR(32));");
	std::println("  INSERT INTO t VALUES (1, 'alice'), (2, 'bob');");
	std::println("  SELECT name FROM t WHERE id >= 1 AND name != 'bob';");
	std::println("  DELETE FROM t WHERE id = 2;");
	std::println("  EXPLAIN SELECT * FROM t WHERE id = 1;");
	std::println("shell commands:");
	std::println("  .tables          list tables");
	std::println("  .schema <t>      a table's CREATE TABLE");
	std::println("  .check <t>       heap <-> index consistency + tree invariants");
	std::println("  .stats           file and buffer-pool counters");
	std::println("  .help            this list");
	std::println("  .quit            shut down (flush + fsync) and exit");
	std::println("every statement that changes data is on disk before its result prints;");
	std::println("Ctrl-C cancels the statement being typed, Ctrl-D quits.");
}

void PrintSchema(const Table& table) {
	const Schema& schema = table.GetSchema();
	std::string out = "CREATE TABLE " + table.Name() + " (";
	for (std::size_t i = 0; i < schema.ColumnCount(); ++i) {
		const Column& column = schema.Columns()[i];
		if (i > 0) out += ", ";
		out += column.name;
		out += column.type == ColumnType::kInt64
		           ? " INT64"
		           : " VARCHAR(" + std::to_string(column.max_length) + ")";
		if (i == schema.PrimaryKeyColumn()) out += " PRIMARY KEY";
	}
	std::println("{});", out);
}

// Returns false when the shell should stop.
bool RunShellCommand(Database& db, const std::filesystem::path& path, std::string_view line) {
	const std::size_t space = line.find_first_of(" \t");
	const std::string_view cmd = line.substr(0, space);
	const std::string_view arg =
	    space == std::string_view::npos ? std::string_view{} : Trim(line.substr(space));
	Catalog& catalog = db.GetCatalog();

	const auto table_arg = [&]() -> Table* {
		Result<Table*> table = catalog.GetTable(arg);
		if (!table.has_value()) {
			std::println("{}: no such table '{}'", cmd, arg);
			return nullptr;
		}
		return *table;
	};

	if (cmd == ".quit" || cmd == ".exit") return false;
	if (cmd == ".help") {
		PrintHelp();
	} else if (cmd == ".tables") {
		for (const std::string& name : catalog.TableNames()) std::println("{}", name);
	} else if (cmd == ".schema") {
		if (Table* table = table_arg()) PrintSchema(*table);
	} else if (cmd == ".check") {
		if (Table* table = table_arg())
			std::println("{}: {}", table->Name(),
			             table->CheckConsistency() ? "ok" : "INCONSISTENT (see log)");
	} else if (cmd == ".stats") {
		std::error_code ec;
		const auto bytes = std::filesystem::file_size(path, ec);
		const PoolStats st = db.Pool().GetStats();
		std::println("file: {} pages", ec ? 0 : bytes / PAGE_SIZE);
		std::println("pool: {} frames, {} free, {} resident, {} pinned", st.capacity,
		             st.free_frames, st.resident_frames, st.pinned_frames);
	} else {
		std::println("unknown command '{}' — try .help", cmd);
	}
	return true;
}

}  // namespace

int main(int argc, char** argv) {
	const std::filesystem::path db_path =
	    (argc > 1) ? std::filesystem::path(argv[1])
	               : std::filesystem::temp_directory_path() / "kernsql.db";

	auto db = Database::Open(db_path);
	if (!db) {
		std::println(stderr, "cannot open {}: {}", db_path.string(), db.error().message());
		return 1;
	}
	Executor executor((*db)->GetCatalog());
	InstallInterruptHandler();

	std::println("kernSQL — {} ({} tables)", db_path.string(), (*db)->GetCatalog().TableCount());
	std::println("end statements with ';'. `.help` for commands, `.quit` to shut down cleanly.");

	std::string buffer;  // a statement being typed, across lines
	std::string line;
	while (true) {
		g_interrupted = 0;
		std::print("{}", buffer.empty() ? "kernsql> " : "    ...> ");
		std::cout.flush();
		if (!std::getline(std::cin, line)) {
			if (g_interrupted) {
				// Ctrl-C: drop the half-typed statement and start a fresh prompt.
				std::cin.clear();
				clearerr(stdin);
				buffer.clear();
				std::println("");
				continue;
			}
			// std::println("") rather than std::println(): the zero-argument overload is P3142,
			// C++26, and libc++ 18 (what CI builds with) does not have it.
			std::println("");  // EOF (ctrl-D): a clean quit; a half-typed statement is dropped
			break;
		}
		if (buffer.empty() && Trim(line).starts_with('.')) {
			if (!RunShellCommand(**db, db_path, Trim(line))) break;
			continue;
		}

		buffer += line;
		buffer += '\n';
		SplitInput split = SplitStatements(buffer);
		for (const std::string& statement : split.statements) {
			RunStatement(**db, executor, statement);
		}
		buffer = std::move(split.remainder);
	}

	// Shutdown() is the durable operation, not the destructor (DD-002). Nothing is pinned: every
	// statement's operators and guards died inside Execute.
	Status st = (*db)->Shutdown();
	if (!st.ok()) {
		std::println(stderr, "shutdown failed: {}", st.message());
		return 1;
	}
	std::println("shutdown ok");
	return 0;
}
