RF Survey turns the Flipper Zero into a logging spectrum survey tool. You give it a
frequency range (anywhere the CC1101 radio can tune), it sweeps that range, reads the
signal strength (RSSI) at every step, shows a live spectrum across several views, records
every sweep to a CSV on the SD card, helps you walk a signal to its source, and can record a
located sub-GHz signal to a .sub you replay in the stock SubGHz app.

Think of it as the stock Frequency Analyzer, but it scans a range you choose, keeps a time
history, and logs everything for analysis on a PC. Sub-GHz RX only: it never transmits.

What it can do
- Sweep a user-chosen range across the CC1101 bands (300-348, 387-464, 779-928 MHz),
  skipping the gaps the radio cannot tune. Pick from a Band list of known allocations
  (433 ISM, 868 ISM, PMR446, LTE800 and more, each tagged) or type an exact Start/End range.
- Five live views: Bars (per-bin peak-hold, cursor, zoom that narrows the RX bandwidth for a
  sharp peak), Connected (per-frequency average over a time window plus a peak envelope),
  Waterfall (horizontal time by frequency heat map with a 5 s to 10 min window and a time
  scale), Numeric (strongest peaks with adjacent bins grouped, ranked by peak-hold), and
  History (the session's busiest channels with hit counts).
- A settable Trigger floor, a haptic locate cue that vibrates when a bin clears the trigger,
  and a Gain control that caps the AGC so a strong nearby source does not saturate the input.
- Log every sweep to a timestamped CSV (one row per sweep, one column per frequency) for
  analysis in a spreadsheet or pandas.
- Capture a located OOK or 2FSK signal to a .sub (RSSI-gated, like the stock RAW recorder)
  that replays in the stock SubGHz app.

What it cannot do (be realistic)
- It measures signal strength and captures static OOK/2FSK bursts; it does not decode or
  identify arbitrary signals.
- The radio is a CC1101, not an SDR. It only covers the sub-GHz bands above, so it cannot see
  Wi-Fi or Bluetooth (2.4 GHz) devices, and it cannot make sense of cellular (GSM/LTE)
  traffic even when that traffic is in band. It is not a universal bug detector.

A note on region: to read signal strength across the full CC1101 range, including bands
outside the firmware default region window, RF Survey installs a permissive region for its
lifetime and restores the previous one on exit. This only affects receiving; the app never
transmits.

Controls
- Left and Right flip views, or move the Bars cursor / Numeric selection.
- Up and Down set the time window, or move the Numeric selection.
- OK short zooms into the cursor band on Bars, or opens capture for the selected peak on
  Numeric. OK long starts and stops CSV logging on any view.
- Back zooms out, then returns to config; from config it exits.
