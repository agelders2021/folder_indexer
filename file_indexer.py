"""
File Indexer - Directory Walker & Searchable Database
======================================================
Walks a directory tree, records every file (not folders) with its full path,
and stores the results in a local SQLite database.  Includes a Tkinter GUI
for indexing and searching.

Requirements: Python 3.8+  (Tkinter ships with the standard Windows installer)
Run:  python file_indexer.py
"""

import os
import sqlite3
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from datetime import datetime
from pathlib import Path

# ─── Database helpers ────────────────────────────────────────────────────────

DB_PATH = Path.home() / "file_index.db"


def get_connection() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with get_connection() as conn:
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS files (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                filename    TEXT NOT NULL,
                extension   TEXT,
                full_path   TEXT NOT NULL UNIQUE,
                directory   TEXT NOT NULL,
                size_bytes  INTEGER,
                indexed_at  TEXT NOT NULL,
                human_dir   TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_filename  ON files(filename COLLATE NOCASE);
            CREATE INDEX IF NOT EXISTS idx_extension ON files(extension COLLATE NOCASE);
            CREATE INDEX IF NOT EXISTS idx_directory ON files(directory);
        """)
        # Migrate existing databases that pre-date the human_dir column
        cols = {row[1] for row in conn.execute("PRAGMA table_info(files)")}
        if "human_dir" not in cols:
            conn.execute("ALTER TABLE files ADD COLUMN human_dir TEXT")


def _strip_gdrive_root(path: str) -> str:
    """Remove the machine-specific Google Drive prefix (everything up to and
    including 'My Drive\' or 'Shared drives\') so paths are portable."""
    if not path:
        return path
    lower = path.lower()
    for marker in ("my drive" + os.sep, "shared drives" + os.sep):
        idx = lower.find(marker)
        if idx != -1:
            return path[idx + len(marker):]
    return path


def _find_gdrive_root(root_dir: str) -> str:
    """Walk up from root_dir until we find the folder that contains
    .shortcut-targets-by-id — that is the Google Drive mount root."""
    p = Path(root_dir)
    while True:
        if (p / ".shortcut-targets-by-id").exists():
            return str(p)
        parent = p.parent
        if parent == p:          # reached the filesystem root
            break
        p = parent
    return root_dir              # fallback: search from where the user pointed


def _resolve_shortcuts(search_root: str, log=None) -> dict:
    """Read every .lnk file under search_root via PowerShell and return
    {os.path.normcase(target_path): human_prefix} where human_prefix is the
    human-readable path the shortcut represents (lnk directory + lnk stem)."""
    if log:
        log(f"Reading shortcuts under: {search_root}")
    ps = (
        "$shell = New-Object -ComObject WScript.Shell; "
        "Get-ChildItem -LiteralPath '" + search_root.replace("'", "''") + "' "
        "-Recurse -Filter '*.lnk' -ErrorAction SilentlyContinue | "
        "ForEach-Object { try { "
        "$t = $shell.CreateShortcut($_.FullName).TargetPath; "
        "if ($t) { $_.FullName + [char]9 + $t } } catch {} }"
    )
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
            capture_output=True, text=True, timeout=300
        )
        mapping = {}
        for line in r.stdout.splitlines():
            if "\t" in line:
                lnk, target = line.split("\t", 1)
                lnk, target = lnk.strip(), target.strip()
                if target:
                    lnk_p = Path(lnk)
                    # human_prefix = the path the shortcut *represents*,
                    # e.g. "G:\My Drive\505 SAR Dogs"  (no .lnk extension)
                    human_prefix = str(lnk_p.parent / lnk_p.stem)
                    mapping[os.path.normcase(target)] = human_prefix
        # Sort longest prefix first so nested shortcuts resolve correctly
        mapping = dict(sorted(mapping.items(), key=lambda kv: len(kv[0]), reverse=True))
        if log:
            log(f"Resolved {len(mapping):,} shortcut target(s).")
        return mapping
    except Exception as exc:
        if log:
            log(f"Shortcut resolution failed: {exc}")
        return {}


def index_directory(root_dir: str,
                    progress_callback=None,
                    done_callback=None,
                    cancel_event: threading.Event = None,
                    log_callback=None):
    """Walk root_dir and upsert every file into the database."""
    now = datetime.now().isoformat(timespec="seconds")

    # Build a map of shortcut targets -> human-readable directory
    gdrive_root = _find_gdrive_root(root_dir)
    shortcut_map = _resolve_shortcuts(gdrive_root, log=log_callback)

    batch = []
    total = 0

    upsert_sql = """
        INSERT INTO files
            (filename, extension, full_path, directory, size_bytes, indexed_at, human_dir)
        VALUES (?,?,?,?,?,?,?)
        ON CONFLICT(full_path) DO UPDATE SET
            filename   = excluded.filename,
            extension  = excluded.extension,
            directory  = excluded.directory,
            size_bytes = excluded.size_bytes,
            indexed_at = excluded.indexed_at,
            human_dir  = excluded.human_dir
    """

    with get_connection() as conn:
        for dirpath, _dirs, filenames in os.walk(root_dir):
            if cancel_event and cancel_event.is_set():
                break
            for fname in filenames:
                full = os.path.join(dirpath, fname)
                ext  = Path(fname).suffix.lower() or None
                try:
                    size = os.path.getsize(full)
                except OSError:
                    size = None
                nc_full = os.path.normcase(full)
                human_dir = None
                for nc_target, human_pfx in shortcut_map.items():
                    if nc_full.startswith(nc_target + os.sep):
                        # File lives inside a shortcut-target folder;
                        # reconstruct its human path by replacing the
                        # id-based prefix with the shortcut's name.
                        suffix = full[len(nc_target):]   # includes leading sep
                        human_dir = os.path.dirname(human_pfx + suffix)
                        break
                    elif nc_full == nc_target:
                        # File itself is the direct shortcut target
                        human_dir = os.path.dirname(human_pfx)
                        break
                batch.append((fname, ext, full, dirpath, size, now, human_dir))
                total += 1
                if len(batch) >= 500:
                    conn.executemany(upsert_sql, batch)
                    conn.commit()
                    batch.clear()
                if progress_callback:
                    progress_callback(total)

        if batch:
            conn.executemany(upsert_sql, batch)
            conn.commit()

    if done_callback:
        done_callback(total)


def search_files(query: str,
                 search_by: str = "filename",
                 extension_filter: str = "",
                 limit: int = 2000):
    """
    search_by: 'filename' | 'path' | 'extension'
    Returns a list of sqlite3.Row objects.
    """
    q = f"%{query}%"
    ext = extension_filter.strip().lstrip(".")

    if search_by == "filename":
        sql = "SELECT * FROM files WHERE filename LIKE ?"
        params = [q]
    elif search_by == "path":
        sql = "SELECT * FROM files WHERE full_path LIKE ?"
        params = [q]
    else:  # extension
        sql = "SELECT * FROM files WHERE extension LIKE ?"
        params = [f"%.{query.strip().lstrip('.')}%" if query else "%"]

    if ext:
        sql += " AND extension = ?"
        params.append(f".{ext}")

    sql += f" ORDER BY filename COLLATE NOCASE LIMIT {limit}"

    with get_connection() as conn:
        return conn.execute(sql, params).fetchall()


def get_stats():
    with get_connection() as conn:
        total   = conn.execute("SELECT COUNT(*) FROM files").fetchone()[0]
        indexed = conn.execute(
            "SELECT MAX(indexed_at) FROM files").fetchone()[0] or "never"
        exts    = conn.execute(
            "SELECT extension, COUNT(*) AS cnt FROM files "
            "GROUP BY extension ORDER BY cnt DESC LIMIT 10"
        ).fetchall()
    return total, indexed, exts


def clear_index():
    with get_connection() as conn:
        conn.execute("DELETE FROM files")
        conn.commit()


# ─── GUI ─────────────────────────────────────────────────────────────────────

DARK_BG   = "#1e1e2e"
PANEL_BG  = "#2a2a3e"
ACCENT    = "#7c6af7"
ACCENT2   = "#a78bfa"
FG        = "#e2e8f0"
FG_DIM    = "#94a3b8"
GREEN     = "#4ade80"
RED       = "#f87171"
YELLOW    = "#fbbf24"
FONT_BODY = ("Consolas", 10)
FONT_HEAD = ("Segoe UI", 11, "bold")
FONT_BIG  = ("Segoe UI", 18, "bold")


class FileIndexerApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("File Indexer")
        self.geometry("1100x720")
        self.minsize(800, 550)
        self.configure(bg=DARK_BG)
        self._cancel = threading.Event()

        init_db()
        self._build_ui()
        self._refresh_stats()

    # ── Layout ──────────────────────────────────────────────────────────────

    def _build_ui(self):
        # ── Header bar ──────────────────────────────────────────────────────
        hdr = tk.Frame(self, bg=PANEL_BG, pady=10)
        hdr.pack(fill="x")
        tk.Label(hdr, text="📂  File Indexer", font=FONT_BIG,
                 bg=PANEL_BG, fg=ACCENT2).pack(side="left", padx=20)
        self.stats_lbl = tk.Label(hdr, text="", font=("Segoe UI", 9),
                                  bg=PANEL_BG, fg=FG_DIM)
        self.stats_lbl.pack(side="right", padx=20)

        # ── Notebook (tabs) ──────────────────────────────────────────────────
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TNotebook",           background=DARK_BG, borderwidth=0)
        style.configure("TNotebook.Tab",       background=PANEL_BG, foreground=FG,
                        font=FONT_HEAD, padding=[14, 6])
        style.map("TNotebook.Tab",
                  background=[("selected", ACCENT)],
                  foreground=[("selected", "white")])
        style.configure("Treeview",            background=PANEL_BG, foreground=FG,
                        fieldbackground=PANEL_BG, rowheight=22, font=FONT_BODY)
        style.configure("Treeview.Heading",    background=ACCENT, foreground="white",
                        font=("Segoe UI", 9, "bold"))
        style.map("Treeview",
                  background=[("selected", ACCENT)],
                  foreground=[("selected", "white")])
        style.configure("TScrollbar", background=PANEL_BG, troughcolor=DARK_BG,
                        arrowcolor=FG_DIM)

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=12, pady=8)

        self._build_index_tab(nb)
        self._build_search_tab(nb)
        self._build_stats_tab(nb)

    # ── Index tab ───────────────────────────────────────────────────────────

    def _build_index_tab(self, nb):
        frame = tk.Frame(nb, bg=DARK_BG)
        nb.add(frame, text="  Index  ")

        # Directory picker row
        top = tk.Frame(frame, bg=DARK_BG, pady=14)
        top.pack(fill="x", padx=16)

        tk.Label(top, text="Directory to index:", font=FONT_HEAD,
                 bg=DARK_BG, fg=FG).pack(side="left")
        self.dir_var = tk.StringVar()
        tk.Entry(top, textvariable=self.dir_var, font=FONT_BODY,
                 bg=PANEL_BG, fg=FG, insertbackground=FG,
                 relief="flat", width=60).pack(side="left", padx=10, ipady=5)
        self._btn(top, "Browse…", self._browse_dir).pack(side="left", padx=4)
        self._btn(top, "▶  Start Indexing", self._start_index,
                  bg=ACCENT).pack(side="left", padx=4)
        self._btn(top, "■  Cancel", self._cancel_index,
                  bg="#374151").pack(side="left", padx=4)
        self._btn(top, "🗑  Clear DB", self._clear_db,
                  bg="#7f1d1d").pack(side="right", padx=4)

        # Progress bar
        prog_frame = tk.Frame(frame, bg=DARK_BG)
        prog_frame.pack(fill="x", padx=16)
        self.progress_lbl = tk.Label(prog_frame, text="Ready.",
                                     font=FONT_BODY, bg=DARK_BG, fg=FG_DIM)
        self.progress_lbl.pack(anchor="w")
        self.pbar = ttk.Progressbar(prog_frame, mode="indeterminate",
                                    length=400)
        self.pbar.pack(anchor="w", pady=4)

        # Log box
        tk.Label(frame, text="Index log:", font=FONT_HEAD,
                 bg=DARK_BG, fg=FG).pack(anchor="w", padx=16)
        log_frame = tk.Frame(frame, bg=DARK_BG)
        log_frame.pack(fill="both", expand=True, padx=16, pady=(0, 14))
        self.log_box = tk.Text(log_frame, font=FONT_BODY, bg=PANEL_BG, fg=FG,
                               insertbackground=FG, relief="flat",
                               state="disabled", wrap="none")
        vsb = ttk.Scrollbar(log_frame, command=self.log_box.yview)
        self.log_box["yscrollcommand"] = vsb.set
        vsb.pack(side="right", fill="y")
        self.log_box.pack(fill="both", expand=True)

    # ── Search tab ──────────────────────────────────────────────────────────

    def _build_search_tab(self, nb):
        frame = tk.Frame(nb, bg=DARK_BG)
        nb.add(frame, text="  Search  ")

        top = tk.Frame(frame, bg=DARK_BG, pady=14)
        top.pack(fill="x", padx=16)

        tk.Label(top, text="Search:", font=FONT_HEAD,
                 bg=DARK_BG, fg=FG).pack(side="left")
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self._do_search())
        search_entry = tk.Entry(top, textvariable=self.search_var,
                                font=FONT_BODY, bg=PANEL_BG, fg=FG,
                                insertbackground=FG, relief="flat", width=40)
        search_entry.pack(side="left", padx=10, ipady=5)
        search_entry.focus()

        tk.Label(top, text="Search by:", font=FONT_BODY,
                 bg=DARK_BG, fg=FG_DIM).pack(side="left", padx=(10, 2))
        self.search_by = tk.StringVar(value="filename")
        for val, lbl in [("filename", "Filename"), ("path", "Full Path"),
                          ("extension", "Extension")]:
            tk.Radiobutton(top, text=lbl, variable=self.search_by, value=val,
                           font=FONT_BODY, bg=DARK_BG, fg=FG,
                           selectcolor=ACCENT, activebackground=DARK_BG,
                           command=self._do_search).pack(side="left", padx=4)

        tk.Label(top, text="Ext filter:", font=FONT_BODY,
                 bg=DARK_BG, fg=FG_DIM).pack(side="left", padx=(14, 2))
        self.ext_var = tk.StringVar()
        self.ext_var.trace_add("write", lambda *_: self._do_search())
        tk.Entry(top, textvariable=self.ext_var, font=FONT_BODY,
                 bg=PANEL_BG, fg=FG, insertbackground=FG,
                 relief="flat", width=8).pack(side="left", padx=4, ipady=5)

        self.result_count_lbl = tk.Label(top, text="", font=FONT_BODY,
                                         bg=DARK_BG, fg=YELLOW)
        self.result_count_lbl.pack(side="right", padx=10)

        # Results tree
        cols = ("filename", "directory", "size")
        tree_frame = tk.Frame(frame, bg=DARK_BG)
        tree_frame.pack(fill="both", expand=True, padx=16, pady=(0, 4))

        self.tree = ttk.Treeview(tree_frame, columns=cols,
                                 show="headings", selectmode="browse")
        widths = {"filename": 260, "directory": 500, "size": 80}
        headers = {"filename": "Filename", "directory": "Directory", "size": "Size"}
        for c in cols:
            self.tree.heading(c, text=headers[c],
                              command=lambda _c=c: self._sort_tree(_c))
            self.tree.column(c, width=widths[c], minwidth=50)

        vsb = ttk.Scrollbar(tree_frame, command=self.tree.yview)
        hsb = ttk.Scrollbar(tree_frame, orient="horizontal",
                            command=self.tree.xview)
        self.tree["yscrollcommand"] = vsb.set
        self.tree["xscrollcommand"] = hsb.set
        vsb.pack(side="right", fill="y")
        hsb.pack(side="bottom", fill="x")
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<Button-3>", self._copy_menu)

        # Status bar
        self.status_lbl = tk.Label(frame,
                                   text="Right-click a row to copy directory or filename",
                                   font=("Segoe UI", 8), bg=DARK_BG, fg=FG_DIM)
        self.status_lbl.pack(anchor="w", padx=16, pady=(0, 6))

    # ── Stats tab ───────────────────────────────────────────────────────────

    def _build_stats_tab(self, nb):
        frame = tk.Frame(nb, bg=DARK_BG)
        nb.add(frame, text="  Stats  ")

        self.stats_text = tk.Text(frame, font=("Consolas", 11),
                                  bg=PANEL_BG, fg=FG, insertbackground=FG,
                                  relief="flat", state="disabled", wrap="none")
        self.stats_text.pack(fill="both", expand=True, padx=16, pady=16)

        self._btn(frame, "⟳  Refresh Stats", self._refresh_stats,
                  bg=ACCENT).pack(pady=(0, 16))

    # ── Helpers ─────────────────────────────────────────────────────────────

    def _btn(self, parent, text, cmd, bg=PANEL_BG):
        return tk.Button(parent, text=text, command=cmd,
                         font=("Segoe UI", 9, "bold"),
                         bg=bg, fg="white", activebackground=ACCENT2,
                         activeforeground="white", relief="flat",
                         cursor="hand2", padx=10, pady=5)

    def _log(self, msg: str):
        self.log_box["state"] = "normal"
        self.log_box.insert("end", msg + "\n")
        self.log_box.see("end")
        self.log_box["state"] = "disabled"

    def _human_size(self, n):
        if n is None:
            return ""
        for unit in ("B", "KB", "MB", "GB", "TB"):
            if n < 1024:
                return f"{n:.1f} {unit}"
            n /= 1024
        return f"{n:.1f} PB"

    # ── Actions ─────────────────────────────────────────────────────────────

    def _browse_dir(self):
        d = filedialog.askdirectory(title="Select directory to index")
        if d:
            self.dir_var.set(d)

    def _start_index(self):
        root = self.dir_var.get().strip()
        if not root or not os.path.isdir(root):
            messagebox.showerror("Error", "Please select a valid directory first.")
            return
        self._cancel.clear()
        self.pbar.start(12)
        self._log(f"[{datetime.now():%H:%M:%S}] Starting index of: {root}")
        self.progress_lbl["text"] = "Indexing…"

        def on_progress(n):
            self.after(0, lambda: self.progress_lbl.config(
                text=f"Files found so far: {n:,}"))

        def on_done(n):
            self.after(0, lambda: [
                self.pbar.stop(),
                self.progress_lbl.config(
                    text=f"Done — {n:,} files indexed.", fg=GREEN),
                self._log(f"[{datetime.now():%H:%M:%S}] Finished. "
                          f"{n:,} files indexed from: {root}"),
                self._refresh_stats(),
            ])

        def on_log(msg):
            self.after(0, lambda m=msg: self._log(m))

        threading.Thread(
            target=index_directory,
            args=(root,),
            kwargs=dict(progress_callback=on_progress,
                        done_callback=on_done,
                        cancel_event=self._cancel,
                        log_callback=on_log),
            daemon=True
        ).start()

    def _cancel_index(self):
        self._cancel.set()
        self.pbar.stop()
        self.progress_lbl["text"] = "Cancelled."
        self._log("Indexing cancelled by user.")

    def _clear_db(self):
        if messagebox.askyesno("Confirm", "Clear the entire file index?"):
            clear_index()
            self._log("Database cleared.")
            self._refresh_stats()

    def _do_search(self, *_):
        q   = self.search_var.get().strip()
        ext = self.ext_var.get().strip()
        by  = self.search_by.get()

        for row in self.tree.get_children():
            self.tree.delete(row)

        if not q and not ext:
            self.result_count_lbl["text"] = ""
            return

        rows = [r for r in search_files(q, search_by=by, extension_filter=ext)
                if r["extension"] != ".lnk"]
        for r in rows:
            is_shortcut = ".shortcut-targets-by-id" in r["full_path"]
            human_dir   = r["human_dir"]
            display_dir  = _strip_gdrive_root(human_dir) if human_dir else ("" if is_shortcut else r["directory"])
            display_path = (_strip_gdrive_root(os.path.join(human_dir, r["filename"]))
                            if human_dir else
                            ("" if is_shortcut else r["full_path"]))
            self.tree.insert("", "end", values=(
                r["filename"],
                display_dir,
                self._human_size(r["size_bytes"]),
            ))
        self.result_count_lbl["text"] = f"{len(rows):,} result(s)"

    def _sort_tree(self, col):
        data = [(self.tree.set(k, col), k) for k in self.tree.get_children("")]
        data.sort()
        for i, (_, k) in enumerate(data):
            self.tree.move(k, "", i)

    def _copy_menu(self, event):
        sel = self.tree.identify_row(event.y)
        if not sel:
            return
        self.tree.selection_set(sel)
        directory = self.tree.set(sel, "directory")
        filename  = self.tree.set(sel, "filename")
        menu = tk.Menu(self, tearoff=0, bg=PANEL_BG, fg=FG,
                       activebackground=ACCENT)
        menu.add_command(label="Copy directory",
                         command=lambda: self._copy_to_clipboard(directory))
        menu.add_command(label="Copy filename",
                         command=lambda: self._copy_to_clipboard(filename))
        menu.tk_popup(event.x_root, event.y_root)

    def _copy_to_clipboard(self, text: str):
        self.clipboard_clear()
        self.clipboard_append(text)
        self.status_lbl["text"] = f"Copied: {text[:80]}"

    def _refresh_stats(self):
        total, last, exts = get_stats()
        self.stats_lbl["text"] = (
            f"DB: {DB_PATH.name}  |  {total:,} files indexed  |  Last run: {last}"
        )
        lines = [
            f"  Database location : {DB_PATH}",
            f"  Total files indexed: {total:,}",
            f"  Last indexed at    : {last}",
            "",
            "  Top 10 extensions by file count:",
            "  " + "-" * 38,
        ]
        for row in exts:
            ext = row["extension"] or "(none)"
            cnt = row["cnt"]
            bar = "█" * min(int(cnt / max(total, 1) * 200), 30)
            lines.append(f"  {ext:<12}  {cnt:>8,}   {bar}")

        txt = "\n".join(lines)
        self.stats_text["state"] = "normal"
        self.stats_text.delete("1.0", "end")
        self.stats_text.insert("end", txt)
        self.stats_text["state"] = "disabled"


# ─── Entry point ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = FileIndexerApp()
    app.mainloop()
