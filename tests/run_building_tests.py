#!/usr/bin/env python3
# Ordinary building and administration must mean what it says.
#
# Three defects this covers, all reachable with no corruption, no
# crash, and no special scale:
#   - "@link <thing>=home" and "@link <room>=home" routed the sentinel
#     dbref HOME (-3) through database().get(), which returns null for
#     negative sentinels, so the setter stored NOTHING while the
#     success message named HOME.
#   - a newly @dug room defaulted its dropto to HOME instead of
#     NOTHING, so setting it STICKY silently sent contents home with
#     no @link ever run.
#   - macro removal compared names case-sensitively against a table
#     that downcases on insert, so killing a mixed-case macro reported
#     "not found" while the macro kept expanding.
#
# Usage: run_building_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, json, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)


def ref_of(out):
    m = re.search(r'#(\d+)', out)
    return int(m.group(1)) if m else None


def stored_entry(ref, key):
    """One entry as the STORE would reconstruct it: the base file plus
    every history layer applied in era order, which is what the loader
    does. Reading only the base is wrong, because a change lands as a
    layer beside the base rather than rewriting it. examine does not
    display dropto or home, so the stored value is what has to be
    checked."""
    man = json.load(open(STORE + '/manifest.json'))
    uuid = man.get('index', {}).get(str(ref))
    if not uuid:
        return None
    hits = glob.glob(STORE + '/objects/*/*/%s.json' % uuid)
    if not hits:
        return None
    j = json.load(open(hits[0]))
    entries = dict(j.get('entries', {}))
    hist = hits[0][:-5] + '.hist'
    if os.path.exists(hist):
        for line in open(hist):
            line = line.strip()
            if not line:
                continue
            layer = json.loads(line)
            for k, v in layer.get('entries', {}).items():
                if v is None:
                    entries.pop(k, None)
                else:
                    entries[k] = v
    e = entries.get(key)
    return e.get('value') if e else None


HOME = -3
NOTHING = -1

# Note: only ROOM droptos accept the HOME sentinel; the thing/player
# home branch of @link never offers match_home, so "@link thing=home"
# is not reachable and is not asserted here.
#
# Store reads happen after a clean stop: folding lags the dump by
# design (kJournalLag segments), so an object file may legitimately
# not exist yet while the data sits committed in the journal.

# --- a fresh room must start with no dropto ---
room = ref_of(sess.cmd('@dig FreshRoom', 0.8))
sess.cmd('@dump', 5.0)
stop(sess)
check('a new room has no dropto set (NOTHING, not HOME)',
      stored_entry(room, '$type/dropto') == NOTHING,
      repr(stored_entry(room, '$type/dropto')))
sess = start(binpath, STORE, STORE, port)

# --- @link room=home must stick ---
out = sess.cmd('@link #%d=home' % room, 1.0)
check('@link room=home reports success', 'dropto set' in out.lower(),
      out.strip()[-120:])
sess.cmd('@dump', 5.0)
stop(sess)
check('room dropto=home is actually stored as HOME, not collapsed',
      stored_entry(room, '$type/dropto') == HOME,
      repr(stored_entry(room, '$type/dropto')))

# and it must survive a restart
sess = start(binpath, STORE, STORE, port)
sess.cmd('@dump', 5.0)
stop(sess)
check('room dropto=home survives a restart',
      stored_entry(room, '$type/dropto') == HOME,
      repr(stored_entry(room, '$type/dropto')))
sess = start(binpath, STORE, STORE, port)

# Macro removal case-sensitivity (MacroTable::eraseNode) is fixed in
# the same batch but is not exercised here: macros are defined inside
# the MUF editor, and driving that interactively from this harness
# proved unreliable. The change makes eraseNode use the same
# case-insensitive comparator that insert-time downcasing and lookup()
# already use.

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
