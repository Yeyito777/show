# show

A tiny image and PDF viewer.

## Usage

```bash
show <file>
```

## Controls

- `Esc`: quit
- `Enter` / `f`: toggle fullscreen
- `=` / `-`: normal zoom in / out
- `+` / `_`: finer zoom in / out
- `0`: reset zoom and pan
- `h` / `j` / `k` / `l`: scroll left / down / up / right
- `r`: rotate clockwise 90°
- `Shift+r`: rotate counterclockwise 90°
- `Shift+j` / `Shift+l`: next PDF page
- `Shift+k` / `Shift+h`: previous PDF page
- `Home`: first PDF page

## Build

```bash
make
sudo make install
```
