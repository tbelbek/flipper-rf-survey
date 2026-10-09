# RF Survey

A logging spectrum survey tool for the Flipper Zero. Give it a frequency range, it sweeps
it, reads signal strength (RSSI) at every step, shows a live spectrum across several views,
logs every sweep to a CSV on the SD card, helps you walk a signal to its source, and can
record a located sub-GHz signal to a `.sub` you replay in the stock SubGHz app.

Think of it as the stock Frequency Analyzer, but it scans a range you choose, keeps a time
history, and logs everything for analysis on a PC. **Sub-GHz RX only — it never transmits.**

## What it is (and isn't)

- It **measures signal strength and location** and **captures** static OOK/2FSK bursts; it
  does **not decode or identify** arbitrary signals.
- The radio is a CC1101, **not an SDR**. It only covers the sub-GHz bands **300-348,
  387-464, 779-928 MHz**. So it **cannot** see Wi-Fi/Bluetooth (2.4 GHz) devices, and it
  **cannot** make sense of cellular (GSM/LTE) traffic even when that traffic is in band.
- Used honestly it's great for: locating a sub-GHz transmitter by watching a bin rise as you
  move, surveying which frequencies in a band are busy over time, and grabbing a remote.

## Features

- **Range config** — scroll a Band list (full-spectrum / full-band / ~26 known allocations,
  each tagged, e.g. 433 ISM, 868 ISM, PMR446, LTE800), or type an exact Start/End range in a
  Frequency-Analyzer-style editor. Pick modulation preset (AM650/AM270/FM238/FM476) and step.
- **Five live views**, flip with Left/Right:
  - **Bars** — grouped bars with per-bin peak-hold, a movable cursor, and a dotted trigger
    line. OK zooms into the cursor band and **narrows the RX bandwidth** so a carrier shows a
    sharp peak, not a wide mountain.
  - **Connected** — per-frequency average over a time window plus a dotted peak envelope.
  - **Waterfall** — horizontal time × frequency heat map with an adjustable window (5 s to
    10 min) and a time scale, so you can read how often and how regularly a band is busy.
  - **Numeric** — the strongest peaks (adjacent bins grouped into one), ranked by peak-hold;
    select one and capture it.
  - **History** — the session's busiest channels with hit counts, tagged by band.
- **Trigger** — a settable dBm floor that drives the waterfall, the haptic cue and capture.
- **Haptic** — the Flipper vibrates when a bin clears the trigger (hands-free locating), more
  often the stronger it is.
- **Gain** — cap the CC1101 AGC so a strong nearby source doesn't saturate the front end.
- **CSV logging** — long-press OK on any view to record every sweep to a timestamped CSV
  (`/ext/apps_data/rfsurvey/survey_<ts>.csv`, one row per sweep) for analysis on a PC.
- **`.sub` capture** — from Numeric, lock the selected peak and record it (RSSI-gated, like
  the stock RAW recorder) to `/ext/subghz/`, replayable in the stock SubGHz app.

## A note on region

To read signal strength across the full CC1101 range — including bands outside the firmware's
default region window — RF Survey installs a permissive region for its lifetime and restores
the previous one on exit. This only affects **receiving**: the app never transmits. Measuring
how strong a signal is, and where it comes from, is a passive RX operation.

## Build & install

Built with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt):

```bash
ufbt            # build rfsurvey.fap into dist/
ufbt launch     # build, upload to a connected Flipper and run
```

Or copy `dist/rfsurvey.fap` to `/ext/apps/Sub-GHz/` on the SD card.

## Controls

- **Left / Right** — flip views (or move the Bars cursor / Numeric selection).
- **Up / Down** — time window (Connected/Waterfall), or move the Numeric selection.
- **OK (short)** — zoom into the cursor band (Bars) · open capture for the selected peak (Numeric).
- **OK (long)** — start/stop CSV logging (any view).
- **Back** — zoom out, then return to config; from config, exit.

## License

MIT — see [LICENSE](LICENSE).
