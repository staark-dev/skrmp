#pragma once
// Shared by the production SDK adapter and SQLite integration tests.
namespace skr::phone_m5 {
inline constexpr const char* kPoolSql = R"SQL(CREATE TABLE IF NOT EXISTS phone_number_pool(number TEXT PRIMARY KEY CHECK(length(number)=7 AND number GLOB '555[0-9][0-9][0-9][0-9]')))SQL";
inline constexpr const char* kFillPoolSql = R"SQL(WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<9999) INSERT OR IGNORE INTO phone_number_pool SELECT printf('555%04d',x) FROM n)SQL";
inline constexpr const char* kProfileSql = R"SQL(CREATE TABLE IF NOT EXISTS phone_profiles(nick TEXT PRIMARY KEY REFERENCES phone_numbers(nick), stage INTEGER NOT NULL DEFAULT 0 CHECK(stage BETWEEN 0 AND 4), language INTEGER NOT NULL DEFAULT 0 CHECK(language=0), wallpaper INTEGER NOT NULL DEFAULT 0 CHECK(wallpaper BETWEEN 0 AND 1), scale INTEGER NOT NULL DEFAULT 100 CHECK(scale BETWEEN 80 AND 125), banners INTEGER NOT NULL DEFAULT 1 CHECK(banners IN (0,1))))SQL";
// Run only during DB initialization, before serving allocations. Existing
// owners are grandfathered as activated. New owners get a profile atomically.
inline constexpr const char* kLegacyProfilesSql = R"SQL(INSERT OR IGNORE INTO phone_profiles(nick,stage) SELECT nick,4 FROM phone_numbers)SQL";
// Parameters are escaped by the SDK adapter; test adapter binds them directly.
inline constexpr const char* kAllocateSql = R"SQL(INSERT INTO phone_numbers(nick,number,created) SELECT '%s',p.number,%u FROM phone_number_pool p WHERE NOT EXISTS(SELECT 1 FROM phone_numbers n WHERE n.number=p.number) ORDER BY p.number LIMIT 1)SQL";
inline constexpr const char* kSaveProfileSql = R"SQL(UPDATE phone_profiles SET stage=%u,language=%u,wallpaper=%u,scale=%u,banners=%u WHERE nick='%s' AND (stage=%u OR stage+1=%u))SQL";
inline constexpr const char* kMarkReadSql = R"SQL(INSERT OR REPLACE INTO phone_reads(owner,peer,last_id) SELECT '%s','%s',MAX(COALESCE((SELECT last_id FROM phone_reads WHERE owner='%s' AND peer='%s'),0),COALESCE(MAX(id),0)) FROM phone_messages WHERE recipient='%s' AND sender='%s' AND id<=%u)SQL";
inline constexpr const char* kReceiptSql = R"SQL(CREATE TABLE IF NOT EXISTS phone_sms_receipts(owner TEXT NOT NULL,session INTEGER NOT NULL,request INTEGER NOT NULL,message_id INTEGER NOT NULL,PRIMARY KEY(owner,session,request)))SQL";
inline constexpr const char* kReadsSql = R"SQL(CREATE TABLE IF NOT EXISTS phone_reads(owner TEXT NOT NULL,peer TEXT NOT NULL,last_id INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(owner,peer)))SQL";
}
