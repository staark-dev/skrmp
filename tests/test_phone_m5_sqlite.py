"""Real SQLite tests against production SQL constants; never opens a runtime DB."""
from contextlib import closing
import concurrent.futures
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SQL = dict(re.findall(r'const char\* (\w+) = R"SQL\((.*?)\)SQL";',
                     (ROOT / 'server/skr-gate-omp/phone_m5_sql.h').read_text(), re.S))

def connect(path):
    return sqlite3.connect(path, timeout=5, isolation_level=None)

def migrate(db):
    db.execute('BEGIN IMMEDIATE')
    try:
        for name in ('kPoolSql', 'kFillPoolSql', 'kProfileSql', 'kLegacyProfilesSql', 'kReceiptSql', 'kReadsSql'):
            db.execute(SQL[name])
        db.execute('COMMIT')
    except BaseException:
        db.execute('ROLLBACK')
        raise

def allocate(path, nick, crash=False):
    with closing(connect(path)) as db:
        db.execute('BEGIN IMMEDIATE')
        try:
            row = db.execute('SELECT number FROM phone_numbers WHERE nick=?', (nick,)).fetchone()
            if row:
                db.execute('COMMIT')
                return row[0]
            # Exact production INSERT/SELECT, with bound identity in the test adapter.
            query = SQL['kAllocateSql'].replace("'%s'", '?').replace('%u', '?')
            db.execute(query, (nick, 1))
            row = db.execute('SELECT number FROM phone_numbers WHERE nick=?', (nick,)).fetchone()
            if not row:
                db.execute('ROLLBACK')
                return None
            db.execute('INSERT INTO phone_profiles(nick) VALUES(?)', (nick,))
            if crash:
                raise RuntimeError('simulated interruption before commit')
            db.execute('COMMIT')
            return row[0]
        except BaseException:
            db.execute('ROLLBACK')
            raise

class PhoneM5(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "out" / "phone-m5-sqlite"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.path = pathlib.Path(self.temp.name) / 'phone.db'
        self.db = connect(self.path)
        self.db.executescript('''CREATE TABLE phone_numbers(nick TEXT PRIMARY KEY,number TEXT NOT NULL UNIQUE,created INTEGER NOT NULL);
          CREATE TABLE phone_messages(id INTEGER PRIMARY KEY,sender TEXT,recipient TEXT,text TEXT,ts INTEGER);
          CREATE TABLE phone_contacts(owner TEXT,number TEXT,name TEXT);
          INSERT INTO phone_numbers VALUES('Legacy','1000000',1);
          INSERT INTO phone_messages VALUES(1,'1000000','5551234','original',1);
          INSERT INTO phone_contacts VALUES('1000000','5551234','Friend');''')
        migrate(self.db)

    def tearDown(self):
        self.db.close()
        self.temp.cleanup()

    def test_range_exhaustion_and_uniqueness(self):
        self.assertEqual(self.db.execute('SELECT COUNT(*),MIN(number),MAX(number) FROM phone_number_pool').fetchone(), (10000,'5550000','5559999'))
        self.assertEqual(allocate(self.path,'First'),'5550000')
        self.db.execute("INSERT INTO phone_numbers SELECT 'filler'||number,number,1 FROM phone_number_pool WHERE number NOT IN ('5550000','5559999')")
        self.assertEqual(allocate(self.path,'Last'),'5559999')
        self.assertIsNone(allocate(self.path,'Exhausted'))
        self.assertEqual(allocate(self.path,'First'),'5550000')
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO phone_numbers VALUES('Forged','5550000',1)")

    def test_concurrent_same_and_different_identities(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            same = list(pool.map(lambda _: allocate(self.path,'Same'),range(16)))
            different = list(pool.map(lambda n: allocate(self.path,f'Player{n}'),range(32)))
        self.assertEqual(len(set(same)),1)
        self.assertEqual(len(set(different)),32)
        self.assertNotIn(same[0],different)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM phone_profiles WHERE stage=0').fetchone()[0],33)

    def test_interruption_rolls_back_number_and_profile(self):
        with self.assertRaises(RuntimeError):
            allocate(self.path,'Crash',crash=True)
        self.assertIsNone(self.db.execute("SELECT number FROM phone_numbers WHERE nick='Crash'").fetchone())
        self.assertEqual(allocate(self.path,'AfterCrash'),'5550000')

    def test_migration_retains_legacy_sms_contacts_and_setup(self):
        self.assertEqual(allocate(self.path,'Legacy'),'1000000')
        self.assertEqual(self.db.execute("SELECT stage FROM phone_profiles WHERE nick='Legacy'").fetchone()[0],4)
        allocate(self.path,'New')
        self.db.execute("UPDATE phone_profiles SET stage=2,wallpaper=1 WHERE nick='New'")
        migrate(self.db)
        self.assertEqual(self.db.execute("SELECT stage,wallpaper FROM phone_profiles WHERE nick='New'").fetchone(),(2,1))
        self.assertEqual(self.db.execute('SELECT text FROM phone_messages').fetchone()[0],'original')
        self.assertEqual(self.db.execute('SELECT name FROM phone_contacts').fetchone()[0],'Friend')
        self.assertEqual(allocate(self.path,'New'),'5550000')

    def test_migration_failure_is_atomic(self):
        self.db.execute('DROP TABLE phone_profiles')
        self.db.execute('CREATE TABLE phone_profiles(invalid_column TEXT)')
        with self.assertRaises(sqlite3.OperationalError):
            migrate(self.db)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM phone_numbers').fetchone()[0],1)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM phone_messages').fetchone()[0],1)

    def test_backup_rollback_restores_complete_database(self):
        backup = connect(pathlib.Path(self.temp.name)/'backup.db')
        self.db.backup(backup)
        allocate(self.path,'AfterBackup')
        backup.backup(self.db)
        self.assertIsNone(self.db.execute("SELECT number FROM phone_numbers WHERE nick='AfterBackup'").fetchone())
        self.assertEqual(self.db.execute('SELECT text FROM phone_messages').fetchone()[0],'original')
        backup.close()

    def test_setup_progress_cannot_skip_or_reset_and_is_identity_scoped(self):
        allocate(self.path, 'Owner')
        allocate(self.path, 'Other')
        query = SQL['kSaveProfileSql'].replace("'%s'", '?').replace('%u', '?')
        def save(stage, who='Owner'):
            self.db.execute(query, (stage, 0, 1, 125, 0, who, stage, stage))
        save(4)  # forged skip has no effect
        self.assertEqual(self.db.execute("SELECT stage FROM phone_profiles WHERE nick='Owner'").fetchone()[0], 0)
        for stage in range(1, 5):
            save(stage)
        save(0)  # activation cannot be reset
        self.assertEqual(self.db.execute("SELECT stage,wallpaper,scale,banners FROM phone_profiles WHERE nick='Owner'").fetchone(), (4, 1, 125, 0))
        self.assertEqual(self.db.execute("SELECT stage FROM phone_profiles WHERE nick='Other'").fetchone()[0], 0)

    def test_persistent_reads_are_monotonic_and_do_not_mark_future_messages(self):
        self.db.execute("INSERT INTO phone_messages VALUES(2,'5550001','5550000','incoming',1)")
        query = SQL['kMarkReadSql'].replace("'%s'", '?').replace('%u', '?')
        self.db.execute(query, ('5550000', '5550001', '5550000', '5550001', '5550000', '5550001', 2))
        self.db.execute("INSERT INTO phone_messages VALUES(3,'5550001','5550000','later',1)")
        self.db.execute(query, ('5550000', '5550001', '5550000', '5550001', '5550000', '5550001', 1))
        self.assertEqual(self.db.execute("SELECT last_id FROM phone_reads").fetchone()[0], 2)
        with closing(connect(self.path)) as reconnected:
            self.assertEqual(reconnected.execute("SELECT COUNT(*) FROM phone_messages WHERE recipient='5550000' AND id>(SELECT last_id FROM phone_reads)").fetchone()[0], 1)

    def test_receipt_uniqueness_survives_reconnect(self):
        self.db.execute("INSERT INTO phone_sms_receipts VALUES('Legacy',42,7,1)")
        with closing(connect(self.path)) as reconnected:
            self.assertEqual(reconnected.execute("SELECT message_id FROM phone_sms_receipts WHERE owner='Legacy' AND session=42 AND request=7").fetchone()[0], 1)
            with self.assertRaises(sqlite3.IntegrityError):
                reconnected.execute("INSERT INTO phone_sms_receipts VALUES('Legacy',42,7,2)")

    def test_operator_tool_dry_run_backup_and_restore(self):
        tool = ROOT / 'tools/phone_m5_migrate.py'
        def run(*extra):
            return subprocess.run([sys.executable, str(tool), str(self.path), *extra],
                                  check=True, capture_output=True, text=True).stdout
        self.assertIn('source database unchanged', run())
        self.assertIn('migration committed', run('--apply'))
        backup = next(pathlib.Path(self.temp.name).glob('*.pre-m5-*.sqlite'))
        allocate(self.path, 'AfterOperatorBackup')
        self.assertIn('database restored', run('--apply', '--restore', str(backup)))
        self.assertIsNone(self.db.execute("SELECT number FROM phone_numbers WHERE nick='AfterOperatorBackup'").fetchone())

if __name__ == '__main__':
    unittest.main(verbosity=2)
