/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API database statistics implementation
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "http-common.h"
#include "routes.h"
#include "json_macros.h"
#include "shmem.h"
#include "datastructure.h"
// logg()
#include "log.h"
// FTL_db
#include "../database/common.h"

int api_stats_database_overTime_history(struct mg_connection *conn)
{
	int from = 0, until = 0;
	const int interval = 600;
	const struct mg_request_info *request = mg_get_request_info(conn);
	if(request->query_string != NULL)
	{
		int num;
		if((num = get_int_var(request->query_string, "from")) > 0)
			from = num;
		if((num = get_int_var(request->query_string, "until")) > 0)
			until = num;
	}

	// Check if we received the required information
	if(from == 0 || until == 0)
	{
		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 400,
		"bad_request",
		"You need to specify both \"from\" and \"until\" in the request.",
		json);
	}

	// Unlock shared memory (DNS resolver can continue to work while we're preforming database queries)
	unlock_shm();

	// Open the database (this also locks the database)
	dbopen();

	// Build SQL string
	const char *querystr = "SELECT (timestamp/:interval)*:interval interval,status,COUNT(*) FROM queries "
	                       "WHERE (status != 0) AND timestamp >= :from AND timestamp <= :until "
	                       "GROUP by interval,status ORDER by interval";


	// Prepare SQLite statement
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(FTL_db, querystr, -1, &stmt, NULL);
	if( rc != SQLITE_OK ){
		logg("api_stats_database_overTime_history() - SQL error prepare (%i): %s",
		     rc, sqlite3_errmsg(FTL_db));
		return false;
	}

	// Bind interval to prepared statement
	if((rc = sqlite3_bind_int(stmt, 1, interval)) != SQLITE_OK)
	{
		logg("api_stats_database_overTime_history(): Failed to bind interval (error %d) - %s",
		     rc, sqlite3_errmsg(FTL_db));
		sqlite3_reset(stmt);
		sqlite3_finalize(stmt);
		dbclose();

		// Relock shared memory
		lock_shm();

		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 500,
		"internal_error",
		"Failed to bind interval",
		json);
	}

	// Bind from to prepared statement
	if((rc = sqlite3_bind_int(stmt, 2, from)) != SQLITE_OK)
	{
		logg("api_stats_database_overTime_history(): Failed to bind from (error %d) - %s",
		     rc, sqlite3_errmsg(FTL_db));
		sqlite3_reset(stmt);
		sqlite3_finalize(stmt);
		dbclose();

		// Relock shared memory
		lock_shm();

		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 500,
		"internal_error",
		"Failed to bind from",
		json);
	}

	// Bind until to prepared statement
	if((rc = sqlite3_bind_int(stmt, 3, until)) != SQLITE_OK)
	{
		logg("api_stats_database_overTime_history(): Failed to bind until (error %d) - %s",
		     rc, sqlite3_errmsg(FTL_db));
		sqlite3_reset(stmt);
		sqlite3_finalize(stmt);
		dbclose();

		// Relock shared memory
		lock_shm();

		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 500,
		"internal_error",
		"Failed to bind until",
		json);
	}

	// Loop over and accumulate results
	cJSON *json = JSON_NEW_ARRAY();
	cJSON *item = NULL;
	int previous_timestamp = 0, blocked = 0, total = 0;
	while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
	{
		const int timestamp = sqlite3_column_int(stmt, 0);
		// Begin new array item for each new timestamp
		if(timestamp != previous_timestamp)
		{
			previous_timestamp = timestamp;
			if(item != NULL)
			{
				JSON_OBJ_ADD_NUMBER(item, "total_queries", total);
				total = 0;
				JSON_OBJ_ADD_NUMBER(item, "blocked_queries", blocked);
				blocked = 0;
				JSON_ARRAY_ADD_ITEM(json, item);
			}

			item = JSON_NEW_OBJ();
			JSON_OBJ_ADD_NUMBER(item, "timestamp", timestamp);
		}

		const int status = sqlite3_column_int(stmt, 1);
		const int count = sqlite3_column_int(stmt, 2);
		// Always add to total count
		total += count;

		// Add to blocked count if this is the result for a blocked status
		switch (status)
		{
		case QUERY_GRAVITY:
		case QUERY_WILDCARD:
		case QUERY_BLACKLIST:
		case QUERY_EXTERNAL_BLOCKED_IP:
		case QUERY_EXTERNAL_BLOCKED_NULL:
		case QUERY_EXTERNAL_BLOCKED_NXRA:
			blocked += count;
			break;
		
		default:
			break;
		}
		
	}

	// Finalize statement and close (= unlock) database connection
	sqlite3_finalize(stmt);
	dbclose();

	// Re-lock shared memory before returning back to router subroutine
	lock_shm();
	JSON_SEND_OBJECT(json);
}

int api_stats_database_top_items(bool blocked, bool domains, struct mg_connection *conn)
{
	int from = 0, until = 0, show = 10;
	const struct mg_request_info *request = mg_get_request_info(conn);
	if(request->query_string != NULL)
	{
		int num;
		if((num = get_int_var(request->query_string, "from")) > 0)
			from = num;
		if((num = get_int_var(request->query_string, "until")) > 0)
			until = num;

		// Get blocked queries not only for .../top_blocked
		// but also for .../top_domains?blocked=true
		if((num = get_bool_var(request->query_string, "blocked")) > 0)
			blocked = true;

		// Does the user request a non-default number of replies?
		// Note: We do not accept zero query requests here
		if((num = get_int_var(request->query_string, "show")) > 0)
			show = num;
	}

	// Check if we received the required information
	if(from == 0 || until == 0)
	{
		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 400,
		"bad_request",
		"You need to specify both \"from\" and \"until\" in the request.",
		json);
	}

	// Unlock shared memory (DNS resolver can continue to work while we're preforming database queries)
	unlock_shm();

	// Open the database (this also locks the database)
	dbopen();

	// Build SQL string
	const char *querystr;
	if(domains)
	{
		if(!blocked)
		{
			querystr = "SELECT domain,COUNT(*) AS cnt FROM queries "
			           "WHERE (status == 2 OR status == 3) "
			           "AND timestamp >= :from AND timestamp <= :until "
			           "GROUP by domain ORDER by cnt DESC "
			           "LIMIT :show";
		}
		else
		{
			querystr = "SELECT domain,COUNT(*) AS cnt FROM queries "
			           "WHERE status != 0 AND status != 2 AND status != 3 "
			           "AND timestamp >= :from AND timestamp <= :until "
			           "GROUP by domain ORDER by cnt DESC "
			           "LIMIT :show";
		}
	}
	else
	{
		if(!blocked)
		{
			querystr = "SELECT client,COUNT(*) AS cnt FROM queries "
			           "WHERE (status == 2 OR status == 3) "
			           "AND timestamp >= :from AND timestamp <= :until "
			           "GROUP by client ORDER by cnt DESC "
			           "LIMIT :show";
		}
		else
		{
			querystr = "SELECT client,COUNT(*) AS cnt FROM queries "
			           "WHERE status != 0 AND status != 2 AND status != 3 "
			           "AND timestamp >= :from AND timestamp <= :until "
			           "GROUP by client ORDER by cnt DESC "
			           "LIMIT :show";
		}
	}
	
	
	// Prepare SQLite statement
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(FTL_db, querystr, -1, &stmt, NULL);
	if( rc != SQLITE_OK ){
		logg("api_stats_database_overTime_history() - SQL error prepare (%i): %s",
		     rc, sqlite3_errmsg(FTL_db));
		return false;
	}

	// Bind from to prepared statement
	if((rc = sqlite3_bind_int(stmt, 1, from)) != SQLITE_OK)
	{
		logg("api_stats_database_overTime_history(): Failed to bind from (error %d) - %s",
		     rc, sqlite3_errmsg(FTL_db));
		sqlite3_reset(stmt);
		sqlite3_finalize(stmt);
		dbclose();

		// Relock shared memory
		lock_shm();

		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 500,
		"internal_error",
		"Failed to bind from",
		json);
	}

	// Bind until to prepared statement
	if((rc = sqlite3_bind_int(stmt, 2, until)) != SQLITE_OK)
	{
		logg("api_stats_database_overTime_history(): Failed to bind until (error %d) - %s",
		     rc, sqlite3_errmsg(FTL_db));
		sqlite3_reset(stmt);
		sqlite3_finalize(stmt);
		dbclose();

		// Relock shared memory
		lock_shm();

		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 500,
		"internal_error",
		"Failed to bind until",
		json);
	}

	// Bind show to prepared statement
	if((rc = sqlite3_bind_int(stmt, 3, show)) != SQLITE_OK)
	{
		logg("api_stats_database_overTime_history(): Failed to bind show (error %d) - %s",
		     rc, sqlite3_errmsg(FTL_db));
		sqlite3_reset(stmt);
		sqlite3_finalize(stmt);
		dbclose();

		// Relock shared memory
		lock_shm();

		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_ADD_NUMBER(json, "from", from);
		JSON_OBJ_ADD_NUMBER(json, "until", until);
		return send_json_error(conn, 500,
		"internal_error",
		"Failed to bind show",
		json);
	}

	// Loop over and accumulate results
	cJSON *top_items = JSON_NEW_ARRAY();
	int total = 0;
	while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
	{
		const char* string = (char*)sqlite3_column_text(stmt, 0);
		const int count = sqlite3_column_int(stmt, 1);
		cJSON *item = JSON_NEW_OBJ();
		JSON_OBJ_COPY_STR(item, (domains ? "domain" : "ip"), string);
		// Add empty name field for top_client requests
		if(!domains)
			JSON_OBJ_REF_STR(item, "name", "");
		JSON_OBJ_ADD_NUMBER(item, "count", count);
		JSON_ARRAY_ADD_ITEM(top_items, item);
		total += count;
	}

	// Finalize statement and close (= unlock) database connection
	sqlite3_finalize(stmt);
	dbclose();

	// Re-lock shared memory before returning back to router subroutine
	lock_shm();

	cJSON *json = JSON_NEW_OBJ();
	JSON_OBJ_ADD_ITEM(json, (domains ? "top_domains" : "top_clients"), top_items);
	JSON_OBJ_ADD_NUMBER(json, (blocked ? "blocked_queries" : "total_queries"), total);
	JSON_SEND_OBJECT(json);
}
