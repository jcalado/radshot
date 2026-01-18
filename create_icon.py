#!/usr/bin/env python3
"""
Convert radshot.png to radshot.ico with multiple sizes for Windows application icon.
Requires: pip install Pillow
"""

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow library not found.")
    print("Install it with: pip install Pillow")
    sys.exit(1)

def create_ico(png_path: str, ico_path: str, sizes: list[int] = None):
    """Convert PNG to ICO with multiple sizes."""
    if sizes is None:
        sizes = [256, 48, 32, 16]

    png_path = Path(png_path)
    ico_path = Path(ico_path)

    if not png_path.exists():
        print(f"ERROR: Source file not found: {png_path}")
        sys.exit(1)

    # Open the source image
    try:
        img = Image.open(png_path)
    except Exception as e:
        print(f"ERROR: Failed to open image: {e}")
        sys.exit(1)

    # Convert to RGBA if necessary
    if img.mode != 'RGBA':
        img = img.convert('RGBA')

    # Warn if source is smaller than largest requested size
    max_size = max(sizes)
    if img.width < max_size or img.height < max_size:
        print(f"WARNING: Source image ({img.width}x{img.height}) is smaller than {max_size}x{max_size}")

    # Create resized versions
    icons = []
    for size in sizes:
        resized = img.resize((size, size), Image.Resampling.LANCZOS)
        icons.append(resized)

    # Save as ICO
    try:
        icons[0].save(
            ico_path,
            format='ICO',
            sizes=[(s, s) for s in sizes],
            append_images=icons[1:]
        )
    except Exception as e:
        print(f"ERROR: Failed to save ICO file: {e}")
        sys.exit(1)

    print(f"Created: {ico_path}")
    print(f"  Sizes: {', '.join(f'{s}x{s}' for s in sizes)}")

if __name__ == '__main__':
    script_dir = Path(__file__).parent
    png_file = script_dir / 'radshot.png'
    ico_file = script_dir / 'radshot.ico'

    print("Creating application icon...")
    create_ico(png_file, ico_file)
    print("Done!")
