# Review hardening: filesystem errors and render-target ownership

## Evidence checked before changing code

Mario Maker's private executable and its generated safe translation agree at
its DeleteFile call (`0x00288bd0`) and recovery branch (`0x00287ee4` through
`0x00287ef8`). The code inspects the FS module and description ranges, rather
than treating every negative result alike. A private host test executing its
actual translated classifier functions confirms that FileNotFound belongs to
the missing-file class, while the new I/O and permission results do not.
The game-derived source and executable are not part of this repository.

Brain Age's private executable confirms the indirect call at `0x0039b928`.
The saved Cemu watchpoint caught renderer writeback at the native address of
the game's `darc` layout archive; the fixed earlier replay verified the complete
736,232-byte archive and successful `NbackU.bclyt` lookup. The archive is guest
application data after that reuse. Age alone cannot authorize an old target's
pixels to overwrite it.

## Filesystem behavior

DeleteFile now checks the nonthrowing file status before removal. A missing
path returns FileNotFound without calling remove. This matters because the
examined Aroma ContentRedirectionModule `FSRemoveWrapper` handles several
errno values but falls through to MEDIA_ERROR for ENOENT. That wrapper can
manufacture an EIO report for an ordinary absent file; it does not prove that
every earlier EIO had this cause. The examined wrapper also maps failed path
stat calls to NOT_FOUND without preserving errno. The runtime can distinguish
only errors supplied by its host interface; recovering errors already lost by
that external layer requires a separate module correction and hardware test.

Errors remain nonfatal to the host, but are no longer all mislabeled as absent:
permission/read-only failures return CommandNotAllowed; path and directory
errors retain their own classes; unknown host errors return FS/Internal/Permanent
with an unspecified description. This generic result does not claim an exact
reverse-engineered 3DS media-error description. Failed deletes retain the save
cache entry; forgetting it and notifying a save change occur after success.

The actual DeleteFile IPC regression covers success, missing preflight while
remove would return EIO, genuine injected EIO on an existing file, permission,
read-only, directory, ELOOP and invalid-archive failures. Existing data and cache
ownership survive failure. A missing path and a real I/O failure produce
different guest results. Tests compile with exceptions disabled.

## Render-target behavior

Completed filesystem reads now queue the actual number of bytes written through
the existing GSP CPU-write notification, in order with GX commands. The renderer
can observe archive loads even when the game does not issue an additional cache
notification.

A single definite write covering an entire target supersedes its GPU contents.
It clears GPU-dirty ownership, so normal age/reference checks can retire that
allocation without writing back into guest memory. If the target is reused for
drawing first, it uploads the replacement guest contents. Partial writes retain
the remaining GPU pixels. Two separated writes whose bounding span covers the
target are not sufficient: the span may contain unwritten gaps.

The actual guest-write and retirement functions pass 10,000 overwrite/retire
cycles with no retained-target accumulation. Boundary, partial, gapped, active
reference and post-redraw ownership cases pass; the archive-preservation test
still performs no eviction writeback.

This improves reclamation when ownership is demonstrably replaced. It is not
a hard global cache budget: GPU-dirty content with no proven replacement or
readback is still retained. A general bounded backing-store/lifetime design and
long-session hardware testing remain open. Discarding those pixels based only
on age would introduce a different correctness bug.

## Validation status

Targeted filesystem and ownership regressions pass. Both changed PowerPC runtime
units rebuild and link with the existing Mario Maker game-code objects. The
private candidate has not been deployed or tested in Cemu/hardware; the working
Wii U installation remains the previous hardware-tested release. Earlier hardware
success validates that previous revision, not this review hardening.
