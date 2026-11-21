#!/usr/bin/env bash
# Decodes JPEGs with the OS's decoder (jpegbench) and compares each with libjpeg (PIL). 4:2:0
# pictures land near 32 dB (libjpeg smooths chroma when upsampling; the OS repeats it); 4:4:4 and
# grey are near-exact.
# Usage: ./jpegcheck.sh [files...]   (no files: makes baseline + progressive test pictures)
set -e
H="$(cd "$(dirname "$0")" && pwd)"
"$H/build.sh" >/dev/null
echo 'char stack_top[1];' > "$H/obj/stub.c"
gcc -O2 -w -I"$H/shim" -I"$H/../../include" "$H/jpegbench.c" "$H/obj/stub.c" "$H"/obj/{image,svg,host_heap,string,klib,kmath,css,html,layout,jsdom,js,regex,markdown}.o -o "$H/jpegbench" -lm
T="$H/obj/jpg"; mkdir -p "$T"
if [ $# -eq 0 ]; then
python3 - "$T" <<'PY'
import sys, random
from PIL import Image
T = sys.argv[1]
w, h = 333, 217
im = Image.new('RGB', (w, h)); px = im.load(); random.seed(1)
for y in range(h):
    for x in range(w):
        px[x, y] = ((x * 255 // w) ^ (y & 31), y * 255 // h, ((x + y) * 3 + random.randint(0, 40)) & 255)
for name, prog, sub, q in [('b420', 0, 2, 85), ('p420', 1, 2, 85), ('p444', 1, 0, 85), ('p422', 1, 1, 85), ('p_q30', 1, 2, 30), ('p_q98', 1, 0, 98)]:
    im.save(f'{T}/{name}.jpg', progressive=bool(prog), subsampling=sub, quality=q)
im.convert('L').save(f'{T}/pgray.jpg', progressive=True, quality=80)
PY
command -v cjpeg >/dev/null && { python3 -c "from PIL import Image; Image.open('$T/b420.jpg').save('$T/src.ppm')"; cjpeg -progressive -restart 3 -quality 80 "$T/src.ppm" > "$T/p_rst.jpg"; }
set -- "$T"/*.jpg "$H/tests/prog.jpg"
fi
for f in "$@"; do
  PPM="$T/out.ppm" "$H/jpegbench" "$f" >/dev/null || { echo "FAIL $f (decoder error)"; continue; }
  python3 - "$f" "$T/out.ppm" <<'PY'
import sys, math
from PIL import Image, ImageChops, ImageStat
a = Image.open(sys.argv[1]).convert('RGB'); b = Image.open(sys.argv[2]).convert('RGB')
if a.size != b.size: print('FAIL', sys.argv[1], 'size', a.size, b.size); sys.exit()
st = ImageStat.Stat(ImageChops.difference(a, b)); mse = sum(v * v for v in st.rms) / 3
psnr = 10 * math.log10(255 * 255 / mse) if mse else 99
print('%-4s %-40s PSNR %.1f dB' % ('ok' if psnr > 30 else 'FAIL', sys.argv[1].split('/')[-1], psnr))
PY
done
