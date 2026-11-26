# A BDF font as a Text face

A `.wbf` file is the form to keep, because the bitmap is a small fraction of the BDF text. BDF writes every glyph as lines of hex. The bitmap font stores each pixel as one bit.

[PixelFonts](https://github.com/mcer12/PixelFonts) publishes its faces as BDF in [`bdf/`](https://github.com/mcer12/PixelFonts/tree/main/bdf). The eighteen English faces there, the files named `pixelfont_<rows>x<cols>_<weight>.bdf`, are 210404 bytes. This script writes those same faces as 27442 bytes of `.wbf`, 13% of the BDF. `pixelfont_5x5_thin` goes from 9357 bytes to 480, which is 5% of its BDF, and `pixelfont_21x11_bold` goes from 15606 bytes to 3395, which is 22%.

Text draws that bitmap. The 4x6 face stays in the firmware. Every further face is a `.wbf` file in `/moonlive/fonts`, and the font menu lists that folder. The English `.wbf` files under `moonlive/fonts` come from the PixelFonts repository, and the five WLED faces are files there in this same layout. A BDF from that repository, or any other BDF whose glyphs fit a 64 by 64 cell, converts with `docs/reference/examples/bdf_to_wbf.py`.

```bash
python3 docs/reference/examples/bdf_to_wbf.py pixelfont_5x5_thin.bdf pixelfont_5x5_thin.wbf
```

Upload the `.wbf` in the file manager, into `moonlive/fonts`. It appears in the font menu. Choosing a name that is not on the device yet downloads the copy from this repository. A face you converted yourself is already on the device, so the menu uses that file.

The script writes the layout `src/light/powerfunctions/wbf.h` reads. Twelve bytes name the height, the widest glyph and the first and last code point. A variable-width face then stores one width byte per glyph. Each glyph follows as a row of bits, the high bit on the left, padded to a whole byte. A code point past ASCII is stored the way WLED stores one, so the reader finds it from the header's unicode base.
