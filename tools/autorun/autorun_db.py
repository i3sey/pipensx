"""
AutoRun Game Compatibility Database Manager for pipensx.
Handles querying, filtering, and formatting compatibility data.
"""

from __future__ import annotations
import json
from pathlib import Path
from typing import List, Dict, Any, Optional

DEFAULT_DB_PATH = Path(__file__).resolve().parents[2] / "resources" / "autorun" / "compatibility_db.json"


class AutorunDB:
    def __init__(self, db_path: Optional[Path] = None):
        self.db_path = db_path or DEFAULT_DB_PATH
        self.records: List[Dict[str, Any]] = []
        self.load()

    def load(self) -> None:
        if self.db_path.exists():
            with open(self.db_path, "r", encoding="utf-8") as f:
                self.records = json.load(f)
        else:
            self.records = []

    def save(self) -> None:
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.db_path, "w", encoding="utf-8") as f:
            json.dump(self.records, f, ensure_ascii=False, indent=2)

    def find(self, query: str) -> List[Dict[str, Any]]:
        q = query.lower().strip()
        results = []
        for r in self.records:
            if q in r.get("game_title", "").lower() or q in r.get("notes_and_fixes", "").lower():
                results.append(r)
        return results

    def get_by_title(self, title: str) -> Optional[Dict[str, Any]]:
        t = title.lower().strip()
        for r in self.records:
            if r.get("game_title", "").lower() == t:
                return r
        # Partial match fallback
        for r in self.records:
            if t in r.get("game_title", "").lower():
                return r
        return None

    def filter_status(self, badge: str) -> List[Dict[str, Any]]:
        return [r for r in self.records if r.get("status_badge") == badge]

    def add_or_update(self, entry: Dict[str, Any]) -> None:
        title = entry.get("game_title", "").strip()
        if not title:
            return
        for i, existing in enumerate(self.records):
            if existing.get("game_title", "").lower() == title.lower():
                self.records[i] = entry
                return
        self.records.append(entry)
