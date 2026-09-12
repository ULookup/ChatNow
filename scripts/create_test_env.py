#!/usr/bin/env python3
"""Create synthetic credentials for a fresh, isolated Compose test checkout."""
import argparse
import json
import os
from pathlib import Path
import secrets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--github-env', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    destination = root / '.env'
    # Never rotate a live database's credentials or overwrite a developer's env.
    if destination.exists() or (root / 'middle/data').exists():
        parser.error('requires a fresh checkout without .env or middle/data')
    names = [line.split('=', 1)[0] for line in (root / '.env.example').read_text().splitlines()
             if line and not line.startswith('#')]
    values = {name: secrets.token_hex(24) for name in names}
    values['CHATNOW_JWT_CONFIG'] = json.dumps({'auth': {'jwt': {
        'current_kid': 'ci', 'keys': {'ci': secrets.token_hex(32)},
        'access_ttl_sec': 7200, 'refresh_ttl_sec': 2592000}}}, separators=(',', ':'))
    values['MYSQL_DSN'] = f"root:{values['CHATNOW_MYSQL_ROOT_PASSWORD']}@tcp(127.0.0.1:3306)/chatnow?parseTime=true"
    values['MINIO_ACCESS_KEY'] = values['CHATNOW_MEDIA_S3_ACCESS_KEY']
    values['MINIO_SECRET_KEY'] = values['CHATNOW_MEDIA_S3_SECRET_KEY']
    with os.fdopen(os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
        for name, value in values.items():
            stream.write(f"{name}='{value}'\n")
    if args.github_env:
        # Register masks before subsequent commands can emit any test credentials.
        for value in values.values():
            print(f'::add-mask::{value}')
        with open(os.environ['GITHUB_ENV'], 'a') as stream:
            for name, value in values.items():
                stream.write(f'{name}={value}\n')
    print('Synthetic test environment created.')


if __name__ == '__main__':
    main()
