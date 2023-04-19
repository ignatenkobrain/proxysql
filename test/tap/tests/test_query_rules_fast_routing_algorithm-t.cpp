/**
 * @file test_query_rules_fast_routing_algorithm-t.cpp
 * @brief This test performs the following checks:
 *   - That multiple 'rules_fast_routing' are being properly evaluated.
 *   - That 'mysql-query_rules_fast_routing_algorithm' properly controls from which hashmaps the query
 *     rules are searched.
 *   - That used memory increases/decreases as expected depending on the value selected for
 *     'mysql-query_rules_fast_routing_algorithm'.
 */

#include <cstring>
#include <stdio.h>
#include <unistd.h>
#include <fstream>

#include <mysql.h>
#include <mysql/mysqld_error.h>

#include "tap.h"
#include "utils.h"
#include "command_line.h"

#include "json.hpp"

using std::pair;
using std::string;
using nlohmann::json;
using std::fstream;
using std::vector;

// Used for 'extract_module_host_port'
#include "modules_server_test.h"

//       UTILS TAKEN FROM PR 4161 - TODO: REMOVE AFTER PR MERGED
///////////////////////////////////////////////////////////////////////////////

#include <regex>

string _get_env(const string& var) {
	string f_path {};

	char* p_infra_datadir = std::getenv(var.c_str());
	if (p_infra_datadir != nullptr) {
		f_path = p_infra_datadir;
	}

	return f_path;
}

int _open_file_and_seek_end(const string& f_path, std::fstream& f_stream) {
	const char* c_f_path { f_path.c_str() };
	f_stream.open(f_path.c_str(), std::fstream::in | std::fstream::out);

	if (!f_stream.is_open() || !f_stream.good()) {
		diag("Failed to open '%s' file: { path: %s, error: %d }", basename(c_f_path), c_f_path, errno);
		return EXIT_FAILURE;
	}

	f_stream.seekg(0, std::ios::end);

	return EXIT_SUCCESS;
}

using _line_match_t = std::tuple<std::fstream::pos_type, std::string, std::smatch>;
enum _LINE_MATCH_T { POS, LINE, MATCHES };

std::vector<_line_match_t> _get_matching_lines(std::fstream& f_stream, const std::string& regex) {
	std::vector<_line_match_t> found_matches {};

	std::string next_line {};
	std::fstream::pos_type init_pos { f_stream.tellg() };

	while (std::getline(f_stream, next_line)) {
		std::regex regex_err_line { regex };
		std::smatch match_results {};

		if (std::regex_search(next_line, match_results, regex_err_line)) {
			found_matches.push_back({ f_stream.tellg(), next_line, match_results });
		}
	}

	if (found_matches.empty() == false) {
		const std::string& last_match { std::get<_LINE_MATCH_T::LINE>(found_matches.back()) };
		const std::fstream::pos_type last_match_pos { std::get<_LINE_MATCH_T::POS>(found_matches.back()) };

		f_stream.clear(f_stream.rdstate() & ~std::ios_base::failbit);
		f_stream.seekg(last_match_pos);
	} else {
		f_stream.clear(f_stream.rdstate() & ~std::ios_base::failbit);
		f_stream.seekg(init_pos);
	}

	return found_matches;
}

///////////////////////////////////////////////////////////////////////////////

void parse_result_json_column(MYSQL_RES *result, json& j) {
	if(!result) return;
	MYSQL_ROW row;

	while ((row = mysql_fetch_row(result))) {
		j = json::parse(row[0]);
	}
}

int extract_internal_session(MYSQL* proxy, nlohmann::json& j_internal_session) {
	MYSQL_QUERY_T(proxy, "PROXYSQL INTERNAL SESSION");
	MYSQL_RES* myres = mysql_store_result(proxy);
	parse_result_json_column(myres, j_internal_session);
	mysql_free_result(myres);

	return EXIT_SUCCESS;
}

int get_query_int_res(MYSQL* admin, const string& q, int& val) {
	MYSQL_QUERY_T(admin, q.c_str());
	MYSQL_RES* myres = mysql_store_result(admin);
	MYSQL_ROW myrow = mysql_fetch_row(myres);

	int res = EXIT_FAILURE;

	if (myrow && myrow[0]) {
		char* p_end = nullptr;
		val = std::strtol(myrow[0], &p_end, 10);

		if (p_end == myrow[0]) {
			diag("Failed to parse query result as 'int' - res: %s, query: %s", myrow[0], q.c_str());
		} else {
			res = EXIT_SUCCESS;
		}
	} else {
		diag("Received empty result for query `%s`", q.c_str());
	}

	mysql_free_result(myres);

	return res;
}

int extract_sess_qpo_dest_hg(MYSQL* proxy) {
	json j_internal_session {};
	int j_err = extract_internal_session(proxy, j_internal_session);
	if (j_err) {
		diag("Failed to extract and parse result from 'PROXYSQL INTERNAL SESSION'");
		return -2;
	}

	int dest_hg = -2;
	try {
		dest_hg = j_internal_session["qpo"]["destination_hostgroup"];
	} catch (const std::exception& e) {
		diag("Processing of 'PROXYSQL INTERNAL SESSION' failed with exc: %s", e.what());
		return -2;
	}

	return dest_hg;
}

int check_fast_routing_rules(MYSQL* proxy, uint32_t rng_init, uint32_t rng_end) {
	for (uint32_t i = rng_init; i < rng_end; i += 2) {
		const string schema { "randomschemaname" + std::to_string(i) };

		diag("Changing schema to '%s'", schema.c_str());
		if (mysql_select_db(proxy, schema.c_str())) {
			fprintf(stdout, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxy));
			return EXIT_FAILURE;
		}

		diag("Issuing simple 'SELECT 1' to trigger WRITER rule for '%s'", schema.c_str());
		MYSQL_QUERY_T(proxy, "SELECT 1");
		mysql_free_result(mysql_store_result(proxy));

		int dest_hg = extract_sess_qpo_dest_hg(proxy);
		if (dest_hg == -2) {
			return EXIT_FAILURE;
		}

		ok(i == dest_hg, "Destination hostgroup matches expected - Exp: %d, Act: %d", i, dest_hg);

		diag("Issuing simple 'SELECT 2' to trigger READER rule for '%s'", schema.c_str());
		MYSQL_QUERY_T(proxy, "SELECT 2");
		mysql_free_result(mysql_store_result(proxy));

		dest_hg = extract_sess_qpo_dest_hg(proxy);
		if (dest_hg == -2) {
			return EXIT_FAILURE;
		}

		ok(i + 1 == dest_hg, "Destination hostgroup matches expected - Exp: %d, Act: %d", i + 1, dest_hg);
	}

	return EXIT_SUCCESS;
};

int main(int argc, char** argv) {
	// `5` logic checks + 20*3 checks per query rule, per test
	plan((5 + 20*3) * 2);

	CommandLine cl;

	if (cl.getEnv()) {
		diag("Failed to get the required environmental variables.");
		return EXIT_FAILURE;
	}

	MYSQL* proxy = mysql_init(NULL);
	MYSQL* admin = mysql_init(NULL);

	if (!mysql_real_connect(proxy, cl.host, cl.username, cl.password, NULL, cl.port, NULL, 0)) {
		fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxy));
		return EXIT_FAILURE;
	}

	if (!mysql_real_connect(admin, cl.host, cl.admin_username, cl.admin_password, NULL, cl.admin_port, NULL, 0)) {
		fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(admin));
		return EXIT_FAILURE;
	}

	const auto create_mysql_servers_range = [] (
		const CommandLine& cl, MYSQL* admin, const pair<string,int>& host_port, uint32_t rng_init, uint32_t rng_end
	) -> int {
		const string init { std::to_string(rng_init) };
		const string end { std::to_string(rng_end) };

		MYSQL_QUERY_T(admin, ("DELETE FROM mysql_servers WHERE hostgroup_id BETWEEN " + init + " AND " + end).c_str());

		for (uint32_t i = rng_init; i < rng_end; i += 2) {
			std::string q = "INSERT INTO mysql_servers (hostgroup_id, hostname, port) VALUES ";
			q += "(" + std::to_string(i)   + ",'" + host_port.first + "'," + std::to_string(host_port.second) + ")";
			q += ",";
			q += "(" + std::to_string(i+1) + ",'" + host_port.first + "'," + std::to_string(host_port.second) + ")";
			MYSQL_QUERY(admin, q.c_str());
		}

		return EXIT_SUCCESS;
	};

	const auto create_fast_routing_rules_range = [] (
		const CommandLine& cl, MYSQL* admin, const pair<string,int>& host_port, uint32_t rng_init, uint32_t rng_end
	) -> int {
		const string init { std::to_string(rng_init) };
		const string end { std::to_string(rng_end) };

		MYSQL_QUERY_T(admin, ("DELETE FROM mysql_query_rules_fast_routing WHERE destination_hostgroup BETWEEN " + init + " AND " + end).c_str());

		for (uint32_t i = rng_init; i < rng_end; i += 2) {
			const string schema { "randomschemaname" + std::to_string(i) + "" };
			const string user { cl.username };
			string q = "INSERT INTO mysql_query_rules_fast_routing (username, schemaname, flagIN, destination_hostgroup, comment) VALUES ";

			q += "('" + user + "', '" + schema + "' , 0, " + std::to_string(i)   + ", 'writer" + std::to_string(i) +   "'),";
			q += "('" + user + "', '" + schema + "' , 1, " + std::to_string(i+1) + ", 'reader" + std::to_string(i+1) + "')";

			MYSQL_QUERY(admin, q.c_str());
		}

		return EXIT_SUCCESS;
	};

	const auto test_fast_routing_algorithm = [&create_mysql_servers_range, &create_fast_routing_rules_range] (
		const CommandLine& cl, MYSQL* admin, MYSQL* proxy, const pair<string,int>& host_port, fstream& errlog, int init_algo, int new_algo
	) {
		uint32_t rng_init = 1000;
		uint32_t rng_end = 1020;
		const char query_rules_mem_stats_query[] {
			"SELECT variable_value FROM stats_memory_metrics WHERE variable_name='mysql_query_rules_memory'"
		};

		// Enable Admin debug and increase verbosity for Query_Processor
		MYSQL_QUERY_T(admin, "SET admin-debug=1");
		MYSQL_QUERY_T(admin, "LOAD ADMIN VARIABLES TO RUNTIME");
		MYSQL_QUERY_T(admin, "UPDATE debug_levels SET verbosity=7 WHERE module='debug_mysql_query_processor'");
		MYSQL_QUERY_T(admin, "LOAD DEBUG TO RUNTIME");

		int c_err = create_mysql_servers_range(cl, admin, host_port, rng_init, rng_end);
		if (c_err) {
			return EXIT_FAILURE;
		}
		MYSQL_QUERY_T(admin, "LOAD MYSQL SERVERS TO RUNTIME");

		printf("\n");
		diag("Testing 'query_rules_fast_routing_algorithm=%d'", init_algo);
		MYSQL_QUERY_T(admin, ("SET mysql-query_rules_fast_routing_algorithm=" + std::to_string(init_algo)).c_str());
		MYSQL_QUERY_T(admin, "LOAD MYSQL VARIABLES TO RUNTIME");

		// Always cleanup the rules before the test to get proper memory usage diff
		MYSQL_QUERY_T(admin, "DELETE FROM mysql_query_rules_fast_routing");
		MYSQL_QUERY_T(admin, "LOAD MYSQL QUERY RULES TO RUNTIME");

		int init_rules_mem_stats = -1;
		int get_mem_stats_err = get_query_int_res(admin, query_rules_mem_stats_query, init_rules_mem_stats);
		if (get_mem_stats_err) {
			return EXIT_FAILURE;
		}
		diag("Initial 'mysql_query_rules_memory' of '%d'", init_rules_mem_stats);

		// Check that fast_routing rules are being properly triggered
		c_err = create_fast_routing_rules_range(cl, admin, host_port, rng_init, rng_end);
		if (c_err) {
			return EXIT_FAILURE;
		}
		MYSQL_QUERY_T(admin, "LOAD MYSQL QUERY RULES TO RUNTIME");

		// Seek end of file for error log
		errlog.seekg(0, std::ios::end);

		// Check that fast_routing rules are properly working for the defined range
		check_fast_routing_rules(proxy, rng_init, rng_end);

		// Give some time for the error log to be written
		usleep(100*1000);

		const string init_algo_scope { init_algo == 1 ? "thread-local" : "global" };
		const string init_search_regex { "Searching " + init_algo_scope + " 'rules_fast_routing' hashmap" };
		vector<_line_match_t> matched_lines { _get_matching_lines(errlog, init_search_regex) };

		ok(
			matched_lines.size() == rng_end - rng_init,
			"Number of '%s' searchs in error log should match issued queries - Exp: %d, Act: %ld",
			init_algo_scope.c_str(), rng_end - rng_init, matched_lines.size()
		);
		printf("\n");

		int old_mem_stats = -1;
		get_mem_stats_err = get_query_int_res(admin, query_rules_mem_stats_query, old_mem_stats);
		if (get_mem_stats_err) {
			return EXIT_FAILURE;
		}

		// Changing the algorithm shouldn't have any effect
		diag("Testing 'query_rules_fast_routing_algorithm=%d'", new_algo);
		MYSQL_QUERY_T(admin, ("SET mysql-query_rules_fast_routing_algorithm=" + std::to_string(new_algo)).c_str());
		MYSQL_QUERY_T(admin, "LOAD MYSQL VARIABLES TO RUNTIME");

		errlog.seekg(0, std::ios::end);

		diag("Search should still be performed 'per-thread'. Only variable has changed.");
		check_fast_routing_rules(proxy, rng_init, rng_end);
		matched_lines = _get_matching_lines(errlog, init_search_regex);

		// Give some time for the error log to be written
		usleep(100*1000);

		ok(
			matched_lines.size() == rng_end - rng_init,
			"Number of 'thread-local' searchs in error log should match issued queries - Exp: %d, Act: %ld",
			rng_end - rng_init, matched_lines.size()
		);

		int new_mem_stats = -1;
		get_mem_stats_err = get_query_int_res(admin, query_rules_mem_stats_query, new_mem_stats);
		if (get_mem_stats_err) {
			return EXIT_FAILURE;
		}

		diag("Memory SHOULDN'T have changed just because of a variable change");
		ok(
			old_mem_stats - init_rules_mem_stats == new_mem_stats - init_rules_mem_stats,
			"Memory stats shouldn't increase just by the variable change - old: %d, new: %d",
			old_mem_stats - init_rules_mem_stats, new_mem_stats - init_rules_mem_stats
		);
		printf("\n");

		MYSQL_QUERY_T(admin, "LOAD MYSQL QUERY RULES TO RUNTIME");
		diag("Search should now be using the per thread-maps");

		// Seek end of file for error log
		errlog.seekg(0, std::ios::end);
		check_fast_routing_rules(proxy, rng_init, rng_end);

		// Give some time for the error log to be written
		usleep(100*1000);

		const string new_algo_scope { new_algo == 1 ? "thread-local" : "global" };
		const string new_search_regex { "Searching " + new_algo_scope + " 'rules_fast_routing' hashmap" };
		vector<_line_match_t> global_matched_lines { _get_matching_lines(errlog, new_search_regex) };

		ok(
			global_matched_lines.size() == rng_end - rng_init,
			"Number of '%s' searchs in error log should match issued queries - Exp: %d, Act: %ld",
			new_algo_scope.c_str(), rng_end - rng_init, global_matched_lines.size()
		);

		get_mem_stats_err = get_query_int_res(admin, query_rules_mem_stats_query, new_mem_stats);
		if (get_mem_stats_err) {
			return EXIT_FAILURE;
		}

		bool mem_check_res = false;
		string exp_change { "" };

		if (init_algo == 1 && new_algo == 2) {
			mem_check_res = (old_mem_stats - init_rules_mem_stats) > (new_mem_stats - init_rules_mem_stats);
			exp_change = "decrease";
		} else if (init_algo == 2 && new_algo == 1) {
			mem_check_res = (old_mem_stats - init_rules_mem_stats) < (new_mem_stats - init_rules_mem_stats);
			exp_change = "increase";
		} else {
			mem_check_res = (old_mem_stats - init_rules_mem_stats) == (new_mem_stats - init_rules_mem_stats);
			exp_change = "not change";
		}

		ok(
			mem_check_res,
			"Memory stats should %s after 'LOAD MYSQL QUERY RULES TO RUNTIME' - old: %d, new: %d",
			exp_change.c_str(), (old_mem_stats - init_rules_mem_stats), (new_mem_stats - init_rules_mem_stats)
		);

		return EXIT_SUCCESS;
	};

	pair<string,int> host_port {};
	int host_port_err = extract_module_host_port(admin, "sqliteserver-mysql_ifaces", host_port);

	if (host_port_err) {
		goto cleanup;
	}

	MYSQL_QUERY_T(admin, "DELETE FROM mysql_query_rules");
	MYSQL_QUERY_T(admin, "INSERT INTO mysql_query_rules (rule_id, active, match_pattern, flagOUT, cache_ttl) VALUES (1,1,'^SELECT 1$', 0, 600000)");
	MYSQL_QUERY_T(admin, "INSERT INTO mysql_query_rules (rule_id, active, match_pattern, flagOUT, cache_ttl) VALUES (2,1,'^SELECT 2$', 1, 600000)");
	MYSQL_QUERY_T(admin, "DELETE FROM mysql_query_rules_fast_routing");
	MYSQL_QUERY_T(admin, "LOAD MYSQL QUERY RULES TO RUNTIME");

	{
		const string f_path { _get_env("REGULAR_INFRA_DATADIR") + "/proxysql.log" };
		fstream errlog {};

		int of_err = _open_file_and_seek_end(f_path, errlog);
		if (of_err) {
			diag("Failed to open ProxySQL log file. Aborting further testing...");
			goto cleanup;
		}

		int test_err = test_fast_routing_algorithm(cl, admin, proxy, host_port, errlog, 1, 2);
		if (test_err) { goto cleanup; }

		test_err = test_fast_routing_algorithm(cl, admin, proxy, host_port, errlog, 2, 1);
		if (test_err) { goto cleanup; }
	}

cleanup:

	mysql_close(proxy);
	mysql_close(admin);

	return exit_status();
}