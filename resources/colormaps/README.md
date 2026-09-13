# Colormaps

Sweep++ ships twelve colormaps compiled in — `spectral`, `midnight`, `rainbow`,
`meadow`, `ember`, `magma`, `plasma`, `grayscale`, `ice`, `fire`, `classic` and
`aurora`. They need no files.

Drop a `.toml` here, or in your config directory, to add your own. A file whose
`name` matches a built-in replaces it.

```toml
[colormap]
name = "my-gradient"
stops = [
  { pos = 0.0,  color = "#000080" },
  { pos = 0.35, color = "#00FFFF" },
  { pos = 0.55, color = "#00FF00" },
  { pos = 0.75, color = "#FFFF00" },
  { pos = 1.0,  color = "#800000" },
]
```

`pos` runs 0 to 1 across the gradient range; colours interpolate linearly
between stops. At least two stops are required. `#RRGGBBAA` is accepted where
alpha matters.

The gradient editor in the app writes exactly this format, so "save as
colormap…" and hand-editing produce interchangeable files.

## Choosing one

- **`meadow`** is perceptually uniform: equal steps in level look like equal
  steps in brightness. Use it when the display is a measurement — a
  non-uniform palette invents structure that is not in the data.
- **`spectral`** is the familiar analyser look and has the highest perceived
  contrast, which makes weak signals easy to spot. It is not perceptually
  uniform, so judge levels from the scale rather than the colour.
- **`midnight`** is the dark theme's default. Its floor is near-black, so empty
  bands read as empty and anything above the noise stands out.
- **`grayscale`** survives being printed, and is the easiest for judging
  relative level.
