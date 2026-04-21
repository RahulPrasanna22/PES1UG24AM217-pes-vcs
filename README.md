# PES-VCS Lab Report
**Name:** Rahul Prasanna
**SRN:** PES1UG24AM217

---

## Phase 5: Branching and Checkout

### Q5.1 — Implementing `pes checkout <branch>`

A branch in PES-VCS (and Git) is simply a file at `.pes/refs/heads/<branch>` containing a commit hash. Creating a branch is literally creating that file. Given this, implementing `pes checkout <branch>` requires changes at three levels:

**Files that need to change in `.pes/`:**
- `.pes/HEAD` must be updated to point to the new branch: `ref: refs/heads/<branch>`
- If the branch doesn't exist yet, `.pes/refs/heads/<branch>` must be created containing the current commit hash

**What must happen to the working directory:**
1. Read the target branch's commit hash from `.pes/refs/heads/<branch>`
2. Read that commit object to get its tree hash
3. Walk the tree recursively and compare every file's content against the current working directory
4. For every file in the target tree that differs from what's currently on disk, overwrite the working directory file with the version from the object store
5. For files that exist in the current tree but not the target tree, delete them from the working directory
6. Update `.pes/HEAD` to point to the target branch

**What makes this complex:**
The difficulty is safely handling the transition. You cannot just delete everything and re-extract — if any step fails halfway, the working directory is left in a broken state. Git solves this with a three-stage approach: first verify the switch is safe (no conflicts), then update the index, then update the working directory. The other source of complexity is dirty file detection — if a file has been modified locally and also differs between branches, checkout must refuse rather than silently discard the user's changes (covered in Q5.2).

---

### Q5.2 — Detecting a Dirty Working Directory Conflict

To detect whether a checkout is safe, you only need the index and the object store — no need to diff entire files byte-by-byte upfront.

**The algorithm:**

For each file tracked in the current index:
1. Read the file's `mtime` and `size` from the index entry
2. `stat()` the actual file on disk to get its current `mtime` and `size`
3. If they differ, the file is potentially modified — re-hash it and compare with the blob hash stored in the index
4. If the hash differs, the file is genuinely dirty

Then, for each file that is dirty:
- Look up whether that same path exists in the target branch's tree (by reading the target commit → its tree → walking recursively)
- If the target branch has a different blob hash for that path than the current index does, there is a conflict — the checkout must refuse with an error like: `error: your local changes to 'file.txt' would be overwritten by checkout`

If the dirty file doesn't exist in the target branch at all, it's still a conflict — checking out would delete uncommitted work.

**Why this works without hashing everything:** The `mtime`/`size` fast-check (borrowed directly from Git's index design) means you only re-hash files that have actually been touched on disk, keeping the check fast even for large repositories.

---

### Q5.3 — Detached HEAD and Recovery

**What "detached HEAD" means:**
Normally, `.pes/HEAD` contains `ref: refs/heads/main`, an indirect pointer to a branch. In detached HEAD state, it contains a raw commit hash directly, e.g. `a1b2c3d4...`. There is no branch name involved.

**What happens if you commit in this state:**
Commits still get created normally — a new commit object is written to the object store and HEAD is updated to point to the new commit hash directly. The problem is that no branch file is updated. The new commits are reachable only via HEAD, which changes with every new commit. The moment you checkout a branch, HEAD moves away and those commits become unreachable — no branch, no ref, nothing points to them. Git's garbage collector will eventually delete them.

**How a user can recover those commits:**
1. Before switching away, note the commit hash shown in `pes log` — the topmost one is the detached HEAD commit
2. Create a new branch pointing to it: `git branch recovery-branch <hash>` (or the equivalent in PES-VCS: write that hash into `.pes/refs/heads/recovery-branch` and update HEAD to `ref: refs/heads/recovery-branch`)
3. If you already switched away and lost the hash, Git provides `git reflog` which tracks every position HEAD has pointed to. PES-VCS does not implement reflog, so without it, recovery would require manually scanning all objects in `.pes/objects/` to find commit objects and reconstruct the chain — possible but tedious.

---

## Phase 6: Garbage Collection and Space Reclamation

### Q6.1 — Algorithm to Find and Delete Unreachable Objects

**The algorithm (mark-and-sweep):**

**Mark phase — find all reachable objects:**
1. Collect all starting points: read every file under `.pes/refs/heads/` to get all branch tip commit hashes. Also read `.pes/HEAD`.
2. For each commit hash, do a BFS/DFS traversal:
   - Read the commit object → mark its hash as reachable
   - Add its tree hash to the queue
   - If it has a parent, add the parent commit hash to the queue
3. For each tree hash in the queue:
   - Read the tree object → mark it as reachable
   - For each entry: if it's a blob, mark it reachable; if it's a subtree, add it to the queue
4. For each blob hash encountered: mark it as reachable

**Sweep phase — delete unreachable objects:**
1. Walk every file under `.pes/objects/` (i.e. `find .pes/objects -type f`)
2. For each file, reconstruct its hash from its path (first 2 chars of directory + rest of filename)
3. If that hash is NOT in the reachable set, delete the file
4. Remove any now-empty shard directories

**Data structure:** A hash set (e.g. a hash table or a sorted array of 32-byte hashes) is ideal for the reachable set — O(1) average lookup, compact in memory.

**Estimate for 100,000 commits and 50 branches:**
Assuming an average project with ~20 files per commit, each commit has: 1 commit object + ~5 tree objects (for directory structure) + ~20 blob objects = ~26 objects per commit. Over 100,000 commits that's roughly **2.6 million objects** to visit in the mark phase. Many blobs will be shared across commits (unchanged files), so the reachable set will be smaller than 2.6M, but the traversal still visits all of them.

---

### Q6.2 — Race Condition Between GC and Concurrent Commit

**The race condition:**

Consider this sequence of events with two concurrent processes — a `pes commit` and `pes gc`:

1. `pes commit` calls `tree_from_index()` and writes several blob objects and tree objects to `.pes/objects/`. These objects now exist on disk but nothing points to them yet — no commit references them, so they look unreachable.
2. At this exact moment, `pes gc` runs its mark phase. It starts from all branch refs, walks the reachable graph, and correctly determines that the newly written blobs and trees are unreachable (because the commit object hasn't been written yet).
3. `pes gc` runs its sweep phase and **deletes** those blob and tree objects.
4. `pes commit` now calls `object_write()` for the commit object, which references the deleted tree hash. The commit is written successfully, but when anything tries to read it, the tree object it points to is gone — the repository is now corrupt.

**How Git avoids this:**

Git uses a grace period strategy in its real GC (`git gc`). Specifically:

- Objects younger than a threshold (default: 2 weeks, configurable via `gc.pruneExpire`) are never deleted by GC, even if they appear unreachable. This gives any in-progress commit operation more than enough time to complete and create a reference to the new objects.
- Git also writes a `.git/gc.pid` lockfile so only one GC runs at a time.
- For truly concurrent safety in server environments, Git uses reference transaction hooks and pack-refs locking so that the mark phase and the commit operation cannot interleave in a dangerous way.

In PES-VCS, the simplest safe approach would be to check each object's filesystem `mtime` during the sweep phase and skip deletion of any object created in the last few minutes.
