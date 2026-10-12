# Setting up PAM: what to do on a clean system

This document describes setup from scratch: what to build, where to put it,
and which changes to make to `/etc/pam.d/common-auth`. Third-party files not
related to PAM play no role in this scheme and are not touched.

The only requirements are the module source and the stock compiler. No extra
packages — `libpam0g-dev` is not needed.

---

## Where everything goes

| What | Where |
|---|---|
| `pam_failguard.so` | `/lib/<triplet>/security/` — the PAM module directory |
| config edits | `/etc/pam.d/common-auth` — edited in place, the file is not replaced wholesale |
| state | `/var/lib/pam_failguard/` — created by the module itself, no manual work |

`<triplet>` is `i386-linux-gnu` or `x86_64-linux-gnu`, depending on
architecture. The Makefile derives it from the location of `libpam.so.0`.

## Installing the module

```sh
make install
```

Builds the module and places it into the PAM module directory. Nothing else —
the config is edited separately, by hand (next section). Only the stock
compiler is needed; `libpam0g-dev` is not required.

Rollback:

```sh
make uninstall
```

The build must happen **on the target system**, for its architecture: a `.so`
copied from another machine will not load. Carry the source instead.

### What else to know about installation

Three things that make the module silently do nothing on the lock screen
(linking with `libpam`, two install directories, the per-process cache of
failed loads) are covered in `README.md`, section "Three things that break the
module". They are not repeated here.

For the config changes one fact matters: the lock-screen service pulls
`common-auth` through `@include`, so its own file needs no editing — neither
in GNOME (`gnome-screensaver`) nor in Unity 15.10 (`unity`).

## Three edits to `/etc/pam.d/common-auth`

Edit this system's **own** `common-auth`. Do not drop the snapshot from
`etc/pam.d/` over it wholesale: with a different version, `pam-auth-update`
would destroy the file's regular contents. The diff below is version
independent.

```diff
 # here are the per-package modules (the "Primary" block)
-auth	[success=1 default=ignore]	pam_unix.so nullok_secure
+auth	[success=2 default=ignore]	pam_unix.so nullok_secure
+auth optional pam_failguard.so record deny=1 delay=10
 # here's the fallback if no module succeeds
 auth	requisite			pam_deny.so
 # prime the stack with a positive return value if there isn't one already;
@@ -23,3 +24,6 @@
 auth	required			pam_permit.so
 # and here are more per-package modules (the "Additional" block)
 # end of pam-auth-update config
+
+# pam_failguard: refuse login for 10 s after 1 failed attempt
+auth required pam_failguard.so refuse deny=1 delay=10
```

Net: one flag replaced, one line inserted after `pam_unix`, two lines appended
at the end.

## Nothing else to do

Writing to `auth.log` goes through `pam_syslog()` in facility `auth` — nothing
to configure. Service files such as `lightdm`, `sudo`, `su`, `login`, `sshd`
need no changes: the mechanism applies everywhere `common-auth` is included.

---

## The resulting auth stack

```
auth	[success=2 default=ignore]	pam_unix.so nullok_secure
auth optional pam_failguard.so record deny=1 delay=10
auth	requisite			pam_deny.so
auth	required			pam_permit.so
auth required pam_failguard.so refuse deny=1 delay=10
```

Execution paths:

| Event | What happens |
|---|---|
| password correct, window not expired | `pam_unix` → success, jump over `record` and `pam_deny` → `pam_permit` → `refuse` **rejects** |
| password correct, window expired | `pam_unix` → success → `pam_permit` → `refuse` passes → **login** |
| password wrong | `pam_unix` → `default=ignore` → `record` **records the timestamp** → `pam_deny` → **refused** |
| password wrong, the window was active | the same, plus `record` moves the timestamp — the window gets extended |

### Why `success=2` instead of `success=1`

The flag on `pam_unix` means "on success, skip N modules ahead". It used to be
`success=1` — only `pam_deny` was skipped. With `success=2`, both `record` and
`pam_deny` are skipped, so on success the stack lands on `pam_permit`, while
`refuse` stays last and can still refuse.

The counter in `success=N` is relative, so adding modules **before** `pam_unix`
does not break it.

### Why `refuse` sits after `pam_permit`

The refusal must happen after PAM has requested the password. In
`unity-greeter` (`src/greeter-list.vala`, lines 776/803/859/870) the flag
`prompted` is raised only when the PAM dialog actually requested a password:

```vala
if (prompted)  show_message (_("Invalid password, please try again"), true);
else           show_message (_("Failed to authenticate"), true);
```

If you refuse before `pam_unix`, the dialog never runs, `prompted` stays
`false`, and the login window shows the emergency screen with a **Retry**
button instead of the regular message. Placing `refuse` earlier is not
possible: the `success=2` jump would skip it too, and `pam_deny` would have to
be removed — then on a failed password check the stack would reach `pam_permit`
and **grant access**.

---

## Parameters

| Parameter | Value | Meaning |
|---|---|---|
| `delay=10` | 10 **seconds** | how long the refusal lasts |
| `deny=1` | 1 failure | how many failed attempts switch the refusal on |
| `record` | mode | record the timestamp; only on failure |
| `refuse` | mode | check the window and refuse, only after a successful check |

Both calls carry both parameters — so each line is fully self-describing. For
other values, only the text of these two lines changes.

## Services covered by this configuration

Services include `common-auth` through `@include`, so the change applies to
every login path at once, without touching service files:

| Service | `@include common-auth` |
|---|---|
| `sudo` | yes |
| `lightdm` (login window) | yes |
| `gnome-screensaver` (screen lock, GNOME) | yes |
| `unity` (screen lock, Ubuntu 15.10) | yes |
| `su` | yes |
| `login` | yes |
| `sshd` | yes |
| `lightdm-greeter` | no — a greeter session of its own; no password is entered there |

The lock-screen service name depends on the desktop: in GNOME it is
`gnome-screensaver`, in Unity 15.10 it is `unity` (served by `compiz`). Check
the actual name in `/var/log/auth.log` after a failed attempt:
`pam_unix(<service>:auth): authentication failure`.

---

## Rollback

```sh
make uninstall                                   # remove the module
sudo rm -rf /var/lib/pam_failguard               # remove the state
```

The three edits to `common-auth` are reverted by hand — the diff above,
backwards.

`common-auth.pristine` is not meant for rollback: it is not part of the setup
scheme, just a snapshot of the original file against which the diff is shown.

---

## Project files

| Path | Role in the setup |
|---|---|
| `src/pam_failguard.c` | **required** — the source from which the module is built |
| `Makefile` | optional; the same `cc` invocation |
| `etc/pam.d/common-auth` | reference: the final state of `common-auth` |
| `etc/pam.d/common-auth.pristine` | reference: the original state, the diff's base |
