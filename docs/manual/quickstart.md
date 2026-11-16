# Quickstart

## 1. Install

Unpack `TW3SE-1.2.1.zip` into the game folder (or install it with Vortex). The game then has:

```
bin\x64_dx12\dinput8.dll       the loader
bin\x64_dx12\tw3se\tw3se.dll   TW3SE
```

## 2. Check the log

Start the game and open `bin\x64_dx12\tw3se\tw3se.log`:

```
TW3SE 1.2.1 in pid ..., witcher3.exe build 6ac555a3
address CCommonGame::Tick: found by signature
...
addresses: ..., 0 missing (unknown build 6ac555a3)
ready
```

`0 missing` means TW3SE found everything it needs. If something is missing, the log names it and TW3SE stays
passive. "unknown build" is the normal case on 5.01: TW3SE finds each address by its bytes.

## 3. Talk to the game (optional)

TW3SE opens the pipe `\\.\pipe\tw3se`. Write a line, read the answer. With Python:

```python
with open(r"\\.\pipe\tw3se", "r+b", buffering=0) as p:
    p.write(b"version\n")
    print(p.read(4096).decode())    # TW3SE 1.2.1, interface 21, game build 6ac555a3
```

## 4. Next

- Lua: [Your first Lua mod](first-lua-mod.md), then [Lua](../LUA.md)
- WitcherScript: [Script natives](script-natives.md), [Live reload](live-reload.md) and [World edits](world-edit.md)
- C++: [Your first plugin](first-plugin.md)
