# WynlandOS - docs/brand/*.png from the .jpgs: the black background made
# transparent.  usage: py -3 tools/brand_cutout.py  (numpy, Pillow, scipy)
#
# Black background -> alpha. Over pure black a pixel is alpha * colour; the
# JPEGs' edges are darkened by compression, so an edge pixel takes the colour
# of the nearest pixel well inside its shape and the alpha that colour needs
import numpy as np
from PIL import Image
from scipy import ndimage

def cut(src, dst, thr=24, inset=3, pad=6):
    c = np.asarray(Image.open(src).convert('RGB')).astype(np.float32)
    m = c.max(axis=2)
    shape = m > thr
    lab, n = ndimage.label(shape)
    sizes = ndimage.sum(np.ones_like(m), lab, range(1, n + 1))
    shape = np.isin(lab, 1 + np.nonzero(sizes >= 400)[0])       # no JPEG specks
    holes, nh = ndimage.label(ndimage.binary_fill_holes(shape) & ~shape)
    hs = ndimage.sum(np.ones_like(m), holes, range(1, nh + 1))
    shape |= np.isin(holes, 1 + np.nonzero(hs < 60)[0])         # pinholes only
    deep = ndimage.binary_erosion(shape, iterations=inset)
    idx = ndimage.distance_transform_edt(~deep, return_distances=False, return_indices=True)
    near = c[idx[0], idx[1]]
    a = np.clip(m / np.maximum(near.max(axis=2), 1), 0, 1)
    a = np.where(deep, 1.0, a)
    a = np.where(ndimage.binary_dilation(shape, iterations=2), a, 0)
    a = np.where(a < 0.06, 0, a)
    rgb = np.where(deep[..., None], c, near)
    out = np.dstack([rgb, a * 255]).round().astype(np.uint8)
    ys, xs = np.nonzero(a > 0)
    y0, y1 = max(ys.min() - pad, 0), min(ys.max() + pad + 1, a.shape[0])
    x0, x1 = max(xs.min() - pad, 0), min(xs.max() + pad + 1, a.shape[1])
    Image.fromarray(out[y0:y1, x0:x1], 'RGBA').save(dst, optimize=True)

for n in ['mascot', 'icon', 'logo']:
    cut(f'docs/brand/{n}.jpg', f'docs/brand/{n}.png')
