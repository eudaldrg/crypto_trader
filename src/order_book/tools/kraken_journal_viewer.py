#!/usr/bin/env python3
"""Step-through visualizer for a Kraken level3 journal, driven entirely by
frames dumped by the real C++ order book (kraken_journal_dump) -- this
script does no order-book logic of its own, just renders what the real
book already computed. That split exists specifically so the picture can
never drift from what the real book does (see decisions/0006, and
exchanges/kraken.md for two real bugs that only ever showed up against a
real captured session -- exactly the kind of thing a from-scratch Python
reimplementation here could have silently gotten wrong all over again).

Usage:
    cmake --build build/debug --target kraken_journal_dump
    ./build/debug/bin/kraken_journal_dump \\
        build/debug/order_book_test_data/kraken_l3_capture.journal /tmp/frames.json
    python3 src/order_book/tools/kraken_journal_viewer.py /tmp/frames.json

The checked-in capture is unpacked into the build tree by CMake (see
CMakeLists.txt); a journal captured with different instrument settings or
subscription depth needs the dump tool's --price-decimals/--quantity-decimals/
--depth flags, since the book cannot infer them from the journal.

Controls: F5 steps to the next message; Shift+F5 steps back.
"""
import json
import sys
import tkinter as tk
from tkinter import ttk


class Viewer:
    def __init__(self, root: tk.Tk, frames: list[dict]) -> None:
        self.frames = frames
        self.index = 0

        self.status = ttk.Label(root, font=("TkDefaultFont", 12, "bold"), anchor="center")
        self.status.pack(fill="x", padx=8, pady=(8, 4))

        columns_frame = ttk.Frame(root)
        columns_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        bid_frame = ttk.LabelFrame(columns_frame, text="Bids (best first)")
        bid_frame.pack(side="left", fill="both", expand=True, padx=(0, 4))
        self.bid_tree = self._make_tree(bid_frame)

        ask_frame = ttk.LabelFrame(columns_frame, text="Asks (best first)")
        ask_frame.pack(side="left", fill="both", expand=True, padx=(4, 0))
        self.ask_tree = self._make_tree(ask_frame)

        hint = ttk.Label(
            root,
            text="F5: next message    Shift+F5: previous message",
            anchor="center",
            foreground="#666666",
        )
        hint.pack(fill="x", padx=8, pady=(0, 8))

        # bind_all, not bind: the Treeview widgets take focus for their own
        # (unrelated) navigation, and a plain root.bind() only fires when
        # root itself has focus.
        root.bind_all("<F5>", self._next)
        root.bind_all("<Shift-F5>", self._prev)

        self._render()

    @staticmethod
    def _make_tree(parent: ttk.LabelFrame) -> ttk.Treeview:
        tree = ttk.Treeview(parent, columns=("qty",), show="tree headings")
        tree.heading("#0", text="price / order id")
        tree.heading("qty", text="quantity")
        tree.column("#0", width=220)
        tree.column("qty", width=140, anchor="e")
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=scrollbar.set)
        tree.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        return tree

    def _next(self, _event: tk.Event | None = None) -> None:
        if self.index < len(self.frames) - 1:
            self.index += 1
            self._render()

    def _prev(self, _event: tk.Event | None = None) -> None:
        if self.index > 0:
            self.index -= 1
            self._render()

    def _fill_side(self, tree: ttk.Treeview, levels: list[dict]) -> None:
        tree.delete(*tree.get_children())
        for level in levels:
            level_id = tree.insert(
                "", "end", text=f"{level['price']:.1f}", values=(f"{level['total_qty']:.8f}",), open=True
            )
            for order in level["orders"]:
                tree.insert(level_id, "end", text=order["id"], values=(f"{order['qty']:.8f}",))

    def _render(self) -> None:
        frame = self.frames[self.index]
        checksum_text = "checksum OK" if frame["checksum_ok"] else "CHECKSUM MISMATCH"
        self.status.configure(
            text=(
                f"message {self.index + 1} / {len(self.frames)}    "
                f"type={frame['type']}    {checksum_text} ({frame['checksum']})"
            ),
            foreground="black" if frame["checksum_ok"] else "red",
        )
        self._fill_side(self.bid_tree, frame["bids"])
        self._fill_side(self.ask_tree, frame["asks"])


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <frames.json>", file=sys.stderr)
        raise SystemExit(1)

    with open(sys.argv[1]) as handle:
        frames = json.load(handle)
    if not frames:
        print("no frames in file", file=sys.stderr)
        raise SystemExit(1)

    root = tk.Tk()
    root.title("Kraken level3 order book viewer")
    root.geometry("900x600")
    Viewer(root, frames)
    root.mainloop()


if __name__ == "__main__":
    main()
