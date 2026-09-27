#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
#
# pngify-icon-theme.py -- build a PNG-only freedesktop icon theme for Phoenix-RTOS.
#
# GTK 3 on Phoenix has no SVG loader (librsvg is Rust): GTK skips every .svg in a theme
# (gtkicontheme.c checks whether gdk-pixbuf can load image/svg), so an SVG-only icon is
# simply missing. This script copies the PNGs of one or more source themes and renders
# the SVGs on the build host into fixed-size directories:
#
#   NAME-symbolic.svg -> <size>/<context>/NAME-symbolic.symbolic.png  (gtk-encode-symbolic-svg:
#                        GTK 3 recolours these like the SVG, foreground/success/warning/error)
#   NAME.svg          -> <size>/<context>/NAME.png   (GdkPixbuf with the host's SVG loader;
#                        only where the source has no PNG of that size)
#
# and writes index.theme (every directory Type=Fixed). Run gtk-update-icon-cache on the
# result (the build does): without a cache GTK stats the whole tree over NFS.
#
# Usage: pngify-icon-theme.py --name N --inherits I --sizes 16,24,32,48 [--jobs J]
#            [--include-sizes 16x16,22x22,...] [--comment C] DST SRC...
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

SIZE_DIR = re.compile(r'^(\d+)x(\d+)(@2x)?$')

CONTEXTS = {
    'actions': 'Actions', 'animations': 'Animations', 'apps': 'Applications', 'categories': 'Categories',
    'devices': 'Devices', 'emblems': 'Emblems', 'emotes': 'Emotes', 'intl': 'International',
    'legacy': 'Legacy', 'mimetypes': 'MimeTypes', 'places': 'Places', 'status': 'Status',
    'stock': 'Stock', 'ui': 'UI', 'panel': 'Panel',
}


def render_svg(svg, png, size):
    import gi
    gi.require_version('GdkPixbuf', '2.0')
    from gi.repository import GdkPixbuf
    pb = GdkPixbuf.Pixbuf.new_from_file_at_size(svg, size, size)
    os.makedirs(os.path.dirname(png), exist_ok=True)
    pb.savev(png, 'png', [], [])


def encode_symbolic(svg, outdir, size):
    os.makedirs(outdir, exist_ok=True)
    subprocess.run(['gtk-encode-symbolic-svg', '-o', outdir, svg, '%dx%d' % (size, size)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--inherits', default='hicolor')
    ap.add_argument('--comment', default='PNG-only icon theme for Phoenix-RTOS (no SVG loader)')
    ap.add_argument('--sizes', default='16,24,32,48', help='sizes the SVGs are rendered at')
    ap.add_argument('--include-sizes', default='', help='PNG size dirs to copy (default: all)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('dst')
    ap.add_argument('srcs', nargs='+')
    a = ap.parse_args()
    sizes = [int(s) for s in a.sizes.split(',') if s]
    include = set(s for s in a.include_sizes.split(',') if s)

    if os.path.exists(a.dst):
        shutil.rmtree(a.dst)
    os.makedirs(a.dst)
    have = set()          # (sizedir, context, name-without-ext) of PNGs present
    svgs = []             # (path, context, name)
    for src in a.srcs:
        for sizedir in sorted(os.listdir(src)):
            sd = os.path.join(src, sizedir)
            if not os.path.isdir(sd):
                continue
            for ctx in sorted(os.listdir(sd)):
                cd = os.path.join(sd, ctx)
                if not os.path.isdir(cd):
                    continue
                for f in sorted(os.listdir(cd)):
                    p = os.path.join(cd, f)
                    if f.endswith('.png') and SIZE_DIR.match(sizedir) and not sizedir.endswith('@2x') \
                            and (not include or sizedir in include):
                        out = os.path.join(a.dst, sizedir, ctx, f)
                        os.makedirs(os.path.dirname(out), exist_ok=True)
                        shutil.copyfile(os.path.realpath(p), out)
                        have.add((sizedir, ctx, f[:-4]))
                    elif f.endswith('.svg'):
                        svgs.append((p, ctx, f[:-4]))

    jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for p, ctx, name in svgs:
            for s in sizes:
                sizedir = '%dx%d' % (s, s)
                if name.endswith('-symbolic'):
                    if (sizedir, ctx, name + '.symbolic') in have:
                        continue
                    have.add((sizedir, ctx, name + '.symbolic'))
                    jobs.append(ex.submit(encode_symbolic, os.path.realpath(p), os.path.join(a.dst, sizedir, ctx), s))
                else:
                    if (sizedir, ctx, name) in have:
                        continue
                    have.add((sizedir, ctx, name))
                    jobs.append(ex.submit(render_svg, os.path.realpath(p), os.path.join(a.dst, sizedir, ctx, name + '.png'), s))
        failed = 0
        for j in jobs:
            try:
                j.result()
            except Exception as e:  # noqa: BLE001 -- report every failure, then fail
                failed += 1
                sys.stderr.write('pngify: %s\n' % e)
    if failed:
        sys.stderr.write('pngify: %d render(s) failed\n' % failed)
        return 1

    dirs = []
    for sizedir in sorted(os.listdir(a.dst), key=lambda d: int(SIZE_DIR.match(d).group(1))):
        for ctx in sorted(os.listdir(os.path.join(a.dst, sizedir))):
            dirs.append((sizedir, ctx))
    with open(os.path.join(a.dst, 'index.theme'), 'w') as f:
        f.write('[Icon Theme]\nName=%s\nComment=%s\nInherits=%s\nExample=folder\n' % (a.name, a.comment, a.inherits))
        f.write('Directories=%s\n' % ','.join('%s/%s' % d for d in dirs))
        for sizedir, ctx in dirs:
            n = int(SIZE_DIR.match(sizedir).group(1))
            f.write('\n[%s/%s]\nSize=%d\nContext=%s\nType=Fixed\n' % (sizedir, ctx, n, CONTEXTS.get(ctx, ctx.capitalize())))
    n_png = sum(len(fs) for _, _, fs in os.walk(a.dst)) - 1
    print('pngify: %s: %d directories, %d PNG files (%d rendered from SVG)' % (a.name, len(dirs), n_png, len(jobs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
