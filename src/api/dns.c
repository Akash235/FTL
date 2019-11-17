/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation /api/dns
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "api.h"
#include "database/gravity-db.h"
#include "log.h"
// {s,g}et_blockingstatus()
#include "setupVars.h"
// floor()
#include <math.h>
// set_blockingmode_timer()
#include "timers.h"

int api_dns_status(struct mg_connection *conn)
{
	int method = http_method(conn);
	if(method == HTTP_GET)
	{
		// Return current status
		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_REF_STR(json, "status", (get_blockingstatus() ? "enabled" : "disabled"));
		JSON_SENT_OBJECT(json);
	}
	else if(method == HTTP_POST)
	{
		// Verify requesting client is allowed to access this ressource
		if(check_client_auth(conn) < 0)
		{
			cJSON *json = JSON_NEW_OBJ();
			JSON_OBJ_REF_STR(json, "key", "unauthorized");
			JSON_SENT_OBJECT_CODE(json, 401);
		}

		char buffer[1024];
		int data_len = mg_read(conn, buffer, sizeof(buffer) - 1);
		if ((data_len < 1) || (data_len >= (int)sizeof(buffer))) {
			mg_send_http_error(conn, 400, "%s", "No request body data");
			return 400;
		}
		buffer[data_len] = '\0';

		cJSON *obj = cJSON_Parse(buffer);
		if (obj == NULL) {
			mg_send_http_error(conn, 400, "%s", "Invalid request body data");
			return 400;
		}

		cJSON *elem1 = cJSON_GetObjectItemCaseSensitive(obj, "action");
		if (!cJSON_IsString(elem1)) {
			cJSON_Delete(obj);
			mg_send_http_error(conn, 400, "%s", "No \"action\" string in body data");
			return 400;
		}
		const char *action = elem1->valuestring;

		unsigned int delay = -1;
		cJSON *elem2 = cJSON_GetObjectItemCaseSensitive(obj, "time");
		if (cJSON_IsNumber(elem2) && elem2->valuedouble > 0.0)
		{
			delay = elem2->valueint;
		}

		cJSON *json = JSON_NEW_OBJ();
		if(strcmp(action, "enable") == 0)
		{
			cJSON_Delete(obj);
			JSON_OBJ_REF_STR(json, "key", "enabled");
			// If no "time" key was present, we call this subroutine with
			// delay == -1 which will disable all previously set timers
			set_blockingmode_timer(delay, false);
			set_blockingstatus(true);
		}
		else if(strcmp(action, "disable") == 0)
		{
			cJSON_Delete(obj);
			JSON_OBJ_REF_STR(json, "key", "disabled");
			// If no "time" key was present, we call this subroutine with
			// delay == -1 which will disable all previously set timers
			set_blockingmode_timer(delay, true);
			set_blockingstatus(false);
		}
		else
		{
			cJSON_Delete(obj);
			JSON_OBJ_REF_STR(json, "key", "unsupported action");
		}
		JSON_SENT_OBJECT(json);
	}
	else
	{
		// This results in error 404
		return 0;
	}
}

static int api_dns_somelist_read(struct mg_connection *conn, bool exact, bool whitelist)
{
	cJSON *json = JSON_NEW_ARRAY();
	const char *domain = NULL;

	int table;
	if(whitelist)
		if(exact)
			table = EXACT_WHITELIST_TABLE;
		else
			table = REGEX_WHITELIST_TABLE;
	else
		if(exact)
			table = EXACT_BLACKLIST_TABLE;
		else
			table = REGEX_BLACKLIST_TABLE;

	gravityDB_getTable(table);
	while((domain = gravityDB_getDomain()) != NULL)
	{
		JSON_ARRAY_COPY_STR(json, domain);
	}
	gravityDB_finalizeTable();

	JSON_SENT_OBJECT(json);
}

static int api_dns_somelist_POST(struct mg_connection *conn,
                                   bool store_exact,
                                   bool whitelist)
{
	char buffer[1024];
	int data_len = mg_read(conn, buffer, sizeof(buffer) - 1);
	if ((data_len < 1) || (data_len >= (int)sizeof(buffer))) {
		mg_send_http_error(conn, 400, "%s", "No request body data");
		return 400;
	}
	buffer[data_len] = '\0';

	cJSON *obj = cJSON_Parse(buffer);
	if (obj == NULL) {
		mg_send_http_error(conn, 400, "%s", "Invalid request body data");
		return 400;
	}

	cJSON *elem = cJSON_GetObjectItemCaseSensitive(obj, "domain");

	if (!cJSON_IsString(elem)) {
		cJSON_Delete(obj);
		mg_send_http_error(conn, 400, "%s", "No \"domain\" string in body data");
		return 400;
	}
	const char *domain = elem->valuestring;

	const char *table;
	if(whitelist)
		if(store_exact)
			table = "whitelist";
		else
			table = "regex_whitelist";
	else
		if(store_exact)
			table = "blacklist";
		else
			table = "regex_blacklist";

	cJSON *json = JSON_NEW_OBJ();
	if(gravityDB_addToTable(table, domain))
	{
		JSON_OBJ_REF_STR(json, "key", "added");
		JSON_OBJ_COPY_STR(json, "domain", domain);
		cJSON_Delete(obj);
		JSON_SENT_OBJECT(json);
	}
	else
	{
		JSON_OBJ_REF_STR(json, "key", "error");
		JSON_OBJ_COPY_STR(json, "domain", domain);
		cJSON_Delete(obj);
		// Send 500 internal server error
		JSON_SENT_OBJECT_CODE(json, 500);
	}
}

static int api_dns_somelist_DELETE(struct mg_connection *conn,
                                   bool store_exact,
                                   bool whitelist)
{
	const struct mg_request_info *request = mg_get_request_info(conn);

	char domain[1024];
	// Advance one character to strip "/"
	const char *encoded_uri = strrchr(request->local_uri, '/')+1u;
	// Decode URL (necessar for regular expressions, harmless for domains)
	mg_url_decode(encoded_uri, strlen(encoded_uri), domain, sizeof(domain)-1u, 0);

	const char *table;
	if(whitelist)
		if(store_exact)
			table = "whitelist";
		else
			table = "regex_whitelist";
	else
		if(store_exact)
			table = "blacklist";
		else
			table = "regex_blacklist";

	cJSON *json = JSON_NEW_OBJ();
	if(gravityDB_delFromTable(table, domain))
	{
		JSON_OBJ_REF_STR(json, "key", "removed");
		JSON_OBJ_REF_STR(json, "domain", domain);
		JSON_SENT_OBJECT(json);
	}
	else
	{
		JSON_OBJ_REF_STR(json, "key", "error");
		JSON_OBJ_REF_STR(json, "domain", domain);
		// Send 500 internal server error
		JSON_SENT_OBJECT_CODE(json, 500);
	}
}

int api_dns_somelist(struct mg_connection *conn, bool exact, bool whitelist)
{
	// Verify requesting client is allowed to see this ressource
	if(check_client_auth(conn) < 0)
	{
		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_REF_STR(json, "key", "unauthorized");
		JSON_SENT_OBJECT_CODE(json, 401);
	}

	int method = http_method(conn);
	if(method == HTTP_GET)
	{
		return api_dns_somelist_read(conn, exact, whitelist);
	}
	else if(method == HTTP_POST)
	{
		// Add domain from exact white-/blacklist when a user sends
		// the request to the general address /api/dns/{white,black}list
		return api_dns_somelist_POST(conn, exact, whitelist);
	}
	else if(method == HTTP_DELETE)
	{
		// Delete domain from exact white-/blacklist when a user sends
		// the request to the general address /api/dns/{white,black}list
		return api_dns_somelist_DELETE(conn, exact, whitelist);
	}
	else
	{
		// This results in error 404
		return 0;
	}
}
