#!/usr/bin/env python3
"""Visual side-by-side screenshot comparator (tkinter GUI).

Usage:
    python3 tools/compare_screenshots.py DIR_A DIR_B
        [--label-a NAME] [--label-b NAME]
        [--first N]

Keys:
    Left/Right or A/D  — prev/next frame
    Home/End           — first/last frame
    number + Enter     — jump to frame number
"""

import sys, tkinter as tk
from pathlib import Path
from tkinter import font as tkfont

def load_png_tk(path: Path) -> tk.PhotoImage:
    return tk.PhotoImage(file=str(path))

def main():
    args = sys.argv[1:]
    first = 0; label_a = ""; label_b = ""
    positional = []; i = 0
    while i < len(args):
        if args[i] == "--first" and i+1 < len(args):
            first = int(args[i+1]); i += 2
        elif args[i] == "--label-a" and i+1 < len(args):
            label_a = args[i+1]; i += 2
        elif args[i] == "--label-b" and i+1 < len(args):
            label_b = args[i+1]; i += 2
        else:
            positional.append(args[i]); i += 1

    if len(positional) < 2:
        print(__doc__); sys.exit(1)

    dir_a = Path(positional[0])
    dir_b = Path(positional[1])
    if not label_a: label_a = dir_a.name
    if not label_b: label_b = dir_b.name

    files_a = {f.name for f in dir_a.glob("frame_*.png")}
    files_b = {f.name for f in dir_b.glob("frame_*.png")}
    frames = sorted(files_a & files_b)
    if first: frames = frames[:first]
    if not frames:
        print("No matching frames found."); sys.exit(1)

    # ── GUI ──────────────────────────────────────────────────────────────────
    root = tk.Tk()
    root.title("Frame Comparator")
    root.configure(bg="#1a1a1a")

    mono = tkfont.Font(family="monospace", size=10)

    # Top bar
    top = tk.Frame(root, bg="#222")
    top.pack(fill="x")
    status_var = tk.StringVar()
    tk.Label(top, textvariable=status_var, bg="#222", fg="#ccc",
             font=mono, anchor="w", padx=8).pack(side="left")
    jump_var = tk.StringVar()
    tk.Label(top, text="Go:", bg="#222", fg="#888", font=mono).pack(side="right", padx=(0,4))
    jump_entry = tk.Entry(top, textvariable=jump_var, width=6, bg="#333", fg="#ccc",
                          insertbackground="#ccc", font=mono)
    jump_entry.pack(side="right", padx=(0,8), pady=3)

    # Column headers
    hdr = tk.Frame(root, bg="#2a2a2a")
    hdr.pack(fill="x")
    tk.Label(hdr, text=label_a, bg="#2a2a2a", fg="#8f8", font=mono,
             width=32, anchor="center").pack(side="left", expand=True, fill="x")
    tk.Label(hdr, text=label_b, bg="#2a2a2a", fg="#f88", font=mono,
             width=32, anchor="center").pack(side="left", expand=True, fill="x")

    # Image area
    img_frame = tk.Frame(root, bg="#1a1a1a")
    img_frame.pack(pady=4)
    lbl_a = tk.Label(img_frame, bg="#1a1a1a", bd=2, relief="flat")
    lbl_a.pack(side="left", padx=4)
    lbl_b = tk.Label(img_frame, bg="#1a1a1a", bd=2, relief="flat")
    lbl_b.pack(side="left", padx=4)

    idx = [0]
    imgs = [None, None]  # keep references

    def show(n):
        idx[0] = max(0, min(n, len(frames)-1))
        name = frames[idx[0]]
        frame_num = name.replace("frame_","").replace(".png","").lstrip("0") or "0"

        pa = dir_a / name
        pb = dir_b / name
        imgs[0] = load_png_tk(pa) if pa.exists() else None
        imgs[1] = load_png_tk(pb) if pb.exists() else None

        lbl_a.config(image=imgs[0] or "", relief="flat")
        lbl_b.config(image=imgs[1] or "", relief="flat")

        # Highlight border if mismatch (different file sizes as quick check)
        try:
            diff = pa.stat().st_size != pb.stat().st_size
        except Exception:
            diff = False
        c = "#f44" if diff else "#1a1a1a"
        lbl_a.config(bg=c if diff else "#1a1a1a")
        lbl_b.config(bg=c if diff else "#1a1a1a")

        status_var.set(f"Frame {frame_num}  [{idx[0]+1}/{len(frames)}]  {name}")
        jump_var.set(frame_num)

    def goto_frame(num_str):
        try:
            n = int(num_str)
            # find by frame number
            target = f"frame_{int(n):06d}.png"
            if target in (files_a & files_b):
                show(frames.index(target))
        except Exception:
            pass

    root.bind("<Right>",     lambda e: show(idx[0]+1))
    root.bind("<Left>",      lambda e: show(idx[0]-1))
    root.bind("d",           lambda e: show(idx[0]+1))
    root.bind("a",           lambda e: show(idx[0]-1))
    root.bind("<Home>",      lambda e: show(0))
    root.bind("<End>",       lambda e: show(len(frames)-1))
    root.bind("<Return>",    lambda e: goto_frame(jump_var.get()))
    jump_entry.bind("<Return>", lambda e: goto_frame(jump_var.get()))

    show(0)
    root.mainloop()

if __name__ == "__main__":
    main()
