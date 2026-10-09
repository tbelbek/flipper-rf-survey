## 1.0
- Range config: Band presets (full-spectrum / full-band / ~26 tagged known allocations) plus
  a Frequency-Analyzer-style custom Start/End editor, modulation preset and step.
- Five live views: Bars (peak-hold, cursor, zoom that narrows RX bandwidth), Connected
  (windowed average plus peak envelope), Waterfall (horizontal time-by-frequency with a
  5 s to 10 min window and a time scale), Numeric (strongest peaks, adjacent bins grouped,
  select-to-capture), and History (busiest channels with hit counts).
- Trigger floor, haptic locate cue (vibrate above trigger), and an AGC Gain cap.
- CSV logging of every sweep to a timestamped file (long-press OK).
- On-device, RSSI-gated .sub capture of a located OOK/2FSK signal, replayable in SubGHz.
- Sub-GHz RX only, never transmits.

## 0.1
- Initial skeleton: sweep a fixed 779-928 MHz range, read RSSI per step, live bar
  spectrum with per-bin peak-hold, background sweep worker.
