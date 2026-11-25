---
title: Title
description: Opens the window and starts a game
---

Opens the `screen` view in its own 1280×720 window with `view_open` and shows the title. When the player presses Begin, it sends the first `Turn` (a new state and the window's session) and ends. The `screen` view is declared here and shared by every screen. `params.nix` holds the title text and the size of a new game.
