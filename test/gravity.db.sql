PRAGMA FOREIGN_KEYS=ON;
BEGIN TRANSACTION;

CREATE TABLE "group"
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	enabled BOOLEAN NOT NULL DEFAULT 1,
	name TEXT NOT NULL,
	description TEXT
);

CREATE TABLE whitelist
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	domain TEXT UNIQUE NOT NULL,
	enabled BOOLEAN NOT NULL DEFAULT 1,
	date_added INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	date_modified INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	comment TEXT
);

CREATE TABLE whitelist_by_group
(
	whitelist_id INTEGER NOT NULL REFERENCES whitelist (id),
	group_id INTEGER NOT NULL REFERENCES "group" (id),
	PRIMARY KEY (whitelist_id, group_id)
);

CREATE TABLE blacklist
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	domain TEXT UNIQUE NOT NULL,
	enabled BOOLEAN NOT NULL DEFAULT 1,
	date_added INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	date_modified INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	comment TEXT
);

CREATE TABLE blacklist_by_group
(
	blacklist_id INTEGER NOT NULL REFERENCES blacklist (id),
	group_id INTEGER NOT NULL REFERENCES "group" (id),
	PRIMARY KEY (blacklist_id, group_id)
);

CREATE TABLE regex_blacklist
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	domain TEXT UNIQUE NOT NULL,
	enabled BOOLEAN NOT NULL DEFAULT 1,
	date_added INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	date_modified INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	comment TEXT
);

CREATE TABLE regex_blacklist_by_group
(
	regex_blacklist_id INTEGER NOT NULL REFERENCES regex_blacklist (id),
	group_id INTEGER NOT NULL REFERENCES "group" (id),
	PRIMARY KEY (regex_blacklist_id, group_id)
);

CREATE TABLE regex_whitelist
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	domain TEXT UNIQUE NOT NULL,
	enabled BOOLEAN NOT NULL DEFAULT 1,
	date_added INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	date_modified INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	comment TEXT
);

CREATE TABLE regex_whitelist_by_group
(
	regex_whitelist_id INTEGER NOT NULL REFERENCES regex_whitelist (id),
	group_id INTEGER NOT NULL REFERENCES "group" (id),
	PRIMARY KEY (regex_whitelist_id, group_id)
);

CREATE TABLE adlist
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	address TEXT UNIQUE NOT NULL,
	enabled BOOLEAN NOT NULL DEFAULT 1,
	date_added INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	date_modified INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int)),
	comment TEXT
);

CREATE TABLE adlist_by_group
(
	adlist_id INTEGER NOT NULL REFERENCES adlist (id),
	group_id INTEGER NOT NULL REFERENCES "group" (id),
	PRIMARY KEY (adlist_id, group_id)
);

CREATE TABLE gravity
(
	domain TEXT NOT NULL,
	adlist_id INTEGER NOT NULL REFERENCES adlist (id),
	PRIMARY KEY(domain, adlist_id)
);

CREATE TABLE client
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	ip TEXT NOL NULL UNIQUE
);

CREATE TABLE client_by_group
(
	client_id INTEGER NOT NULL REFERENCES client (id),
	group_id INTEGER NOT NULL REFERENCES "group" (id),
	PRIMARY KEY (client_id, group_id)
);

CREATE TABLE domain_audit
(
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	domain TEXT UNIQUE NOT NULL,
	date_added INTEGER NOT NULL DEFAULT (cast(strftime('%s', 'now') as int))
);

CREATE TABLE info
(
	property TEXT PRIMARY KEY,
	value TEXT NOT NULL
);

CREATE VIEW vw_gravity AS SELECT domain, adlist_by_group.group_id AS group_id
    FROM gravity
    LEFT JOIN adlist_by_group ON adlist_by_group.adlist_id = gravity.adlist_id
    LEFT JOIN adlist ON adlist.id = gravity.adlist_id
    LEFT JOIN "group" ON "group".id = adlist_by_group.group_id
    WHERE adlist.enabled = 1 AND (adlist_by_group.group_id IS NULL OR "group".enabled = 1);

CREATE VIEW vw_whitelist AS SELECT domain, whitelist.id AS id, whitelist_by_group.group_id AS group_id
    FROM whitelist
    LEFT JOIN whitelist_by_group ON whitelist_by_group.whitelist_id = whitelist.id
    LEFT JOIN "group" ON "group".id = whitelist_by_group.group_id
    WHERE whitelist.enabled = 1 AND (whitelist_by_group.group_id IS NULL OR "group".enabled = 1)
    ORDER BY whitelist.id;

CREATE TRIGGER tr_whitelist_update AFTER UPDATE ON whitelist
    BEGIN
      UPDATE whitelist SET date_modified = (cast(strftime('%s', 'now') as int)) WHERE domain = NEW.domain;
    END;

CREATE VIEW vw_blacklist AS SELECT domain, blacklist.id AS id, blacklist_by_group.group_id AS group_id
    FROM blacklist
    LEFT JOIN blacklist_by_group ON blacklist_by_group.blacklist_id = blacklist.id
    LEFT JOIN "group" ON "group".id = blacklist_by_group.group_id
    WHERE blacklist.enabled = 1 AND (blacklist_by_group.group_id IS NULL OR "group".enabled = 1)
    ORDER BY blacklist.id;

CREATE TRIGGER tr_blacklist_update AFTER UPDATE ON blacklist
    BEGIN
      UPDATE blacklist SET date_modified = (cast(strftime('%s', 'now') as int)) WHERE domain = NEW.domain;
    END;

CREATE VIEW vw_regex_blacklist AS SELECT DISTINCT domain, regex_blacklist.id AS id, regex_blacklist_by_group.group_id AS group_id
    FROM regex_blacklist
    LEFT JOIN regex_blacklist_by_group ON regex_blacklist_by_group.regex_blacklist_id = regex_blacklist.id
    LEFT JOIN "group" ON "group".id = regex_blacklist_by_group.group_id
    WHERE regex_blacklist.enabled = 1 AND (regex_blacklist_by_group.group_id IS NULL OR "group".enabled = 1)
    ORDER BY regex_blacklist.id;

CREATE TRIGGER tr_regex_blacklist_update AFTER UPDATE ON regex_blacklist
    BEGIN
      UPDATE regex_blacklist SET date_modified = (cast(strftime('%s', 'now') as int)) WHERE domain = NEW.domain;
    END;

CREATE VIEW vw_regex_whitelist AS SELECT DISTINCT domain, regex_whitelist.id AS id, regex_whitelist_by_group.group_id AS group_id
    FROM regex_whitelist
    LEFT JOIN regex_whitelist_by_group ON regex_whitelist_by_group.regex_whitelist_id = regex_whitelist.id
    LEFT JOIN "group" ON "group".id = regex_whitelist_by_group.group_id
    WHERE regex_whitelist.enabled = 1 AND (regex_whitelist_by_group.group_id IS NULL OR "group".enabled = 1)
    ORDER BY regex_whitelist.id;

CREATE TRIGGER tr_regex_whitelist_update AFTER UPDATE ON regex_whitelist
    BEGIN
      UPDATE regex_whitelist SET date_modified = (cast(strftime('%s', 'now') as int)) WHERE domain = NEW.domain;
    END;

CREATE VIEW vw_adlist AS SELECT DISTINCT address
    FROM adlist
    LEFT JOIN adlist_by_group ON adlist_by_group.adlist_id = adlist.id
    LEFT JOIN "group" ON "group".id = adlist_by_group.group_id
    WHERE adlist.enabled = 1 AND (adlist_by_group.group_id IS NULL OR "group".enabled = 1)
    ORDER BY adlist.id;

CREATE TRIGGER tr_adlist_update AFTER UPDATE ON adlist
    BEGIN
      UPDATE adlist SET date_modified = (cast(strftime('%s', 'now') as int)) WHERE address = NEW.address;
    END;

INSERT INTO whitelist VALUES(1,'whitelisted.test.pi-hole.net',1,1559928803,1559928803,'Migrated from /etc/pihole/whitelist.txt');
INSERT INTO whitelist VALUES(2,'regex1.test.pi-hole.net',1,1559928803,1559928803,'');
INSERT INTO regex_whitelist VALUES(1,'regex2',1,1559928803,1559928803,'');
INSERT INTO regex_whitelist VALUES(2,'discourse',1,1559928803,1559928803,'');

INSERT INTO blacklist VALUES(1,'blacklist-blocked.test.pi-hole.net',1,1559928803,1559928803,'Migrated from /etc/pihole/blacklist.txt');
INSERT INTO regex_blacklist VALUES(1,'regex[0-9].test.pi-hole.net',1,1559928803,1559928803,'Migrated from /etc/pihole/regex.list');

INSERT INTO adlist VALUES(1,'https://hosts-file.net/ad_servers.txt',1,1559928803,1559928803,'Migrated from /etc/pihole/adlists.list');

INSERT INTO gravity VALUES('whitelisted.test.pi-hole.net',1);
INSERT INTO gravity VALUES('gravity-blocked.test.pi-hole.net',1);
INSERT INTO gravity VALUES('discourse.pi-hole.net',1);

INSERT INTO "group" VALUES(1,0,'Test group','A disabled test group');
INSERT INTO blacklist VALUES(2,'blacklisted-group-disabled.com',1,1559928803,1559928803,'Entry disabled by a group');
INSERT INTO blacklist_by_group VALUES(2,1);

INSERT INTO domain_audit VALUES(1,'google.com',1559928803);

INSERT INTO client VALUES(1,"127.0.0.1");

INSERT INTO client VALUES(2,"127.0.0.2");
INSERT INTO "group" VALUES(2,1,"Second test group","A group associated with client 127.0.0.2");
INSERT INTO client_by_group VALUES(2,2);
INSERT INTO adlist_by_group VALUES(1,2);
INSERT INTO regex_blacklist_by_group VALUES(1,2);

INSERT INTO client VALUES(3,"127.0.0.3");
INSERT INTO "group" VALUES(3,1,"Third test group","A group associated with client 127.0.0.3");
INSERT INTO client_by_group VALUES(3,3);

INSERT INTO info VALUES("version","4");

COMMIT;
