#!/usr/bin/env python3
"""Convert a PNG to raw BGR888 for embedding as splash screen."""
import sys
from PIL import Image

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} input.png output.bin", file=sys.stderr)
    sys.exit(1)

img = Image.open(sys.argv[1]).convert('RGB')
import numpy as np
rgb = np.array(img)
bgr = rgb[:, :, ::-1].tobytes()

with open(sys.argv[2], 'wb') as f:
    f.write(bgr)

print(f"gen_splash: {img.size[0]}x{img.size[1]} -> {len(bgr)} bytes")
