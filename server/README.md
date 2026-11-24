# Matchmaking server

`matchmaker.py` pairs players for quick play, private rooms (six-letter codes) and King of the Hill (the
winner stays on; challengers wait in line, a loser goes to the back). It tells the two games each other's
public address so they connect directly, and relays their packets through itself when the routers between
them won't allow that. The games run the match themselves (rollback netplay); the server sees only a few
small packets a second per match, or the match's input packets when relaying (about 30 kB/s per match).
For King of the Hill it also keeps each match's inputs (4 bytes a field, about 15 kB a minute) and hands them
to the players waiting in line, who watch the fight by running it themselves.

Python 3.8 or later, standard library only. One UDP port, 7777 unless `PORT` says otherwise.

## Deploying on fly.io

```
cd port/server
fly launch --no-deploy --copy-config --name <your-app-name>   # or edit `app` in fly.toml
fly ips allocate-v4                                            # UDP needs a dedicated IPv4 address
fly deploy
fly logs                                                       # "match ... : A vs B" as players pair up
```

`fly.toml` keeps one machine running (UDP traffic doesn't wake a stopped machine). On fly.io the server binds
the `fly-global-services` address, which UDP services need there.

Point the game at it: `server = <your-app-name>.fly.dev:7777` in `dojo.ini` (beside `dojo.exe`), or
`DOJO_SERVER` for a session. The default is `dojo-matchmaker.fly.dev:7777`.

## Running it locally

```
python port/server/matchmaker.py
DOJO_SERVER=127.0.0.1:7777 port/build/dojo.exe
```
