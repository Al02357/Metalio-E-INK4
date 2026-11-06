#!/usr/bin/python3
"""用系统 Python 的 GTK 把文字放进剪贴板，并守着它一会儿（X11 剪贴板要有进程在才能粘贴）。
打印 ready 后由调用方发粘贴键。用法：clip_helper.py <秒数>，文字从 stdin 读。"""
import sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk, GLib

text = sys.stdin.read()
hold = float(sys.argv[1]) if len(sys.argv) > 1 else 30
cb = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
cb.set_text(text, -1)
cb.store()
owned = [True]


def on_change(*_):
    # 别人复制了新东西：我们就不用再守了
    if cb.wait_for_text() != text:
        Gtk.main_quit()


GLib.timeout_add(150, lambda: (print("ready", flush=True), False)[1])
GLib.timeout_add(int(hold * 1000), Gtk.main_quit)
cb.connect("owner-change", on_change)
Gtk.main()
