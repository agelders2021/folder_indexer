# File Indexer

A single-file Python desktop application that walks a directory tree, records every file (excluding folders) with its full path, and stores the results in a local SQLite database. A Tkinter GUI provides indexing controls and a live-search interface.

## Purpose

The goal is to build a searchable, persistent index of files — including those hosted on Google Drive shared folders — so that any file can be located quickly by name without relying on the OS search tool.

---

## Tech Stack

| Layer       | Technology                                      |
|-------------|--------------------------------------------------|
| GUI         | Tkinter (stdlib) with ttk themed widgets         |
| Database    | SQLite via the `sqlite3` stdlib module           |
| Threading   | `threading.Thread` for non-blocking indexing     |
| File I/O    | `os.walk` + `pathlib.Path`                       |
| Shortcuts   | PowerShell (`subprocess`) to resolve `.lnk` targets |
| Runtime     | Python 3.8+ — **no third-party packages needed** |

---

## File Structure

```
file_indexer.py      # Entire application — single self-contained file
file_index.db        # SQLite database, created at runtime in the user's home dir
README.md            # This file
```

---

## Database Schema

Database is stored at `~/file_index.db` (i.e. `C:\Users\<user>\file_index.db` on Windows).

```sql
CREATE TABLE files (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    filename    TEXT NOT NULL,          -- bare filename, e.g. "report.pdf"
    extension   TEXT,                  -- lowercase with dot, e.g. ".pdf"
    full_path   TEXT NOT NULL UNIQUE,  -- absolute path on the local filesystem
    directory   TEXT NOT NULL,         -- parent directory only
    size_bytes  INTEGER,               -- file size; NULL if unreadable
    indexed_at  TEXT NOT NULL,         -- ISO-8601 timestamp of last index run
    human_dir   TEXT                   -- resolved human-readable directory (Google Drive shortcuts)
);

CREATE INDEX idx_filename  ON files(filename  COLLATE NOCASE);
CREATE INDEX idx_extension ON files(extension COLLATE NOCASE);
CREATE INDEX idx_directory ON files(directory);
```

`full_path` is the unique key. Re-indexing an already-indexed path upserts records rather than duplicating them.

Existing databases are automatically migrated to add `human_dir` on first run.

---

## Application Modules (all in `file_indexer.py`)

### Database helpers (top of file)

| Function | Purpose |
|---|---|
| `get_connection()` | Returns a `sqlite3.Connection` with `row_factory = sqlite3.Row` |
| `init_db()` | Creates the `files` table and indexes if they don't exist; migrates existing DBs |
| `_find_gdrive_root(root_dir)` | Walks up the directory tree to find the Google Drive mount root |
| `_resolve_shortcuts(search_root)` | Runs a PowerShell command to read all `.lnk` targets; returns a prefix map |
| `_strip_gdrive_root(path)` | Strips the machine-specific `My Drive\` or `Shared drives\` prefix for portable display |
| `index_directory(root, ...)` | Walks `root` with `os.walk`, resolves shortcuts, upserts files in batches of 500 |
| `search_files(query, search_by, extension_filter, limit)` | Parameterised `LIKE` query; returns list of `Row` objects |
| `get_stats()` | Returns total file count, last indexed timestamp, top-10 extensions |
| `clear_index()` | Deletes all rows from `files` |

### `FileIndexerApp(tk.Tk)`

The main window class. Builds three notebook tabs:

- **Index tab** — directory picker, Start/Cancel/Clear buttons, indeterminate progress bar, scrollable log box. Indexing runs in a daemon thread; progress and completion are posted back to the main thread via `self.after()`. The log box shows shortcut resolution progress before the file walk begins.

- **Search tab** — live search entry (fires on every keystroke via `StringVar.trace_add`), radio buttons for filename / full path / extension search mode, optional extension filter field, a sortable `Treeview` results table with columns **Filename**, **Directory**, **Size**. Right-click context menu copies the directory or filename to the clipboard.

- **Stats tab** — plain-text display of database location, total file count, last-indexed timestamp, and a text bar chart of the top 10 extensions by count.

---

## Google Drive Shortcut Resolution

When indexing a Google Drive folder synced via Google Drive for Desktop, files accessed through shortcuts are stored under a hidden `.shortcut-targets-by-id/<id>/` path on the local filesystem. These ID-based paths are meaningless to users.

The indexer handles this automatically:

1. Before walking files, it locates the Google Drive mount root by searching upward for the `.shortcut-targets-by-id` folder.
2. It runs a single PowerShell command to read every `.lnk` file under that root and builds a map of `{target_path → human_prefix}` (e.g. `G:\.shortcut-targets-by-id\1abc\Dogs → G:\My Drive\505 SAR Dogs`).
3. During the file walk, any file whose path starts with a known shortcut target gets its `human_dir` populated using prefix substitution.
4. At display time, the `My Drive\` or `Shared drives\` prefix is stripped, leaving a portable relative path (e.g. `505 SAR Dogs\Training`).

`.lnk` shortcut files themselves are suppressed from search results.

---

## Key Behaviours & Design Decisions

- **Single file** — no package structure, no imports beyond stdlib. Easy to copy anywhere and run.
- **Upsert on conflict** — `INSERT ... ON CONFLICT(full_path) DO UPDATE` means the same directory can be re-indexed safely without duplicates.
- **Batch writes** — files are accumulated in a list and written 500 at a time to avoid excessive commit overhead on large trees.
- **Cancel support** — a `threading.Event` is checked during the walk loop; setting it stops indexing cleanly without killing the thread.
- **Dark theme** — custom colour palette applied via `ttk.Style` and `tk.configure`; colours are defined as module-level constants at the top of the file for easy modification.
- **Portable display paths** — the Google Drive mount prefix (`G:\My Drive\`) is stripped at display time, so paths shown to users are root-independent.

---

## Possible Enhancements

- Export search results to CSV or JSON.
- Add a `modified_at` column (from `os.path.getmtime`) to support date-range filtering.
- Full-text search on file contents for text files (would require `sqlite3` FTS5 extension).
- Scheduled/automatic re-indexing on a timer.
- CLI mode (argparse) to allow headless indexing from the command line or a batch script.
- Multi-root support — index several directories in one session and tag each file with its root.
- Exclude-pattern support (e.g. skip `node_modules`, `.git`, system folders).

---

## Running

```bash
python file_indexer.py
```

Python 3.8 or newer required. Tkinter and SQLite ship with the standard Windows Python installer — no `pip install` is needed.
