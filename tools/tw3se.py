"""Sends one command to TW3SE in the running game (pipe \\\\.\\pipe\\tw3se) and prints the answer.

    python tw3se.py ping                      -> pong
    python tw3se.py --redkit ping             (the REDkit editor: its own pipe, tw3se_redkit)
    python tw3se.py reloadscripts force       live reload of WitcherScript
    python tw3se.py lua "return w3.player():GetMoney()"
    python tw3se.py help                      every command
    python tw3se.py --pipe tw3se_bg ping      a game whose tw3se.config sets pipe = tw3se_bg

The environment variable TW3SE_PIPE sets the name for every script that imports send().
"""
import os, sys, time

PIPE = "\\\\.\\pipe\\" + os.environ.get("TW3SE_PIPE", "tw3se")


def send(cmd, wait=60.0):
    end = time.time() + wait
    while True:
        try:
            with open(PIPE, "r+b", buffering=0) as f:
                f.write(cmd.encode() + b"\n")
                return f.read(4096).decode(errors="replace").strip()
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.02)  # a moment between two pipe instances: try again at once


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # replies can carry any text (compiler messages)
    args = sys.argv[1:]
    if args[:1] == ["--redkit"]:  # the TW3SE inside the REDkit editor has its own pipe
        PIPE = r"\\.\pipe\tw3se_redkit"
        args = args[1:]
    elif args[:1] == ["--pipe"] and len(args) > 1:  # a game with its own pipe = <name> in tw3se.config
        PIPE = "\\\\.\\pipe\\" + args[1]
        args = args[2:]
    print(send(" ".join(args) or "ping"))
