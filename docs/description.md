RF Survey turns the Flipper Zero into a logging spectrum survey tool. You give it a
frequency range (anywhere the CC1101 radio can tune), it sweeps that range, reads the
signal strength (RSSI) at every step, shows a live spectrum, and records every sweep to a
CSV file on the SD card so you can analyse it on a computer: which frequencies are active,
and whether a source is getting stronger as you move around.

It is a locator and a logger, not a decoder. Think of it as the stock Frequency Analyzer,
but it scans a range you choose and keeps a time history.

What it can do
- Sweep a user-chosen range across all CC1101 bands (300-348, 387-464, 779-928 MHz),
  skipping the gaps the radio cannot tune.
- Live spectrum with per-bin peak-hold, so a short, bursty transmitter still shows up.
- Log every sweep to a timestamped CSV (wide format: one row per sweep, one column per
  frequency) for analysis in a spreadsheet or pandas.
- Walk around and watch a bin rise or fall to find where a signal is coming from.

What it cannot do (be realistic)
- It measures signal strength; it does not decode or identify what a signal is.
- The radio is a CC1101, not an SDR. It only covers the sub-GHz bands above, so it cannot
  see Wi-Fi or Bluetooth (2.4 GHz) devices, and it cannot make sense of cellular (GSM/LTE)
  traffic even when that traffic is in band. It is not a universal bug detector.

Controls
- Back exits.
- (More views and on-device capture are being added.)
