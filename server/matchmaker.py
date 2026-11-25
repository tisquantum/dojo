"""Dojo matchmaking server: quick play, private rooms, and King of the Hill.

One UDP port. Players register, ask for a kind of match, and are paired; the server tells each player the
other's public address so the two games can talk directly, and relays their traffic when the routers between
them won't allow that. The games themselves run the match (rollback netplay, port/src/netplay.c).

Messages are text, "BBMM <WORD> args..." (player <-> server); relayed packets are "BBRL" + a little-endian
32-bit match id + the packet, forwarded to the match's other player unchanged.

King of the Hill can be watched: the players of a hill's match send its inputs as both have settled them
("BBSP" packets: match id, first field, count, the simulation's ticks per frame, the total so far, then each
field's two controllers as a 32-bit word), the server keeps the match's log, and the players waiting in line
pull it ("WATCH <match id> <field>") to run the same fight themselves a moment behind. SPACK tells the
players how much of the log has come; SPECTATE and SPECEND tell those in line a match began and ended.

Player -> server                          Server -> player
  HELLO <protocol> <disc> <name>            WELCOME <public ip:port>
  PING                                      PONG
  QUICK                                     QUEUED <players online>
  CREATE                                    ROOM <code>
  JOIN <code>                               ERROR <text>
  KOTH <code or ->                          HILL <code or -> <position> <queue length> <streak> <king>
  RESULT <match id> <winning side>          MATCH <match id> <side> <peer ip> <peer port> <peer name>
  LEAVE                                     PEERLEFT <match id>
  BYE                                       SPECTATE <match id> <king> <challenger>
  DECLINE <match id> <ms>                   DECLINED <match id> <ms>   (quick play: too far apart; both
                                            are queued again, and not paired with each other for a while)
  WATCH <match id> <field>                  SPECEND <match id> <fields>
  (BBSP packets)                            SPACK <match id> <fields>, (BBSP packets)

On fly.io the UDP socket must be bound to the fly-global-services address (and the app needs a dedicated
IPv4 address); elsewhere it listens on every address. PORT (default 7777) sets the port. """

import asyncio
import os
import random
import socket
import string
import struct
import time

PROTOCOL = 1
CLIENT_TIMEOUT = 15.0
ROOM_LETTERS = string.ascii_uppercase
SPECTATE_HEADER = struct.Struct('<4sIIHHI')   # magic, match id, first field, count, ticks, total
SPECTATE_BATCH = 120                         # fields a packet carries
ENDED_LOG_SECONDS = 60.0                     # how long a finished match can still be watched


class Client:
    def __init__(self, addr):
        self.addr = addr
        self.name = 'Samurai'
        self.disc = ''
        self.seen = time.monotonic()
        self.where = None      # None, 'quick', ('room', code), ('hill', code)
        self.match = None


class Match:
    def __init__(self, match_id, players, mode, hill=None):
        self.id = match_id
        self.players = players  # [side 0, side 1]
        self.mode = mode        # 'quick', 'room', 'hill'
        self.hill = hill
        self.log = []           # settled inputs, a 32-bit word a field (King of the Hill, for watching)
        self.ticks = 1
        self.ended = None       # when it ended, for keeping its log a while


class Hill:
    def __init__(self, code):
        self.code = code        # '' for the public hill
        self.queue = []         # clients waiting, first in line first
        self.king = None
        self.streak = 0
        self.match = None


class Matchmaker(asyncio.DatagramProtocol):
    def __init__(self):
        self.clients = {}
        self.quick = []
        self.rooms = {}
        self.hills = {'': Hill('')}
        self.matches = {}
        self.ended = {}         # match id -> Match, finished but still watchable
        self.next_id = random.randint(1, 1 << 20)
        self.apart = {}         # frozenset of two addresses -> until when quick play keeps them apart

    # -- plumbing ------------------------------------------------------------------------------------------

    def connection_made(self, transport):
        self.transport = transport

    def error_received(self, exc):
        # Windows reports a send to a player who has gone as an error on a later receive: no matter.
        pass

    def send(self, client, text):
        self.transport.sendto(('BBMM ' + text).encode(), client.addr)

    def datagram_received(self, data, addr):
        if data[:4] == b'BBRL' and len(data) >= 8:
            self.relay(data, addr)
            return
        if data[:4] == b'BBSP' and len(data) >= SPECTATE_HEADER.size:
            self.log_inputs(data, addr)
            return
        if data[:5] != b'BBMM ':
            return
        try:
            words = data[5:].decode('utf-8', 'replace').split()
        except Exception:
            return
        if not words:
            return
        client = self.clients.get(addr)
        if words[0] == 'HELLO':
            self.hello(addr, words[1:])
            return
        if client is None:
            self.transport.sendto(b'BBMM ERROR Say hello first.', addr)
            return
        client.seen = time.monotonic()
        handler = getattr(self, 'cmd_' + words[0].lower(), None)
        if handler:
            handler(client, words[1:])

    def relay(self, data, addr):
        client = self.clients.get(addr)
        if client is None or client.match is None:
            return
        (match_id,) = struct.unpack_from('<I', data, 4)
        match = client.match
        if match.id != match_id:
            return
        client.seen = time.monotonic()
        for other in match.players:
            if other is not client:
                self.transport.sendto(data, other.addr)

    def log_inputs(self, data, addr):
        """A hill match's player sends settled inputs: the log grows, and the players hear how far it is."""
        client = self.clients.get(addr)
        if client is None or client.match is None:
            return
        _, match_id, first, count, ticks, _ = SPECTATE_HEADER.unpack_from(data)
        match = client.match
        if match.id != match_id or match.hill is None:
            return
        client.seen = time.monotonic()
        match.ticks = ticks or 1
        words = struct.unpack_from('<%dI' % count, data, SPECTATE_HEADER.size) if len(data) >= SPECTATE_HEADER.size + 4 * count else ()
        if words and first <= len(match.log) < first + len(words):
            match.log.extend(words[len(match.log) - first:])
        self.send(client, 'SPACK %d %d' % (match.id, len(match.log)))

    def cmd_watch(self, client, args):
        """A player in line asks for a match's inputs from a field on."""
        try:
            match_id, start = int(args[0]), int(args[1])
        except (IndexError, ValueError):
            return
        match = self.matches.get(match_id) or self.ended.get(match_id)
        if match is None:
            return
        for batch in range(4):
            first = start + batch * SPECTATE_BATCH
            words = match.log[first:first + SPECTATE_BATCH]
            if not words:
                break
            packet = SPECTATE_HEADER.pack(b'BBSP', match.id, first, len(words), match.ticks, len(match.log))
            self.transport.sendto(packet + struct.pack('<%dI' % len(words), *words), client.addr)
        if match.ended is not None and start >= len(match.log):
            self.send(client, 'SPECEND %d %d' % (match.id, len(match.log)))

    def announce_spectate(self, hill, clients):
        match = hill.match
        if match is None:
            return
        for client in clients:
            self.send(client, 'SPECTATE %d %s %s' % (match.id, match.players[0].name, match.players[1].name))

    # -- commands ------------------------------------------------------------------------------------------

    def hello(self, addr, args):
        if len(args) < 3:
            return
        try:
            protocol = int(args[0])
        except ValueError:
            return
        if protocol != PROTOCOL:
            self.transport.sendto(b'BBMM ERROR This server needs another version of the game. Update it.', addr)
            return
        client = self.clients.get(addr) or Client(addr)
        client.disc = args[1]
        client.name = ''.join(c for c in args[2] if c.isalnum() or c in '_-')[:20] or 'Samurai'
        client.seen = time.monotonic()
        self.clients[addr] = client
        self.send(client, 'WELCOME %s:%d' % addr)

    def cmd_ping(self, client, args):
        self.send(client, 'PONG')

    def cmd_bye(self, client, args):
        self.leave(client)
        self.clients.pop(client.addr, None)

    def cmd_leave(self, client, args):
        self.leave(client)

    def kept_apart(self, a, b):
        until = self.apart.get(frozenset((a.addr, b.addr)))
        return until is not None and until > time.monotonic()

    def cmd_quick(self, client, args):
        self.leave(client)
        self.queue_quick(client)

    def queue_quick(self, client, front=False):
        partner = next((c for c in self.quick if c.disc == client.disc and not self.kept_apart(c, client)), None)
        if partner:
            self.quick.remove(partner)
            partner.where = None
            self.start_match([partner, client], 'quick')
            return
        client.where = 'quick'
        if front:
            self.quick.insert(0, client)
        else:
            self.quick.append(client)
        self.send(client, 'QUEUED %d' % len(self.clients))

    def cmd_decline(self, client, args):
        """A quick match one of the two found too far apart: both go back to the front of the queue, kept
        apart from each other for ten minutes."""
        try:
            match = self.matches.get(int(args[0]))
            ms = int(args[1]) if len(args) > 1 else 0
        except (ValueError, IndexError):
            return
        if match is None or match.mode != 'quick' or client not in match.players:
            return
        a, b = match.players
        self.apart[frozenset((a.addr, b.addr))] = time.monotonic() + 600
        self.end_match(match, None)
        for player in (a, b):
            self.send(player, 'DECLINED %d %d' % (match.id, ms))
        for player in (b, a):
            self.queue_quick(player, front=True)
        print('match %d declined at %d ms' % (match.id, ms), flush=True)

    def cmd_create(self, client, args):
        self.leave(client)
        while True:
            code = ''.join(random.choice(ROOM_LETTERS) for _ in range(6))
            if code not in self.rooms and code not in self.hills:
                break
        self.rooms[code] = client
        client.where = ('room', code)
        self.send(client, 'ROOM ' + code)

    def cmd_join(self, client, args):
        code = (args[0] if args else '').upper()
        owner = self.rooms.get(code)
        if owner is None or owner is client:
            self.send(client, 'ERROR No room has that code.')
            return
        if owner.disc != client.disc:
            self.send(client, 'ERROR That room is for another disc.')
            return
        self.leave(client)
        del self.rooms[code]
        owner.where = None
        self.start_match([owner, client], 'room')

    def cmd_koth(self, client, args):
        code = (args[0] if args else '-').upper()
        code = '' if code == '-' else code
        self.leave(client)
        hill = self.hills.get(code)
        if hill is None:
            hill = self.hills[code] = Hill(code)
        client.where = ('hill', code)
        hill.queue.append(client)
        self.schedule_hill(hill)
        self.announce_hill(hill)
        if client in hill.queue:
            self.announce_spectate(hill, [client])   # a fight is on: it can be watched from its start

    def cmd_result(self, client, args):
        if len(args) < 2 or client.match is None:
            return
        match = client.match
        try:
            match_id, side = int(args[0]), int(args[1])
        except ValueError:
            return
        if match.id != match_id or side not in (0, 1):
            return
        self.end_match(match, match.players[side])

    # -- matches -------------------------------------------------------------------------------------------

    def start_match(self, players, mode, hill=None):
        self.next_id = (self.next_id + 1) & 0x7FFFFFFF or 1
        match = Match(self.next_id, players, mode, hill)
        self.matches[match.id] = match
        for side, player in enumerate(players):
            other = players[1 - side]
            player.match = match
            self.send(player, 'MATCH %d %d %s %d %s' % (match.id, side, other.addr[0], other.addr[1], other.name))
        if hill:
            hill.match = match
            self.announce_spectate(hill, hill.queue)
        print('match %d (%s): %s vs %s' % (match.id, mode, players[0].name, players[1].name), flush=True)

    def end_match(self, match, winner):
        """A match is over: `winner` won it (or None). King of the Hill moves on."""
        if self.matches.pop(match.id, None) is None:
            return
        for player in match.players:
            player.match = None
        hill = match.hill
        if hill is None:
            return
        hill.match = None
        match.ended = time.monotonic()
        self.ended[match.id] = match
        for client in hill.queue:
            self.send(client, 'SPECEND %d %d' % (match.id, len(match.log)))
        loser = next((p for p in match.players if p is not winner), None) if winner else None
        if winner is not None and winner.addr in self.clients:
            hill.streak = hill.streak + 1 if hill.king is winner else 1
            hill.king = winner
            winner.where = ('hill', hill.code)
        if loser is not None and loser.addr in self.clients and loser.where == ('hill', hill.code):
            hill.queue.append(loser)   # winner stays on; the loser waits again at the back of the line
        if hill.king is not None and hill.king not in (winner,) and hill.king.where != ('hill', hill.code):
            hill.king, hill.streak = None, 0
        self.schedule_hill(hill)
        self.announce_hill(hill)

    def schedule_hill(self, hill):
        if hill.match is not None:
            return
        if hill.king is None and len(hill.queue) >= 2:
            hill.king = hill.queue.pop(0)
            hill.streak = 0
        if hill.king is not None and hill.queue:
            challenger = hill.queue.pop(0)
            self.start_match([hill.king, challenger], 'hill', hill)

    def announce_hill(self, hill):
        king = hill.king.name if hill.king else '-'
        code = hill.code or '-'
        if hill.king is not None and hill.match is None:
            self.send(hill.king, 'HILL %s 0 %d %d %s' % (code, len(hill.queue), hill.streak, king))
        for position, client in enumerate(hill.queue, 1):
            self.send(client, 'HILL %s %d %d %d %s' % (code, position, len(hill.queue), hill.streak, king))

    def leave(self, client):
        """Takes a player out of wherever they wait or play; a match they were in is the other's."""
        if client in self.quick:
            self.quick.remove(client)
        for code, owner in list(self.rooms.items()):
            if owner is client:
                del self.rooms[code]
        where = client.where
        client.where = None
        if client.match is not None:
            match = client.match
            other = next(p for p in match.players if p is not client)
            self.send(other, 'PEERLEFT %d' % match.id)
            self.end_match(match, other if match.hill else None)
        if isinstance(where, tuple) and where[0] == 'hill':
            hill = self.hills.get(where[1])
            if hill:
                if client in hill.queue:
                    hill.queue.remove(client)
                if hill.king is client:
                    hill.king, hill.streak = None, 0
                self.schedule_hill(hill)
                self.announce_hill(hill)
                if hill.code and not hill.queue and hill.king is None and hill.match is None:
                    del self.hills[hill.code]

    async def sweep(self):
        """Forgets players who stopped talking."""
        while True:
            await asyncio.sleep(5)
            now = time.monotonic()
            for match_id, match in list(self.ended.items()):
                if now - match.ended > ENDED_LOG_SECONDS:
                    del self.ended[match_id]
            for client in list(self.clients.values()):
                if now - client.seen > CLIENT_TIMEOUT:
                    self.leave(client)
                    self.clients.pop(client.addr, None)


async def main():
    port = int(os.environ.get('PORT', '7777'))
    host = 'fly-global-services' if os.environ.get('FLY_APP_NAME') else '0.0.0.0'
    loop = asyncio.get_running_loop()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    transport, protocol = await loop.create_datagram_endpoint(Matchmaker, sock=sock)
    print('matchmaker listening on %s:%d' % (host, port), flush=True)
    await protocol.sweep()


if __name__ == '__main__':
    if os.name == 'nt':
        # The selector loop keeps a datagram socket open through those errors; the proactor loop doesn't.
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
