from PIL import Image

im = Image.open(r"C:\Users\znwaj\AppData\Local\Temp\freebuff-desktop-pastes\paste-1790551075542-97612.png").convert("RGB")
w, h = im.size
px = im.load()

def close(c, t, tol=30):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))

# 1. Find the purple button (base #6c5ce7, hover #8d7cf3)
purples = [(x, y) for y in range(h) for x in range(w)
           if close(px[x, y], (108, 92, 231), 45) or close(px[x, y], (141, 124, 243), 45)]
bx0 = min(p[0] for p in purples); bx1 = max(p[0] for p in purples)
by0 = min(p[1] for p in purples); by1 = max(p[1] for p in purples)
print(f"button bbox: x {bx0}..{bx1}  y {by0}..{by1}  -> {bx1-bx0+1} x {by1-by0+1} px")

# 2. White pixels inside the button
whites = [(x, y) for y in range(by0, by1 + 1) for x in range(bx0, bx1 + 1)
          if px[x, y][0] > 200 and px[x, y][1] > 200 and px[x, y][2] > 200]

# 3. Split into two clusters on the widest x-gap
xs = sorted(set(p[0] for p in whites))
gaps = [(xs[i + 1] - xs[i], xs[i]) for i in range(len(xs) - 1)]
gap, at = max(gaps)
left = [(x, y) for (x, y) in whites if x <= at]
right = [(x, y) for (x, y) in whites if x > at]

def stats(cluster, name):
    ys = [p[1] for p in cluster]
    x0 = min(p[0] for p in cluster); x1 = max(p[0] for p in cluster)
    print(f"{name}: x {x0}..{x1}  y {min(ys)}..{max(ys)}  height {max(ys)-min(ys)+1}  center y {(min(ys)+max(ys))/2}")
    return (min(ys) + max(ys)) / 2, max(ys) - min(ys) + 1

gc, gh = stats(left, "glyph(+)")
tc, th = stats(right, "text(cap band)")

scale = 36.0 / (by1 - by0 + 1)   # actual button height is 36 logical px
d = (tc - gc) * scale
print(f"\nscale (logical/screenshot px): {scale:.3f}")
print(f"glyph center is {d:+.2f} logical px vs cap-band center (negative = glyph HIGH)")
print(f"glyph height {gh*scale:.1f} logical vs cap band {th*scale:.1f} logical")
