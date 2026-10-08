#!/usr/bin/env python3
"""Build offline companion UI assets with the already-installed local models.
No model downloads or online translation. Descriptions are separate assets.
"""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
module_spec = importlib.util.spec_from_file_location('offline', ROOT / 'scripts/build_offline_descriptions.py')
offline = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(offline)

extra = {
    'tabs.library': 'Installed', 'catalog.games': 'Games', 'catalog.ports': 'Ports',
    'catalog.count': 'Showing {0} of {1}',
    'library.title': 'Installed games',
    'library.sub': 'Check updates and manage games installed on your Switch.',
    'library.search': 'Search installed games…', 'library.all': 'All installed',
    'library.updates': 'Updates available', 'library.refresh': 'Check updates',
    'library.updateAll': 'Update all', 'library.update': 'Install update',
    'library.uninstall': 'Uninstall', 'library.current': 'Installed version',
    'library.count': '{0} of {1} installed games', 'library.dlc': '{0} DLC installed',
    'library.mods': 'Mods detected', 'library.unknown': 'Unknown',
    'library.update_available': 'Update available', 'library.up_to_date': 'Up to date',
    'library.source_unknown': 'No update information', 'library.check_error': 'Unable to compare versions',
    'library.not_checked': 'Not checked', 'library.checking': 'Checking updates',
    'library.empty': 'No games match this filter.', 'library.loading': 'Loading installed games…',
    'library.failed': 'Could not load installed games',
    'library.updateAsk': 'Update {0} from {1} to {2}? The installed base will be preserved.',
    'library.uninstallAsk': 'Uninstall {0} ({1}) and its installed content from the Switch? This action cannot be undone from the panel.',
    'library.modsWarning': 'This game has mods. An update may make them incompatible. Review your mods before continuing.',
    'library.updateAllAsk': 'Queue updates for {0} installed games? Games with mods must be reviewed individually.',
    'library.updateQueued': 'Update queued. Follow progress in Downloads.',
    'library.queuedCount': '{0} updates queued',
    'library.job.queued': 'Queued', 'library.job.working': 'Working',
    'library.job.done': 'Completed', 'library.job.error': 'Error',
    'queue.summary': 'queued {0} · paused {1} · errors {2}', 'queue.position': 'in queue',
    'storage.free': 'free of {0}', 'peer.none': 'no peers', 'peer.stalled': 'stalled', 'peer.slow': 'slow',
    'log.copied': 'Log copied', 'log.copyFailed': 'Could not copy log',
    'common.on': 'on', 'common.off': 'off', 'catalog.failed': 'Failed to load the catalog',
    'remove.data': 'Also delete downloaded data',
}
for state in ['Queued', 'Checking', 'Fetching', 'Downloading', 'Installing', 'Committing', 'Verifying', 'Removing', 'Paused', 'Completed', 'Installed', 'Error']:
    extra['status.' + state] = state

def flatten(node, prefix=''):
    values = {}
    for key, value in node.items():
        path = prefix + '.' + key
        if isinstance(value, dict): values.update(flatten(value, path))
        elif isinstance(value, str): values[path] = value
    return values

def placeholders(text):
    return re.findall(r'\{\d+\}', text)

def main():
    script = "const fs=require('fs'),vm=require('vm');const s=fs.readFileSync('resources/web/app.js','utf8');const a=s.indexOf('const I18N = ')+12,b=s.indexOf('Object.assign(I18N');process.stdout.write(JSON.stringify(vm.runInNewContext('('+s.slice(a,b).trim().replace(/;$/,'')+')')));"
    source = json.loads(subprocess.check_output(['node', '-e', script], cwd=ROOT, text=True, encoding='utf-8'))
    source['en'].update(extra)
    english = source['en']
    models = offline.prepare_models(ROOT / '.codex/translation-models', False)
    native_en = flatten(json.loads((ROOT / 'resources/i18n/en-US/pipensx.json').read_text(encoding='utf-8')))
    result = {'en': english}
    locales = {'ru':'ru', 'pt-BR':'pt', 'fr':'fr', 'es':'es', 'it':'it', 'zh-CN':'zh', 'ko':'ko'}
    for locale, code in locales.items():
        native = flatten(json.loads((ROOT / f'resources/i18n/{locale}/pipensx.json').read_text(encoding='utf-8')))
        exact = {value: native[path] for path, value in native_en.items() if path in native}
        translations = dict(source.get(locale, {}))
        pending = []
        for key, value in english.items():
            if key in translations: continue
            if value in exact and placeholders(exact[value]) == placeholders(value): translations[key] = exact[value]
            else: pending.append(key)
        if pending:
            engine = offline.Translator(models[('en', code)], 'cpu', 2)
            texts = [english[key] for key in pending]
            translated = engine.translate(texts)
            for key, value in zip(pending, translated):
                if placeholders(value) != placeholders(english[key]):
                    pieces = re.split(r'(\{\d+\})', english[key])
                    phrases = [piece for piece in pieces if piece.strip() and not re.fullmatch(r'\{\d+\}', piece)]
                    rendered = iter(engine.translate(phrases))
                    value = ''.join(piece if not piece.strip() or re.fullmatch(r'\{\d+\}', piece) else next(rendered) for piece in pieces)
                translations[key] = value
            del engine
        result[locale] = translations
        print(f'{locale}: {len(translations)} strings', flush=True)
    # Human-reviewed primary controls (native terminology) and critical prompts.
    reviewed = {
        'pt-BR': {'tabs.library':'Instalados','catalog.games':'Jogos','catalog.ports':'Ports',
            'catalog.count':'Mostrando {0} de {1}', 'library.title':'Jogos instalados',
            'library.refresh':'Verificar atualizações', 'library.update':'Instalar atualização',
            'library.updateAll':'Atualizar todos', 'library.uninstall':'Desinstalar',
            'library.current':'Versão instalada', 'library.count':'{0} de {1} jogos instalados',
            'library.dlc':'{0} DLC instalados',
            'library.updateAsk':'Atualizar {0} de {1} para {2}? O jogo base instalado será preservado.',
            'library.uninstallAsk':'Desinstalar {0} ({1}) e seu conteúdo instalado do Switch? Não é possível desfazer esta ação pelo painel.',
            'library.updateAllAsk':'Colocar na fila atualizações de {0} jogos instalados? Jogos com mods precisam ser revisados individualmente.',
            'library.modsWarning':'Este jogo tem mods. Uma atualização pode torná-los incompatíveis. Revise os mods antes de continuar.',
            'queue.summary':'na fila {0} · pausados {1} · erros {2}', 'storage.free':'livres de {0}'},
        'fr': {'tabs.library':'Installés','catalog.games':'Jeux','catalog.ports':'Ports','library.uninstall':'Désinstaller','library.update':'Installer la mise à jour','library.refresh':'Vérifier les mises à jour'},
        'es': {'tabs.library':'Instalados','catalog.games':'Juegos','catalog.ports':'Ports','library.uninstall':'Desinstalar','library.update':'Instalar actualización','library.refresh':'Buscar actualizaciones'},
        'it': {'tabs.library':'Installati','catalog.games':'Giochi','catalog.ports':'Port','library.uninstall':'Disinstalla','library.update':'Installa aggiornamento','library.refresh':'Verifica aggiornamenti'},
        'ru': {'tabs.library':'Установленные','catalog.games':'Игры','catalog.ports':'Порты','library.uninstall':'Удалить','library.update':'Установить обновление','library.refresh':'Проверить обновления'},
        'zh-CN': {'tabs.library':'已安装','catalog.games':'游戏','catalog.ports':'移植游戏','library.uninstall':'卸载','library.update':'安装更新','library.refresh':'检查更新'},
        'ko': {'tabs.library':'설치됨','catalog.games':'게임','catalog.ports':'포트','library.uninstall':'제거','library.update':'업데이트 설치','library.refresh':'업데이트 확인'},
    }
    for locale, overrides in reviewed.items(): result[locale].update(overrides)
    from apply_port_layout_translations import merge
    merge(result)
    for locale, values in result.items():
        assert set(values) == set(english), locale
        for key, text in values.items():
            assert placeholders(text) == placeholders(english[key]), (locale, key)
    payload = json.dumps(result, ensure_ascii=False, separators=(',', ':'))
    (ROOT / 'resources/web/i18n.json').write_text(payload + '\n', encoding='utf-8')
    (ROOT / 'resources/web/i18n.js').write_text('"use strict";window.PIPENSX_I18N=' + payload + ';\n', encoding='utf-8')
    print('Web translations verified: eight languages, complete keys and placeholders.', flush=True)

if __name__ == '__main__': main()
