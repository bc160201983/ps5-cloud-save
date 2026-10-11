# Design notes from ProsperoMgr

Notes on what makes [ProsperoMgr](https://github.com/notmaj0r/ProsperoMgr)'s web
dashboard feel clean, and how each idea could apply to PSCloud. ProsperoMgr is
GPLv3 and built with React and Tailwind; these are design observations only, and no
code is copied. PSCloud stays a single embedded HTML page.

ProsperoMgr also detects the running game with
`sceSystemServiceGetAppIdOfRunningBigApp()` plus `sceSystemServiceGetAppTitleId()`,
which PSCloud adopted in 0.18.2.

## What ProsperoMgr does

### 1. One small set of colour tokens, each with one meaning
- Graphite/navy surfaces: background, panel, raised panel and border, each a step
  apart.
- **Accent (blue)** only means "primary action, selection, navigation".
- A **separate info colour** means "something is happening" (transfers, progress).
  Keeping these two apart stops one colour from tinting the whole app.
- **Green is reserved for real status** (running, installed, signed in) and never
  used for decoration. The same applies to warning yellow and error red.
- A full light theme redefines the same tokens.

### 2. Motion tokens
A few fixed durations (about 120, 180, 260, 360 and 480 ms) and easing curves.
Every animation uses one of them, so the app feels consistent. Game tiles fade in
with a small stagger.

### 3. Home page order: status, now playing, quick actions, library
1. **Status strip:** a greeting with the user's name, a coloured connection dot and
   3 small stats (temperature, autoloader, uptime). Each stat is a link to its page.
2. **Currently playing card,** shown only while a game runs: icon, a green "live"
   pill, the name, the title ID and one secondary action.
3. **Quick actions:** a grid of 5 tiles, each with an icon square, a bold label, a
   one-line description and a chevron.
4. **Game library panel:** a toolbar (search, sort drop-down, a PS5/PS4 segmented
   filter, refresh), an "N titles" count, then the tiles.

### 4. Game tiles
- Horizontal: cover art on the left (about 120–140 px), details on the right.
- Name in bold (2 lines at most), then a monospace title-ID chip.
- One short metadata line in priority order (playtime, last played, version).
  **Anything unknown is left out**, never padded with filler text.
- One rounded primary button at the bottom right.
- A running game gets a green border and a "Running" pill on its art.

### 5. Consistent building blocks
- **Panel:** a header bar with the title on the left and actions on the right, a
  thin divider, then the body.
- **Pill:** small rounded status labels in neutral, ok, warn, error and info tones;
  an optional pulsing dot means "live".
- **Empty state:** an icon, a title, then **what to do next** and an action button,
  not just "nothing here".
- Loading skeletons instead of "Loading…" text.

### 6. Notifications
- Toasts stack in the top-right corner, close themselves after a fixed time, and
  close when tapped.
- Every toast is also kept in a **notifications bell** (the last 50), so nothing is
  lost when it disappears.
- Long jobs show in one persistent **background activity** indicator instead of
  pop-ups.

### 7. Navigation
- Pages are grouped: Main, Tools, Advanced, System. Logs and processes sit in
  Advanced, out of the way.
- The side rail can collapse to icons.
- On phones, a bottom bar holds the 4 main pages plus "More".
- A Ctrl/Cmd+K command palette jumps anywhere.

### 8. Save backups page
- A user filter (all users, or one).
- "Back up now", plus a "Back up all" action with progress.
- Backup history: the newest marked "latest", older ones grouped below.
- Import, download as ZIP, restore and delete, each with a clear confirmation.
- The automatic backup watcher is a single toggle with a one-line description.

### 9. Details that matter on PS5 browsers
- The PS5's own WebKit browser lacks `aspect-ratio` and `dvh` units, so fallbacks
  are provided. PSCloud should avoid both or add fallbacks, since some people open
  it on the console itself.

## How this maps to PSCloud

| Idea | PSCloud today | Suggested change |
|---|---|---|
| Colour tokens with fixed meanings | Purple accent used for actions, badges and progress alike | Define tokens; one accent for actions, a separate info colour for uploads and progress, green only for "running" and "uploaded" |
| Status strip | Large marketing-style banner ("Your progress. Ready when you are.") | Compact strip: greeting with profile name, cloud status dot, queued uploads, last backup time, each linking to its page |
| Currently playing | Now playing card inside the banner (0.19.0) | Its own slim card with a green live pill and "Back up now after closing" status |
| Quick actions | Receive panel is a large block on the home page | Tiles: Receive a shared save, Upload queue, Back up all, Cloud settings |
| Game tiles | Tall vertical cards; generic subtitle text | Horizontal tiles with art left, last backup date and slot count, running state, one primary button |
| Library toolbar | Search and refresh | Search, sort (name, last backup), filter (installed, saves only), count |
| Panel, Pill, Empty state | Ad-hoc styles per section | One panel style, pills for status, empty states that say what to do next |
| Notifications | Bottom toasts; separate upload notice | Top-right stack plus a bell with recent history; uploads in one background-activity indicator |
| Navigation | 4 flat items | Groups (Library: My games, Uploads, Receive; System: Cloud settings, Activity); bottom bar on phones |
| Light theme | Dark only | Optional, through the same tokens |

**Suggested order:** tokens and building blocks first (low risk, used everywhere),
then the home page (status strip, now playing, quick actions), then the game tiles
and toolbar, then notifications and navigation.
