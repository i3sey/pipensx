"""Merge maintained offline port editor translations into the companion assets."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEYS = ['files', 'help', 'ready', 'package', 'destination', 'automatic', 'browse',
        'pending', 'members', 'preview', 'save', 'start', 'started', 'saved',
        'folderLimit', 'back', 'useFolder', 'failed']

def merge(translations):
    values = json.loads((ROOT / 'scripts/port_layout_translations.json').read_text(encoding='utf-8'))
    assert set(values) == set(translations)
    for locale, strings in values.items():
        assert len(strings) == len(KEYS)
        translations[locale].update({'port.' + key: value for key, value in zip(KEYS, strings)})
    return translations

if __name__ == '__main__':
    web = ROOT / 'resources/web'
    values = merge(json.loads((web / 'i18n.json').read_text(encoding='utf-8')))
    (web / 'i18n.json').write_text(json.dumps(values, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (web / 'i18n.js').write_text('window.PIPENSX_I18N=' + json.dumps(values, ensure_ascii=False, separators=(',', ':')) + ';\n', encoding='utf-8')
