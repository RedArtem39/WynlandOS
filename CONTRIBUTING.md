# Contributing to WynlandOS

Thank you for wanting to help. Fixes, drivers, syscalls, ports and tests are
all welcome.

## The license of what you send

WynlandOS is licensed under the **GNU General Public License, version 2 or
(at your option) any later version** (GPL-2.0-or-later). By sending a
contribution (a pull request, a patch, a commit) you agree that:

1. **Your contribution is licensed under GPL-2.0-or-later**, as the rest of
   the project.

2. **The maintainer may relicense it.** You grant Red_Artem39, the
   maintainer of WynlandOS, the right to distribute your contribution, as
   part of WynlandOS, under another license as well -- for example to move
   the whole repository to a newer GPL, or to a different license that
   other code we want to use requires.

3. **But never to a closed one.** Any license the project moves to must be
   a free software license: approved by the Free Software Foundation or by
   the Open Source Initiative, keeping the source available to everyone
   with the right to use, study, change and share it. WynlandOS may not be
   made proprietary, source-unavailable or "source-available only", in
   whole or in part, and the grant in point 2 does not cover that.

4. Whatever license the project moves to, **the versions already
   published stay available under the licenses they were published
   under.** A relicense never takes rights away from anyone who already
   has a copy.

5. **You have the right to send it**: it is your own work, or you are
   allowed to contribute it under these terms (and your employer, if it
   has a claim to your work, agrees).

Mark your agreement by signing off your commits (`git commit -s`), which
adds a line like:

```
Signed-off-by: Your Name <you@example.com>
```

## Code from other projects

Code taken from other projects keeps its own license. Bring it only under
a license compatible with GPL-2.0-or-later (MIT, BSD, LGPL,
GPL-2.0-or-later; GPL-3.0 and Apache-2.0 too, which make the combined
program GPL-3.0), keep its copyright and license notices in the files, and add its license text to
[.licenses](.licenses). Code under AGPL, GPL-2.0-only, or without any
license cannot come in. Point 2 above applies to your own work, not to
someone else's code you bring along.

Images, fonts and other media need a known license too (see
`rootfs/usr/share/wynland/wallpapers/CREDITS`): pictures found through a
search engine with no known author are not usable.

## How to work

- Build and boot it as the [README](README.md) says; run the tests
  (`make run-gl AUTOTEST=1`, or `AUTOTEST=sh` for the shell ones) before
  you send a change. A change that breaks a test that passed before will
  not be merged.
- Write code that reads like the code around it: the same naming, the same
  comment style. Comments say why, in plain English.
- One change per pull request, with a message that says what and why.
- The kernel's syscall ABI is Linux's; our own calls are described in
  [docs/syscalls.md](docs/syscalls.md). Update it when you add or change
  one.
- Security problems: do not open a public issue; write to the maintainer
  first.
