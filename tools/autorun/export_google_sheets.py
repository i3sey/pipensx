#!/usr/bin/env python3
"""
Create and style a public Google Spreadsheet with AutoRun Switch game compatibility data.
Uses Google OAuth with the local client credentials and automatically shares the sheet with public view access.
"""

from __future__ import annotations
import sys
import os
import json
import csv
import webbrowser
from pathlib import Path
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build
from google.oauth2.credentials import Credentials

SCOPES = [
    'https://www.googleapis.com/auth/spreadsheets',
    'https://www.googleapis.com/auth/drive',
    'https://www.googleapis.com/auth/drive.file'
]

REPO_ROOT = Path(__file__).resolve().parents[2]
CLIENT_SECRETS_FILE = Path.home() / ".config" / "goose" / "google_calendar_credentials.json"
TOKEN_FILE = REPO_ROOT / "resources" / "autorun" / "google_sheets_token.json"
CSV_PATH = REPO_ROOT / "resources" / "autorun" / "compatibility_db.csv"
JSON_PATH = REPO_ROOT / "resources" / "autorun" / "compatibility_db.json"


def get_authenticated_services():
    creds = None
    if TOKEN_FILE.exists():
        try:
            creds = Credentials.from_authorized_user_file(str(TOKEN_FILE), SCOPES)
        except Exception:
            creds = None

    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            from google.auth.transport.requests import Request
            try:
                creds.refresh(Request())
            except Exception:
                creds = None

        if not creds:
            if not CLIENT_SECRETS_FILE.exists():
                print(f"[ERROR] Client secrets not found at {CLIENT_SECRETS_FILE}", file=sys.stderr)
                sys.exit(1)

            flow = InstalledAppFlow.from_client_secrets_file(
                str(CLIENT_SECRETS_FILE),
                scopes=SCOPES
            )
            auth_url, _ = flow.authorization_url(prompt='consent', access_type='offline')
            print("\n" + "=" * 70)
            print("🚀 Запускаем авторизацию Google...")
            print("Ссылка для авторизации (если не откроется сама):")
            print(auth_url)
            print("=" * 70)
            try:
                import subprocess
                subprocess.Popen(["/usr/lib/firefox/firefox", auth_url])
            except Exception as e:
                print(f"[Warning] Не удалось запустить Firefox автоматически: {e}")

            creds = flow.run_local_server(port=8080, open_browser=False)

            TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
            with open(TOKEN_FILE, "w", encoding="utf-8") as f:
                f.write(creds.to_json())
            print("✓ Авторизация успешно сохранена!")

    sheets_service = build('sheets', 'v4', credentials=creds)
    drive_service = build('drive', 'v3', credentials=creds)
    return sheets_service, drive_service


def create_and_style_spreadsheet():
    sheets_service, drive_service = get_authenticated_services()

    # Load data
    with open(JSON_PATH, "r", encoding="utf-8") as f:
        records = json.load(f)

    headers_ru = [
        "Статус", "Название игры", "Совместимость", "FPS", "Разрешение / Графика",
        "Рендерер", "Транслятор CPU", "Разгон UltraNX", "LSFG",
        "Рабочий репак / Версия", "Заметки, фиксы и баги", "Твики реестра / user.reg",
        "Путь в AutoRun на Switch", "Источник", "Автор теста", "Дата теста"
    ]
    fieldnames = [
        "status_badge", "game_title", "status", "fps", "graphics_resolution",
        "renderer_and_tools", "cpu_translator", "overclock", "lsfg",
        "repack_version", "notes_and_fixes", "registry_tweaks",
        "autorun_dir", "source", "author", "date"
    ]

    print("\n[1/4] Создаем новую Google Таблицу...")
    spreadsheet_body = {
        'properties': {
            'title': 'Nintendo Switch - Совместимость ПК-игр (AutoRun / Wine / Box64 / FEX)'
        },
        'sheets': [{
            'properties': {
                'title': 'Совместимость игр',
                'gridProperties': {
                    'rowCount': len(records) + 50,
                    'columnCount': len(headers_ru) + 2,
                    'frozenRowCount': 1
                }
            }
        }]
    }
    spreadsheet = sheets_service.spreadsheets().create(
        body=spreadsheet_body,
        fields='spreadsheetId,sheets(properties(sheetId))'
    ).execute()

    spreadsheet_id = spreadsheet['spreadsheetId']
    sheet_id = spreadsheet['sheets'][0]['properties']['sheetId']
    print(f"✓ Таблица создана! ID: {spreadsheet_id}")

    # Make spreadsheet public (anyone with link can view)
    print("\n[2/4] Открываем публичный доступ по ссылке (Reader)...")
    permission = {
        'type': 'anyone',
        'role': 'reader'
    }
    drive_service.permissions().create(
        fileId=spreadsheet_id,
        body=permission,
        fields='id'
    ).execute()
    print("✓ Доступ по ссылке открыт!")

    # Populate values
    print("\n[3/4] Загружаем строки и данные...")
    rows = [headers_ru]
    for r in records:
        rows.append([r.get(k, "") for k in fieldnames])

    value_body = {
        'values': rows
    }
    sheets_service.spreadsheets().values().update(
        spreadsheetId=spreadsheet_id,
        range='Совместимость игр!A1',
        valueInputOption='RAW',
        body=value_body
    ).execute()
    print(f"✓ Загружено {len(rows)} строк.")

    # Apply Styling
    print("\n[4/4] Применяем форматирование (цвета, шрифты, границы, автоширину)...")
    requests = []

    # 1. Header style: Dark blue background, white bold text, centered
    requests.append({
        'repeatCell': {
            'range': {
                'sheetId': sheet_id,
                'startRowIndex': 0,
                'endRowIndex': 1,
                'startColumnIndex': 0,
                'endColumnIndex': len(headers_ru)
            },
            'cell': {
                'userEnteredFormat': {
                    'backgroundColor': {'red': 0.12, 'green': 0.28, 'blue': 0.49},
                    'textFormat': {
                        'foregroundColor': {'red': 1.0, 'green': 1.0, 'blue': 1.0},
                        'bold': True,
                        'fontSize': 11
                    },
                    'horizontalAlignment': 'CENTER',
                    'verticalAlignment': 'MIDDLE',
                    'wrapStrategy': 'WRAP'
                }
            },
            'fields': 'userEnteredFormat(backgroundColor,textFormat,horizontalAlignment,verticalAlignment,wrapStrategy)'
        }
    })

    # 2. Row styling based on status badge
    for idx, r in enumerate(records, start=1):
        badge = r.get("status_badge", "")
        bg = None
        if badge == "🟢":
            bg = {'red': 0.88, 'green': 0.94, 'blue': 0.85} # soft green
        elif badge == "🟡":
            bg = {'red': 1.0, 'green': 0.95, 'blue': 0.8}   # soft yellow
        elif badge == "🔴":
            bg = {'red': 0.99, 'green': 0.89, 'blue': 0.84} # soft red

        if bg:
            # Highlight first 3 columns with status tint
            requests.append({
                'repeatCell': {
                    'range': {
                        'sheetId': sheet_id,
                        'startRowIndex': idx,
                        'endRowIndex': idx + 1,
                        'startColumnIndex': 0,
                        'endColumnIndex': 3
                    },
                    'cell': {
                        'userEnteredFormat': {
                            'backgroundColor': bg
                        }
                    },
                    'fields': 'userEnteredFormat.backgroundColor'
                }
            })

    # 3. Center alignment for badge, status, translator, LSFG
    for col_idx in [0, 2, 6, 8]:
        requests.append({
            'repeatCell': {
                'range': {
                    'sheetId': sheet_id,
                    'startRowIndex': 1,
                    'endRowIndex': len(rows),
                    'startColumnIndex': col_idx,
                    'endColumnIndex': col_idx + 1
                },
                'cell': {
                    'userEnteredFormat': {
                        'horizontalAlignment': 'CENTER',
                        'verticalAlignment': 'MIDDLE'
                    }
                },
                'fields': 'userEnteredFormat(horizontalAlignment,verticalAlignment)'
            }
        })

    # 4. Wrap text for notes, graphics, fps
    for col_idx in [1, 3, 4, 7, 10, 11]:
        requests.append({
            'repeatCell': {
                'range': {
                    'sheetId': sheet_id,
                    'startRowIndex': 1,
                    'endRowIndex': len(rows),
                    'startColumnIndex': col_idx,
                    'endColumnIndex': col_idx + 1
                },
                'cell': {
                    'userEnteredFormat': {
                        'wrapStrategy': 'WRAP',
                        'verticalAlignment': 'MIDDLE'
                    }
                },
                'fields': 'userEnteredFormat(wrapStrategy,verticalAlignment)'
            }
        })

    # 5. Column widths
    col_widths = {
        0: 60,   # Статус
        1: 240,  # Название
        2: 170,  # Совместимость
        3: 180,  # FPS
        4: 200,  # Разрешение
        5: 140,  # Рендерер
        6: 140,  # Транслятор
        7: 180,  # Разгон
        8: 80,   # LSFG
        9: 160,  # Репак
        10: 360, # Заметки
        11: 180, # Реестр
        12: 260, # Путь
        13: 190, # Источник
        14: 130, # Автор
        15: 140  # Дата
    }
    for col_idx, pixel_size in col_widths.items():
        requests.append({
            'updateDimensionProperties': {
                'range': {
                    'sheetId': sheet_id,
                    'dimension': 'COLUMNS',
                    'startIndex': col_idx,
                    'endIndex': col_idx + 1
                },
                'properties': {
                    'pixelSize': pixel_size
                },
                'fields': 'pixelSize'
            }
        })

    # Execute batch formatting
    sheets_service.spreadsheets().batchUpdate(
        spreadsheetId=spreadsheet_id,
        body={'requests': requests}
    ).execute()

    sheet_url = f"https://docs.google.com/spreadsheets/d/{spreadsheet_id}/edit?usp=sharing"
    print("\n" + "=" * 75)
    print("🎉 ГОТОВО! Ваша открытая Google Таблица успешно создана и оформлена:")
    print(sheet_url)
    print("=" * 75 + "\n")
    return sheet_url


if __name__ == "__main__":
    create_and_style_spreadsheet()
