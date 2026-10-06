# Gygax Grid Portal (Chrome extension)

A small Manifest V3 client for the Gygax service. It sends a directive to `POST /directive`, polls `GET /status` for the page built by the Mother/Father/Child agent hierarchy, and shows the result in the popup.

## Use

1. Start the service: `gygax serve` (loopback, no token) or `gygax serve --token-file ~/.gygax-token`.
2. Open `chrome://extensions`, enable Developer mode, choose Load unpacked and select this folder.
3. Open the popup. If the service has a token, paste it under Access and press Save token; it is stored in `chrome.storage.local` and sent as `Authorization: Bearer ...`.
4. Pick a preset or type a directive and send it.

The extension only talks to `http://127.0.0.1:1984` (see `host_permissions` in `manifest.json`); edit the manifest and `CONFIG` in `js/popup.js` and `js/background.js` to point elsewhere.

## Files

```
manifest.json        permissions: activeTab, scripting, storage; host 127.0.0.1:1984
popup.html, css/     UI
js/popup.js          directive submission, polling, token storage
js/background.js     periodic /status health check and badge
icons/               toolbar icons
```
