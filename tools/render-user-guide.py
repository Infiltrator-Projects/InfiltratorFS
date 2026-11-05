#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render a project user guide from its maintained Markdown source.

Requires reportlab and DejaVu Sans fonts for guide authors. Published packages install
the checked-in PDF and do not require reportlab at runtime or package build.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.platypus import (BaseDocTemplate, Frame, HRFlowable,
                               PageBreak, PageTemplate, Paragraph,
                               Preformatted, Spacer, Table, TableStyle)

FONT_DIR = Path('/usr/share/fonts/truetype/dejavu')
pdfmetrics.registerFont(TTFont('GuideSans', str(FONT_DIR / 'DejaVuSans.ttf')))
pdfmetrics.registerFont(TTFont('GuideSans-Bold', str(FONT_DIR / 'DejaVuSans-Bold.ttf')))
pdfmetrics.registerFont(TTFont('GuideMono', str(FONT_DIR / 'DejaVuSansMono.ttf')))
pdfmetrics.registerFontFamily('GuideSans', normal='GuideSans', bold='GuideSans-Bold')

NAVY = colors.HexColor('#142235')
TEAL = colors.HexColor('#007E87')
INK = colors.HexColor('#203040')
GREY = colors.HexColor('#5C6B77')
PALE = colors.HexColor('#EDF5F5')
WIDTH, HEIGHT = A4
MARGIN = 20 * mm
BODY_WIDTH = WIDTH - MARGIN * 2
STYLES = {
    'kicker': ParagraphStyle('kicker', fontName='GuideSans-Bold', fontSize=9,
                             leading=13, textColor=TEAL, spaceAfter=8),
    'cover_title': ParagraphStyle('cover-title', fontName='GuideSans-Bold',
                                  fontSize=32, leading=38, textColor=NAVY,
                                  spaceAfter=10),
    'cover_subtitle': ParagraphStyle('cover-subtitle', fontName='GuideSans',
                                     fontSize=14, leading=20, textColor=GREY,
                                     spaceAfter=18),
    'heading': ParagraphStyle('heading', fontName='GuideSans-Bold', fontSize=16,
                              leading=21, textColor=NAVY, spaceBefore=15,
                              spaceAfter=8, keepWithNext=True),
    'body': ParagraphStyle('body', fontName='GuideSans', fontSize=10.3,
                           leading=15.4, textColor=INK, spaceAfter=8),
    'bullet': ParagraphStyle('bullet', fontName='GuideSans', fontSize=10.1,
                             leading=15, textColor=INK, leftIndent=15,
                             firstLineIndent=-10, spaceAfter=5),
    'small': ParagraphStyle('small', fontName='GuideSans', fontSize=9,
                            leading=13, textColor=GREY, spaceAfter=7),
    'code': ParagraphStyle('code', fontName='GuideMono', fontSize=8.1,
                           leading=11.5, textColor=NAVY, leftIndent=10,
                           spaceAfter=8),
    'card': ParagraphStyle('card', fontName='GuideSans-Bold', fontSize=9,
                           leading=12, textColor=NAVY, alignment=TA_CENTER),
}


def rich(value: str) -> str:
    value = escape(value)
    value = re.sub(r'`([^`]+)`', r'<font name="GuideMono">\1</font>', value)
    value = re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', value)
    return value


class GuideCanvas(canvas.Canvas):
    def __init__(self, *args, **kwargs):
        kwargs['invariant'] = 1
        super().__init__(*args, **kwargs)


class Guide(BaseDocTemplate):
    def __init__(self, destination: Path, title: str):
        self.guide_title = title
        super().__init__(str(destination), pagesize=A4, title=title + ' User Guide',
                         author='Shannon Smith', leftMargin=MARGIN,
                         rightMargin=MARGIN, topMargin=25 * mm,
                         bottomMargin=22 * mm)
        frame = Frame(MARGIN, 22 * mm, BODY_WIDTH, HEIGHT - 47 * mm,
                      leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
        self.addPageTemplates(PageTemplate(id='guide', frames=frame, onPage=self.decorate))

    def decorate(self, page, doc):
        page.saveState()
        if doc.page > 1:
            page.setFillColor(NAVY)
            page.rect(0, HEIGHT - 17 * mm, WIDTH, 17 * mm, fill=1, stroke=0)
            page.setFont('GuideSans-Bold', 9)
            page.setFillColor(colors.white)
            page.drawString(MARGIN, HEIGHT - 11 * mm, self.guide_title.upper() + '  /  USER GUIDE')
            page.setFillColor(TEAL)
            page.rect(MARGIN, HEIGHT - 17 * mm, 28 * mm, 2.5, fill=1, stroke=0)
        page.setStrokeColor(colors.HexColor('#D2DBDE'))
        page.line(MARGIN, 15 * mm, WIDTH - MARGIN, 15 * mm)
        page.setFont('GuideSans', 8)
        page.setFillColor(GREY)
        page.drawString(MARGIN, 10 * mm, 'Infiltrator Projects  |  User guide')
        page.drawRightString(WIDTH - MARGIN, 10 * mm, str(doc.page))
        page.restoreState()


def panel(title: str):
    if title == 'Calendar':
        labels = ['PANEL CLOCK', 'CALENDAR VIEW', 'EVENTS']
        explanation = 'One applet, with independently installed calendar and clock resources.'
    else:
        labels = ['MANAGER + TOOLS', 'PORTABLE FORMAT', 'LINUX DRIVER']
        explanation = 'A portable filesystem connected to the host through native adapters.'
    row = [Paragraph(escape(label), STYLES['card']) for label in labels]
    table = Table([row], colWidths=[BODY_WIDTH / 3] * 3, rowHeights=[43])
    table.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, -1), PALE),
        ('BOX', (0, 0), (-1, -1), .6, colors.HexColor('#CCDDDF')),
        ('INNERGRID', (0, 0), (-1, -1), .6, colors.HexColor('#CCDDDF')),
        ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
        ('LEFTPADDING', (0, 0), (-1, -1), 5),
        ('RIGHTPADDING', (0, 0), (-1, -1), 5),
    ]))
    return [table, Spacer(1, 3 * mm), Paragraph(explanation, STYLES['small'])]


def render(source: Path, destination: Path):
    lines = source.read_text(encoding='utf-8').splitlines()
    if not lines or not lines[0].startswith('# '):
        raise ValueError('Guide must start with a title')
    title = lines[0][2:].strip()
    if title not in ('Calendar', 'InfiltratorFS'):
        raise ValueError('Unexpected guide title')
    destination.parent.mkdir(parents=True, exist_ok=True)
    story = [Spacer(1, 20 * mm), Paragraph('INFILTRATOR PROJECTS', STYLES['kicker']),
             Paragraph(escape(title.upper()), STYLES['cover_title']),
             Paragraph('User guide and operating overview', STYLES['cover_subtitle']),
             HRFlowable(width='100%', thickness=2, color=TEAL), Spacer(1, 18 * mm)]
    story.extend(panel(title))
    story.extend([Spacer(1, 18 * mm),
                  Paragraph('Practical information for using the released application and its system components.', STYLES['body']),
                  Spacer(1, 24 * mm),
                  Paragraph('Copyright 1993-2026 Shannon Smith', STYLES['small']),
                  PageBreak()])
    code = []
    def flush_code():
        if code:
            story.append(Preformatted('\n'.join(code), STYLES['code'], maxLineLength=94))
            code.clear()
    for line in lines[1:]:
        if line.startswith('    '):
            code.append(line[4:])
            continue
        flush_code()
        line = line.strip()
        if not line:
            continue
        if line.startswith('## '):
            story.append(Paragraph(rich(line[3:]), STYLES['heading']))
        elif line.startswith('- '):
            story.append(Paragraph('&bull; ' + rich(line[2:]), STYLES['bullet']))
        elif re.match(r'^\d+\. ', line):
            story.append(Paragraph(rich(line), STYLES['bullet']))
        else:
            story.append(Paragraph(rich(line), STYLES['body']))
    flush_code()
    Guide(destination, title).build(story, canvasmaker=GuideCanvas)
    if not destination.read_bytes().startswith(b'%PDF-'):
        raise RuntimeError('Guide rendering did not produce a PDF')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    render(args.source, args.destination)
