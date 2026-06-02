# Rain Graph watchface

A sample watchface that shows the current time plus an **adaptive line graph of
upcoming rain intensity** for your location.

![layout](time on top, peak/onset subtitle, rain line graph below)

## Adaptive time span

The graph's horizon stretches based on *when* the next rain is expected, so the
interesting part of the forecast always fills the screen:

| Situation                       | Window shown                                  |
| ------------------------------- | --------------------------------------------- |
| Rain within the next hour       | next 60 minutes (zoomed in)                   |
| Rain later today                | ~1.5× the time until it starts (onset at ⅔)   |
| No rain expected                | next 60 minutes, flat ("No rain expected")    |

The window is always capped at the end of the local day. The left x-axis label
is "now"; the right label is the clock time at the end of the window. A dotted
vertical marker shows when the rain is expected to begin.

## Data source

Precipitation comes from the [Open-Meteo](https://open-meteo.com) forecast API
using the **KNMI HARMONIE model** (`models=knmi_seamless`) — the same KNMI
forecast data behind the [KNMI app](https://gitlab.com/KNMI-OSS/KNMI-App). It is
keyless, returns JSON and is CORS friendly, which makes it a good fit for a
PebbleKit JS app.

- `minutely_15=precipitation` gives 15-minute resolution for the whole day,
  which the near-term (zoomed) window samples from.
- `hourly=precipitation` is used as a fallback when 15-minute data is missing.
- Intensity is converted to mm/h (15-minute accumulation × 4) and the peak in
  the window is shown in the subtitle.

Location is taken from the phone's GPS (`navigator.geolocation`), falling back
to De Bilt (KNMI HQ) when location is unavailable.

## How it works

The phone (`src/js/pebble-js-app.js`) does the heavy lifting: it fetches the
forecast, chooses the adaptive window, samples it into `NUM_POINTS` values
normalised to a 1 mm/h floor, and sends them as a byte array over AppMessage.
The watch (`src/rain_graph_watchface.c`) just draws the clock and the line
graph, and asks the phone to refresh on launch and every 10 minutes. The last
forecast is persisted so the face is not blank right after launch.

## Building

This is a standard Pebble SDK project. With the Pebble SDK / `pebble` tool
installed:

```sh
pebble build
pebble install --emulator basalt   # or a connected watch
```
