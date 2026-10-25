# OSINT and geolocation

`gygax::intel` (`include/gygax/intel/`) turns open-source event feeds and photograph metadata into assessments you can
act on. It has no dependencies beyond the Gygax core.

## Events and levels

An `Event` has a position, a `Level` (Local, Regional, National, Theater, Global), a `Domain` (infrastructure, maritime,
cyber, aerospace, geophysical, security, other), a severity from 1 to 5, a source, a time and the source's own
confidence. `OsintEngine::loadJson` accepts `{"events":[...]}`, a bare array, or a GeoJSON FeatureCollection of points
(USGS earthquake feeds work as they are). `loadFile` reads a file; `fetch` does a plain HTTP GET (no TLS).

A monitor at a level sees events at that level or wider, and lower-level events that are severe enough: severity at
least `min(5, 3 + gap)`, where `gap` is how many levels the event sits below the monitor.

## Confidence, threat and reconciliation

- Reports of the same domain from different sources within 25 km and 24 h corroborate each other: combined confidence is
  `1 - Π(1 - c)`.
- Threat score is `Σ severity × confidence × 0.5^(age / 6 h)` over events near the position. Thresholds: 0.5 advisory,
  2 elevated, 5 critical. A severity 5 event with confidence 0.6 or more is always critical.
- `reconcile` compares a status report with that evidence. A "secure" claim against elevated-or-worse evidence, or a
  "hostile" claim against clear evidence, is a discrepancy.

## Image geolocation

`parseImage` reads EXIF (TIFF, JPEG APP1, PNG `eXIf`) and IPTC (JPEG APP13): GPS position and heading, camera, software,
date, title, caption, city, country and keywords. Every read is bounds-checked; malformed files fail cleanly.
`buildTiff` and `buildJpeg` write the same fields.

`verifyLocation` finds the nearest reference site (five landmarks are built in; add yours with `addSite` or
`loadSites`). A fix inside the site's radius scores high; confidence drops with distance, when the caption names a
different site, and when the software tag shows an image editor. Verified means confidence of 0.8 or more.

## Service tools

`osint.ingest` (events as JSON), `osint.feed` (`level`, `domain`), `osint.assess` (`latitude`, `longitude`,
`radius_km`) and `geo.verify` (`latitude`, `longitude`, `description`, `software`). They take their data inline; none
reads files or opens connections on a caller's behalf.

## Example

```bash
./build/recon_survey --evac-test --seconds 2      # sensors contradict a "secure" report: exits 10, writes a report
./build/recon_survey --verify-image photo.jpg     # prints the geolocation verdict as JSON
```

In the interactive HUD (`--keys`): `O` toggles the overlay, `L` cycles its level, `E` forces evacuation, `Q` quits.
