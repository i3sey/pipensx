# Port archive alternatives

Port installs prefer `switch.7z` or `switch.zip` when the same directory also
contains a known Russian alternative: `switch_russmod`, `switch_rusmod`,
`switch_russian`, `switch_rus`, or `switch_ru` (ZIP or 7z, ignoring letter case).
These alternatives replace the main payload; they are not extra archive volumes.

New port selections skip that alternative. Existing completed downloads retain
their cached files and skip the alternative during deployment, allowing the main
archive to be extracted without downloading it again. A standalone alternative,
archives in other directories, and complementary archives remain eligible.

This preference applies only to port installs. Ordinary Switch package installs
and strict destination collision checks remain unchanged. Tests cover selection,
collision handling, original payload extraction, and standalone alternatives.
