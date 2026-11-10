# Catalog search

The native catalog accepts literal titles, title IDs, genres, game initials and
common abbreviations: `botw`, `totk`, `smo`, `mk8`, `mk8d`, `acnh`, `ssbu`, `mhr`,
`fe3h`, and `xc3`. Known abbreviations are matched to their game names rather
than treated as misspelled words. Initials can match a contiguous sequence of
words inside a longer title, such as Breath of the Wild inside a Zelda title.

Title search also tolerates insertions, deletions, substitutions and adjacent
transpositions. Words of four to six characters allow one edit; words of seven
or more allow two. Short words, title IDs and genres keep literal matching.
ASCII and Cyrillic case folding happens before matching, and edit distances
operate on characters rather than UTF-8 bytes. Release tags in square brackets
remain searchable literally but do not contribute to fuzzy matches or initials.

Relevance takes priority while a query is active: exact title, title prefix,
title substring or matching words, acronym, literal genre/ID, then fuzzy title.
The selected sort and its direction order results within each relevance group.
Clearing the query restores normal sorting.

For multiple visible releases sharing a title ID, search cards show only facts
that differ: format, version, update, included DLC, text languages and voice
languages. When IDs are absent, a name without square-bracket release tags is
used. Sequel numbers are preserved; multi-game bundles are excluded. Missing
facts stay absent. DLC requires a DLC package type, an explicit `+DLC` marker or
a positive DLC count. An unqualified mention of DLC does not assert inclusion.

The comparison chips reuse the existing neutral StatusChip and theme tokens.
They occupy the bottom of the cover with wrapping and bounded label widths;
the title, size and live task/installed badge keep their existing space. The
favorite marker uses the top-right corner so it remains separate from both
comparison chips and the top-left selection mark. The cover and focused card
remain the screen's focal point; the turquoise focus accent keeps its existing
role. Design dials: ENERGY 1 / RHYTHM 1 / MOTION 1, matching the native catalog.

## Validation

`tests/test_catalog.cpp` covers English/Cyrillic typos, transposition, short and
long queries, metadata titles, aliases, unrelated games, ranking across all
sort modes/directions, differing release facts, missing facts, title fallback,
sequels, bundles and coexistence with live badges. Existing cancellation tests
exercise the browse worker. The bounded distance calculation uses three fixed
33-element rows and at most eight query words, with a 128-byte fuzzy-query cap.
Sorting and comparison work stays in the existing cancellable background job.

An optional full-catalog host benchmark is available without adding catalog
data to Git:

```sh
make -f Makefile.pc build-pc/tests/test_catalog
PIPENSX_SEARCH_BENCHMARK_CATALOG=/path/to/catalog.json \
  ./build-pc/tests/test_catalog
```

This measures the complete browse builder five times per query on the host CPU,
including filtering, sorting and release comparison. It is not a Switch timing
measurement. Physical-console visual checks and timings require a reachable
Switch; the configured console did not respond during implementation.
