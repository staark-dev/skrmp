"""Operator-only migration for an existing M4 phone DB; inspection is default.

Stop every process using the DB before --apply or --restore. No renumbering.
The compiled Gate performs this same additive migration at startup.
"""
import argparse
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path
import re
import sqlite3

ROOT = Path(__file__).resolve().parents[1]

def sql_constants():
    return dict(re.findall(r'const char\* (\w+) = R"SQL\((.*?)\)SQL";',
                (ROOT/'server/skr-gate-omp/phone_m5_sql.h').read_text(), re.S))

def open_existing(path):
    return sqlite3.connect(path.resolve().as_uri()+'?mode=rw', uri=True, isolation_level=None)

def summary(db):
    integrity = db.execute('PRAGMA integrity_check').fetchone()[0]
    if integrity != 'ok':
        raise RuntimeError('Database integrity check failed')
    print('Integrity: ok')
    for table in ('phone_numbers', 'phone_contacts', 'phone_messages'):
        print(f'{table}: {db.execute("SELECT COUNT(*) FROM "+table).fetchone()[0]} rows')
    print('Existing phone identities are retained; no contacts or SMS are rewritten.')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('database', type=Path)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--restore', type=Path, help='Restore a full SQLite backup, requires --apply')
    args=parser.parse_args()
    if args.restore and not args.apply:
        parser.error('--restore requires --apply')
    if args.restore and args.restore.resolve()==args.database.resolve():
        parser.error('backup and destination must differ')
    with closing(open_existing(args.database)) as db:
        summary(db)
        if not args.apply:
            # Prove migration against a private in-memory copy, with no source changes.
            with closing(sqlite3.connect(':memory:', isolation_level=None)) as preview:
                db.backup(preview)
                migrate(preview)
                print('Dry run: migration succeeded; source database unchanged.')
            return
        backup=args.database.with_name(args.database.name+'.pre-m5-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')+'.sqlite')
        with backup.open('xb'):
            pass
        with closing(sqlite3.connect(backup)) as target:
            db.backup(target)
        print(f'Full backup: {backup}')
        if args.restore:
            with closing(open_existing(args.restore)) as source:
                if source.execute('PRAGMA integrity_check').fetchone()[0]!='ok':
                    raise RuntimeError('Backup integrity check failed')
                source.backup(db)
            print('Full database restored. Post-backup writes are absent by design.')
        else:
            migrate(db)
            print('M5 migration committed; old numbers remain allocated.')

def migrate(db):
    sql=sql_constants()
    db.execute('BEGIN IMMEDIATE')
    try:
        for key in ('kPoolSql','kFillPoolSql','kProfileSql','kLegacyProfilesSql','kReceiptSql','kReadsSql'):
            db.execute(sql[key])
        db.execute('COMMIT')
    except BaseException:
        db.execute('ROLLBACK')
        raise

if __name__=='__main__':
    main()
