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
cc -Wall -O2 -fPIC -shared -o pam_failguard.so src/pam_failguard.c
```

## Install and rollback

```sh
make install     # build and place the module into the PAM modules directory
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
* Authentication that never asks for a password at all (e.g. by a public
  key) creates no false failure record — the module does not get in the way
  of such logins.
* State decays by itself: no manual unlock needed.

### State directory permissions

`/var/lib/pam_failguard` is created with mode **0711** owned by root, while a
state file is chowned to the account owner (`0600`).

This matters: during **screen unlock** the PAM stack runs as the user, not
root (`pam_unix(gnome-screensaver:auth): ... uid=1000
euid=1000`). With `0700` the user could not open their own file, the module
logged `cannot open state file (Permission denied)` and **did not work at
all** on the lock screen.

The traverse bit (`x`) grants access to one's own file by its known name but
not the right to list the directory; the directory denies user writes, so the
state for another account cannot be forged. If a user's state file does not
exist yet (no root-side call happened), the module exits quietly without
restriction — login must never be blocked.

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

### State directory permissions

`/var/lib/pam_failguard` is created with mode **0711** owned by root, while a
state file is chowned to the account owner (`0600`).

This matters: during **screen unlock** the PAM stack runs as the user, not
root (`pam_unix(gnome-screensaver:auth): ... uid=1000
euid=1000`). With `0700` the user could not open their own file, the module
logged `cannot open state file (Permission denied)` and **did not work at
all** on the lock screen.

The traverse bit (`x`) grants access to one's own file by its known name but
not the right to list the directory; the directory denies user writes, so the
state for another account cannot be forged. If a user's state file does not
exist yet (no root-side call happened), the module exits quietly without
restriction — login must never be blocked.

## Files

```
src/pam_failguard.c              module source
Makefile                         build
etc/pam.d/common-auth            resulting config (snapshot from the system)
etc/pam.d/common-auth.pristine   original config (snapshot from the system)
PAM-CONFIG.md                    all PAM configuration changes
```

Move **the source**, not a built `.so`, to another system: the module must be
built for the target architecture.
