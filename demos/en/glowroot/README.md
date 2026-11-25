# Glowroot

A small incremental game whose screens are Cards. Run the Workflow: `title/page`
opens the game's window (a Dawn view), and each screen in turn draws to the
same window. Click the sprout for light, buy roots that grow light on their
own, replant for seeds and spend them on the skill tree. After the third
replant the Bloom node ends the game, in about ten minutes.

Every arrow carries a `Turn`: the game's state and the window's session.
The run history shows each Turn, so the game can be followed screen by
screen. `replant/apply` and `skills/validate` are pure rules: rerun them
alone with their saved inputs to check a rule. The balance is in the
`params.nix` of each Card.
