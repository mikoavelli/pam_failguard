CC      ?= cc
CFLAGS  ?= -Wall -O2 -fPIC
TARGET   = pam_failguard.so
SRC      = src/pam_failguard.c

# Install directories. PAM searches several module paths, and the set of
# paths depends on how the owning process linked libpam:
#
#   $(PAMDIR)   the stock PAM library's module directory (multiarch); used
#               by sshd, su, sudo, login;
#   /lib/security
#               the path compiz checks - it is the one that authenticates
#               screen unlocking on Ubuntu 15.10 (Unity). Without a copy
#               here, the lock screen yields
#               "PAM unable to dlopen(pam_failguard.so)".
#
# Install into both: a redundant 12 KB copy hurts nothing, and the lock
# screen keeps working.
PAMDIR  ?= $(shell dirname "$$(ldconfig -p | grep -m1 -oE '/[^ ]*libpam\.so\.0')")/security
PAMDIRS ?= $(PAMDIR) /lib/security

# The module MUST be linked with libpam, otherwise it will not load in
# processes that do not link libpam themselves.
#
# The pam_get_user/pam_syslog symbols were undefined at load time: the
# module relied on them being in the global scope of whatever process
# loaded it. True for sshd/su/sudo (they link -lpam), false for compiz:
# it reaches PAM through dlopen, and libpam.so.0 is not global in its
# scope. On the Unity lock screen the result was:
#
#   PAM unable to dlopen(pam_failguard.so): undefined symbol: pam_syslog
#
# Link against the versioned soname (-l:libpam.so.0), not -lpam: the
# latter needs libpam0g-dev, which may be absent on the target system.
# The module's own dependencies remain just libc and libpam.
LDFLAGS ?= -l:libpam.so.0

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@for d in $(PAMDIRS); do \
	    mkdir -p "$$d" && install -m 644 $(TARGET) "$$d/$(TARGET)" \
	        && echo "installed to $$d" \
	        || echo "NOT INSTALLED to $$d (check the path manually)" >&2; \
	done

uninstall:
	@for d in $(PAMDIRS); do rm -f "$$d/$(TARGET)"; done
	rm -rf /var/lib/pam_failguard

.PHONY: all clean install uninstall
