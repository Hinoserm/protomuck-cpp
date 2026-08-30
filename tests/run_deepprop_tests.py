#!/usr/bin/env python3
# Deeply nested property paths must not crash the server.
#
# The propdir walkers descend one level per path component and each
# level used to put a 64KB component buffer on the stack, so an
# ordinary player storing "a/a/a/.../a" overflowed the game thread's
# stack and took the whole server down. Thousands of components fit
# inside a single property name, and no privilege is required.
#
# Usage: run_deepprop_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, resource

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker, server_pid, _alive

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)

# A REALISTIC stack limit, inherited by the server we spawn. This test
# is worthless without it: an unlimited stack (common in a dev shell)
# lets the recursion grow until it happens to finish, and the crash
# this suite exists to catch never reproduces.
try:
    resource.setrlimit(resource.RLIMIT_STACK, (8 * 1024 * 1024,
                                               resource.RLIM_INFINITY))
except (ValueError, OSError) as e:
    print('WARNING: could not set an 8MB stack limit (%s); a deep-'
          'recursion crash may not reproduce' % e)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
pid = server_pid('.')

out = sess.cmd('@create DeepSubject', 1.0)
ref = int(re.search(r'#(\d+)', out).group(1))

# each depth gets its OWN root component: erasing one deep path erases
# its whole subtree, which would otherwise wipe a deeper path that
# shares the prefix (that is correct behavior, not a bug)
for depth in (50, 200, 1000, 4000):
    path = 'd%d/' % depth + '/'.join(['a'] * depth)
    sess.cmd('@set #%d=/%s:deep%d' % (ref, path, depth), 2.0)
    check('server survived setting a %d-deep property path' % depth,
          _alive(pid))
    if not _alive(pid):
        break

    out = sess.cmd('ex #%d=/%s' % (ref, path), 2.0)
    check('the %d-deep value reads back' % depth, 'deep%d' % depth in out,
          out.strip()[-120:])

# a deep tree must also survive removal, the dump, and a real restart
if _alive(pid):
    sess.cmd('@set #%d=/d1000/%s:' % (ref, '/'.join(['a'] * 1000)), 2.0)
    check('server survived removing a 1000-deep path', _alive(pid))

if _alive(pid):
    sess.cmd('@dump', 8.0)
    check('server survived dumping a deep tree', _alive(pid))
    stop(sess)
    sess = start(binpath, STORE, STORE, port)
    out = sess.cmd('ex #%d=/d4000/%s' % (ref, '/'.join(['a'] * 4000)), 3.0)
    check('a 4000-deep value survived a restart', 'deep4000' in out,
          out.strip()[-120:])
    out = sess.cmd('@stats', 3.0)
    check('database intact after deep-path round trip',
          'total objects' in out, out[-200:])
    stop(sess)

shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
