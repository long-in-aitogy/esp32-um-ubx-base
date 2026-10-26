"""Render the four Markdown catalogues to PDF. Requires reportlab."""
from pathlib import Path
import argparse
import html
import re

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate, Frame, PageTemplate, Paragraph, Spacer,
    LongTable, TableStyle, Image, KeepTogether,
)

ROOT = Path(__file__).resolve().parent
INK = colors.HexColor("#162D42")
ACCENT = colors.HexColor("#087E8B")
MUTED = colors.HexColor("#536678")
WIDTH = A4[0] - 36 * mm


def fonts(directory=None):
    if directory:
        base = Path(directory)
        paths = [base / name for name in (
            "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSansMono.ttf")]
    elif Path("/System/Library/Fonts/Supplemental/Arial.ttf").exists():
        base = Path("/System/Library/Fonts/Supplemental")
        paths = [base / "Arial.ttf", base / "Arial Bold.ttf", base / "Arial Italic.ttf",
                 Path("/System/Library/Fonts/Menlo.ttc")]
    else:
        base = Path("/usr/share/fonts/truetype/dejavu")
        paths = [base / name for name in (
            "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSansMono.ttf")]
    for name, path in zip(("Body", "BodyBold", "BodyItalic", "Mono"), paths):
        if not path.is_file():
            raise SystemExit(f"Missing font: {path}. Supply --font-dir with DejaVu fonts.")
        pdfmetrics.registerFont(TTFont(name, str(path)))
    pdfmetrics.registerFontFamily("Body", normal="Body", bold="BodyBold", italic="BodyItalic", boldItalic="BodyBold")


def inline(text):
    text = html.escape(text)
    text = re.sub(r"`([^`]+)`", r'<font name="Mono" size="8">\1</font>', text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    return re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2" color="#087E8B">\1</a>', text)


def styles():
    body = ParagraphStyle("body", fontName="Body", fontSize=9.5, leading=14,
                          textColor=INK, spaceAfter=7, splitLongWords=True,
                          allowWidows=False, allowOrphans=False)
    return {
        "body": body,
        "h1": ParagraphStyle("h1", parent=body, fontName="BodyBold", fontSize=21,
                             leading=27, spaceBefore=12, spaceAfter=15, keepWithNext=True),
        "h2": ParagraphStyle("h2", parent=body, fontName="BodyBold", fontSize=14,
                             leading=19, textColor=ACCENT, spaceBefore=16, spaceAfter=9, keepWithNext=True),
        "h3": ParagraphStyle("h3", parent=body, fontName="BodyBold", fontSize=11,
                             leading=16, spaceBefore=11, spaceAfter=7, keepWithNext=True),
        "cell": ParagraphStyle("cell", parent=body, fontSize=8.5, leading=12, spaceAfter=0),
        "head": ParagraphStyle("head", parent=body, fontName="BodyBold", fontSize=8.5,
                               leading=12, textColor=colors.white, spaceAfter=0),
        "code": ParagraphStyle("code", parent=body, fontName="Mono", fontSize=8,
                               leading=12, backColor=colors.HexColor("#F1F5F8"),
                               borderPadding=8, spaceBefore=3, spaceAfter=9),
        "caption": ParagraphStyle("caption", parent=body, fontSize=8, leading=12, textColor=MUTED),
        "list": ParagraphStyle("list", parent=body, leftIndent=13, firstLineIndent=0,
                               bulletIndent=0, spaceAfter=5),
    }


class Catalogue(BaseDocTemplate):
    def afterFlowable(self, flowable):
        if isinstance(flowable, Paragraph) and flowable.style.name in ("h1", "h2", "h3"):
            level = {"h1": 0, "h2": 1, "h3": 2}[flowable.style.name]
            self.bookmark_count += 1
            key = f"section-{self.bookmark_count}"
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(flowable.getPlainText(), key, level=level, closed=False)


def render(source, output):
    sheet = styles()
    lines = source.read_text().splitlines()
    story = []
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            i += 1
            continue
        if line.startswith("```"):
            block = []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                block.append(html.escape(lines[i]).replace(" ", "&#160;"))
                i += 1
            story.append(KeepTogether([Paragraph("<br/>".join(block) or "&#160;", sheet["code"])]))
            i += 1
            continue
        if line.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                cells = [cell.strip() for cell in lines[i].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-+:?", cell.replace(" ", "")) for cell in cells):
                    style = sheet["head"] if not rows else sheet["cell"]
                    rows.append([Paragraph(inline(cell), style) for cell in cells])
                i += 1
            count = len(rows[0])
            ratios = [0.43, 0.57] if count == 2 else [1 / count] * count
            table = LongTable(rows, colWidths=[WIDTH * ratio for ratio in ratios],
                              repeatRows=1, hAlign="LEFT", spaceAfter=10)
            table.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, 0), INK),
                ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F1F5F8")]),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 7),
                ("RIGHTPADDING", (0, 0), (-1, -1), 7),
                ("TOPPADDING", (0, 0), (-1, -1), 6),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
                ("LINEBELOW", (0, 0), (-1, 0), 0.5, ACCENT),
            ]))
            story.append(table)
            continue
        match = re.fullmatch(r"!\[([^\]]+)\]\(([^)]+)\)", line)
        if match:
            picture = Image(str(source.parent / match[2]))
            factor = min(WIDTH / picture.imageWidth, 105 * mm / picture.imageHeight)
            picture.drawWidth = picture.imageWidth * factor
            picture.drawHeight = picture.imageHeight * factor
            story.append(KeepTogether([picture, Spacer(1, 4), Paragraph(inline(match[1]), sheet["caption"]), Spacer(1, 8)]))
            i += 1
            continue
        match = re.match(r"^(#{1,3}) (.+)$", line)
        if match:
            story.append(Paragraph(inline(match[2]), sheet[f"h{len(match[1])}"]))
            i += 1
            continue
        match = re.match(r"^(\d+\.|-) (.+)$", line)
        if match:
            story.append(Paragraph(inline(match[2]), sheet["list"], bulletText=match[1]))
            i += 1
            continue
        paragraph = [line]
        i += 1
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#|\||```|!\[|\d+\. |\- )", lines[i]):
            paragraph.append(lines[i].strip())
            i += 1
        story.append(Paragraph(inline(" ".join(paragraph)), sheet["body"]))

    title = lines[0].removeprefix("# ")
    label = "NỘI BỘ" if "inside" in source.stem else "KHÁCH HÀNG"

    def page(canvas, doc):
        canvas.saveState()
        canvas.setStrokeColor(ACCENT)
        canvas.setLineWidth(2)
        canvas.line(18 * mm, A4[1] - 15 * mm, A4[0] - 18 * mm, A4[1] - 15 * mm)
        canvas.setFont("Body", 8)
        canvas.setFillColor(MUTED)
        canvas.drawString(18 * mm, 13 * mm, f"AITOGY | {label} | V2.3.5 | 25/10/2026")
        canvas.drawRightString(A4[0] - 18 * mm, 13 * mm, f"Trang {doc.page}")
        canvas.restoreState()

    doc = Catalogue(str(output), pagesize=A4, title=title, author="AITOGY",
                    subject="ESP32 GNSS base station operation and integration",
                    leftMargin=18 * mm, rightMargin=18 * mm, topMargin=22 * mm,
                    bottomMargin=22 * mm)
    doc.bookmark_count = 0
    doc.addPageTemplates(PageTemplate(id="catalogue", frames=[Frame(
        doc.leftMargin, doc.bottomMargin, doc.width, doc.height,
        leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)], onPage=page))
    doc.build(story)
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font-dir", help="Directory containing the four DejaVu TTF fonts")
    parser.add_argument("--output-dir", type=Path, default=ROOT)
    args = parser.parse_args()
    fonts(args.font_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for source in sorted(ROOT.glob("Catalogue-*.md")):
        render(source, args.output_dir / source.with_suffix(".pdf").name)
