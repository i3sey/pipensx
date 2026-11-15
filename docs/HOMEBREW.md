# Homebrew catalog

The native **Homebrew** tab lists the packages from `homebrew.json` separately
from torrent games and ports. Search covers names, authors and descriptions.
The grid shows five apps per row with square icons, two-line titles and
version information. Installed apps show their recorded version below the title.
The detail page shows the version, description, changelog and
destination files before installation. Descriptions use the current UI language
when the catalog supplies a translation.

Descriptions and changelogs support a basic Markdown subset: ATX headings,
paragraphs, bold and italic text, ordered and unordered lists, quotes,
horizontal rules, inline code and fenced code blocks. HTTP(S) links have buttons
that open a QR code for the target URL. Fragment links retain their label;
images retain their alt text without fetching remote images. HTML is displayed
as text. Long words and code wrap to the available column width.

The catalog follows the GitLab or GitVerse mirror selected in Settings and
falls back to the other built-in sources. GitHub remains the default first
source. This is a separate catalog: a custom game catalog URL does not replace
the homebrew feed. Valid metadata is cached under the app's `homebrew/`
directory; a failed refresh retains the previous catalog. Relative icon URLs
are resolved against the mirror that supplied the metadata.

Downloads use HTTPS directly from the package's release URL, without a torrent
or debrid provider. They run from the detail page rather than the torrent
download queue. Leaving that page cancels an active installation. Individual
downloads are limited to 200 MiB and extracted files to 512 MiB.

Before changing existing files, pipensx checks the exact download size and
SHA-256, stages every required file, and checks the ZIP member paths and CRCs.
Up to two outer ZIP folders are recognized, matching the catalog builder.
Only declared files under `switch/`, `config/` or `atmosphere/` can be installed;
the pipensx application directory is excluded. Symlink destinations, traversal
paths and duplicate destinations are refused.

`U:` files replace the corresponding SD files. `G:` files are created only
when missing, preserving user configuration. Existing files are backed up
before replacement. A journal allows rollback on failure, cancellation or the
next homebrew catalog load after an interrupted installation. A committed
journal completes the installed-version receipt before deleting the backups.
Metadata replacement retains a `.previous` copy on filesystems that refuse
renaming over an existing file; reads can recover that copy after interruption.
Installation does not remove files that disappeared from a later package.

Packages containing Atmosphère files carry a restart notice. Overlay packages
show their catalog-specified dependencies; dependencies are not installed
automatically. Installed versions are recorded only for installations made by
pipensx and shown when all declared files still exist.

`tests/test_homebrew_catalog.cpp` covers metadata validation, mirror fallback,
cached icons, size/checksum failures, cancellation, ZIP extraction, wrapper
folders, configuration preservation, unsafe destinations and journal recovery.
The test emulates Switch rename behavior by refusing an existing destination.
PC tests do not establish physical Switch behavior.
