<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18.61

0.18.61 fixes the Linux GTK3 manager headerbar contrast regression visible in
the System/Night theme. The Common palette itself was correct; the local GTK CSS
cascade was not.

The manager previously assigned the normal text colour to the universal `*`
selector. GTK renders button text and symbolic icons as child CSS nodes, so
those children received that explicit colour instead of inheriting each
button's foreground. In the night palette this produced pale labels/icons on
the deliberately pale button surface, including the theme selector and window
controls.

The fix removes the universal foreground assignment, explicitly carries each
button state's foreground to its label/icon children, keeps header icons fully
opaque, and strengthens the title/subtitle typography. A regression policy now
rejects reintroducing a foreground on the universal GTK selector.

No filesystem semantics or on-disk structures change. The on-disk format
remains 0.18.
