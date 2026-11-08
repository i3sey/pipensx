# Live torrent sharing

Direct BitTorrent shares verified pieces while downloading and installing.
Settings → Downloads → Share torrent pieces enables it by default. A message
shown once explains that sharing helps keep torrents available and can affect
download speed. Debrid downloads do not join the torrent swarm.

Stream installations keep pieces in a separate RAM ring. New verified pieces
replace the oldest ones when the ring fills. The cache uses the memory left
after the installation buffers, engine allowance and application reserve; it
has no fixed 1 GiB ceiling. It allocates stable blocks of at most 64 MiB (or one
piece for larger piece sizes), splitting failed allocations further. If memory
runs out, it retains every slot already allocated instead of halving the whole
cache. Changes in the shared memory budget can discard and
rebuild a smaller cache. Application mode is the supported Switch launch mode.

After installation, retained pieces remain available until pipensx exits,
sharing is disabled, the task is removed or another transfer needs the memory.
Small torrents can remain entirely in RAM. Larger torrents share only the
retained window, without downloading the missing pieces again. Ordinary
downloads share from SD. Starting another task closes the previous sharing
session before allocating its engine. Pure sharing does not block auto-sleep.

The peer bitfield describes only pieces that can actually be read. Eviction
does not change download progress. Fast-extension peers receive request
rejections for missing pieces; older peers are choked and unchoked. Frames
already being sent own their payload so eviction cannot corrupt a partial
send. Four upload slots share a 1 MiB/s limit. The task records payload bytes
accepted by the transport, excluding protocol headers.

Upload still requires other peers to request a retained piece. A connected
peer can already have that piece or the complete torrent. The health log
includes interested peers, open upload slots, queued requests, requests and
rejections on active connections, and total uploaded bytes. Cache creation
logs both requested and allocated bytes and the number of backing blocks.

The installer finishes asynchronously while the torrent loop continues serving
requests. A full completion announcement waits for installation success and
full readable coverage. Partial sharing keeps a nonzero `left` and uses the
HTTP partial-seed event. Private torrents disable the engine's DHT and PEX.
RAM contents and active sharing are not restored after a restart; uploaded
totals are stored in queue version 9, with older queue versions still readable.

This behavior supersedes the post-install full-cache-only rule in the
[October design](seeding-ram-research.md). It does not implement background
sharing after exit, automatic re-downloading to refresh the cache, or advanced
upload-slot rotation.

Tests cover cache eviction, hash rejection, request cancellation, partial sends,
tracker accounting, nonblocking install completion and manager handoff from SD
sharing to a RAM-backed installation. `test_live_seeding` uses local HTTP and
the PC install backend; it does not install its synthetic package on a Switch.
