#!/bin/sh
# Render the LVGL / EEZ Studio screens at every supported panel size and
# capture each one, so a layout regression shows up without swapping hardware.
#
# Resolution is a runtime setting, so the whole matrix runs on the dev machine.
# The capture is read back from the renderer inside the app (lvgl-test-shot),
# not screenshotted from the compositor, so it is pixel-exact and needs no
# window manager -- SDL's "offscreen" video driver makes it work headless/CI.
#
#   make USE_LVGL=1 release
#   tools/ui-sweep.sh                 # all sizes
#   tools/ui-sweep.sh 1280x480        # just one
#
# Output: out/ui-sweep/<w>x<h>.png plus a contact sheet, sweep.png.
#
# The aspect bands the UI is designed against (see docs/lvgl-ui-blueprint.md):
#   standard  1.5-2.0   400x234 400x240 480x272 800x480 1024x600
#   wide      >= 2.0    1280x480
set -e

APP=${APP:-./out/app}
OUT=${OUT:-./out/ui-sweep}
FRAMES_TIMEOUT=${FRAMES_TIMEOUT:-20}
SIZES=${*:-"400x234 400x240 480x272 800x480 1024x600 1280x480"}
# Which screen to capture: picker (default) or settings.
SCREEN=${SCREEN:-picker}

if [ ! -x "$APP" ]; then
    echo "error: $APP not found -- build it first: make USE_LVGL=1 release" >&2
    exit 1
fi

# Headless when there is no display; SDL's offscreen driver still renders.
if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ] && [ -z "$SDL_VIDEODRIVER" ]; then
    SDL_VIDEODRIVER=offscreen
    export SDL_VIDEODRIVER
    echo "no display -- using SDL_VIDEODRIVER=offscreen"
fi

mkdir -p "$OUT"
failed=0
shots=""

for size in $SIZES; do
    w=${size%x*}
    h=${size#*x}
    cfg="$OUT/$SCREEN-$size.txt"
    bmp="$OUT/$SCREEN-$size.bmp"
    png="$OUT/$SCREEN-$size.png"
    rm -f "$bmp" "$png"

    cat > "$cfg" <<EOF
lvgl-test = true
lvgl-start-screen = $SCREEN
icon-theme = ${ICON_THEME:-builtin}
lvgl-test-shot = $bmp
renderer = sdl
window-mode = 0
width = $w
height = $h
log-level = 3
audio-driver = dummy
EOF

    printf '%-10s ' "$size"
    if ! timeout "$FRAMES_TIMEOUT" "$APP" "$cfg" >"$OUT/$SCREEN-$size.log" 2>&1; then
        : # the app exits itself after the capture; a non-zero code is only
          # fatal if the capture is missing, which is checked below
    fi

    if [ -f "$bmp" ]; then
        if command -v magick >/dev/null 2>&1; then
            magick "$bmp" "$png" && rm -f "$bmp"
        elif command -v convert >/dev/null 2>&1; then
            convert "$bmp" "$png" && rm -f "$bmp"
        else
            png="$bmp" # no ImageMagick: keep the BMP
        fi
        echo "ok   -> $png"
        shots="$shots $png"
    else
        echo "FAIL -- no capture (see $OUT/$size.log)"
        failed=$((failed + 1))
    fi
done

# Contact sheet: all sizes side by side, at their true relative scale, so a
# layout that breaks on one panel is obvious at a glance.
if [ -n "$shots" ]; then
    if command -v magick >/dev/null 2>&1; then
        magick montage $shots -background '#202020' -geometry +8+8 -label '%f %wx%h' "$OUT/sweep-$SCREEN.png" 2>/dev/null && \
            echo "contact sheet: $OUT/sweep-$SCREEN.png"
    elif command -v montage >/dev/null 2>&1; then
        montage $shots -background '#202020' -geometry +8+8 -label '%f %wx%h' "$OUT/sweep-$SCREEN.png" 2>/dev/null && \
            echo "contact sheet: $OUT/sweep-$SCREEN.png"
    fi
fi

[ "$failed" -eq 0 ] || { echo "$failed size(s) failed" >&2; exit 1; }
echo "all sizes rendered"
