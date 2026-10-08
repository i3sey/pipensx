# Port files and destinations in the remote panel

In **Downloads**, a task in port mode has **Port files and destinations**.
This editor is available in the browser; ordinary Switch game tasks retain
their existing base/update/DLC installation flow.

Every task in port mode waits for review in this panel before any extraction,
copying or package installation. Downloading may finish first. Restarting the
app or an old auto-copy marker cannot start the port installation automatically.

1. Select which downloaded files to deploy. NSP/NSZ files use the package
   installer; other files can have an explicit destination folder.
2. Browse existing SD directories or enter a new absolute SD path, such as
   `/switch/myport`. Leave a destination empty to retain automatic mapping.
3. Open the ZIP/7z preview to see its members and final paths. A dash means that
   a member has no automatic destination. An explicit archive destination
   preserves the archive's internal directory tree under the chosen folder.
4. Review every file and destination, then save all choices. You may explicitly
   confirm the automatic destinations by leaving those fields empty. The port
   waits for **Install port** in this panel, including after restarting the app.
5. Install once the download is complete and the preview reports no conflicts.

For an archive containing `switch/myport/data.bin`, choose `/` to obtain
`/switch/myport/data.bin`. For an archive containing `data/data.bin`, choose
`/switch/myport` to obtain `/switch/myport/data/data.bin`. The preview shows
these exact paths before installation. A loose file keeps its filename inside
the chosen folder. Use the paths expected by the port and its bundled launcher.

Selections affect extraction and copying of files already included in the
task's download. They do not create new Real-Debrid torrents or fetch files
previously excluded from the download. The archive header preview becomes
available when the archive has finished downloading. It supports ZIP and 7z.

The server checks PIN and same origin for every editor request. It rejects
path traversal, FAT name collisions, symlink destinations and writes into
`/switch/pipensx`. Manual destinations cannot overwrite existing archive
files. Choices freeze during deployment and after its receipt has been saved.
Actual destinations are recorded in the existing deployment receipts, including
members placed outside `/switch`, so later validation/removal uses those paths.

Editor rules are stored separately in `port-layout-<task-id>.json` (version 1)
under the app directory. The download queue schema and debrid provider flow
are unchanged. Folder browsing is capped at 512 entries; archive previews at
16,384 members.

API: `GET /api/ports/<task-id>`, `GET /api/ports/folders?path=/switch`,
`POST /api/ports/<task-id>/preview`, `PATCH /api/ports/<task-id>`, and
`POST /api/ports/<task-id>/start`. Preview/save bodies contain `files`, an array
of `{source, enabled, directory}` rules. Start requires saved choices and a
completed, valid port deployment plan.
