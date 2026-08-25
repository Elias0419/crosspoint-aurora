#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum --force-autohint > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum --force-autohint > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
# Medium, not Regular, is the UI text weight: 1-bit rasterisation at these sizes
# snaps stems to whole pixels, and Regular lands on 2px where Medium lands on 3.
UI_FONT_STYLES=("Medium" "Bold")

python generate-ui-noto-fonts.py

# System (UI) font faces, selectable at runtime via the "System Font" setting.
# Both carry a Hebrew fallback (0x05D0-0x05EA) so every shipped language renders.
#   ubuntu_*     -> the Vietnamese-localized Ubuntu cut (Latin + Vietnamese +
#                   Cyrillic/Greek from the cut, Hebrew from Noto). "Ubuntu" option.
#   notosansui_* -> Noto Sans (the default Aurora look). "Noto Sans" option.
for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    # UI-specific optical faces retain readable stroke weight under --mono.
    if [ "$style" = "Bold" ]; then noto_style="UIBold"; else noto_style="UIMedium"; fi
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${noto_style}.ttf"
    arabic_path="../builtinFonts/source/NotoSansArabic/NotoSansArabic-${noto_style}.ttf"
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    # Every face in this stack is optically weighted for monochrome rendering.
    # The Noto faces are variable-font instances and carry no hints, so Hebrew
    # needs the auto-hinter to reach the same even stem width; Arabic stays on
    # the plain grid fit, where the auto-hinter costs more ink than
    # verify-ui-noto-fonts.py allows. Ubuntu Bold keeps its own hints, but
    # Ubuntu Medium's bytecode grid-fits U+0442 wider than its advance and lands
    # its left bearing at -1, so that face runs on the auto-hinter as well.
    autohint_args=(--autohint-font $hebrew_path)
    if [ "$style" = "Medium" ]; then autohint_args+=(--autohint-font $font_path); fi
    python fontconvert.py $font_name $size $font_path $hebrew_path $arabic_path $viet_path \
      --mono "${autohint_args[@]}" --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

python verify-ui-noto-fonts.py

# --- Aurora-only UI families -------------------------------------------------
# Upstream's loop above covers Ubuntu, and generate-ui-noto-fonts.py covers the
# Noto Sans UI faces. These two exist only on aurora, where the System Font
# setting offers them alongside Noto Sans and Ubuntu. They keep --force-autohint
# rather than upstream's --mono: neither has an optically monochrome cut to pair
# with, and the autohinter is what gave them even stems at 10/12px.
UI_EXTRA_HEBREW_REGULAR="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-UIMedium.ttf"
UI_EXTRA_HEBREW_BOLD="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-UIBold.ttf"

# EB Garamond UI font (extract static instances from variable font first with instancer).
# Source: google/fonts main/ofl/ebgaramond/EBGaramond[wght].ttf -> instanced at wght=400/700.
for size in 10 12; do
  python fontconvert.py "ebgaramond_${size}_regular" $size     "../builtinFonts/source/EBGaramond/EBGaramond-Regular.ttf" "$UI_EXTRA_HEBREW_REGULAR"     --additional-intervals 0x05D0,0x05EA --force-autohint > "../builtinFonts/ebgaramond_${size}_regular.h"
  python fontconvert.py "ebgaramond_${size}_bold" $size     "../builtinFonts/source/EBGaramond/EBGaramond-Bold.ttf" "$UI_EXTRA_HEBREW_BOLD"     --additional-intervals 0x05D0,0x05EA --force-autohint > "../builtinFonts/ebgaramond_${size}_bold.h"
  echo "Generated ebgaramond_${size}_{regular,bold}.h"
done

# SFU Goudy UI font (single Medium weight used for both regular and bold positions).
# Source: SFUGoudyMedium.TTF (local, copy into builtinFonts/source/SFUGoudy/).
for size in 10 12; do
  python fontconvert.py "sfugoudy_${size}_regular" $size     "../builtinFonts/source/SFUGoudy/SFUGoudyMedium.ttf" "$UI_EXTRA_HEBREW_REGULAR"     --additional-intervals 0x05D0,0x05EA --force-autohint > "../builtinFonts/sfugoudy_${size}_regular.h"
  python fontconvert.py "sfugoudy_${size}_bold" $size     "../builtinFonts/source/SFUGoudy/SFUGoudyMedium.ttf" "$UI_EXTRA_HEBREW_BOLD"     --additional-intervals 0x05D0,0x05EA --force-autohint > "../builtinFonts/sfugoudy_${size}_bold.h"
  echo "Generated sfugoudy_${size}_{regular,bold}.h"
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  ../builtinFonts/source/NotoSansArabic/NotoSansArabic-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" --force-autohint > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
