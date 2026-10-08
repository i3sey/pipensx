#!/usr/bin/env python3
"""Validate the companion's shipped offline translations and markup keys."""
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent

def main():
    web = ROOT / 'resources/web'
    values = json.loads((web / 'i18n.json').read_text(encoding='utf-8'))
    assert set(values) == {'en', 'ru', 'pt-BR', 'fr', 'es', 'it', 'zh-CN', 'ko'}
    source = values['en']
    for locale, translations in values.items():
        assert set(translations) == set(source), locale
        for key, text in translations.items():
            assert isinstance(text, str) and text.strip(), (locale, key)
            assert sorted(re.findall(r'\{\d+\}', text)) == sorted(re.findall(r'\{\d+\}', source[key])), (locale, key)
    for key in re.findall(r'data-i18n(?:-ph)?="([^"]+)"', (web / 'index.html').read_text(encoding='utf-8')):
        assert key in source, key
    js = (web / 'i18n.js').read_text(encoding='utf-8')
    assert json.loads(js.split('window.PIPENSX_I18N=', 1)[1].strip().removesuffix(';')) == values
    print(f'web i18n: {len(source)} strings in all 8 languages; markup and script match')

if __name__ == '__main__':
    main()
