#!/bin/sh
# Regenerate main/font_num_*.c: large numeric faces, DIGITS ONLY.
#
# Text uses LVGL's built-in Montserrat (enabled in sdkconfig.defaults) -- its
# charset covers the whole UI. Only the big display numerals are generated,
# because the built-ins stop at 48 and carry a full ASCII set we do not need
# at 96px.
#
# Charset: space ! % + , - . / 0-9 : and the currency marks $ c/ L- Y= E=
#   ui_fmt guarantees the numeric label only ever receives these characters
#   (see its numeric_only out-param and the host test that sweeps it). A glyph
#   missing from an LVGL font draws as NOTHING, so that invariant is load
#   bearing -- a letter leaking in here makes the value vanish from the tile.
#   '!' is included as the parse-error placeholder, and the currency marks
#   because a panel can carry a prefix and "$" in the digits-only face would
#   otherwise draw as nothing -- the exact failure this comment warns about.
#   The euro sign is outside the built-in Montserrat range (0x20-0x7E), so a
#   currency panel is the one case that could not be served by falling back to
#   a text face.
#
# Sizes chosen by fitting, not taste:
#   44  1x1 tile value, gauge centre       (10+17+4+54+10 = 95 <= 128)
#   64  2x1 tile value                     (72 overflows a 128px tile by 1px)
#   96  2x2 hero value and the detail view (104 makes 5 digits 374px in a 360px box)
set -e
cd "$(dirname "$0")/../main"

TTF=../managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf
RANGE='0x20,0x21,0x24,0x25,0x2B,0x2C,0x2D,0x2E,0x2F,0x30-0x39,0x3A,0xA2,0xA3,0xA5,0x20AC'

for sz in 44 64 96; do
    echo "  font_num_$sz.c"
    npx --yes lv_font_conv@1.5.3 --bpp 4 --size "$sz" \
        --font "$TTF" -r "$RANGE" \
        --format lvgl --no-compress -o "font_num_$sz.c"
done
echo "done; add any new sizes to fonts.h and main/CMakeLists.txt"
