# Example Lua mods

Each file here is a small, complete example. To run one, copy it as `init.lua` into a mod folder
(`Mods\modExample\tw3se\init.lua`, it runs when the game starts) or paste a line into the console of the TW3SE Menu
(F2).

- `heal_on_hotkey.lua`: press a key (or a controller combination) to refill health.
- `rich.lua`: add money once at startup.
- `weather.lua`: a function to set the weather from the console.
- `godmode.lua`: keep health topped up every tick while on.
- `double_health.lua`: wrap the player's GetMaxHealth (a WitcherScript method) to return double.
- `log_hits.lua`: observe every hit the player reacts to, without changing anything.
- `spawn.lua`: press P to spawn a copy of the nearest NPC in front of the player.
- `hud.lua`: show health and money in the corner.
- `nearby_hud.lua`: count the actors within 30 m, live.
- `inspect.lua`: press I to dump the player's fields to `tw3se\out\player_dump.txt`.
- `mod_settings.lua`: a mod with its settings in the TW3SE Menu (a checkbox, a slider, a key) and a tab of its own.
- `ui_window.lua`: a window of your own while the TW3SE Menu is open.
- `world_markers.lua`: a marker and a label over every actor nearby.
- `crowd_magnet.lua`: the people nearby go limp and slide toward the player.
- `events.lua`: game events: a kill heals, picked-up items show on screen, a new level gives crowns.
- `damage_numbers.lua`: the damage of each hit over the one who takes it.
- `bounty.lua`: crowns for every kill, by the enemy's level; the total across all games.
- `kill_stats.lua`: what you killed, in this save and in all games, in a tab of its own.
- `rest_anywhere.lua`: a key asks in the game's window whether to wait some hours.
- `sign_refund.lua`: a chance that a sign gives its stamina back.
- `toxicity_warning.lua`: a warning when the toxicity gets high (English and German texts).
- `companion.lua`: a key calls a wolf that follows you and fights beside you.
- `dev_watch.lua`: F9 shows position, health, time, world and the target live over the game.

These call the game's own functions through TW3SE. See `docs/LUA.md` and `lua/types/` for the full API.
