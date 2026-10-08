# Offline catalog descriptions

Game descriptions follow the selected homebrew language. The companion game
dialog follows its language selector, which can also save the homebrew language
through Settings. The supported description languages are English, Portuguese,
Russian, French, Spanish, Italian, Simplified Chinese and Korean.

Descriptions are pretranslated on a PC and bundled in the NRO. Looking up a
description does not contact a translation service, require a translation API
key, or download a language model on the Switch. The companion requests a
translated description from the Switch on the local network.

The text packs cover the catalog snapshots used when building them. A new or
changed description retains its original text until a matching offline pack is
shipped. Matching uses the SHA-1 of the exact original description, rather than
a title or a game hash, so a catalog update cannot display an old description
for a different source text. The supplied translations are machine generated;
names and wording can need editorial correction.

Reviewed wording can be added to `resources/catalog/description_overrides.json`,
keyed by language and the exact source-description SHA-1. The generator applies
these corrections when building the packs without changing source matching.

## Rebuilding the packs

Use a Python virtual environment with `ctranslate2`, `sentencepiece`, `requests`
and `zstandard`. CUDA is optional; use `--device cpu` without a supported GPU.
The first build requires downloading the Argos language models:

```sh
python scripts/build_offline_descriptions.py resources/catalog/switch_games.json path/to/current_catalog.json --download-models --device cpu
python scripts/check_offline_descriptions.py
make switch
```

Subsequent generation uses the local models and checkpointed translations
without `--download-models`. Checkpoints and models default to project-local
`.codex` directories and are not required by the NRO. Input catalog snapshots
are merged to retain coverage across releases. The generator applies the
native catalog parser's 4096-byte source limit before calculating the key.

Packs under `resources/catalog/descriptions/` contain a sorted index and small
Zstandard blocks. The reader loads the selected language's index and one
decoded block, capped at 1 MiB, rather than all descriptions or all languages.
`coverage.json` records complete pack coverage; the validator rejects sample
packs, invalid UTF-8, mismatched language indexes and malformed block bounds.

The diagnostic endpoint `/api/catalog/coverage?lang=pt-BR` checks the actual
loaded catalog against the bundled index. This endpoint reports matching
entries; it does not request translation or modify the Real-Debrid account.

## Installation measurements

The protected `/api/benchmark` diagnostic compares cached package input batches
of 256 KiB, 1 MiB and 4 MiB. It performs a warm-up followed by three repetitions
per configuration in alternating order. Results appear in the companion Logs
tab. Selection uses median time and retains 1 MiB unless another configuration
improves that median by more than 5%.

This measurement reads complete cached NSP/NSZ files, runs the package decoder
and writes temporary NCM placeholders. It deletes only the placeholders owned
by that test and does not register games or tickets. Its time includes cached
reads, decoding, writing and cleanup; it excludes internet downloads and normal
final game registration. A complete ordinary installation must be measured
separately before calling its result a total installation time.

Only run a benchmark while other transfers are idle. Source files remain
leased during the test. The selected batch applies to subsequent installations
in that app session; it does not alter stored download-state format. Results
depend on package compression, SD performance and the console's operating
conditions and should not be treated as a universal maximum.

The shipped installation default is 4 MiB, selected from the physical Celeste
test (three repetitions per configuration). The diagnostic still uses 1 MiB as
its reference and can select a different batch for subsequent installations.
Live HTTP input is capped at 1 MiB per batch so installation begins while the
network is still producing data. Complete local caches use the measured batch.
Installing a completed catalog download upgrades the existing task in place:
its stored torrent metadata, debrid identity and data path are retained, and
the add endpoint does not submit another cloud torrent or resolve the swarm.
Catalog downloads without torrent metadata use the saved file manifest. The
selection matches full relative paths and byte sizes, preserving nested
updates/DLC and avoiding basename collisions or provider index ordering.
