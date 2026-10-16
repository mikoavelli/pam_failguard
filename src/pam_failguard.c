/*
 * pam_failguard.so - a PAM module: refuse password entry for a set time
 *                    after a failed attempt.
 *
 * TASK
 *   After a failed password attempt the system must:
 *     - show the usual wrong-password message immediately (nothing hangs);
 *     - accept NO password at all, even a correct one, for the next N seconds;
 *     - behave as usual once the N seconds are over.
 *
 * UNITS
 *   delay is given in SECONDS (delay=10 = 10 seconds).
 *
 * USE IN THE STACK
 *   The module is needed twice: the password-check result is not available
 *   to modules placed BEFORE it, so "record a failure" and "refuse" are
 *   split.
 *
 *     auth  [success=2 default=ignore]  pam_unix.so nullok_secure
 *     auth  optional   pam_failguard.so record deny=1 delay=10
 *     auth  requisite  pam_deny.so
 *     auth  required   pam_permit.so
 *     auth  required   pam_failguard.so refuse deny=1 delay=10
 *
 *   record - placed after pam_unix with default=ignore, so it runs ONLY
 *            on failure and records the timestamp.
 *   refuse - last in the stack, fires only after a SUCCESSFUL password
 *            check: if the window has not expired, login is refused anyway.
 *
 * WHY THE REFUSAL COMES AFTER THE PASSWORD CHECK, NOT BEFORE IT
 *   unity-greeter (src/greeter-list.vala) keeps a prompted flag that is
 *   raised ONLY when PAM actually requested a password. If the stack refuses
 *   before pam_unix, the dialog never runs, prompted stays false, and the
 *   greeter shows the emergency screen "Failed to authenticate" with a
 *   Retry button instead of the regular "Invalid password, please try
 *   again". Hence the password check goes first, and the refusal is the
 *   last module.
 *
 *   check  - placed before pam_unix, refuses if a recent failure exists.
 *   record - placed after pam_unix with default=ignore, so it runs ONLY
 *            on failure and records the timestamp.
 *
 * STATE
 *   /var/lib/pam_failguard/<user> holds "count time".
 *   The state decays by itself: if more than delay has passed between
 *   attempts, the counter resets. The file itself is NOT removed: it is
 *   owned by the user and is needed by the screen-unlock stack, which runs
 *   as the user and cannot recreate it in the root-owned directory.
 *   Therefore nothing accumulates (the counter is always bounded by the
 *   window) and no manual unlock is needed.
 *
 * REFUSAL UNDER TIME (sliding window)
 *   The timestamp is the moment of the last refusal. Both record (a failed
 *   password check) and refuse (a refusal on an active window) move it, so
 *   every attempt inside the window restarts the delay countdown and
 *   continuous guessing is held off indefinitely. The failure count does
 *   not affect the refusal length: it is always exactly delay seconds and
 *   does not grow.
 *
 * FAILURE MODES
 *   Any I/O error is treated as "open" - the module returns PAM_IGNORE and
 *   does not block login. A broken state directory must not lock accounts.
 *   The exception is a clock moved backwards: then the timestamp counts as
 *   expired (also open), otherwise the account would be locked for the
 *   duration of the difference.
 *
 * BUILD
 *   gcc -Wall -O2 -fPIC -shared -o pam_failguard.so pam_failguard.c
 *   (the stock compiler is enough, libpam0g-dev is not required)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <syslog.h>

/* ------------------------------------------------------------------ *
 * Minimal Linux-PAM ABI declarations.
 * There is no libpam0g-dev on the system, and installing packages for
 * headers is unnecessary: the Linux-PAM structures and constants are
 * stable, and only three of them are needed.
 * ------------------------------------------------------------------ */
#define PAM_EXTERN extern

typedef struct pam_handle pam_handle_t;

#define PAM_SUCCESS        0
#define PAM_BUF_ERR        5
#define PAM_AUTH_ERR       7
#define PAM_IGNORE        25

extern int  pam_get_user(pam_handle_t *pamh, const char **user, const char *prompt);
extern void pam_syslog(const pam_handle_t *pamh, int priority, const char *format, ...);

#define STATE_DIR "/var/lib/pam_failguard"
/*
 * The state directory is owned by root with mode 0711, not 0700: the PAM
 * stack during screen unlocking runs AS THE USER, not as root, and must be
 * able to reach its own state file. The traverse bit (x) grants access to
 * the file by its known name but does not allow listing the directory.
 * A user cannot create a state file - it is created by the first password
 * check running as root (login window, ssh, su, sudo), and afterwards it
 * stays in place and is updated in place (see expired-state handling
 * below).
 */
#define STATE_DIR_MODE 0711
#define USERNAME_MAX 256
#define PATH_MAX_LEN 512      /* STATE_DIR + USERNAME_MAX + separator */

/* defaults */
#define DEFAULT_DENY  1
#define DEFAULT_DELAY 10      /* seconds */

enum mode { MODE_RECORD, MODE_REFUSE };

struct options {
    enum mode mode;
    long deny;
    long delay;               /* seconds */
    int  debug;
};

struct state {
    long count;               /* failures within the delay window */
    long last;                /* time of the last attempt (unix time) */
    int  existed;             /* did the state file exist */
};

static void
log_msg(pam_handle_t *pamh, int priority, const char *fmt, ...)
{
    va_list ap;
    char buf[512];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pam_syslog(pamh, priority, "%s", buf);
}

/* parse module arguments. 0 - error, 1 - ok */
static int
parse_args(int argc, const char **argv, struct options *o)
{
    int i;

    o->mode  = MODE_REFUSE;   /* if the keyword is absent, assume refuse */
    o->deny  = DEFAULT_DENY;
    o->delay = DEFAULT_DELAY;
    o->debug = 0;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "refuse") == 0) {
            o->mode = MODE_REFUSE;
        } else if (strcmp(argv[i], "record") == 0) {
            o->mode = MODE_RECORD;
        } else if (strncmp(argv[i], "deny=", 5) == 0) {
            char *end = NULL;
            long v = strtol(argv[i] + 5, &end, 10);
            if (end == argv[i] + 5 || (end && *end != '\0') || v < 0) {
                return 0;
            }
            o->deny = v;
        } else if (strncmp(argv[i], "delay=", 6) == 0) {
            char *end = NULL;
            long v = strtol(argv[i] + 6, &end, 10);
            if (end == argv[i] + 6 || (end && *end != '\0') || v <= 0) {
                return 0;   /* delay must be positive */
            }
            o->delay = v;
        } else if (strcmp(argv[i], "debug") == 0) {
            o->debug = 1;
        } else {
            return 0;       /* unknown argument */
        }
    }
    return 1;
}

/* is it safe to use the username as a file name */
static int
usable_name(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0')
        return 0;
    if (strlen(name) >= USERNAME_MAX)
        return 0;
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '/' || name[i] == '\\')
            return 0;
    }
    return 1;
}

static int
open_state(const char *name, char *path, size_t pathlen)
{
    if (snprintf(path, pathlen, "%s/%s", STATE_DIR, name) >= (int)pathlen)
        return -1;
    return open(path, O_RDWR | O_CREAT, 0600);
}

/* Create the state directory or fix its permissions.
 * 0 - ok, 1 - state unavailable (exit silently), -1 - error (log it) */
static int
ensure_state_dir(pam_handle_t *pamh)
{
    struct stat st;

    if (mkdir(STATE_DIR, STATE_DIR_MODE) == -1 && errno != EEXIST) {
        /* Directory not created and cannot be created: the typical case is
         * screen unlocking, where PAM runs as the user. A warning here is
         * only noise: login is not blocked either way. */
        if (geteuid() != 0)
            return 1;
        log_msg(pamh, LOG_WARNING,
                "pam_failguard: cannot create %s (%s), authentication not restricted",
                STATE_DIR, strerror(errno));
        return -1;
    }

    /* Older versions created the directory with 0700 - the user could not
     * reach it. Fix it when we are root. */
    if (geteuid() == 0 && stat(STATE_DIR, &st) == 0 &&
        (st.st_mode & 07777) != STATE_DIR_MODE) {
        if (chmod(STATE_DIR, STATE_DIR_MODE) == -1) {
            log_msg(pamh, LOG_WARNING,
                    "pam_failguard: cannot set mode of %s (%s)",
                    STATE_DIR, strerror(errno));
        }
    }
    return 0;
}

/*
 * The state file is chowned to the account owner. Otherwise the PAM stack
 * running as the user (screen unlock) could not open it.
 * No security impact: a user can write state only for their own account,
 * and the directory forbids creating new files.
 */
static void
own_state_file(int fd, const char *username)
{
    struct passwd *pw;

    if (geteuid() != 0)
        return;                   /* not our file - do not touch its ownership */

    pw = getpwnam(username);
    if (pw == NULL)
        return;

    if (fchown(fd, pw->pw_uid, pw->pw_gid) == -1) {
        /* not critical: root reads and writes the file regardless of mode */
    }
}

static int
read_state(int fd, struct state *st)
{
    char buf[64];
    ssize_t n;

    st->count = 0;
    st->last = 0;
    st->existed = 0;

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
        return 0;

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    if (sscanf(buf, "%ld %ld", &st->count, &st->last) != 2)
        return 0;

    st->existed = 1;
    return 1;
}

static int
write_state(int fd, const struct state *st)
{
    char buf[64];
    int n;

    n = snprintf(buf, sizeof(buf), "%ld %ld\n", st->count, st->last);
    if (n < 0 || n >= (int)sizeof(buf))
        return 0;
    if (ftruncate(fd, 0) == -1)
        return 0;
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
        return 0;
    if (write(fd, buf, (size_t)n) != n)
        return 0;
    return 1;
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    const char *username = NULL;
    struct options o;
    struct state st;
    char path[PATH_MAX_LEN];
    long now;
    int fd;
    int ret = PAM_IGNORE;

    if (!parse_args(argc, argv, &o)) {
        log_msg(pamh, LOG_ERR,
                "pam_failguard: invalid module arguments, authentication not restricted");
        return PAM_IGNORE;   /* do not block login over a config error */
    }

    if (pam_get_user(pamh, &username, NULL) != PAM_SUCCESS ||
        !usable_name(username)) {
        return PAM_IGNORE;
    }

    /* The PAM_SILENT flag is deliberately NOT honored.
     *
     * Previously such calls were skipped: a silent call was considered a
     * check without a password request, hence no failure. That may hold for
     * gnome-screensaver - but sudo passes PAM_SILENT ALWAYS. It prints the
     * "[sudo] password" prompt itself and asks PAM not to duplicate output.
     * Because of the skip, the module did nothing for sudo: no attempt was
     * counted and no refusal happened.
     *
     * Dropping this check has no cost on the target system: compiz (the
     * Unity lock screen) calls PAM the regular way, and there are no silent
     * calls in the log. Verified with sudo 1.8.12, Ubuntu 15.10. */
    (void)flags;

    now = (long)time(NULL);

    if (ensure_state_dir(pamh) != 0) {
        if (o.debug) {
            log_msg(pamh, LOG_DEBUG,
                    "pam_failguard: state directory unavailable, authentication not restricted");
        }
        return PAM_IGNORE;
    }

    fd = open_state(username, path, sizeof(path));
    if (fd < 0) {
        /* The PAM stack during screen unlocking runs as the user and cannot
         * create a file in the state directory: it is owned by root. This
         * is not a reason to block login - exit silently; the file will be
         * created on a root-side call (su, sudo, ssh, login window). */
        if (geteuid() != 0 && (errno == EACCES || errno == EPERM ||
                               errno == ENOENT)) {
            if (o.debug) {
                log_msg(pamh, LOG_DEBUG,
                        "pam_failguard: no state file for user [%s] (%s), not restricted",
                        username, strerror(errno));
            }
            return PAM_IGNORE;
        }
        log_msg(pamh, LOG_WARNING,
                "pam_failguard: cannot open state file (%s), authentication not restricted",
                strerror(errno));
        return PAM_IGNORE;
    }
    own_state_file(fd, username);
    if (flock(fd, LOCK_EX) == -1) {
        /* do not block login over a file-locking problem */
        close(fd);
        return PAM_IGNORE;
    }

    if (!read_state(fd, &st)) {
        st.count = 0;
        st.last = 0;
        st.existed = 0;
    }

    /* The state has expired: the clock moved backwards, or more than delay
       has passed between attempts. Reset the counter - in MEMORY only.
       Previously the file was also deleted here, and that broke the very
       case the module exists for. unlink() does not close an open
       descriptor: the write after unlink went to an unnamed file, and the
       just-counted failure was lost. Waiting longer than delay after the
       past failure was enough for the next failed attempt to stop counting
       and a correct password to be accepted immediately.
       Moreover, nobody could recreate the deleted file: the screen-unlock
       stack (compiz, unity service) runs as the user, while the directory
       is owned by root. Keeping the file means the counter is written back
       in place on the next failure. */
    if (st.existed && (st.last > now || (now - st.last) >= o.delay)) {
        st.count = 0;
        st.last = 0;
        st.existed = 0;
    }

    if (o.mode == MODE_RECORD) {
        /* reached only on a failed password check */
        st.count += 1;
        st.last = now;
        if (!write_state(fd, &st)) {
            log_msg(pamh, LOG_WARNING, "pam_failguard: cannot write state file");
        } else if (o.debug) {
            log_msg(pamh, LOG_DEBUG,
                    "pam_failguard: recorded failure #%ld for user [%s]",
                    st.count, username);
        }
        ret = PAM_IGNORE;
    } else {
        /* MODE_REFUSE: reached only after a SUCCESSFUL password check,
           meaning the PAM dialog has already run and the greeter knows a
           password was requested. Refuse here - otherwise the greeter shows
           the emergency screen "Failed to authenticate" instead of the
           regular message. */
        if (st.existed && st.count >= o.deny && st.last > 0) {
            /* A refusal SETS the window to delay seconds from NOW, it does
             * not add delay to the remainder. So while the user keeps
             * trying - with a correct or a wrong password - the timestamp
             * moves and the countdown starts over: entry opens only after a
             * full delay-second pause without a single attempt.
             *
             * This is the intended design (see "REFUSAL UNDER TIME" at the
             * top of the file). Previously this block did NOT move the
             * timestamp, and the window was counted only from the last
             * failed password check. Getting refused on a correct password,
             * the user could retry it a few seconds later and get in: the
             * timestamp stayed old, and of the ten seconds only what had
             * not yet elapsed while typing remained - 6-9 seconds in
             * practice instead of ten. */
            st.last = now;
            if (!write_state(fd, &st)) {
                log_msg(pamh, LOG_WARNING, "pam_failguard: cannot write state file");
            }
            log_msg(pamh, LOG_NOTICE,
                    "pam_failguard: authentication refused for user [%s], %ld failed logins, "
                    "window restarted, %ld s",
                    username, st.count, o.delay);
            ret = PAM_AUTH_ERR;
        } else {
            if (o.debug) {
                log_msg(pamh, LOG_DEBUG,
                        "pam_failguard: no refusal for user [%s] (%ld failed logins)",
                        username, st.count);
            }
            ret = PAM_IGNORE;
        }
    }

    flock(fd, LOCK_UN);
    close(fd);
    return ret;
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_IGNORE;
}
