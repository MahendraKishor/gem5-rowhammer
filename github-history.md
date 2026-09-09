# GitHub Setup & Git Command Reference

Personal notes for **MahendraKishor/gem5-rowhammer**.
Setup performed: **2026-09-09**.

---

## 1. What this repo is

This is a working copy of the **gem5 RowHammer simulator** (gem5 v23.0.1.0), used for
DRAM / RowHammer research. Its history has three layers:

| Layer | Who | What |
|---|---|---|
| gem5 upstream | gem5 project | ~20,900 commits of base gem5 |
| RowHammer extensions | Kaustav Goswami (UC Davis) | RowHammer modelling, ECC in `dram_interface`, variation maps |
| HammerSim mods | sudipto0315 | DRAM cache ctrl, policy manager, ECC configs, reference papers |
| **my work** | **Mahendra** | **anything I commit from here on** |

Total: **20,941 commits**, ~337 MB of git objects.

---

## 2. What was done during setup

1. **Found the problem** — `origin` pointed at `github.com/sudipto0315/gem5-rowhammer`,
   which I have no push rights to. All existing commits were authored by others.

2. **Renamed that remote to `upstream`** and *disabled pushing to it*, so I can never
   accidentally publish to someone else's repo:
   ```bash
   git remote rename origin upstream
   git remote set-url --push upstream DISABLED_use_origin_instead
   ```

3. **Generated an SSH key** for GitHub auth (no passphrase, ed25519):
   ```bash
   ssh-keygen -t ed25519 -C "jaiswalmahendra31@gmail.com" -f ~/.ssh/id_ed25519
   ```
   - Private key: `~/.ssh/id_ed25519`  ← **never leaves this machine, never share**
   - Public key:  `~/.ssh/id_ed25519.pub` ← this is what was pasted into GitHub
   - Added at <https://github.com/settings/ssh/new>

4. **Verified GitHub's host key** before trusting it. The fingerprint matched
   GitHub's published ed25519 value `SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU`,
   confirming no man-in-the-middle.

5. **Created a new empty PRIVATE repo** on GitHub: `MahendraKishor/gem5-rowhammer`.

   > Note: this is a *duplicate*, not a GitHub fork. A fork of a **public** repo is
   > always public — GitHub does not allow private forks — so to keep this private it
   > had to be an independent repo with the history pushed up manually.

6. **Pushed everything**:
   ```bash
   git remote add origin git@github.com:MahendraKishor/gem5-rowhammer.git
   git push -u origin develop
   ```
   All 20,941 commits landed. `-u` set `develop` to track `origin/develop`, so a bare
   `git push` now goes to my repo.

---

## 3. Current remote layout

```
origin    git@github.com:MahendraKishor/gem5-rowhammer.git   fetch + PUSH   (mine, private)
upstream  https://github.com/sudipto0315/gem5-rowhammer.git  fetch only     (push disabled)
```

Check it any time with:
```bash
git remote -v
```

---

## 4. Everyday: commit and push

```bash
# 1. See what changed
git status

# 2. Stage changes
git add path/to/file.cc          # one file
git add src/mem/                 # a whole directory
git add -A                       # everything (careful: includes new files)

# 3. Review exactly what you staged, before committing
git diff --staged

# 4. Commit with a message
git commit -m "mem: describe what you changed"

# 5. Push to YOUR repo
git push
```

`git push` alone works because `develop` tracks `origin/develop`. The long form is
`git push origin develop`.

### Fixing mistakes before you push
```bash
git restore path/to/file          # discard uncommitted changes to a file
git restore --staged path/to/file # unstage, but keep the edits
git commit --amend -m "new msg"   # reword the last commit (only if NOT pushed yet)
git reset --soft HEAD~1           # undo last commit, keep changes staged
```

> Avoid `git reset --hard` unless you are certain — it **destroys** uncommitted work.

---

## 5. Everyday: looking at things in the terminal

### History
```bash
git log --oneline -20                       # last 20 commits, compact
git log --oneline --graph --all -20         # with branch graph
git log --stat -3                           # which files changed
git log -p -1                               # full diff of last commit
git log --author="Mahendra" --oneline       # only my commits
git log --since="2 weeks ago" --oneline     # recent activity
git log --oneline -- src/mem/               # history of one path
```

### A specific commit
```bash
git show 2b0e046863                # full diff of that commit
git show 2b0e046863 --stat         # just the file list
git show HEAD                      # the most recent commit
git show HEAD~3                    # three commits back
```

### Current changes
```bash
git diff                    # unstaged changes
git diff --staged           # staged changes
git diff HEAD               # both combined
git diff upstream/develop   # how I differ from sudipto0315's version
git diff --stat             # summary instead of full text
```

### Branches and sync state
```bash
git branch                  # local branches (* = current)
git branch -a               # include remote branches
git status -sb              # short status + ahead/behind line

# exact ahead/behind counts vs my GitHub repo
git rev-list --left-right --count origin/develop...develop
```

### What is actually on GitHub right now
```bash
git ls-remote origin              # refs on my repo (no download)
git fetch origin                  # update my knowledge of it
git log origin/develop --oneline -5
```

### Who changed a line, and where a file is
```bash
git blame src/mem/dram_interface.cc | head -30
git log -S "rowhammer" --oneline        # commits that added/removed that string
git grep -n "rowhammer" -- src/         # search the working tree
```

---

## 6. Pulling updates from sudipto0315 (upstream)

Because this is a duplicate and not a fork, GitHub shows no "Sync fork" button —
do it locally:

```bash
git fetch upstream                  # download their new commits
git log --oneline develop..upstream/develop   # preview what's new
git merge upstream/develop          # merge into my develop
git push                            # publish the merge to my repo
```

If a merge goes wrong: `git merge --abort` returns you to where you were.

---

## 7. Working on a feature safely

Keep experiments off `develop` so `develop` always builds:

```bash
git switch -c feature/my-experiment      # create + switch to a branch
# ... edit, add, commit as usual ...
git push -u origin feature/my-experiment # publish the branch (first time)

git switch develop                       # go back
git merge feature/my-experiment          # bring the work in
git branch -d feature/my-experiment      # delete the finished branch
```

---

## 8. Gotchas specific to this repo

- **Large file near GitHub's hard limit.** `util/hammersim/prob-005.json.zip` is
  **91.61 MB**. GitHub warns above 50 MB and *hard-rejects* above 100 MB, leaving
  ~8 MB of headroom. If that file ever grows past 100 MB, pushes will fail and
  fixing it means rewriting history (Git LFS), not just one commit.

- **`upstream` push is intentionally broken.** If you see
  `DISABLED_use_origin_instead`, that is not a bug — it is the guard from step 2.

- **Commit identity.** Commits are authored as `Mahendra <jaiswalmahendra31@gmail.com>`
  (from `~/.gitconfig`). GitHub only links commits to my profile if that email is
  registered at <https://github.com/settings/emails>.
  ```bash
  git config user.name && git config user.email    # check
  ```

- **`z_instructions.txt`** in the repo root is a personal note (ACA project setup
  steps), deliberately left untracked.

- **`.gitignore` already excludes** local build artifacts plus `/gem5-resources/`,
  `/ext/json/json/`, `/util/hammersim/prob-005.json`, and `store/tutorial/bench/mm`.
  Check whether something is ignored with:
  ```bash
  git check-ignore -v path/to/file
  ```

---

## 9. Setting this up on another machine

```bash
# 1. New machine needs its OWN key (do not copy the private key around)
ssh-keygen -t ed25519 -C "jaiswalmahendra31@gmail.com" -f ~/.ssh/id_ed25519
cat ~/.ssh/id_ed25519.pub          # paste into github.com/settings/ssh/new

# 2. Confirm GitHub accepts it
ssh -T git@github.com              # expect: "Hi MahendraKishor!"

# 3. Clone, then re-add upstream
git clone git@github.com:MahendraKishor/gem5-rowhammer.git
cd gem5-rowhammer
git remote add upstream https://github.com/sudipto0315/gem5-rowhammer.git
git remote set-url --push upstream DISABLED_use_origin_instead
```

Build instructions for the simulator itself are in `z_instructions.txt` and
[README-RH.md](README-RH.md).
