# Glowroot: an incremental game as Tsugumi Cards (design draft, 2026-11-25)

The goal is to show the final view ABI as a game whose logic is written as Cards and whose screens are Card in Card. One play-through lasts about 10 minutes: grow, buy skills, replant (prestige) 2 or 3 times. This is an original game.

## 1. The game
- **Grow screen.**
  - Click the sprout to gain light (✦).
  - Roots produce light every second.
  - Buy roots: their cost grows 1.15× per purchase.
- **Skill tree screen.** Spend seeds (from replanting) on nodes. There are 3 branches of 4 nodes:
  - Sun: click power;
  - Soil: root output;
  - Seed: replant gain.
  Each node needs its parent.
- **Replant screen.** Available at 1e6 ✦ of lifetime light. You lose light and roots, and gain seeds = floor(sqrt(lifetime / 1e6)).
- **Pacing target** (tuned in params.nix): first replant at about 3 min, second at about 6, third at about 9. After the third, the "Bloom" ending node becomes buyable and shows the end screen.

## 2. Cards (top level = screen flow)
```
[title] ─start─▶ (enter: Merge) ─▶ [grow] ─skills─▶ [skills] ─back─┐
                      ▲              │                              │
                      │              └─replant─▶ [replant] ─done─┐  │
                      └──────────────────────────────────────────┴──┘
[grow] ─bloom─▶ [ending]
```
- Every arrow carries `Turn { state: GameState; view: View }`.
- `View` is the window's session uuid (a capability).
- Each screen Card (grow, skills, replant) is a **subgraph**. Inside it:
  - **page:** a long-lived Card. It calls `route_register(view)`, sends scenes, reads the player's events with `recv`, and leaves through one transition port.
  - **pure rule Cards** run once per transition. For example, `replant/apply` turns a GameState into a replanted GameState and `skills/validate` checks a purchase list. They are ordinary (input) → output Cards, so their results are in the run history and they can be run alone with a saved state, as unit tests.
- `title` opens the window: `view_open("screen", TSUGUMI_VIEW_WINDOW, 1280, 720, &route, &session)`. It sends the title scene and waits for "start".
- The game loop runs inside `grow/page`. The Card is ordinary native code, so it measures time with its language's monotonic clock; `recv(timeout = 100 ms)` only bounds how long it waits for input before it adds the income for the elapsed time.
- Balance lives in params.nix of each Card: cost growth, replant threshold, node costs and effects. Nothing is hard-coded.

## 3. Types (fbs)
```
table GameState { light: double; lifetime: double; roots: [uint]; seeds: uint;
                  nodes: [ubyte]; replants: uint; elapsed_ms: ulong; }
table View  { session: ViewSession; }            // core type, uuid
table Turn  { state: GameState (required); view: View (required); }
// view <-> Card messages (both directions are bytes; these are their types)
table Scene { screen: string; text: [Label]; buttons: [Button]; tree: [Node]; fx: [Fx]; }
table Event { button: string; x: float; y: float; }
```

## 4. The view (one dawn module, "screen")
- **A renderer for this game.** It draws whatever Scene it receives (labels, buttons, tree nodes, small particle effects) and sends an Event when a button is clicked or pressed.
- **No game logic.** The rules stay in the Cards; the view only animates between scenes (counting numbers up, particles).
- **Text** comes from the 5×7 bitmap font used in Halo Warden.
- **Sound** (see section 6): click and buy blips, a replant chime, and a soft generated ambient pad that changes per screen. Scenes ask for sounds with `fx`.

## 5. What it exercises
| Feature | Where |
|---|---|
| `view_open` with WINDOW and a size | title |
| session passed in a port, `route_register` by the next Card | every transition |
| routes closed on leave, window kept | page leaves with `route_unregister` |
| `recv` close event | the player closes the window → the current page ends the run cleanly |
| Card in Card, exports, Merge loop | the top level |
| pure rule Cards as unit tests | replant/apply, skills/validate |
| run history as the game's process | every Turn is a recorded port value |
| keep flag | none needed (no artifacts); a save file later via `hostPaths: saves` |

## 6. Sound without a new effect: a pull export
Sound uses the same direction as `draw`: the host asks and the wasm answers. There is no new import.
```c
/* Optional export. The host calls it on the view's thread, as often as it
 * needs to keep its output buffer filled. The view writes `frames` stereo
 * float32 frames (48 kHz, interleaved) into its own memory and returns the
 * pointer; the host copies them. Missing export = silent view. */
const float* tsugumi_view_audio(uint32_t frames);  /* wasm32: the pointer is an i32 offset into memory; NULL = silence this time */
```
- **Fits the existing rules:**
  - every view call stays on one thread;
  - the host only reads the module's memory, as it does for webgpu arguments;
  - nothing is added to the imports.
- **It is also a clock.** The view learns real time from the frames it is asked for, which fixes Halo Warden's "slower when the window is in the background" (frame-stepped) behaviour.
- **Hosts:**
  - Mac: AVAudioEngine or CoreAudio, with a small ring buffer filled from the view thread;
  - Web: an AudioWorklet fed by postMessage chunks from the page.
- **To confirm with the owner:** the format is fixed at 48 kHz stereo float32 with no negotiation.

## 7. Order
1. Now: this design, and the scene and event schemas.
2. When feat/view-routes has a working SDK on its branch: build the game on that branch, as a test bed that finds ABI holes before merge.
3. After #440, the Input Card removal and view-routes land on main: rebuild on main and ship it as a demo (ja and en). Optionally pack it as a Mac app.
4. Sound: the pull export, as its own small PR after view-routes. The game then gains sound.
