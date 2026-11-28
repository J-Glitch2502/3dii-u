# Experimental save states

Resumable builds return guest execution to the scheduler at calls, service calls,
and preemption boundaries, so checkpoints do not retain native C call stacks.
The left-stick menu exposes Save State and Load State for all resumable titles.
This is an experimental execution mode with a CPU cost; all-title correctness
and performance have not been established.

The checkpoint barrier stops mutation while CPU registers, guest memory, kernel
objects, timers, supported services, DSP state and renderer state are captured.
Saved surfaces stay pinned against idle eviction. Replacing a checkpoint releases
the previous payload before allocating its replacement, avoiding two large RAM
snapshots overlapping. Native microphone capture remains continuous where its
sampling state has not changed.

Persistent saves use `checkpoint-<program-id>.bin` in the title's common save
storage. A same-session load uses the RAM checkpoint when available. After a
relaunch, loading validates the file and reconstructs runtime objects and GPU
resources before committing guest state. Native pointers and arbitrary callbacks
are not written into the archive. Unsupported state fails capture rather than
serializing unknown native resources.

The file is bounded and checksummed, with title, code, build and schema identity
checks. It is currently specific to the executable build that created it;
rebuilding or updating the executable can make an old state incompatible. Save
files are written through a temporary file and retain a backup of the previous
checkpoint. Saving approximately 106 MB on the Wii U can take substantially
longer than loading a RAM checkpoint. These files are separate from ordinary
in-game progress saves.

Persistent capture now stages immutable chunks shared with the RAM checkpoint.
The paused phase still captures state and flushes game saves; a background worker
then checksums and writes those chunks to SD. A same-session RAM load can finish
while that write remains pending. Replacement file-save requests cancel/coalesce
the old writer before a new capture; file loads wait for publication. Shutdown
joins the writer before filesystem/log teardown. A failed disk write retains the
RAM checkpoint. This does not make capture itself instantaneous.

The saving indicator is composed onto temporary TV/GamePad scanout copies, not
the retained game surfaces, and the first frame after completion removes it even
when the game has stopped redrawing. Persistent loads reconstruct and checksum
the payload in one pass, validate it, then commit; they no longer read the entire
payload twice. Checksums, identity checks and bounded reconstruction remain.

Host transaction tests cover a deliberately blocked disk writer, RAM restore
before publication, 30 replacement requests, failure handling and corrupt-file
rejection. The async revisions were built and uploaded for Planet Robobot;
full repeated real-hardware stress testing and all-title behavior remain open.

## Observed validation

- Kirby Triple Deluxe: three repeated save/load cycles on Wii U after the
  replacement-memory fix; the user confirmed working controls and audio.
- Brain Age, Kid Icarus Uprising and Super Mario 3D Land: individual session
  save/load tests succeeded. This is not coverage of every scene or service.
- Mario 3D Land: a file state loaded after a new Cemu process; the user confirmed
  controls and audio. On Wii U, the recording also shows a file save, a new game
  startup and a persistent-checkpoint rebuild, with successful play reported.
- The latest successful Wii U Mario run had FTPiiU disabled. Earlier runs
  detected corrupt native heap links during allocations, including FTPiiU's
  allocator calls. This does not identify the original writer or establish that
  the heap issue is fixed. Long-session stability remains unresolved.
- Mario's patched 60 Hz presentation build showed roughly 41–48 FPS in some
  hardware scenes. No matched before/after measurement isolates the cost of
  resumable execution from the existing patch and scene workload.

## Targeted checks

`tools/test_recomp_resumable.py` executes nested ARM/Thumb calls, loops, SVCs and
jump-table continuations and rewinds CPU and RAM at dispatcher boundaries.
`tools/test_checkpoint_scheduler.py` covers post-service scheduling.
`tools/test_checkpoint_transaction.py` covers rollback, replacement allocations,
memory-budget rejection, file reload and checksum rejection using host fixtures.
`tools/test_checkpoint_surface_lifetime.py` exercises actual eviction/pinning
paths. `tools/test_state_archive.py` covers logical collection serialization,
size/truncation rejection and rejection of native pointers/callbacks. These host
checks complement the hardware tests; they do not emulate the Wii U's allocator
or prove every persistent service combination safe.

## Installed-title storage quota

The Urban Champion WUP test exposed a storage-size mismatch: its metadata
allowed 64 MiB of common save storage, while the captured checkpoint payload was
95,511,936 bytes. RAM restore worked, but the background file write failed.
Homebrew SD redirection and Cemu do not establish that an installed quota fits.
The first workaround requested 1 GiB of common storage. Nintendo reserves this
capacity even when the files are small, so that workaround wasted storage.
New metadata starts at 16 MiB shared and 8 MiB per account (64 MiB per account
for Mario Maker's course files). `quota_growth.cpp` requests larger quotas only
when pending writes need them, using Nintendo's `SAVEUpdateSaveDir` API.

A disposable hardware probe grew a quota from 16 to 32 MiB and preserved a marker
file. This establishes the native API path, not full production coverage.
Automatic checkpoint growth, interrupted-growth recovery, and growth while a
native update is active still require hardware tests. The implementation uses
CFW filesystem access to update only the running generated title's quota fields;
if that access or growth fails, the write fails and its RAM checkpoint remains.
Do not describe dynamic quota growth as release-validated yet.

Large checkpoint writes flush stdio and commit storage periodically, and commit
again after publication. Quota flushes are serialized with the save worker.
Failure logs identify the operation, error and bytes written; the RAM checkpoint
remains available if disk storage fails. The overlay uses storage-neutral text.

The fresh Urban Champion WUP Test 2 subsequently logged a completed
95,298,074-byte background save in 31,398 ms. The user also confirmed loading
the state after closing and reopening the installed game. This establishes the
tested title's persistent path, not universal installed-title service coverage.
A subsequent UI revision adds a pre-submitted loading message during file
reconstruction and flushes the saving overlay's color cache before scanout;
its real-console visibility still needs confirmation.

The first homebrew overlay revision regressed SD persistence: both recorded
attempts failed at the first periodic quota commit (8,427,239 payload bytes),
with FS_STATUS_NOT_FOUND (-6), while retaining the RAM state. ContentRedirection
redirects file operations but its FSAFlushQuota replacement passes the original
path through. Checkpoint commits now inspect the mounted common directory:
ordinary directories use checked file writes/close without title-quota commits;
quota directories retain periodic/final commits. Unknown mount information keeps
the strict commit path. Checkpoints commit only common storage, not an unrelated
account directory. The corrected homebrew build still needs a completed hardware
write and cross-launch restore to verify this regression is resolved.

The SD retest completed a 95,325,063-byte background save in 19,888 ms;
mounted common/account paths both reported ordinary directories, confirming
that the corrected backend selection removed the quota-flush regression.
The user still could not see the progress indicator on hardware. The next
candidate renders it on the actual TV/GamePad target (the same destination as
the working options menu), submits scanout, then restores the clean retained
picture from its backup in GPU command order. Each screen logs show/clear and
backup availability once per transition. This display candidate requires a
hardware visibility test; successful file writes alone do not verify it.

That display candidate logged both TV and GamePad show/clear with valid backup
buffers, but the user still saw no GamePad indicator. The next correction
restores the runtime GX2 context after the backup surface copy and before
issuing the overlay draw. Native GX2CopySurface uses rendering state internally;
Cemu's HLE implementation does not reproduce those state changes. The scanout
fixture deliberately clobbers context on each copy and checks that both screen
indicators are drawn only after rebinding, scanned out, and removed from the
retained paused image. This is a hardware-state hypothesis pending console test.

The user confirmed the context-reset revision finally made the indicator work
on real hardware. A matching Urban Champion WUP Test 2 package was then rebuilt
for an installed-title regression test, retaining its existing test title ID,
1 GiB common quota, and the same executable/shaders as the verified homebrew.


## Latest storage/display verification summary

Urban Champion's fresh WUP test completed a persistent save and restored it after
relaunch. The corrected Homebrew backend completed its SD write, and the user
confirmed the final GX2 context-reset revision made the indicator visible on real
hardware. A matching WUP package was uploaded afterward; this does not establish
that every game/service combination or overlay transition is covered. The earlier
candidate-by-candidate pending notes above document the investigation sequence.
The loading message is submitted before blocking file reconstruction; it is not
an animated progress guarantee while the GPU thread is blocked.


## Native WUP identity and Data Management

WUP base IDs now use `000500001Dxxxx00`, preserving the 3DS unique number in
bits 8..23. The `1D` prefix supports native update lookup; the final
zero byte is required for Data Management to group saves with software.
System Settings masks this byte for save grouping but retains it for software.
The earlier generated IDs could therefore show software and saves separately
even on the same USB drive with otherwise matching metadata.

On November 27, 2026, the corrected Urban Champion base launched on real Wii U.
Its existing account saves were copied to the newly registered title while its
new native save metadata remained intact. The user confirmed correct grouping.
The Builder does not migrate existing installs or saves automatically. Use a
save manager and verify restored progress before removing an old title.

This identity fix does not change checkpoint ownership: persistent states are
still in common storage. Moving states per user and adding a delete-state menu
item remain separate work.
