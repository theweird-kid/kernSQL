#include "sql/executor/executor.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "sql/executor/binder.hpp"
#include "sql/executor/operators.hpp"
#include "sql/executor/planner.hpp"
#include "sql/parser/parser.hpp"
#include "sql/parser/token.hpp"

using namespace kernsql;

namespace {

// Pull every row out of `plan`.
Result<std::vector<Row>> Drain(Operator& plan) {
	std::vector<Row> rows;
	Row row;
	while (true) {
		Result<bool> more = plan.Next(row);
		if (!more.has_value()) return std::unexpected(std::move(more.error()));
		if (!*more) return rows;
		rows.push_back(row);
	}
}

QueryResult ExplainResult(std::vector<std::string> lines) {
	QueryResult result{.columns = {"plan"}};
	for (std::string& line : lines) {
		result.rows.push_back(Row{Value{std::move(line)}});
	}
	result.tag = "EXPLAIN";
	return result;
}

// The plan for a DELETE, as EXPLAIN shows it: the scan, under a "Delete" line.
std::vector<std::string> ExplainDelete(const BoundDelete& del, const Operator& scan) {
	std::vector<std::string> lines{std::format("Delete: {}", del.table->Name())};
	for (std::string& line : ExplainLines(scan)) {
		lines.push_back("  " + line);
	}
	return lines;
}

Result<QueryResult> RunCreate(const BoundCreateTable& create, Catalog& catalog,
                              uint16_t index_fanout) {
	Result<Table*> table = catalog.CreateTable(create.name, create.schema, index_fanout);
	if (!table.has_value()) return std::unexpected(std::move(table.error()));
	return QueryResult{.tag = "CREATE TABLE"};
}

Result<QueryResult> RunInsert(const BoundInsert& insert) {
	// The binder has already checked every row and every key, so each Insert below can fail only
	// in the storage layers.
	for (const Row& row : insert.rows) {
		if (Status st = insert.table->Insert(row); !st.ok()) {
			return std::unexpected(std::move(st));
		}
	}
	return QueryResult{.tag = std::format("INSERT {}", insert.rows.size())};
}

Result<QueryResult> RunSelect(const BoundSelect& select, bool explain) {
	std::unique_ptr<Operator> plan = PlanSelect(select);
	if (explain) {
		return ExplainResult(ExplainLines(*plan));
	}

	QueryResult result;
	const std::vector<Column>& columns = select.table->GetSchema().Columns();
	if (select.projection.empty()) {
		for (const Column& column : columns) result.columns.push_back(column.name);
	} else {
		for (uint16_t index : select.projection) result.columns.push_back(columns[index].name);
	}

	Result<std::vector<Row>> rows = Drain(*plan);
	if (!rows.has_value()) return std::unexpected(std::move(rows.error()));
	result.rows = std::move(*rows);
	result.tag = std::format("SELECT {}", result.rows.size());
	return result;
}

/*
 * COLLECT, THEN MUTATE. The scan holds a read latch on the page it is on, and Table::Delete
 * write-latches pages — deleting while the scan is alive would wait on our own latch forever
 * (operators.hpp). So: run the scan to the end collecting primary keys, destroy it, then delete.
 *
 * This is also the Halloween problem's fix (System R, 1976): a statement that modifies rows while
 * scanning them can meet its own changes. Collecting first means the set of rows to delete is
 * fixed before the first one goes.
 */
Result<QueryResult> RunDelete(const BoundDelete& del, bool explain) {
	std::unique_ptr<Operator> plan = PlanScan(*del.table, del.where);
	if (explain) {
		return ExplainResult(ExplainDelete(del, *plan));
	}

	const Schema& schema = del.table->GetSchema();
	std::vector<index_key_t> keys;
	{
		Row row;
		while (true) {
			Result<bool> more = plan->Next(row);
			if (!more.has_value()) return std::unexpected(std::move(more.error()));
			if (!*more) break;
			keys.push_back(schema.KeyOf(row));
		}
		plan.reset();  // releases the scan's guard; from here the table may be written
	}

	for (index_key_t key : keys) {
		if (Status st = del.table->Delete(key); !st.ok()) {
			return std::unexpected(std::move(st));
		}
	}
	return QueryResult{.tag = std::format("DELETE {}", keys.size())};
}

}  // namespace

Result<QueryResult> Executor::Execute(std::string_view sql) {
	Result<ParsedStatement> parsed = Parser::Parse(sql);
	if (!parsed.has_value()) return std::unexpected(std::move(parsed.error()));

	Result<BoundStatement> bound = Bind(parsed->body, catalog_);
	if (!bound.has_value()) return std::unexpected(std::move(bound.error()));

	const bool explain = parsed->explain;  // the parser allows it only on SELECT and DELETE
	return std::visit(
	    [&](const auto& s) -> Result<QueryResult> {
		    using S = std::decay_t<decltype(s)>;
		    if constexpr (std::is_same_v<S, BoundCreateTable>) {
			    return RunCreate(s, catalog_, index_fanout_);
		    } else if constexpr (std::is_same_v<S, BoundInsert>) {
			    return RunInsert(s);
		    } else if constexpr (std::is_same_v<S, BoundSelect>) {
			    return RunSelect(s, explain);
		    } else {
			    return RunDelete(s, explain);
		    }
	    },
	    *bound);
}

// ---- FormatResult ----

std::string kernsql::FormatResult(const QueryResult& result) {
	if (result.columns.empty()) {
		return result.tag;
	}

	const std::size_t n = result.columns.size();
	std::vector<std::vector<std::string>> cells;
	std::vector<std::size_t> width(n);
	std::vector<bool> right_align(n, false);
	for (std::size_t c = 0; c < n; ++c) width[c] = result.columns[c].size();
	for (const Row& row : result.rows) {
		std::vector<std::string>& line = cells.emplace_back();
		for (std::size_t c = 0; c < n && c < row.size(); ++c) {
			if (const auto* integer = std::get_if<int64_t>(&row[c])) {
				line.push_back(std::to_string(*integer));
				right_align[c] = true;
			} else {
				line.push_back(std::get<std::string>(row[c]));
			}
			width[c] = std::max(width[c], line.back().size());
		}
	}

	// A left-aligned cell in the last column is not padded: no trailing spaces on any line.
	const auto cell = [&](std::size_t c, const std::string& text, bool right) {
		if (right) return std::format("{:>{}}", text, width[c]);
		if (c + 1 == n) return text;
		return std::format("{:<{}}", text, width[c]);
	};

	std::string out;
	for (std::size_t c = 0; c < n; ++c) {
		out += (c == 0 ? " " : " | ");
		out += cell(c, result.columns[c], false);
	}
	out += "\n";
	for (std::size_t c = 0; c < n; ++c) {
		out += (c == 0 ? "" : "+");
		out += std::string(width[c] + 2, '-');
	}
	out += "\n";
	for (const std::vector<std::string>& line : cells) {
		for (std::size_t c = 0; c < line.size(); ++c) {
			out += (c == 0 ? " " : " | ");
			out += cell(c, line[c], right_align[c]);
		}
		out += "\n";
	}
	const std::size_t count = result.rows.size();
	out += std::format("({} {})", count, count == 1 ? "row" : "rows");
	return out;
}

// ---- SplitStatements ----

SplitInput kernsql::SplitStatements(std::string_view buffer) {
	Result<std::vector<Token>> tokens = Lexer::Tokenize(buffer);
	if (!tokens.has_value()) {
		if (tokens.error().message().find("unterminated string") != std::string_view::npos) {
			return SplitInput{.remainder = std::string(buffer)};  // keep reading the string
		}
		return SplitInput{.statements = {std::string(buffer)}};  // let Execute report it
	}

	SplitInput split;
	std::size_t start = 0;
	for (const Token& token : *tokens) {
		if (token.type == TokenType::kSemicolon) {
			const std::size_t end = token.offset + token.length;
			split.statements.emplace_back(buffer.substr(start, end - start));
			start = end;
		}
	}
	// What is left after the last ';' is either nothing worth keeping (whitespace and comments:
	// it lexes to kEnd alone) or the start of the next statement.
	const std::string_view rest = buffer.substr(start);
	Result<std::vector<Token>> rest_tokens = Lexer::Tokenize(rest);
	if (!rest_tokens.has_value() || rest_tokens->size() > 1) {
		split.remainder = std::string(rest);
	}
	return split;
}
