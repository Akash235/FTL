/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "memory.h"
#include "shmem.h"
#include "datastructure.h"
#include "setupVars.h"
#include "files.h"
#include "log.h"
#include "config.h"
#include "database/common.h"
#include "database/query-table.h"
// in_auditlist()
#include "database/gravity-db.h"
#include "overTime.h"
#include "api.h"
#include "version.h"
// enum REGEX
#include "regex_r.h"

#define min(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })

/* qsort comparision function (count field), sort ASC */
static int __attribute__((pure)) cmpasc(const void *a, const void *b)
{
	const int *elem1 = (int*)a;
	const int *elem2 = (int*)b;

	if (elem1[1] < elem2[1])
		return -1;
	else if (elem1[1] > elem2[1])
		return 1;
	else
		return 0;
}

// qsort subroutine, sort DESC
static int __attribute__((pure)) cmpdesc(const void *a, const void *b)
{
	const int *elem1 = (int*)a;
	const int *elem2 = (int*)b;

	if (elem1[1] > elem2[1])
		return -1;
	else if (elem1[1] < elem2[1])
		return 1;
	else
		return 0;
}

void api_stats_summary(struct mg_connection *conn)
{
	const int blocked = counters->blocked;
	const int total = counters->queries;
	float percentage = 0.0f;

	// Avoid 1/0 condition
	if(total > 0)
		percentage = 1e2f*blocked/total;

	// unique_clients: count only clients that have been active within the most recent 24 hours
	int activeclients = 0;
	for(int clientID=0; clientID < counters->clients; clientID++)
	{
		// Get client pointer
		const clientsData* client = getClient(clientID, true);
		if(client->count > 0)
			activeclients++;
	}

	// Send response
	http_send(conn, false,
		  "{\"gravity_size\":%i,"
		  "\"total_queries\":{\"A\":%i,\"AAAA\":%i,\"ANY\":%i,\"SRV\":%i,\"SOA\":%i,\"PTR\":%i,\"TXT\":%i},"
		  "\"blocked_queries\":%i,\"percent_blocked\":%f,"
		  "\"unique_domains\":%i,"
		  "\"forwarded_queries\":%i,\"cached_queries\":%i,"
		  "\"reply_types\":{\"NODATA\":%i,\"NXDOMAIN\":%i,\"CNAME\":%i,\"IP\":%i},"
		  "\"privacy_level\":%i,"
		  "\"total_clients\":%i,\"active_clients\":%i,"
		  "\"status\":\"%s\"}",
		  counters->gravity,
		  counters->querytype[TYPE_A], counters->querytype[TYPE_AAAA],
		  counters->querytype[TYPE_ANY], counters->querytype[TYPE_SRV],
		  counters->querytype[TYPE_SOA], counters->querytype[TYPE_PTR],
		  counters->querytype[TYPE_TXT],
		  blocked, percentage,
		  counters->domains,
		  counters->forwardedqueries, counters->cached,
		  counters->reply_NODATA, counters->reply_NXDOMAIN,
		  counters->reply_CNAME, counters->reply_IP,
		  config.privacylevel,
		  counters->clients, activeclients,
		  counters->gravity > 0 ? "enabled" : "disabled");
}

void api_dns_status(struct mg_connection *conn)
{
	// Send status
	http_send(conn, false, "{\"status\":\"%s\"}",
		  counters->gravity > 0 ? "enabled" : "disabled");
}

void getOverTime(struct mg_connection *conn)
{
	int from = 0, until = OVERTIME_SLOTS;
	bool found = false;
	time_t mintime = overTime[0].timestamp;

	// Start with the first non-empty overTime slot
	for(int slot = 0; slot < OVERTIME_SLOTS; slot++)
	{
		if((overTime[slot].total > 0 || overTime[slot].blocked > 0) &&
		   overTime[slot].timestamp >= mintime)
		{
			from = slot;
			found = true;
			break;
		}
	}

	// End with last non-empty overTime slot
	for(int slot = 0; slot < OVERTIME_SLOTS; slot++)
	{
		if(overTime[slot].timestamp >= time(NULL))
		{
			until = slot;
			break;
		}
	}

	// Check if there is any data to be sent
	if(!found)
		return;

	for(int slot = from; slot < until; slot++)
	{
		http_send(conn, false, "%li %i %i\n",
			overTime[slot].timestamp,
			overTime[slot].total,
			overTime[slot].blocked);
	}
}

void getTopDomains(const bool blocked, struct mg_connection *conn)
{
	int temparray[counters->domains][2], count=10;
	bool audit = false, asc = false;

	// Exit before processing any data if requested via config setting
	get_privacy_level(NULL);
	if(config.privacylevel >= PRIVACY_HIDE_DOMAINS) {
		return;
	}
/*
	// Match both top-domains and top-ads
	// example: >top-domains (15)
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0) {
		// User wants a different number of requests
		count = num;
	}

	// Apply Audit Log filtering?
	// example: >top-domains for audit
	if(command(client_message, " for audit"))
		audit = true;

	// Sort in ascending order?
	// example: >top-domains asc
	if(command(client_message, " asc"))
		asc = true;
*/
	for(int domainID=0; domainID < counters->domains; domainID++)
	{
		// Get domain pointer
		const domainsData* domain = getDomain(domainID, true);

		temparray[domainID][0] = domainID;
		if(blocked)
			temparray[domainID][1] = domain->blockedcount;
		else
			// Count only permitted queries
			temparray[domainID][1] = (domain->count - domain->blockedcount);
	}

	// Sort temporary array
	if(asc)
		qsort(temparray, counters->domains, sizeof(int[2]), cmpasc);
	else
		qsort(temparray, counters->domains, sizeof(int[2]), cmpdesc);


	// Get filter
	const char* filter = read_setupVarsconf("API_QUERY_LOG_SHOW");
	bool showpermitted = true, showblocked = true;
	if(filter != NULL)
	{
		if((strcmp(filter, "permittedonly")) == 0)
			showblocked = false;
		else if((strcmp(filter, "blockedonly")) == 0)
			showpermitted = false;
		else if((strcmp(filter, "nothing")) == 0)
		{
			showpermitted = false;
			showblocked = false;
		}
	}
	clearSetupVarsArray();

	// Get domains which the user doesn't want to see
	char * excludedomains = NULL;
	if(!audit)
	{
		excludedomains = read_setupVarsconf("API_EXCLUDE_DOMAINS");
		if(excludedomains != NULL)
		{
			getSetupVarsArray(excludedomains);
		}
	}

	int n = 0;
	for(int i=0; i < counters->domains; i++)
	{
		// Get sorted index
		const int domainID = temparray[i][0];
		// Get domain pointer
		const domainsData* domain = getDomain(domainID, true);

		// Skip this domain if there is a filter on it
		if(excludedomains != NULL && insetupVarsArray(getstr(domain->domainpos)))
			continue;

		// Skip this domain if already audited
		if(audit && in_auditlist(getstr(domain->domainpos)) > 0)
		{
			if(config.debug & DEBUG_API)
				logg("API: %s has been audited.", getstr(domain->domainpos));
			continue;
		}

		// Hidden domain, probably due to privacy level. Skip this in the top lists
		if(strcmp(getstr(domain->domainpos), HIDDEN_DOMAIN) == 0)
			continue;

		if(blocked && showblocked && domain->blockedcount > 0)
		{
			if(audit && domain->regexmatch == REGEX_BLOCKED)
			{
				http_send(conn, false, "%i %i %s wildcard\n", n, domain->blockedcount, getstr(domain->domainpos));
			}
			else
			{
				http_send(conn, false, "%i %i %s\n", n, domain->blockedcount, getstr(domain->domainpos));
			}
			n++;
		}
		else if(!blocked && showpermitted && (domain->count - domain->blockedcount) > 0)
		{
			http_send(conn, false, "%i %i %s\n",n,(domain->count - domain->blockedcount),getstr(domain->domainpos));
			n++;
		}

		// Only count entries that are actually sent and return when we have send enough data
		if(n == count)
			break;
	}

	if(excludedomains != NULL)
		clearSetupVarsArray();
}

void getTopClients(const bool blocked_only, struct mg_connection *conn)
{
	int temparray[counters->clients][2], count=10;

	// Exit before processing any data if requested via config setting
	get_privacy_level(NULL);
	if(config.privacylevel >= PRIVACY_HIDE_DOMAINS_CLIENTS) {
		return;
	}
/*
	// Match both top-domains and top-ads
	// example: >top-clients (15)
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0) {
		// User wants a different number of requests
		count = num;
	}
*/
	// Show also clients which have not been active recently?
	// This option can be combined with existing options,
	// i.e. both >top-clients withzero" and ">top-clients withzero (123)" are valid
	bool includezeroclients = false;
/*
	if(command(client_message, " withzero"))
		includezeroclients = true;
*/
	// Show number of blocked queries instead of total number?
	// This option can be combined with existing options,
	// i.e. ">top-clients withzero blocked (123)" would be valid
	bool blockedonly = false;
/*
	if(command(client_message, " blocked"))
		blockedonly = true;
*/
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get client pointer
		const clientsData* client = getClient(clientID, true);
		temparray[clientID][0] = clientID;
		// Use either blocked or total count based on request string
		temparray[clientID][1] = blockedonly ? client->blockedcount : client->count;
	}

	// Sort in ascending order?
	// example: >top-clients asc
	bool asc = false;
/*
	if(command(client_message, " asc"))
		asc = true;
*/
	// Sort temporary array
	if(asc)
		qsort(temparray, counters->clients, sizeof(int[2]), cmpasc);
	else
		qsort(temparray, counters->clients, sizeof(int[2]), cmpdesc);

	// Get clients which the user doesn't want to see
	const char* excludeclients = read_setupVarsconf("API_EXCLUDE_CLIENTS");
	if(excludeclients != NULL)
	{
		getSetupVarsArray(excludeclients);
	}

	int n = 0;
	for(int i=0; i < counters->clients; i++)
	{
		// Get sorted indices and counter values (may be either total or blocked count)
		const int clientID = temparray[i][0];
		const int ccount = temparray[i][1];
		// Get client pointer
		const clientsData* client = getClient(clientID, true);

		// Skip this client if there is a filter on it
		if(excludeclients != NULL &&
			(insetupVarsArray(getstr(client->ippos)) || insetupVarsArray(getstr(client->namepos))))
			continue;

		// Hidden client, probably due to privacy level. Skip this in the top lists
		if(strcmp(getstr(client->ippos), HIDDEN_CLIENT) == 0)
			continue;

		// Get client IP and name
		const char *client_ip = getstr(client->ippos);
		const char *client_name = getstr(client->namepos);

		// Return this client if either
		// - "withzero" option is set, and/or
		// - the client made at least one query within the most recent 24 hours
		if(includezeroclients || ccount > 0)
		{
			http_send(conn, false, "%i %i %s %s\n", n, ccount, client_ip, client_name);
			n++;
		}

		if(n == count)
			break;
	}

	if(excludeclients != NULL)
		clearSetupVarsArray();
}


void getForwardDestinations(struct mg_connection *conn)
{
	bool sort = true;
	int temparray[counters->forwarded][2], totalqueries = 0;
/*
	if(command(client_message, "unsorted"))
		sort = false;
*/
	for(int forwardID = 0; forwardID < counters->forwarded; forwardID++)
	{
		// If we want to print a sorted output, we fill the temporary array with
		// the values we will use for sorting afterwards
		if(sort) {
			// Get forward pointer
			const forwardedData* forward = getForward(forwardID, true);

			temparray[forwardID][0] = forwardID;
			temparray[forwardID][1] = forward->count;
		}
	}

	if(sort)
	{
		// Sort temporary array in descending order
		qsort(temparray, counters->forwarded, sizeof(int[2]), cmpdesc);
	}

	totalqueries = counters->forwardedqueries + counters->cached + counters->blocked;

	// Loop over available forward destinations
	for(int i = -2; i < min(counters->forwarded, 8); i++)
	{
		float percentage = 0.0f;
		const char* ip, *name;

		if(i == -2)
		{
			// Blocked queries (local lists)
			ip = "blocklist";
			name = ip;

			if(totalqueries > 0)
				// Whats the percentage of locked queries on the total amount of queries?
				percentage = 1e2f * counters->blocked / totalqueries;
		}
		else if(i == -1)
		{
			// Local cache
			ip = "cache";
			name = ip;

			if(totalqueries > 0)
				// Whats the percentage of cached queries on the total amount of queries?
				percentage = 1e2f * counters->cached / totalqueries;
		}
		else
		{
			// Regular forward destionation
			// Get sorted indices
			int forwardID;
			if(sort)
				forwardID = temparray[i][0];
			else
				forwardID = i;

			// Get forward pointer
			const forwardedData* forward = getForward(forwardID, true);

			// Get IP and host name of forward destination if available
			ip = getstr(forward->ippos);
			name = getstr(forward->namepos);

			// Get percentage
			if(totalqueries > 0)
				percentage = 1e2f * forward->count / totalqueries;
		}

		// Send data:
		// - always if i < 0 (special upstreams: blocklist and cache)
		// - only if percentage > 0.0 for all others (i > 0)
		if(percentage > 0.0f || i < 0)
		{
			http_send(conn, false, "%i %.2f %s %s\n", i, percentage, ip, name);
		}
	}
}


void getQueryTypes(struct mg_connection *conn)
{
	int total = 0;
	for(int i=0; i < TYPE_MAX-1; i++)
	{
		total += counters->querytype[i];
	}

	float percentage[TYPE_MAX] = { 0.0 };

	// Prevent floating point exceptions by checking if the divisor is != 0
	if(total > 0)
	{
		for(int i=0; i < TYPE_MAX; i++)
		{
			percentage[i] = 1e2f*counters->querytype[i]/total;
		}
	}

	http_send(conn, false, "A (IPv4): %.2f\nAAAA (IPv6): %.2f\nANY: %.2f\nSRV: %.2f\nSOA: %.2f\nPTR: %.2f\nTXT: %.2f\n",
		percentage[TYPE_A], percentage[TYPE_AAAA], percentage[TYPE_ANY], percentage[TYPE_SRV],
		percentage[TYPE_SOA], percentage[TYPE_PTR], percentage[TYPE_TXT]);
}

static const char *querytypes[8] = {"A","AAAA","ANY","SRV","SOA","PTR","TXT","UNKN"};

void getAllQueries(const char *client_message, struct mg_connection *conn)
{
	// Exit before processing any data if requested via config setting
	get_privacy_level(NULL);
	if(config.privacylevel >= PRIVACY_MAXIMUM)
		return;

	// Do we want a more specific version of this command (domain/client/time interval filtered)?
	int from = 0, until = 0;

	char *domainname = NULL;
	bool filterdomainname = false;
	int domainid = -1;

	char *clientname = NULL;
	bool filterclientname = false;
	int clientid = -1;

	int querytype = 0;

	char *forwarddest = NULL;
	bool filterforwarddest = false;
	int forwarddestid = 0;
/*
	// Time filtering?
	if(command(client_message, ">getallqueries-time")) {
		sscanf(client_message, ">getallqueries-time %i %i",&from, &until);
	}

	// Query type filtering?
	if(command(client_message, ">getallqueries-qtype")) {
		// Get query type we want to see only
		sscanf(client_message, ">getallqueries-qtype %i", &querytype);
		if(querytype < 1 || querytype >= TYPE_MAX)
		{
			// Invalid query type requested
			return;
		}
	}

	// Forward destination filtering?
	if(command(client_message, ">getallqueries-forward")) {
		// Get forward destination name we want to see only (limit length to 255 chars)
		forwarddest = calloc(256, sizeof(char));
		if(forwarddest == NULL) return;
		sscanf(client_message, ">getallqueries-forward %255s", forwarddest);
		filterforwarddest = true;

		if(strcmp(forwarddest, "cache") == 0)
			forwarddestid = -1;
		else if(strcmp(forwarddest, "blocklist") == 0)
			forwarddestid = -2;
		else
		{
			// Iterate through all known forward destinations
			forwarddestid = -3;
			for(int i = 0; i < counters->forwarded; i++)
			{
				// Get forward pointer
				const forwardedData* forward = getForward(i, true);
				// Try to match the requested string against their IP addresses and
				// (if available) their host names
				if(strcmp(getstr(forward->ippos), forwarddest) == 0 ||
				   (forward->namepos != 0 &&
				    strcmp(getstr(forward->namepos), forwarddest) == 0))
				{
					forwarddestid = i;
					break;
				}
			}
			if(forwarddestid < 0)
			{
				// Requested forward destination has not been found, we directly
				// exit here as there is no data to be returned
				free(forwarddest);
				return;
			}
		}
	}

	// Domain filtering?
	if(command(client_message, ">getallqueries-domain")) {
		// Get domain name we want to see only (limit length to 255 chars)
		domainname = calloc(256, sizeof(char));
		if(domainname == NULL) return;
		sscanf(client_message, ">getallqueries-domain %255s", domainname);
		filterdomainname = true;
		// Iterate through all known domains
		for(int domainID = 0; domainID < counters->domains; domainID++)
		{
			// Get domain pointer
			const domainsData* domain = getDomain(domainID, true);

			// Try to match the requested string
			if(strcmp(getstr(domain->domainpos), domainname) == 0)
			{
				domainid = domainID;
				break;
			}
		}
		if(domainid < 0)
		{
			// Requested domain has not been found, we directly
			// exit here as there is no data to be returned
			free(domainname);
			return;
		}
	}

	// Client filtering?
	if(command(client_message, ">getallqueries-client")) {
		// Get client name we want to see only (limit length to 255 chars)
		clientname = calloc(256, sizeof(char));
		if(clientname == NULL) return;
		sscanf(client_message, ">getallqueries-client %255s", clientname);
		filterclientname = true;

		// Iterate through all known clients
		for(int i = 0; i < counters->clients; i++)
		{
			// Get client pointer
			const clientsData* client = getClient(i, true);
			// Try to match the requested string
			if(strcmp(getstr(client->ippos), clientname) == 0 ||
			   (client->namepos != 0 &&
			    strcmp(getstr(client->namepos), clientname) == 0))
			{
				clientid = i;
				break;
			}
		}
		if(clientid < 0)
		{
			// Requested client has not been found, we directly
			// exit here as there is no data to be returned
			free(clientname);
			return;
		}
	}
*/
	int ibeg = 0, num;
	// Test for integer that specifies number of entries to be shown
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0)
	{
		// User wants a different number of requests
		// Don't allow a start index that is smaller than zero
		ibeg = counters->queries-num;
		if(ibeg < 0)
			ibeg = 0;
	}

	// Get potentially existing filtering flags
	char * filter = read_setupVarsconf("API_QUERY_LOG_SHOW");
	bool showpermitted = true, showblocked = true;
	if(filter != NULL)
	{
		if((strcmp(filter, "permittedonly")) == 0)
			showblocked = false;
		else if((strcmp(filter, "blockedonly")) == 0)
			showpermitted = false;
		else if((strcmp(filter, "nothing")) == 0)
		{
			showpermitted = false;
			showblocked = false;
		}
	}
	clearSetupVarsArray();

	for(int queryID = ibeg; queryID < counters->queries; queryID++)
	{
		const queriesData* query = getQuery(queryID, true);
		// Check if this query has been create while in maximum privacy mode
		if(query->privacylevel >= PRIVACY_MAXIMUM) continue;

		// Verify query type
		if(query->type >= TYPE_MAX)
			continue;
		// Get query type
		const char *qtype = querytypes[query->type];

		// 1 = gravity.list, 4 = wildcard, 5 = black.list
		if((query->status == QUERY_GRAVITY ||
		    query->status == QUERY_WILDCARD ||
		    query->status == QUERY_BLACKLIST) && !showblocked)
			continue;
		// 2 = forwarded, 3 = cached
		if((query->status == QUERY_FORWARDED ||
		    query->status == QUERY_CACHE) && !showpermitted)
			continue;

		// Skip those entries which so not meet the requested timeframe
		if((from > query->timestamp && from != 0) || (query->timestamp > until && until != 0))
			continue;

		// Skip if domain is not identical with what the user wants to see
		if(filterdomainname && query->domainID != domainid)
			continue;

		// Skip if client name and IP are not identical with what the user wants to see
		if(filterclientname && query->clientID != clientid)
			continue;

		// Skip if query type is not identical with what the user wants to see
		if(querytype != 0 && querytype != query->type)
			continue;

		if(filterforwarddest)
		{
			// Does the user want to see queries answered from blocking lists?
			if(forwarddestid == -2 && query->status != QUERY_GRAVITY
			                       && query->status != QUERY_WILDCARD
			                       && query->status != QUERY_BLACKLIST)
				continue;
			// Does the user want to see queries answered from local cache?
			else if(forwarddestid == -1 && query->status != QUERY_CACHE)
				continue;
			// Does the user want to see queries answered by an upstream server?
			else if(forwarddestid >= 0 && forwarddestid != query->forwardID)
				continue;
		}

		// Ask subroutine for domain. It may return "hidden" depending on
		// the privacy settings at the time the query was made
		const char *domain = getDomainString(queryID);

		// Similarly for the client
		const char *clientIPName = NULL;
		// Get client pointer
		const clientsData* client = getClient(query->clientID, true);
		if(strlen(getstr(client->namepos)) > 0)
			clientIPName = getClientNameString(queryID);
		else
			clientIPName = getClientIPString(queryID);

		unsigned long delay = query->response;
		// Check if received (delay should be smaller than 30min)
		if(delay > 1.8e7)
			delay = 0;

		http_send(conn, false, "%li %s %s %s %i %i %i %lu",query->timestamp,qtype,domain,clientIPName,query->status,query->dnssec,query->reply,delay);
		if(config.debug & DEBUG_API)
			http_send(conn, false, " %i", queryID);
		http_send(conn, false, "\n");
	}

	// Free allocated memory
	if(filterclientname)
		free(clientname);

	if(filterdomainname)
		free(domainname);

	if(filterforwarddest)
		free(forwarddest);
}

void getRecentBlocked(const char *client_message, struct mg_connection *conn)
{
	int num=1;

	// Test for integer that specifies number of entries to be shown
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0) {
		// User wants a different number of requests
		if(num >= counters->queries)
			num = 0;
	}

	// Find most recently blocked query
	int found = 0;
	for(int queryID = counters->queries - 1; queryID > 0 ; queryID--)
	{
		const queriesData* query = getQuery(queryID, true);

		if(query->status == QUERY_GRAVITY ||
		   query->status == QUERY_WILDCARD ||
		   query->status == QUERY_BLACKLIST)
		{
			found++;

			// Ask subroutine for domain. It may return "hidden" depending on
			// the privacy settings at the time the query was made
			const char *domain = getDomainString(queryID);

			http_send(conn, false, "%s\n", domain);
		}

		if(found >= num)
			break;
	}
}

void getClientIP(struct mg_connection *conn)
{
	const struct mg_request_info *request = mg_get_request_info(conn);
	http_send(conn, false, "remote_addr:\"%s\"", request->remote_addr);
}
/*
void getQueryTypesOverTime(struct mg_connection *conn)
{
	int from = -1, until = OVERTIME_SLOTS;
	const time_t mintime = overTime[0].timestamp;

	for(int slot = 0; slot < OVERTIME_SLOTS; slot++)
	{
		if((overTime[slot].total > 0 || overTime[slot].blocked > 0) && overTime[slot].timestamp >= mintime)
		{
			from = slot;
			break;
		}
	}

	// End with last non-empty overTime slot
	for(int slot = 0; slot < OVERTIME_SLOTS; slot++)
	{
		if(overTime[slot].timestamp >= time(NULL))
		{
			until = slot;
			break;
		}
	}

	// No data?
	if(from < 0)
		return;

	for(int slot = from; slot < until; slot++)
	{
		float percentageIPv4 = 0.0, percentageIPv6 = 0.0;
		int sum = overTime[slot].querytypedata[0] + overTime[slot].querytypedata[1];

		if(sum > 0) {
			percentageIPv4 = (float) (1e2 * overTime[slot].querytypedata[0] / sum);
			percentageIPv6 = (float) (1e2 * overTime[slot].querytypedata[1] / sum);
		}

		http_send(conn, false, "%li %.2f %.2f\n", overTime[slot].timestamp, percentageIPv4, percentageIPv6);
	}
}
*/
void api_ftl_version(struct mg_connection *conn)
{
	const char *commit = GIT_HASH;
	const char *tag = GIT_TAG;
	const char *version = get_FTL_version();

	// Extract first 7 characters of the hash
	char hash[8];
	memcpy(hash, commit, 7); hash[7] = 0;

	if(strlen(tag) > 1) {
		http_send(conn, false,
			  "{\"version\":\"%s\",\"tag\":\"%s\",\"branch\":\"%s\",\"hash\":\"%s\",\"date\":\"%s\"}",
			  version, tag, GIT_BRANCH, hash, GIT_DATE
		);
	} else {
		http_send(conn, false,
			  "{\"version\":\"vDev-%s\",\"tag\":\"%s\",\"branch\":\"%s\",\"hash\":\"%s\",\"date\":\"%s\"}",
			  hash, tag, GIT_BRANCH, hash, GIT_DATE
		);
	}
}

void api_ftl_db(struct mg_connection *conn)
{
	// Get file details
	unsigned long long int filesize = get_FTL_db_filesize();

	char *prefix = calloc(2, sizeof(char));
	if(prefix == NULL) return;
	double formated = 0.0;
	format_memory_size(prefix, filesize, &formated);

	http_send(conn, false,
		  "{\"queries in database\":%i,"
		  "\"database filesize_raw\":%llu,"
		  "\"database filesize_formatted\":\"%.2f %sB\","
		  "\"SQLite version\":\"%s\"}",
		  get_number_of_queries_in_DB(),
		  filesize,
		  formated, prefix,
		  get_sqlite3_version());
}

void getClientsOverTime(struct mg_connection *conn)
{
	int sendit = -1, until = OVERTIME_SLOTS;

	// Exit before processing any data if requested via config setting
	get_privacy_level(NULL);
	if(config.privacylevel >= PRIVACY_HIDE_DOMAINS_CLIENTS)
		return;

	// Find minimum ID to send
	for(int slot = 0; slot < OVERTIME_SLOTS; slot++)
	{
		if((overTime[slot].total > 0 || overTime[slot].blocked > 0) &&
		   overTime[slot].timestamp >= overTime[0].timestamp)
		{
			sendit = slot;
			break;
		}
	}
	if(sendit < 0)
		return;

	// Find minimum ID to send
	for(int slot = 0; slot < OVERTIME_SLOTS; slot++)
	{
		if(overTime[slot].timestamp >= time(NULL))
		{
			until = slot;
			break;
		}
	}

	// Get clients which the user doesn't want to see
	char * excludeclients = read_setupVarsconf("API_EXCLUDE_CLIENTS");
	// Array of clients to be skipped in the output
	// if skipclient[i] == true then this client should be hidden from
	// returned data. We initialize it with false
	bool skipclient[counters->clients];
	memset(skipclient, false, counters->clients*sizeof(bool));

	if(excludeclients != NULL)
	{
		getSetupVarsArray(excludeclients);

		for(int clientID=0; clientID < counters->clients; clientID++)
		{
			// Get client pointer
			const clientsData* client = getClient(clientID, true);
			// Check if this client should be skipped
			if(insetupVarsArray(getstr(client->ippos)) ||
			   insetupVarsArray(getstr(client->namepos)))
				skipclient[clientID] = true;
		}
	}

	// Main return loop
	for(int slot = sendit; slot < until; slot++)
	{
		http_send(conn, false, "%li", overTime[slot].timestamp);

		// Loop over forward destinations to generate output to be sent to the client
		for(int clientID = 0; clientID < counters->clients; clientID++)
		{
			if(skipclient[clientID])
				continue;

			// Get client pointer
			const clientsData* client = getClient(clientID, true);
			const int thisclient = client->overTime[slot];

			http_send(conn, false, " %i", thisclient);
		}

		http_send(conn, false, "\n");
	}

	if(excludeclients != NULL)
		clearSetupVarsArray();
}

void getClientNames(struct mg_connection *conn)
{
	// Exit before processing any data if requested via config setting
	get_privacy_level(NULL);
	if(config.privacylevel >= PRIVACY_HIDE_DOMAINS_CLIENTS)
		return;

	// Get clients which the user doesn't want to see
	char * excludeclients = read_setupVarsconf("API_EXCLUDE_CLIENTS");
	// Array of clients to be skipped in the output
	// if skipclient[i] == true then this client should be hidden from
	// returned data. We initialize it with false
	bool skipclient[counters->clients];
	memset(skipclient, false, counters->clients*sizeof(bool));

	if(excludeclients != NULL)
	{
		getSetupVarsArray(excludeclients);

		for(int clientID=0; clientID < counters->clients; clientID++)
		{
			// Get client pointer
			const clientsData* client = getClient(clientID, true);
			// Check if this client should be skipped
			if(insetupVarsArray(getstr(client->ippos)) ||
			   insetupVarsArray(getstr(client->namepos)))
				skipclient[clientID] = true;
		}
	}

	// Loop over clients to generate output to be sent to the client
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		if(skipclient[clientID])
			continue;

		// Get client pointer
		const clientsData* client = getClient(clientID, true);
		const char *client_ip = getstr(client->ippos);
		const char *client_name = getstr(client->namepos);

		http_send(conn, false, "%s %s\n", client_name, client_ip);
	}

	if(excludeclients != NULL)
		clearSetupVarsArray();
}
