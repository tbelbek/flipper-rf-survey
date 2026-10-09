# RF Survey

A logging spectrum survey tool for the Flipper Zero. Give it a frequency range, it sweeps
it, reads signal strength (RSSI) at every step, shows a live spectrum, and records every
sweep to a CSV on the SD card so you can analyse it on a PC — which frequencies are active,
and whether a source gets stronger as you move around. Like the stock Frequency Analyzer,
but it scans a range you choose and keeps a time history.

> Status: work in progress. v0.1 is the scan + live bars skeleton; config, waterfall /
> numeric views, CSV logging and on-device `.sub` capture are landing in phases.

## What it is (and isn't)

- It **measures signal strength and location**; it does **not decode or identify** signals.
- The radio is a CC1101, **not an SDR**. It only covers the sub-GHz bands **300-348,
  387-464, 779-928 MHz**. So it **cannot** see Wi-Fi/Bluetooth (2.4 GHz) devices, and it
  **cannot** make sense of cellular (GSM/LTE) traffic even when that traffic is in band.
- It can capture **static OOK/2FSK** (remote-control style) bursts to `.sub` for replay in
  the stock SubGHz app. It is **not** a universal "bug detector".

Used honestly it's great for: locating a sub-GHz transmitter by walking around and watching
a bin rise, surveying which frequencies in a band are busy over time, and grabbing a remote.

## Roadmap

- **v1 — locate:** range config (scroll-select band, auto range, custom + save), sweep
  worker, Bars / Waterfall / Numeric pages, peak-hold, timestamped CSV logging.
- **v1.1 — capture:** lock the peak frequency and record it to `.sub` (async RX → stream
  buffer → writer thread), with overflow counter and a duration cap.
- **v2:** external CC1101 module support for a bigger antenna.

## Build & install

Built with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt):

```bash
ufbt            # build rfsurvey.fap into dist/
ufbt launch     # build, upload to a connected Flipper and run
```

Or copy `dist/rfsurvey.fap` to `/ext/apps/Sub-GHz/` on the SD card.

## Notes

- Uses the firmware's exported CC1101 preset tables (`AM650` / `FM238` / `FM476`), the same
  names the stock Spectrum Analyzer and SubGHz use.
- Sub-GHz RX only — the app never transmits.

## License

MIT — see [LICENSE](LICENSE).
