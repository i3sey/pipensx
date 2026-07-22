#!/usr/bin/env python3
"""Validate the pipensx translation catalogs under resources/i18n/.

en-US is the source of truth. Every other locale must expose exactly the same
leaf keys with exactly the same fmt placeholder count: brls::getStr pipes every
string through fmt::format(fmt::runtime(...)), which throws when a string holds
more placeholders than the call site passes arguments, and silently drops the
extras when it holds fewer.
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
I18N = ROOT / "resources" / "i18n"
SOURCE = "en-US"

# fmt placeholders: {} or {0} or {:>8}. A literal brace is escaped by doubling.
PLACEHOLDER = re.compile(r"\{[^{}]*\}")


def leaves(node, prefix=""):
    """Flatten a locale document into {dotted key: string}."""
    out = {}
    for key, value in node.items():
        path = f"{prefix}/{key}" if prefix else key
        if isinstance(value, dict):
            out.update(leaves(value, path))
        elif isinstance(value, str):
            out[path] = value
        else:
            out[path] = None  # flagged below as a non-string value
    return out


def placeholders(text):
    return len(PLACEHOLDER.findall(text.replace("{{", "").replace("}}", "")))


def unbalanced(text):
    stripped = PLACEHOLDER.sub("", text.replace("{{", "").replace("}}", ""))
    return "{" in stripped or "}" in stripped


def load(locale, name):
    path = I18N / locale / name
    if not path.exists():
        return None
    try:
        return leaves(json.loads(path.read_text(encoding="utf-8")))
    except json.JSONDecodeError as error:
        print(f"i18n: {path.relative_to(ROOT)}: invalid JSON: {error}")
        return False


TR_CALL = re.compile(r'\btr\(\s*"([^"]+)"')
SOURCE_DIRS = ("src", "tools")


def check_call_sites(source_keys):
    """Every tr("...") key in the C++ sources must exist in the en-US catalog.

    brls::internal::getRawStr falls back to returning the key itself when it
    resolves to nothing, so a typo ships as a literal "pipensx/nav/catalog" on
    screen instead of failing anywhere. Catch it here instead.
    """
    failures = 0
    for directory in SOURCE_DIRS:
        for path in sorted((ROOT / directory).rglob("*")):
            if path.suffix not in (".cpp", ".hpp", ".h"):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(text.splitlines(), 1):
                for key in TR_CALL.findall(line):
                    if not key.startswith("pipensx/"):
                        continue
                    if key[len("pipensx/"):] not in source_keys:
                        rel = path.relative_to(ROOT)
                        print(f"i18n: {rel}:{line_number}: "
                              f"undefined key {key}")
                        failures += 1
    return failures


def main():
    failures = 0
    source_dir = I18N / SOURCE
    if not source_dir.is_dir():
        print(f"i18n: missing source locale {source_dir}")
        return 1

    for source_file in sorted(source_dir.glob("*.json")):
        name = source_file.name
        source = load(SOURCE, name)
        if source is False:
            failures += 1
            continue

        for key, value in sorted(source.items()):
            where = f"{SOURCE}/{name}:{key}"
            if value is None:
                print(f"i18n: {where}: value is not a string")
                failures += 1
            elif unbalanced(value):
                print(f"i18n: {where}: unbalanced brace (escape it as {{{{ or }}}})")
                failures += 1

        if name == "pipensx.json":
            failures += check_call_sites(set(source))

        for locale_dir in sorted(I18N.iterdir()):
            if not locale_dir.is_dir() or locale_dir.name == SOURCE:
                continue
            target = load(locale_dir.name, name)
            if target is False:
                failures += 1
                continue
            if target is None:
                continue  # a locale may legitimately omit a whole file

            for key in sorted(set(source) - set(target)):
                print(f"i18n: {locale_dir.name}/{name}: missing key {key}")
                failures += 1
            for key in sorted(set(target) - set(source)):
                print(f"i18n: {locale_dir.name}/{name}: unknown key {key}")
                failures += 1

            for key in sorted(set(source) & set(target)):
                value = target[key]
                where = f"{locale_dir.name}/{name}:{key}"
                if value is None:
                    print(f"i18n: {where}: value is not a string")
                    failures += 1
                    continue
                if unbalanced(value):
                    print(f"i18n: {where}: unbalanced brace (escape it as {{{{ or }}}})")
                    failures += 1
                expected = placeholders(source[key]) if source[key] else 0
                actual = placeholders(value)
                if expected != actual:
                    print(f"i18n: {where}: {actual} placeholders, "
                          f"{SOURCE} has {expected}")
                    failures += 1

    if failures:
        print(f"i18n: {failures} problem(s)")
        return 1
    print("i18n: translation catalogs OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
