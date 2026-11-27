#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Author-time deterministic rendering of the canonical InfiltratorFS guide."""
import argparse
from pathlib import Path

def render(source, destination):
    raw = source.read_text(encoding="utf-8")
    for chars, replacement in [("–—", "-"), ("‘’", "'"), ("“”", '"'), ("→", "->"), ("×", "x"), ("≤", "<="), ("≥", ">="), ("\u00a0", " ")]:
        for char in chars:
            raw = raw.replace(char, replacement)
    raw.encode("ascii")
    def line(s, y, font="F3", size=10):
        s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        return f"0.08 0.12 0.17 rg BT /{font} {size} Tf 43 {y} Td ({s}) Tj ET\n"
    head = "0.07 0.13 0.20 rg 0 785 595 57 re f\n1 1 1 rg BT /F2 15 Tf 43 810 Td (InfiltratorFS | ENGINEERING AND USER MANUAL) Tj ET\n"
    def foot(n):
        return f"0.35 0.39 0.43 rg BT /F1 9 Tf 43 35 Td (Infiltrator Projects | {n}) Tj ET\n"
    def wrap(s, width):
        cur = ""
        for word in s.split():
            while len(word) > width:
                if cur:
                    yield cur
                    cur = ""
                yield word[:width]
                word = word[width:]
            if not word:
                continue
            if len(cur) + len(word) + 1 > width:
                yield cur
                cur = ""
            cur += (" " if cur else "") + word
        if cur:
            yield cur
    pages, toc, body, y = [], [], head, 755
    def flush():
        nonlocal body, y
        pages.append(body + foot(len(pages) + 2))
        body, y = head, 755
    for ln in raw.split("\n")[1:]:
        if not ln.strip():
            if y < 80:
                flush()
            else:
                y -= 7
            continue
        if ln.startswith("## "):
            if y < 115:
                flush()
            toc.append((ln[3:], len(pages) + 2))
            body += line(ln[3:], y, "F2", 13)
            y -= 23
            continue
        for s in wrap(ln, 82):
            if y < 64:
                flush()
            body += line(s, y)
            y -= 13
    if body != head:
        flush()
    pages.insert(0, head + line("Contents", 755, "F2", 17) + "".join(line(f"{s}  {n}", 730-i*26) for i, (s,n) in enumerate(toc)) + foot(1))
    objects = ["<< /Type /Catalog /Pages 2 0 R >>",
               f"<< /Type /Pages /Count {len(pages)} /Kids [" + " ".join(f"{6+i*2} 0 R" for i in range(len(pages))) + "] >>",
               "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
               "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
               "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>"]
    for i, s in enumerate(pages):
        objects.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> /Contents {7+i*2} 0 R >>")
        objects.append(f"<< /Length {len(s)} >>\nstream\n{s}endstream")
    pdf, offsets = "%PDF-1.4\n", [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf += f"{i} 0 obj\n{obj}\nendobj\n"
    xref = len(pdf)
    pdf += f"xref\n0 {len(offsets)}\n0000000000 65535 f \n" + "".join(f"{v:010d} 00000 n \n" for v in offsets[1:]) + f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(pdf.encode("ascii"))
    destination.with_suffix(".pdfstreams").write_bytes("%%Page\n".join(pages).encode("ascii"))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    render(args.source, args.destination)
