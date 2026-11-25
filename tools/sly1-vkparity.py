#!/usr/bin/env python3
"""sly1-vkparity.py BINDIR INSTALLATION OUTDIR [--frames N] [--every K] [--size WxH] [--keys KEYS] [--tolerance T]
                   [--against vulkan|gles]

Sly 1's picture parity check between its renderers (as Sly 2's tools/vkparity.py): tools/sly1-smoke.sh runs the game
twice from the same installation, once on OpenGL and once on Vulkan (SLY1_RENDERER), with a fixed game step
(SLY1_FIXED_DT: the same game state on the same frame) and the smoke run's scripted keys, a screenshot every K frames,
and the screenshots are compared pixel by pixel.

Prints per frame: the share of differing pixels (any channel off by more than --tolerance, default 2) and the largest
channel difference; OUTDIR/report.txt, and OUTDIR/diff-FRAME.png (OpenGL | Vulkan | differing pixels in white) for
every frame that differs. Exit 1 when a frame differs in more than --max-share of its pixels (default 0.5 %).
--against gles compares OpenGL with the OpenGL ES test mode (SLY1_RENDERER=gles: the desktop driver's ES context, the
path Android takes without Vulkan; platform/render/gles_sly1.h) instead of Vulkan;
SLY1_GLES_EMULATE=1 in the environment tests its fallback for drivers without base instance / base vertex.
Movies (FFmpeg) play by the host's clock: frames shot while one plays can differ for that reason alone.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def run(bindir, inst, out, renderer, args):
    env = dict(os.environ, SLY1_RENDERER=renderer, SLY1_FIXED_DT='0.016666668', SLY1_SMOKE_EVERY=str(args.every))
    if args.size:
        env['SLY1_SMOKE_SIZE'] = args.size
    cmd = ['sh', str(ROOT / 'tools' / 'sly1-smoke.sh'), bindir, str(out), inst, str(args.frames)]
    if args.keys:
        cmd.append(args.keys)
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    print(f'{renderer}: {r.stdout.strip()}')
    log = (out / 'game.log').read_text(errors='replace') if (out / 'game.log').exists() else ''
    wanted = {'vulkan': 'Renderer: Vulkan', 'gles': 'OpenGL ES: '}.get(renderer, 'Renderer: OpenGL')
    if wanted not in log:
        print(f'{renderer}: the game did not run on {renderer} (see {out}/game.log)')
        return False
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bindir')
    ap.add_argument('installation')
    ap.add_argument('out')
    ap.add_argument('--frames', type=int, default=4200)
    ap.add_argument('--every', type=int, default=150)
    ap.add_argument('--size', default='')
    ap.add_argument('--keys', default='')
    ap.add_argument('--tolerance', type=int, default=2)
    ap.add_argument('--max-share', type=float, default=0.5)
    ap.add_argument('--against', choices=('vulkan', 'gles'), default='vulkan')
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    ok = run(args.bindir, args.installation, out / 'opengl', 'opengl', args)
    other = args.against
    ok = run(args.bindir, args.installation, out / other, other, args) and ok
    if not ok:
        return 1
    lines, worst, failed = [], 0.0, 0
    for a in sorted((out / 'opengl' / 'shots').glob('frame-*.png')):
        b = out / other / 'shots' / a.name
        if not b.exists():
            lines.append(f'{a.stem}: missing on {other}')
            failed += 1
            continue
        pa = np.asarray(Image.open(a).convert('RGB')).astype(np.int16)
        pb = np.asarray(Image.open(b).convert('RGB')).astype(np.int16)
        if pa.shape != pb.shape:
            lines.append(f'{a.stem}: sizes differ {pa.shape} {pb.shape}')
            failed += 1
            continue
        d = np.abs(pa - pb).max(axis=2)
        mask = d > args.tolerance
        share = 100.0 * mask.mean()
        line = f'{a.stem}: {share:.3f} % of the pixels differ, largest difference {int(d.max())}'
        if share > 0:
            vis = np.concatenate([pa, pb, np.repeat(mask[:, :, None] * 255, 3, axis=2)], axis=1).astype(np.uint8)
            Image.fromarray(vis).save(out / f'diff-{a.stem}.png')
        if share > args.max_share:
            line += '  <-- over the limit'
            failed += 1
        worst = max(worst, share)
        lines.append(line)
    lines.append(f'{len(lines)} frames, worst {worst:.3f} %, {failed} over {args.max_share} %')
    (out / 'report.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
