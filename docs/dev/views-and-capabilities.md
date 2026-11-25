# Views, routes and capabilities (owner decisions, 2026-11-24; ABI confirmed final by the owner)

This replaces the view parts of decisions-2026-11-23 rev 3 (D, G), the artifact-id and grant parts of section E, and decisions-2026-11-24-pending.md. Items marked **[proposed]** were not confirmed by the owner yet.

## 1. Capabilities: knowing a uuid is the permission
- Artifact ids, view session ids and route ids are random 128-bit values (uuid). They cannot be predicted, so holding one proves that it arrived through an input, an output or a message.
- **The core keeps no per-call grant lists.** An operation on an id only checks that the id exists in this project (and, for artifacts, that it is sealed).
- `CardPort.artifacts` is removed. The SDK no longer scans payloads for ids, and the core does not check which ids an output lists.
- **Showing an id to someone gives them access.** Do not put ids where their reader should not have them. Ids shown in run history are usable by whoever can read that history, including the Agent; this is intended.

## 2. Artifact ids
- The id is a random 128-bit uuid everywhere outside the core: in the ABI, in payloads (`table Artifact { id: ArtifactId; }`), in Agent tools and in host APIs.
- Inside the core, storage stays numbered for people: `.tsugumi/artifacts/<run>/<card path>/@<invocation index>/<k>/`. The history db maps the uuid to (invocation sequence, k). The UI may show the `N.k` label next to the uuid.
- `artifact_create(host, flags, &id, &path)` returns a new uuid; flags has `TSUGUMI_ARTIFACT_KEEP` (SDK default on). Without KEEP the artifact lives only while its run runs and is removed when the run ends; `artifact_open(uuid)` links the sealed artifact into the call's own directory (read-only) and returns its path.

## 3. Views
- **Declaration.** A Card declares views in default.nix as before: `views = [ { name; type; blob; } ]`, where `type` is `html` or `dawn`. The names are what `view_open` accepts; nothing else about views is sent to the Card.
- **Opening.** A window exists only because a Card opened it with `view_open`. The user cannot open a view by hand, and the UI no longer lists declared views before a run. View traffic is live: results to keep go to output ports and artifacts.
- **Addresses.** There are two kinds:
  - **Session:** one open window. Its uuid is created by `view_open` and is the address Cards `send` to.
  - **Route:** one Card process's mailbox for one session. Its uuid is created by `view_open` or `route_register` and is the address the view sends to.
- **Delivery is always to one address (1:1).** Nothing is broadcast.
- **Passing a window on.** A session uuid can be put in a port value like any other value. A Card that receives it can `send` to that window, and can `route_register` its own route for it.

## 4. Card ABI (view part of `tsugumi_effects`)
```c
typedef struct { uint8_t bytes[16]; } tsugumi_view_session;  /* a window */
typedef struct { uint8_t bytes[16]; } tsugumi_route;         /* a Card's mailbox for one window */

typedef struct {
  uint32_t kind;                  /* 0 message, 1 close */
  tsugumi_route route;            /* the route it was addressed to */
  tsugumi_view_session session;   /* the window it came from */
  tsugumi_bytes data;             /* message: the bytes; close: empty */
} tsugumi_view_event;

enum {
  TSUGUMI_VIEW_PANEL  = 0,  /* a panel inside the project's window, next to the Card */
  TSUGUMI_VIEW_WINDOW = 1,  /* a window of its own */
  TSUGUMI_VIEW_FULLSCREEN = 2,  /* a window of its own, full screen */
};

/* Open a declared view (html or dawn). Returns the new session and a route of
 * this process bound to it. Unknown names and unknown modes are an error.
 * width/height: the window's initial content size in logical pixels, for
 * TSUGUMI_VIEW_WINDOW; 0,0 lets the UI choose. Only one of them 0, or a
 * nonzero size with TSUGUMI_VIEW_PANEL or TSUGUMI_VIEW_FULLSCREEN, is an error. */
int (*view_open)(void* host, const char* view, uint32_t mode,
                 uint32_t width, uint32_t height,
                 tsugumi_route* route, tsugumi_view_session* session);

/* Create a route of this process for an existing window (a session
 * received in a port value); drop a route. */
int (*route_register)(void* host, const tsugumi_view_session* session, tsugumi_route* route);
int (*route_unregister)(void* host, const tsugumi_route* route);

/* Send to a window. At most TSUGUMI_MESSAGE_MAX bytes, else an immediate error.
 * If the window is closed, the message is dropped and 0 is returned.
 * Blocks (cancellably) only while the next hop has no room for one max-size message. */
int (*send)(void* host, const tsugumi_view_session* to,
            const uint8_t* bytes, size_t size);

/* Receive one event addressed to any route of this process.
 * 0 event, 1 timeout, 2 cancelled, <0 error. timeout_ms < 0 = no timeout. */
int (*recv)(void* host, tsugumi_view_event* out, int64_t timeout_ms);
```
- **Mode.** The mode only says where the view first appears. Afterwards the user may detach, move or resize it; presentation stays the UI's. Mac app: PANEL = a panel in the project window, WINDOW = a separate window. Web UI: PANEL = a panel over the canvas, WINDOW = a child window (`window.open`) sharing the page's connection. A packed app has no canvas: PANEL is the app's main window. FULLSCREEN: Mac app and packed app = a separate window in native full screen; Web UI = a child window that asks the user to click to enter full screen (browsers allow full screen only from a user gesture, and Esc always leaves it).
- **Valid outside a call.** `view_open`, `route_register`, `route_unregister`, `send` and `recv` also work after `run` has returned, for example in a resident (`reuseWorker`) process's own threads. `cancelled`, `remaining_ms`, `artifact_create` and `artifact_open` stay valid only during the call.
- **Routes** live until `route_unregister` or until their process ends. Messages to a dead route are dropped.
- **Close.** When the user closes a window, every live route bound to that session receives one `close` event. The session then ends: later sends to it are dropped.
- **A window outlives its opener (owner, confirmed).** If every route of a window is gone, the window stays open (showing its last frame) until the user closes it.

## 5. dawn view ABI (`tsugumi_view.h`)
```c
/* Send to a route of this window. A route of all zero bytes means the route
 * that view_open returned (the Card that opened this window). A route not
 * bound to this window is an error. */
int32_t tsugumi_view_send(const uint8_t* route /* 16 bytes */,
                          const uint8_t* data, uint32_t size);
int32_t tsugumi_view_read(uint8_t* dst, uint32_t size);   /* only inside message */
```
- The exports are unchanged: `abi`, `start`, `resize`, `draw`, and the optional `message`, `pointer` and `key`.
- The view never needs its own session id. The host knows which window it is.

## 6. html view API (JS)
```js
tsugumi.onmessage = (bytes) => { /* a Card's send to this window */ };
tsugumi.send(bytes, route /* Uint8Array(16), optional: the opener's route */);
```

## 7. Example: screens as Card in Card
```
title:  view_open("game") -> route R1, session S
        send(S, title screen) ... recv: "start" from S on R1
        output { state, view: S } to play, and end its call
play:   route_register(S) -> R2
        send(S, "send to R2 from now on") ... recv on R2
        output { state, view: S } to shop when the player opens the shop
shop:   the same, with R3
```

## 8. Recording
`send` and `recv` are side effects. What to keep is the Card's job: write it to output ports or logs. The core records no view traffic.

## 9. Removed
- `send(view name)` and `recv(view name)`;
- opening views by hand and the pre-run view list in the UI;
- `CardPort.artifacts` and every per-call grant list or grant check on ids;
- sequential artifact ids in the ABI;
- the earlier route-by-Card-id and register-a-session sketches.

## 10. Order
Implement after the open PRs that touch the same code have landed: fix/seal-clonefile, #440 and feat/remove-input-cards. Then update both UIs, both view hosts, the kit SDKs (send/recv/view_open/route in every language), the templates, the demos (point-cloud, live-stream, Halo Warden's pattern), docs/CardABI.md and docs/CoreAndSDK.md.

## 11. Time and sound (owner, 2026-11-25; part of the final view ABI, implemented right after feat/view-routes)
```c
/* The view clock: time since the view started, in 48 kHz frames. 64 bits never wrap in practice. */
typedef uint64_t tsugumi_view_time;

/* Exports (the host calls them; no new imports). */
int32_t tsugumi_view_draw(WGPUTextureView target, tsugumi_view_time present); /* when this frame will be shown */
const float* tsugumi_view_audio(uint32_t frames, tsugumi_view_time heard);
    /* optional. Fill `frames` stereo float32 frames (48 kHz, interleaved) in the view's memory and
     * return the pointer (wasm32: i32 offset); NULL = silence this time. `heard` is when the first of
     * these frames reaches the listener, output latency included. The host decides timing; the view
     * places sounds inside the buffer at offset = target_time - heard. Called on the view's thread. */
int32_t tsugumi_view_pointer(uint32_t kind, float x, float y, uint32_t buttons,
                             float wheel_x, float wheel_y, uint32_t modifiers, tsugumi_view_time at);
int32_t tsugumi_view_key(uint32_t kind, uint32_t code, uint32_t modifiers, tsugumi_view_time at);
```
- `at` is when the input happened, converted by the host from the OS event time to the view clock.
- The clock runs whether or not the view exports `audio`.
- **Confirmed:** `message` and `resize` also take a `tsugumi_view_time at`, so every call carries the current time.
- Hosts: Mac CoreAudio / AVAudioEngine fed from the view thread through a small ring buffer; Web AudioWorklet fed by postMessage chunks.

## 12. Further view decisions (owner, 2026-11-25)
- **Input additions to the dawn ABI:** `tsugumi_view_text` (committed UTF-8), `tsugumi_view_text_input()` (the host asks after draw where text is entered; NULL = no IME), `tsugumi_view_composition` (IME composition), `tsugumi_view_gamepad` (W3C standard layout). All exports, no new imports. The header as implemented is `protocol/include/tsugumi/tsugumi_view.h` (kit `abi/tsugumi_view.h` is byte-identical).
- **view_open flags:** `view_open(host, view, mode, flags, width, height, &route, &session)` with `TSUGUMI_VIEW_HIDE_CURSOR` and `TSUGUMI_VIEW_LOCK_POINTER` (while locked, pointer x/y carry movement; the user can always release it).
- **Fonts:** a kit dawn SDK helper (embedded font rendered into a texture), not ABI.
- **html views:** nothing is added; they use browser APIs (time, Web Audio, inputs, Gamepad API) directly.
- **Agent and views:** the host lets the Agent send input events (pointer, key, text, gamepad) to an open window and take a snapshot of it, through the same export calls. It doubles as automated testing of views and games.
- **Stuck views (confirmed):** never killed automatically. If one call takes more than 0.5 s, the UI overlays "waiting for the view to respond" with a Stop button; the person decides. Mac: the view runs on its own thread, and wasmtime epoch callbacks drive the overlay. Web: the view runs in a Worker (OffscreenCanvas + WebGPU), so the page stays responsive and Stop is worker.terminate().

## 13. Answers to the view-routes questions (owner, 2026-11-25)
- **Web detachment:** the Web UI offers no panel-to-window detachment after opening (an iframe moved to another document reloads). The Card's mode at `view_open` decides. If the browser blocks the WINDOW or FULLSCREEN popup, the panel shows an "Open in a window" button that the user clicks. The Mac app keeps detachment.
- **`view_open` with no UI:** returns a normal session and route; sends to it are dropped and `recv` gets nothing from it. No error and no "not shown" flag.
- **Packed apps target macOS 26 or later** (the prebuilt Dawn). Building Dawn from source for older macOS is for later.

## 14. Web UI windows are in-page virtual windows (owner, 2026-11-25; replaces the Web parts of section 13)
- WINDOW and FULLSCREEN in the Web UI never use browser popups. They are floating in-page windows like the Card settings window: same document, same Worker, GPU device and connection; no popup blocker; no "Open in a window" button.
- WINDOW: a movable, resizable in-page window with the requested content size. FULLSCREEN: the in-page window fills the browser page without a click; a "Full screen" button enters real browser full screen on a user click.
- Panel <-> window detachment is allowed again in the Web UI (only position and size change; the view's DOM is not moved to another document, so html views keep their state).
- Every virtual window can be minimized (hidden to a small strip of minimized windows) and restored. A minimized view keeps running: messages, recv, audio and input routing continue; the host may skip draw calls while it is hidden. Closing the window ends the session (close event to its routes).
- To implement right after feat/view-time-input lands.
- (2026-11-25, follows from "html views get nothing added") Cursor flags apply to dawn views only; html views use the browser's own cursor and pointer-lock APIs. Agent snapshots of html views: Mac app only (WKWebView snapshot); the Web UI snapshots dawn views only, because html runs in opaque-origin sandboxed iframes.

## 15. view_close (owner, 2026-11-25)
`int (*view_close)(void* host, const tsugumi_view_session* session);` Any holder of the session uuid may close the window. Every live route of it receives close; later sends are dropped; closing a closed window does nothing. Valid outside a call. `route_register` on a closed session fails with a distinct "window closed" error (not a generic one).

## 16. View messages are addressed by Card path (owner, 2026-11-25; replaces the route argument of tsugumi_view_send)
- dawn: `int32_t tsugumi_view_send(const char* to, const uint8_t* data, uint32_t size);` where `to` is a Card path from the project root ("grow", "screens/skills") or "*" for every Card registered on this window. html: `tsugumi.send(bytes, to)`.
- A message reaches the live processes of that Card that are registered on this window (`view_open` registers the opener; `route_register(session)` registers another Card). "*" reaches all of them. Unknown or unregistered paths: dropped and reported to the view as an error return.
- No implicit address: the all-zero route meaning "the opener" is removed. Route uuids stay Card-side only (returned by view_open/route_register, used for route_unregister); views never see them. A Card no longer has to tell the view its route.
- Glowroot (en, ja) addresses its screen Cards by path.
