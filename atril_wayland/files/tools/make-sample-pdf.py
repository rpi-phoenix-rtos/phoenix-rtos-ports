#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
#
# make-sample-pdf.py OUT.pdf | --points -- the test document for Atril on Phoenix-RTOS
# (/usr/share/doc/phoenix/sample.pdf), drawn with cairo's PDF surface on the build host.
# Everything in it is generated here: no third-party content (the file is ours, BSD-3).
#
# 3 A4 pages, each with a big page title and a footer "Page N of 3":
#   1  text (DejaVu Sans / Serif / Mono, embedded as subsets by cairo, so the viewer needs
#      no font of its own) and filled vector shapes: a red rectangle, a green circle, a blue
#      triangle, an orange Bezier stroke;
#   2  a raster image: a 256x256 RGB colour field (red grows left->right, green top->bottom,
#      blue constant), embedded as an image, and a black/white checkerboard of 8x8 cells;
#   3  a line grid (a 10x10 table with numbers) and a paragraph.
#
# The host test renders the pages and checks the colour at the points of SHAPES (--points
# prints them); keep them in sync with the drawing. Deterministic: the creation date is fixed, so the same cairo gives the same bytes.
import math
import sys

import cairo

W, H = 595.0, 842.0        # A4 in PDF points
PAGES = 3

# name: (page, x, y, expected RGB at that point in PDF points, from the top-left)
SHAPES = {
    'red-rectangle':   (1, 110.0, 400.0, (0xd0, 0x20, 0x20)),
    'green-circle':    (1, 300.0, 400.0, (0x20, 0xa0, 0x40)),
    'blue-triangle':   (1, 480.0, 420.0, (0x20, 0x40, 0xc0)),
    'image-top-left':  (2, 150.0, 210.0, (0x0a, 0x0a, 0x80)),
    'image-bot-right': (2, 390.0, 450.0, (0xfa, 0xfa, 0x80)),
    'page3-cell':      (3, 97.5, 222.0, (0xe8, 0xf0, 0xff)),
}


def title(cr, text):
    cr.set_source_rgb(0.1, 0.1, 0.1)
    cr.select_font_face('DejaVu Sans', cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
    cr.set_font_size(26)
    cr.move_to(60, 90)
    cr.show_text(text)


def footer(cr, n):
    cr.set_source_rgb(0.3, 0.3, 0.3)
    cr.select_font_face('DejaVu Sans', cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
    cr.set_font_size(11)
    t = 'Page %d of %d  -  Atril on Phoenix-RTOS sample document' % (n, PAGES)
    cr.move_to(60, H - 40)
    cr.show_text(t)
    cr.set_line_width(0.8)
    cr.move_to(60, H - 55)
    cr.line_to(W - 60, H - 55)
    cr.stroke()


def lines(cr, x, y, size, face, texts, lead=1.45):
    cr.select_font_face(face, cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
    cr.set_font_size(size)
    for t in texts:
        cr.move_to(x, y)
        cr.show_text(t)
        y += size * lead
    return y


def page1(cr):
    title(cr, 'Atril on Phoenix-RTOS')
    cr.set_source_rgb(0, 0, 0)
    y = lines(cr, 60, 140, 13, 'DejaVu Sans', [
        'This PDF was drawn by cairo on the build host and is rendered on the',
        'Raspberry Pi 4 by Poppler 26.09 inside Atril 1.28, a GTK 3 program on',
        'Wayland (labwc), on Phoenix-RTOS.',
    ])
    cr.set_source_rgb(0.15, 0.15, 0.45)
    y = lines(cr, 60, y + 10, 13, 'DejaVu Serif', ['Serif: The quick brown fox jumps over the lazy dog.'])
    cr.set_source_rgb(0.2, 0.2, 0.2)
    lines(cr, 60, y, 12, 'DejaVu Sans Mono', ['Mono:  0123456789 {}[]<>=+-*/ ~#%&'])
    # shapes: a red rectangle, a green circle, a blue triangle, an orange Bezier
    cr.set_source_rgb(0xd0 / 255, 0x20 / 255, 0x20 / 255)
    cr.rectangle(60, 340, 110, 120)
    cr.fill()
    cr.set_source_rgb(0x20 / 255, 0xa0 / 255, 0x40 / 255)
    cr.arc(300, 400, 62, 0, 2 * math.pi)
    cr.fill()
    cr.set_source_rgb(0x20 / 255, 0x40 / 255, 0xc0 / 255)
    cr.move_to(480, 330)
    cr.line_to(545, 460)
    cr.line_to(415, 460)
    cr.close_path()
    cr.fill()
    cr.set_source_rgb(0.95, 0.55, 0.1)
    cr.set_line_width(6)
    cr.move_to(60, 560)
    cr.curve_to(200, 470, 380, 650, 535, 540)
    cr.stroke()
    cr.set_source_rgb(0, 0, 0)
    lines(cr, 60, 640, 12, 'DejaVu Sans', [
        'Shapes: red rectangle, green circle, blue triangle, orange curve.',
        'Keys: Page Down / Page Up (or Space / BackSpace) change pages,',
        'F11 toggles full screen, F5 starts the presentation, Esc leaves it.',
    ])


def page2(cr):
    title(cr, 'An embedded image')
    img = cairo.ImageSurface(cairo.FORMAT_RGB24, 256, 256)
    data = img.get_data()
    stride = img.get_stride()
    for yy in range(256):
        for xx in range(256):
            o = yy * stride + xx * 4          # little-endian xRGB32: B, G, R, x
            data[o + 0] = 0x80
            data[o + 1] = yy
            data[o + 2] = xx
            data[o + 3] = 0xff
    img.mark_dirty()
    cr.save()
    cr.translate(140, 200)
    cr.set_source_surface(img, 0, 0)
    cr.get_source().set_filter(cairo.FILTER_NEAREST)
    cr.paint()
    cr.restore()
    # an 8x8 checkerboard (vector) beside it
    for r in range(8):
        for c in range(8):
            cr.set_source_rgb(*((0, 0, 0) if (r + c) % 2 else (1, 1, 1)))
            cr.rectangle(420 + c * 16, 200 + r * 16, 16, 16)
            cr.fill()
    cr.set_source_rgb(0, 0, 0)
    cr.set_line_width(1)
    cr.rectangle(420, 200, 128, 128)
    cr.stroke()
    lines(cr, 60, 520, 12, 'DejaVu Sans', [
        'Left: a 256 x 256 RGB image (red grows to the right, green downwards).',
        'Right: an 8 x 8 checkerboard drawn with vector rectangles.',
    ])


def page3(cr):
    title(cr, 'A table')
    x0, y0, cw, ch = 60.0, 150.0, 47.5, 30.0
    for r in range(10):
        for c in range(10):
            if (r + c) % 2 == 0:
                cr.set_source_rgb(0xe8 / 255, 0xf0 / 255, 0xff / 255)
                cr.rectangle(x0 + c * cw, y0 + r * ch, cw, ch)
                cr.fill()
    cr.set_source_rgb(0.2, 0.2, 0.3)
    cr.set_line_width(1)
    for i in range(11):
        cr.move_to(x0, y0 + i * ch)
        cr.line_to(x0 + 10 * cw, y0 + i * ch)
        cr.move_to(x0 + i * cw, y0)
        cr.line_to(x0 + i * cw, y0 + 10 * ch)
    cr.stroke()
    cr.select_font_face('DejaVu Sans Mono', cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
    cr.set_font_size(11)
    cr.set_source_rgb(0, 0, 0)
    for r in range(10):
        for c in range(10):
            cr.move_to(x0 + c * cw + 12, y0 + r * ch + 19)
            cr.show_text('%d' % ((r + 1) * (c + 1)))
    lines(cr, 60, 500, 12, 'DejaVu Serif', [
        'A multiplication table: every cell is text over a vector grid.',
        'End of the sample document.',
    ])


def main(out):
    surface = cairo.PDFSurface(out, W, H)
    surface.set_metadata(cairo.PDF_METADATA_TITLE, 'Atril on Phoenix-RTOS - sample document')
    surface.set_metadata(cairo.PDF_METADATA_AUTHOR, 'Phoenix-RTOS Raspberry Pi 4 port')
    surface.set_metadata(cairo.PDF_METADATA_CREATOR, 'tools/gpu-lane/atril-wayland/tools/make-sample-pdf.py')
    surface.set_metadata(cairo.PDF_METADATA_CREATE_DATE, '2026-09-27T00:00:00Z')
    cr = cairo.Context(surface)
    for n, draw in enumerate((page1, page2, page3), 1):
        draw(cr)
        footer(cr, n)
        cr.show_page()
    surface.finish()


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--points':
        # the pixel checks for the host test: page:x:y:rrggbb
        for name, (page, x, y, rgb) in SHAPES.items():
            print('%d:%.1f:%.1f:%02x%02x%02x' % ((page, x, y) + rgb))
        sys.exit(0)
    if len(sys.argv) != 2:
        sys.exit('usage: make-sample-pdf.py OUT.pdf | --points')
    main(sys.argv[1])
