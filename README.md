# pam_failguard

A PAM module that **refuses** password entry for a configurable number of
seconds after a failed attempt. The screen does not hang; the message stays
the usual one.

```
wrong password       ->  regular error message right away
                     ->  for the next 10 seconds the system accepts NO password
                     ->  after 10 seconds everything is back to normal
```

## Why not pam_tally2

`pam_tally2` accumulates a persistent counter and locks the account;
`unlock_time` merely lifts the lock after a pause, so waiting defeats the
protection. This module instead *refuses* for exactly `delay` seconds: the
counter decays by itself, no root-side unlock needed.

## Semantics

* `delay=N` — **seconds** (an integer).
* `deny=N` — how many failed attempts enable the refusal.
* `debug` — verbose logging.

With `deny=1 delay=10`:

1. A failed attempt → counter +1, timestamp = now. The error message is shown
   immediately.
2. Any attempt within the next 10 seconds → **refused** (even with the
   correct password). The window **slides**: every *failed* attempt restarts
   the 10-second countdown, so continuous guessing is held off indefinitely.
   A refusal on an active window does not extend the timer — a correct
   password is not a failed attempt.
3. A quiet period longer than 10 seconds → the refusal lapses, the counter
   resets.
4. The counter does not affect refusal length: it is always exactly `delay`
   and never grows.

## The stack

```
auth	[success=2 default=ignore]	pam_unix.so nullok_secure
auth	optional	pam_failguard.so record deny=1 delay=10
auth	requisite	pam_deny.so
auth	required	pam_permit.so
auth	required	pam_failguard.so refuse deny=1 delay=10
```

* `record` — comes after `pam_unix` with `default=ignore`, so it runs **only on
  failure** and records the timestamp. On success it is skipped.
* `refuse` — last module, fires **only after a successful** password check: if
  the window has not expired, login is rejected anyway.
* `success=2` — on success `pam_unix` jumps over `record` and `pam_deny`,
  landing on `pam_permit`; `refuse` stays last and can still refuse.

The password-check result is not available to modules sitting **before**
`pam_unix` — that is why "record a failure" and "refuse" are split across two
positions.

## Why the refusal must come AFTER the password check

This is not decoration; it is a greeter requirement. `unity-greeter`
(`src/greeter-list.vala`) has a flag:

```vala
protected bool prompted = false;            // line 776
...
    prompted = true;                         // line 803, inside the PAM dialog
...
if (prompted)
    show_message (_("Invalid password, please try again"), true);   // 859
else
    show_message (_("Failed to authenticate"), true);              // 870
```

`prompted` is raised **only when PAM actually requested a password**. If the
stack refuses before `pam_unix`, the dialog never runs, `prompted` stays
`false`, and the login window shows the emergency "Failed to authenticate"
screen with a Retry button instead of the regular message. That is exactly
what happened with the first implementation, which placed `check` before
`pam_unix`.

The cost: the password is really checked, and the refusal lands ~2 s later
(normal processing time) instead of instantly. The price of correct behavior
in the window.

## Building

The stock compiler is enough; `libpam0g-dev` is **not required** — the needed
Linux-PAM ABI declarations live in the source itself.

```sh
make
# or directly:
cc -Wall -O2 -fPIC -shared -o pam_failguard.so src/pam_failguard.c -l:libpam.so.0
```

The `-l:libpam.so.0` flag is mandatory: without it the module does not load in
processes that do not link libpam themselves — notably the lock screen on
Ubuntu 15.10, where `compiz` handles authentication. Linking against the
versioned soname avoids needing `libpam0g-dev`.

## Three things that break the module

Tested on Ubuntu 15.10 (Unity), i686, PAM 1.1.8. Each one yields a module that
silently does nothing on the lock screen.

### 1. Linking with libpam

It was assumed unnecessary: symbols like `pam_get_user` and `pam_syslog`
should resolve from the `libpam.so.0` of whichever process loads the module.
True for `sshd`, `su`, `sudo`; not true for the lock screen: `compiz` reaches
PAM through `dlopen`, and its `libpam.so.0` is not global:

```
compiz: PAM unable to dlopen(pam_failguard.so): undefined symbol: pam_syslog
compiz: PAM adding faulty module: pam_failguard.so
```

Fix: `LDFLAGS ?= -l:libpam.so.0` in the `Makefile`.

### 2. Install directories

PAM does not search a single module path, and the set of paths depends on how
the process linked libpam:

| Directory | Who searches it |
|---|---|
| `/lib/i386-linux-gnu/security` | the stock PAM library: `sshd`, `su`, `sudo`, `login` |
| `/lib/security` | `compiz` — the lock screen |

`make install` puts the module in both (`PAMDIRS`) and reports per directory.
Without the `/lib/security` copy:

```
compiz: PAM unable to dlopen(pam_failguard.so): /lib/security/pam_failguard.so:
        cannot open shared object file: No such file or directory
```

This error masks the next one — until the path is fixed, `undefined symbol`
is invisible.

### 3. Cache of failed loads

Linux-PAM remembers modules that failed to load **within a process** and never
retries them. So after fixing the `.so`, the process holding the lock screen
must be restarted — in practice, re-login. Otherwise it keeps going with a
poisoned entry and the module stays silently inactive.

### Who actually authenticates the lock screen

On Ubuntu 15.10 the session is Unity, and unlocking is handled by **`compiz`**
using the **`unity`** PAM service, not `gnome-screensaver`:

```
compiz: pam_unix(unity:auth): authentication failure;
        logname= uid=1000 euid=1000 tty= ruser= rhost=  user=test
```

The `gnome-screensaver` daemon never runs in Unity: its autostart file says
`OnlyShowIn=GNOME-Flashback`. That is why `Ctrl+Alt+L` and
`gnome-screensaver-command -l` do not work there. `/etc/pam.d/unity` needs no
editing — it pulls `common-auth` through `@include`.

You can find the lock-screen service name from `auth.log` after a failed
attempt: `pam_unix(<service>:auth): authentication failure`. In GNOME it is
`gnome-screensaver`; in Unity 15.10 it is `unity`.

### Building on an EOL distro

Ubuntu 15.10 is no longer maintained: the `wily` branches on
`us.archive.ubuntu.com` and `security.ubuntu.com` return 404; only
`old-releases.ubuntu.com` still has them. If those addresses remain in
`/etc/apt/sources.list`, `apt-get update` fails — and with it any build or
install. Every line must be switched to `old-releases.ubuntu.com`. This has
nothing to do with the module itself, but nothing can be installed without it.

## Install and rollback

```sh
make install     # build and place the module into both PAM directories
make uninstall   # remove the module
```

Changes to `/etc/pam.d/common-auth` are made separately, by hand — the diff
and explanation are in `PAM-CONFIG.md`. The state directory
`/var/lib/pam_failguard` is created by the module itself.

## The window is not extended by a refusal

A refusal on an active window **does not move the timer**. The window is
always counted from the last failed attempt.

Extension here broke screen unlocking. The `gnome-screensaver` lock dialog
re-authenticates every 3 seconds and has a hard limit of `MAX_FAILURES = 5`
attempts, after which the window closes for good
(`src/gnome-screensaver-dialog.c`, `auth_check_idle`). While the window was
extended it never expired as long as the user kept pressing Enter, so:

* unlocking the screen was impossible in principle;
* after 5 attempts the dialog closed, and one had to leave and log in
  through the main login window.

Measured on both versions, the scenario "failure, then a correct password
every 3 seconds":

| Version | Result |
|---|---|
| with extension | 5 refusals in a row, entry impossible, the dialog closes |
| without extension | 2 refusals, then entry |

This is exactly the "refuse for exactly 10 seconds" requirement: a correct
password rejected because of the window is not a failed attempt and must not
move the timer. The sliding window is preserved — every *failed* attempt
updates the timestamp.

## Failure modes

* Any I/O error → `PAM_IGNORE`; login is **not** blocked. A broken state
  directory must not lock accounts out.
* Clock moved backwards → the timestamp counts as expired (also open).
* The `PAM_SILENT` flag is deliberately **not** honored. `sudo` passes it on
  every password request — it prints the `[sudo] password` prompt itself and
  asks PAM not to duplicate output. Honoring the flag disabled the module for
  `sudo` entirely: no attempt counted, no refusal happened. There are no
  silent calls on the lock screen, so nothing would ever be skipped anyway.
* State decays by itself: no manual unlock needed.

### State directory permissions

`/var/lib/pam_failguard` is created with mode **0711** owned by root, while a
state file is chowned to the account owner (`0600`).

This matters: during **screen unlock** the PAM stack runs as the user, not
root. Measured on Ubuntu 15.10 (Unity), where `compiz` authenticates the
unlock:

```
compiz: pam_unix(unity:auth): authentication failure;
        logname= uid=1000 euid=1000 tty= ruser= rhost=  user=test
```

With `0700` the user could not open their file, the module logged
`cannot open state file (Permission denied)` and **did not work at all** on
the lock screen.

The traverse bit (`x`) grants access to one's own file by its known name but
not the right to list the directory; the directory denies user writes, so the
state for another account cannot be forged. If a user's state file does not
exist yet (no root-side call happened), the module exits quietly without
restriction — login must never be blocked.

The state file is **never deleted**. The unlock stack needs it: it runs as the
user and cannot create the file afresh in a root-owned directory. The file is
created by the first root-side call (login window, `ssh`, `su`, `sudo`) — i.e.
the user's first login — and stays put. What expires is the counter, not the
file: it resets by age rather than being removed.

Previously the expired file was deleted, and that broke exactly the case the
module exists for. `unlink()` does not close an already-open descriptor, so
the next write went to an unnamed file and the just-counted failure vanished.
Waiting longer than `delay` after a past failure was enough for the next
failure to stop counting and a correct password to be accepted immediately.
Worse, nobody could recreate the deleted file: the user-side stack (lock
screen) cannot create files in the root directory, and the module stayed
inactive until a `su`/`ssh`/login-window entry happened.

## Measured results (Ubuntu 15.10, PAM 1.1.8, i686)

Real `su` client in a pseudo-terminal:

| Step | Time | Result |
|---|---|---|
| correct password, no prior failures | 0.03 s | login |
| wrong password | 2.20 s | refused |
| **correct immediately after** | 1.94 s | **refused** |
| **correct once more** | 2.35 s | **refused** |
| correct after an 11 s pause | 0.03 s | login |
| wrong again | 1.61 s | refused |
| correct immediately | 2.21 s | refused |

Sample `/var/log/auth.log` line (the log is in English — the system language):

```
su[2705]: pam_failguard(su:auth): pam_failguard: authentication refused for user [pamlab], 1 failed logins, 7 s remaining
```

### Test limits

Greeter input cannot be driven automatically, so the greeter paths were tested
by hand. The table above uses a real `su` client in a pseudo-terminal — a real
login path, not the module in isolation.

### Screen lock

On Ubuntu 15.10 (Unity) tested against the real lock screen, authenticated by
`compiz`:

| Step | Result |
|---|---|
| wrong password | refusal window active, state recorded |
| correct password immediately | **refused**: `pam_failguard(unity:auth): authentication refused …, 4 s remaining` |
| pause longer than 10 s | window expired, counter reset — the state file is still there |
| correct password again | **unlock** (`gkr-pam: unlocked login keyring`) |

## Files

```
src/pam_failguard.c              module source
Makefile                         build and install into both PAM directories
etc/pam.d/common-auth            resulting config (snapshot from the system)
etc/pam.d/common-auth.pristine   original config (snapshot from the system)
PAM-CONFIG.md                    all PAM configuration changes
```

Move **the source**, not a built `.so`, to another system: the module must be
built for the target architecture.
