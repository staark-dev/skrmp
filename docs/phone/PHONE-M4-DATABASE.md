# PHONE-M4-DATABASE — Schema & Migration (Revised)

**Date:** 2026-10-24 (revised 2026-10-24)
**Status:** Design specification (M4-B, corrected)

---

## 1. Existing Database

The phone backend uses SQLite via open.mp `db_*` natives.

**File:** `scriptfiles/skr.db`
**Opened by:** `phone.inc` → `SKR_PhoneBankInit()`

### Existing Tables

| Table | Purpose | Schema |
|---|---|---|
| `accounts` | Bank balances | `nick TEXT PK, bank INTEGER, pin TEXT, created_ts INTEGER` |
| `txns` | Transaction log | `id INTEGER PK AUTO, nick TEXT, kind TEXT, amount INTEGER, counterparty TEXT, ts INTEGER` |

The bank tables use **player nickname** as the identity key. The phone tables
below also use **player nickname** as the identity key for `phone_numbers`.
(`SkrPlayerData::accountId` was audited on 2026-10-24 and found to be always 0
in the omp gate — `AuthorizePlayer()` has zero callers. See PHONE-M4-PROTOCOL.md §11.)
Contacts and messages are keyed by phone number, which is itself tied to the
nickname via `phone_numbers`.

---

## 2. New Tables (M4)

All new tables are created in the same `skr.db` file, initialized in
`SKR_PhoneDbInit()` called from `SKR_PhoneInit()` (or from the Gate's
direct SQLite initialization if using Gate-side DB access).

### phone_meta

Key-value store for phone system metadata.

```sql
CREATE TABLE IF NOT EXISTS phone_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

**Seed data:**
```sql
INSERT OR IGNORE INTO phone_meta(key, value) VALUES('next_number', '1000000');
```

### phone_numbers

Persistent phone number registry. One number per account.

```sql
CREATE TABLE IF NOT EXISTS phone_numbers (
    nick    TEXT PRIMARY KEY,
    number  TEXT NOT NULL UNIQUE,
    created INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_phone_number ON phone_numbers(number);
```

| Column | Type | Description |
|---|---|---|
| nick | TEXT PK | Authenticated player nickname (from `GetPlayerName()` after auth) |
| number | TEXT UNIQUE | 7-digit phone number ("1000000"–"9999999") |
| created | INTEGER | Unix timestamp of allocation |

**Migration path:** When `accountId` is wired in the future, add
`account_id INTEGER` column, backfill from nickname→account mapping,
then switch the lookup. No protocol changes needed.

### phone_contacts

Saved contacts per player. Keyed by the owner's phone number.

```sql
CREATE TABLE IF NOT EXISTS phone_contacts (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    owner   TEXT NOT NULL,
    number  TEXT NOT NULL,
    name    TEXT NOT NULL,
    created INTEGER NOT NULL,
    UNIQUE(owner, number)
);
CREATE INDEX IF NOT EXISTS idx_contacts_owner ON phone_contacts(owner);
```

| Column | Type | Description |
|---|---|---|
| id | INTEGER PK | Auto-increment row id |
| owner | TEXT | Owner's phone number (not nickname, not accountId) |
| number | TEXT | Contact's phone number |
| name | TEXT | Display name (player-chosen, ≤23 bytes) |
| created | INTEGER | Unix timestamp |

**Why phone number as owner key?** Contacts belong to the phone number, not
the account directly. The phone number is itself tied to the authenticated
nickname via `phone_numbers`, so the chain is: nick → phone number → contacts.
This makes contact lookups by number fast (no join needed) and means contacts
follow the phone number if it were ever reassigned (not planned, but sound).

**Limit:** Max 64 contacts per owner (enforced in application code).

### phone_messages

Persistent SMS storage.

```sql
CREATE TABLE IF NOT EXISTS phone_messages (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    sender    TEXT NOT NULL,
    recipient TEXT NOT NULL,
    text      TEXT NOT NULL,
    ts        INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_messages_thread
    ON phone_messages(
        MIN(sender, recipient),
        MAX(sender, recipient),
        ts DESC
    );
```

| Column | Type | Description |
|---|---|---|
| id | INTEGER PK | Unique message ID (returned in SmsSendResult) |
| sender | TEXT | Sender's phone number |
| recipient | TEXT | Recipient's phone number |
| text | TEXT | Message content (≤120 bytes) |
| ts | INTEGER | Unix timestamp |

**Changes from previous revision:**
- Removed `delivered INTEGER` column. SmsSendResult.status (Stored/Error)
  is the persistence acknowledgment. True delivery tracking is out of scope.
- Single composite index on `(MIN(sender,recipient), MAX(sender,recipient), ts DESC)`
  covers both directions of a conversation thread. Falls back to two
  separate indexes if SQLite version doesn't support expression indexes:

```sql
-- Fallback for older SQLite:
CREATE INDEX IF NOT EXISTS idx_messages_pair
    ON phone_messages(sender, recipient, ts DESC);
CREATE INDEX IF NOT EXISTS idx_messages_pair_rev
    ON phone_messages(recipient, sender, ts DESC);
```

---

## 3. Queries

### Number Allocation

```sql
BEGIN;
SELECT value FROM phone_meta WHERE key='next_number';
-- parse as integer, check < 10000000
UPDATE phone_meta SET value=? WHERE key='next_number';
INSERT INTO phone_numbers(account_id, number, created) VALUES(?, ?, ?);
COMMIT;
```

### Number Lookup

```sql
-- By accountId (on PHONE_SYNC)
SELECT number FROM phone_numbers WHERE account_id=?;

-- By number (for recipient resolution)
SELECT account_id FROM phone_numbers WHERE number=?;
```

### Contact Operations

```sql
-- List contacts page (page N, 11 per page)
SELECT number, name FROM phone_contacts
WHERE owner=?
ORDER BY name
LIMIT 11 OFFSET ?;
-- offset = page * 11

-- Add contact (idempotent: OR IGNORE)
INSERT OR IGNORE INTO phone_contacts(owner, number, name, created)
    VALUES(?, ?, ?, ?);

-- Remove contact
DELETE FROM phone_contacts WHERE owner=? AND number=?;

-- Count contacts (for CONTACTS_FULL check)
SELECT COUNT(*) FROM phone_contacts WHERE owner=?;
```

### SMS Operations

```sql
-- Send (insert)
INSERT INTO phone_messages(sender, recipient, text, ts) VALUES(?, ?, ?, ?);
-- returns last_insert_rowid() as messageId

-- Thread summaries (conversations for a player, page N, 4 per page)
-- Uses a correlated subquery to deterministically select the latest message:
SELECT
    peer,
    (SELECT text FROM phone_messages m2
     WHERE (m2.sender = ? AND m2.recipient = peer)
        OR (m2.recipient = ? AND m2.sender = peer)
     ORDER BY m2.ts DESC, m2.id DESC LIMIT 1
    ) AS preview,
    latest_ts
FROM (
    SELECT
        CASE WHEN sender = ? THEN recipient ELSE sender END AS peer,
        MAX(ts) AS latest_ts
    FROM phone_messages
    WHERE sender = ? OR recipient = ?
    GROUP BY peer
) threads
ORDER BY latest_ts DESC
LIMIT 4 OFFSET ?;
-- offset = page * 4; ? = player's phone number (bound 5 times)

-- Message history for a conversation pair (page N, 3 per page, newest first)
SELECT id, sender, text, ts
FROM phone_messages
WHERE (sender=?1 AND recipient=?2) OR (sender=?2 AND recipient=?1)
ORDER BY ts DESC, id DESC
LIMIT 3 OFFSET ?3;
-- ?1 = player's number, ?2 = peer's number, ?3 = page * 3
-- Direction is derived by the server: if sender == player's number → sent,
-- otherwise → received. This is set in the `direction` field of the response.
```

### Retention

```sql
-- Prune old messages (keep newest 100 per pair, run after insert)
DELETE FROM phone_messages
WHERE id IN (
    SELECT id FROM phone_messages
    WHERE (sender=?1 AND recipient=?2) OR (sender=?2 AND recipient=?1)
    ORDER BY ts DESC, id DESC
    LIMIT -1 OFFSET 100
);
```

The `ORDER BY ts DESC, id DESC` tiebreaker ensures deterministic ordering
when multiple messages share the same timestamp.

---

## 4. Migration Safety

- All `CREATE TABLE` and `CREATE INDEX` use `IF NOT EXISTS`.
- No existing tables are modified.
- The `phone_meta` seed uses `INSERT OR IGNORE`.
- Migration runs at gamemode init.
- If `skr.db` doesn't exist, `db_open` creates it.
- If the phone tables already exist (restart), the migration is a no-op.
- No `ALTER TABLE` — the schema is final for M4.

---

## 5. Thread Safety

All SQLite operations run on the open.mp main thread (same as Pawn script
execution). The `db_*` natives are synchronous and blocking. Performance:

- Queries are simple indexed lookups — sub-millisecond on a local SSD.
- The message table is bounded (100 per pair pruning).
- No full-table scans in hot paths.
- Number allocation happens once per account per server lifetime.
- Contact and SMS queries happen on user-initiated actions (button press).

The thread summary query involves a correlated subquery. For a player with
many conversation partners, this could be slow. Mitigation: the result is
paginated (4 per page), and the subquery only runs for the 4 selected
threads, not all threads.

---

## 6. Data Model Mapping

| Database | → Protocol field | → Client Model (phone_model.h) |
|---|---|---|
| phone_numbers.number | PhoneSync.ownNumber | Model.ownNumber[16] |
| phone_contacts.{name,number} | ContactList entries | Contact.{name,number} |
| phone_messages (grouped) | SmsThreadList entries (peerName resolved at send time) | ThreadSummary.{peer,preview,time,unread} |
| phone_messages (per pair) | SmsHistory entries (direction derived server-side) | Message.{text,time,delivery} |
| (server call state) | CallState (with callSessionId) | Caller.{name,number} + controller state |

### Direction derivation

The `direction` field in SmsHistory entries (u8, 0=outgoing, 1=incoming) is
computed by the server when building the response:
```cpp
direction = (msg.sender == playerNumber) ? 0 : 1;
```
It is never stored in the database (the sender/recipient columns are the
source of truth).

### Unread count

The `unread` count in SmsThreadList is computed per-thread by counting
messages where the player is the recipient and `ts > lastReadTs`. The
`lastReadTs` per conversation is tracked in server memory (reset on
SMS_HISTORY request for that peer) or optionally in a `phone_read_state`
table (out of M4 minimum scope — M4 may show all threads as unread=0 and
add read tracking in a follow-up).
