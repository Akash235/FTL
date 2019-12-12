/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Regular Expressions
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "regex_r.h"
#include "timers.h"
#include "memory.h"
#include "log.h"
#include "config.h"
// data getter functions
#include "datastructure.h"
#include <regex.h>
#include "database/gravity-db.h"
// bool startup
#include "main.h"

static int num_regex[2] = { 0 };
static regex_t *regex[2] = { NULL };
static bool *regex_available[2] = { NULL };
static int *regex_id[2] = { NULL };
static char **regexbuffer[2] = { NULL };

static const char *regextype[] = { "blacklist", "whitelist" };

static void log_regex_error(const int errcode, const int index, const unsigned char regexid, const char *regexin)
{
	// Regex failed for some reason (probably user syntax error)
	// Get error string and log it
	const size_t length = regerror(errcode, &regex[regexid][index], NULL, 0);
	char *buffer = calloc(length,sizeof(char));
	(void) regerror (errcode, &regex[regexid][index], buffer, length);
	logg("Warning: Invalid regex %s filter \"%s\": %s (error code %i)", regextype[regexid], regexin, buffer, errcode);
	free(buffer);
}

static bool compile_regex(const char *regexin, const int index, const unsigned char regexid)
{
	// compile regular expressions into data structures that
	// can be used with regexec to match against a string
	int regflags = REG_EXTENDED;
	if(config.regex_ignorecase)
		regflags |= REG_ICASE;
	const int errcode = regcomp(&regex[regexid][index], regexin, regflags);
	if(errcode != 0)
	{
		log_regex_error(errcode, index, regexid, regexin);
		return false;
	}

	// Store compiled regex string in buffer if in regex debug mode
	if(config.debug & DEBUG_REGEX)
	{
		regexbuffer[regexid][index] = strdup(regexin);
	}

	return true;
}

bool match_regex(const char *input, const clientsData *client, const unsigned char regexid)
{
	bool matched = false;

	if(client->regex_enabled[regexid] == NULL)
	{
		logg("Regex list %d for client not configured!", regexid);
		return false;
	}

	// Start matching timer
	timer_start(REGEX_TIMER);
	for(int index = 0; index < num_regex[regexid]; index++)
	{
		// Only check regex which have been successfully compiled ...
		if(!regex_available[regexid][index])
		{
			if(config.debug & DEBUG_REGEX)
				logg("Regex %s ID %d not available", regextype[regexid], index);

			continue;
		}
		// ... and are enabled for this client
		if(!client->regex_enabled[regexid][index])
		{
			if(config.debug & DEBUG_REGEX)
				logg("Regex %s ID %d not enabled for this client", regextype[regexid], index);

			continue;
		}

		// Try to match the compiled regular expression against input
		int errcode = regexec(&regex[regexid][index], input, 0, NULL, 0);
		// regexec() returns zero for a successful match or REG_NOMATCH for failure.
		// We are only interested in the matching case here.
		if (errcode == 0)
		{
			// Match, return true
			matched = true;

			// Print match message when in regex debug mode
			if(config.debug & DEBUG_REGEX)
				logg("Regex %s (ID %i) \"%s\" matches \"%s\"", regextype[regexid], regex_id[regexid][index], regexbuffer[regexid][index], input);
			break;
		}
	}

	double elapsed = timer_elapsed_msec(REGEX_TIMER);

	// Only log evaluation times if they are longer than normal
	if(elapsed > 10.0)
		logg("WARN: Regex %s evaluation took %.3f msec", regextype[regexid], elapsed);

	// No match, no error, return false
	return matched;
}

static void free_regex(void)
{
	// Reset cached regex results
	for(int i = 0u; i < counters->domains; i++)
	{
		// Get domain pointer
		domainsData *domain = getDomain(i, true);
		if(domain == NULL)
			continue;

		// Reset blocking status of domain for all clients to unknown
		for(int clientID = 0u; clientID < counters->clients; clientID++)
		{
			domain->clientstatus->set(domain->clientstatus, clientID, UNKNOWN_BLOCKED);
		}
	}

	// Return early if we don't use any regex filters
	if(regex[REGEX_WHITELIST] == NULL &&
	   regex[REGEX_BLACKLIST] == NULL)
		return;

	// Reset client configuration
	for(int i = 0; i < counters->clients; i++)
	{
		// Get client pointer
		clientsData *client = getClient(i, true);
		if(client == NULL)
			continue;

		if(client->regex_enabled[REGEX_WHITELIST] != NULL)
		{
			free(client->regex_enabled[REGEX_WHITELIST]);
			client->regex_enabled[REGEX_WHITELIST] = NULL;
		}

		if(client->regex_enabled[REGEX_BLACKLIST] != NULL)
		{
			free(client->regex_enabled[REGEX_BLACKLIST]);
			client->regex_enabled[REGEX_BLACKLIST] = NULL;
		}
	}

	// Free regex datastructure
	for(int regexid = 0; regexid < 2; regexid++)
	{
		for(int index = 0; index < num_regex[regexid]; index++)
		{
			if(!regex_available[regexid][index])
				continue;

			regfree(&regex[regexid][index]);

			// Also free buffered regex strings if in regex debug mode
			if(config.debug & DEBUG_REGEX && regexbuffer[regexid][index] != NULL)
			{
				free(regexbuffer[regexid][index]);
				regexbuffer[regexid][index] = NULL;
			}
		}

		// Free array with regex datastructure
		if(regex[regexid] != NULL)
		{
			free(regex[regexid]);
			regex[regexid] = NULL;
		}

		// Reset counter for number of regex
		num_regex[regexid] = 0;
	}
}

void allocate_regex_client_enabled(clientsData *client)
{
	client->regex_enabled[REGEX_BLACKLIST] = calloc(num_regex[REGEX_BLACKLIST], sizeof(bool));
	client->regex_enabled[REGEX_WHITELIST] = calloc(num_regex[REGEX_WHITELIST], sizeof(bool));

	// Only initialize regex associations when dnsmasq is ready (otherwise, we're still in history reading mode)
	if(!startup)
	{
		gravityDB_get_regex_client_groups(client, num_regex[REGEX_BLACKLIST],
						regex_id[REGEX_BLACKLIST], REGEX_BLACKLIST,
						"vw_regex_blacklist");
		gravityDB_get_regex_client_groups(client, num_regex[REGEX_WHITELIST],
						regex_id[REGEX_WHITELIST], REGEX_WHITELIST,
						"vw_regex_whitelist");
	}
}

static void read_regex_table(const unsigned char regexid)
{
	// Get database ID
	unsigned char databaseID = (regexid == REGEX_BLACKLIST) ? REGEX_BLACKLIST_TABLE : REGEX_WHITELIST_TABLE;

	// Get number of lines in the regex table
	num_regex[regexid] = gravityDB_count(databaseID);

	if(num_regex[regexid] == 0)
	{
		logg("INFO: No regex %s entries found", regextype[regexid]);
		return;
	}
	else if(num_regex[regexid] == DB_FAILED)
	{
		logg("WARN: Database query failed, assuming there are no regex %s entries", regextype[regexid]);
		num_regex[regexid] = 0;
		return;
	}

	// Allocate memory for regex
	regex[regexid] = calloc(num_regex[regexid], sizeof(regex_t));
	regex_id[regexid] = calloc(num_regex[regexid], sizeof(int));
	regex_available[regexid] = calloc(num_regex[regexid], sizeof(bool));

	// Buffer strings if in regex debug mode
	if(config.debug & DEBUG_REGEX)
		regexbuffer[regexid] = calloc(num_regex[regexid], sizeof(char*));

	// Connect to regex table
	if(!gravityDB_getTable(databaseID))
	{
		logg("read_regex_from_database(): Error getting regex %s table from database", regextype[regexid]);
		return;
	}

	// Walk database table
	const char *domain = NULL;
	int i = 0, rowid = 0;
	while((domain = gravityDB_getDomain(&rowid)) != NULL)
	{
		// Avoid buffer overflow if database table changed
		// since we counted its entries
		if(i >= num_regex[regexid])
			break;

		// Skip this entry if empty: an empty regex filter would match
		// anything anywhere and hence match all incoming domains. A user
		// can still achieve this with a filter such as ".*", however empty
		// filters in the regex table are probably not expected to have such
		// an effect and would immediately lead to "blocking or whitelisting
		// the entire Internet"
		if(strlen(domain) < 1)
			continue;

		// Compile this regex
		regex_available[regexid][i] = compile_regex(domain, i, regexid);
		regex_id[regexid][i] = rowid;

		// Increase counter
		i++;
	}

	// Finalize statement and close gravity database handle
	gravityDB_finalizeTable();
}

void read_regex_from_database(void)
{
	// Free regex filters
	// This routine is safe to be called even when there
	// are no regex filters at the moment
	free_regex();

	// Start timer for regex compilation analysis
	timer_start(REGEX_TIMER);

	// Read and compile regex blacklist
	read_regex_table(REGEX_BLACKLIST);

	// Read and compile regex whitelist
	read_regex_table(REGEX_WHITELIST);


	for(int i = 0; i < counters->clients; i++)
	{
		// Get client pointer
		clientsData *client = getClient(i, true);
		if(client == NULL)
			continue;

		allocate_regex_client_enabled(client);
	}

	// Print message to FTL's log after reloading regex filters
	logg("Compiled %i whitelist and %i blacklist regex filters in %.1f msec",
	     num_regex[REGEX_WHITELIST], num_regex[REGEX_BLACKLIST], timer_elapsed_msec(REGEX_TIMER));
}
