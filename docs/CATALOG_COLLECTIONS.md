# Catalog collections

The native catalog offers collections under **Filters → Collections** and a
**Metacritic** sort option. Collections narrow the current section and combine
with the other filters. Scores appear in the game's facts table, with their
source. Entries without scores stay at the end in either sorting direction.

The app fetches the 17 curated JSON lists from
[Langegen/switch-game-collection](https://github.com/Langegen/switch-game-collection)
when refreshing the catalog. Each list has its own cache under
`catalog/collections/`. Startup reads these caches without network access;
failed or malformed downloads retain the last valid list. Collection failures
do not fail the main catalog refresh. After a network error, remaining lists
use their caches to avoid repeated connection timeouts.

Membership joins by case-insensitive, 16-digit Title ID, including the `titles`
IDs within multi-game torrents. Unmatched IDs are skipped. Repeated membership
records do not duplicate torrents in collection results; info hashes identify
unique torrents. Text and descriptions continue to come from the selected
language catalog and its existing presentation rules.

## Score provenance review (2026-11-09)

The source's
[update_new_release.py](https://github.com/Langegen/switch-game-collection/blob/main/scripts/update_new_release.py)
reads a Nintendo Life/Metacritic CSV maintained in another repository. It
normalizes names, takes the maximum for duplicate names, then tries an exact
name match followed by a substring match. This is not a verified Title ID to
Metacritic mapping. The emitted records do not include the matched review URL
or the review's game/platform identity, so consumers cannot independently
verify the match from those records alone.

For example, the current top 100 assigns 97 to “Super Mario Galaxy 1+2”. That
record describes a bundle but supplies one score without identifying which
review it represents. `new_release` also includes ports and demos. These are
reasons to keep the source disclosure visible rather than present the numbers
as independently verified reviews.

The app displays positive numeric scores up to 100 only. Null, absent, string,
and out-of-range scores remain absent. Conflicting duplicate scores within a
list are discarded. A torrent containing several distinct Title IDs can belong
to a collection via any constituent, but receives no aggregate score. A
single-ID release retains the first available score in source-list order
(top 100 first); upstream name matching can still be wrong.

**Recent torrents** (`new_release`) means entries from the RuTracker Atom feed,
not games ordered by release date. The menu deliberately uses that wording.

Native catalog tests cover partial records, malformed lists, invalid IDs,
case folding, multi-game membership, missing scores, duplicate torrents,
score ordering, language preservation, failed downloads, and cache reload.
