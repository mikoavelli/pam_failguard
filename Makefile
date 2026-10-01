CC      ?= cc
CFLAGS  ?= -Wall -O2 -fPIC
TARGET   = pam_failguard.so
SRC      = src/pam_failguard.c

PAMDIR  ?= $(shell dirname "$$(ldconfig -p | grep -m1 -oE '/[^ ]*libpam\.so\.0')")/security

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -shared -o $@ $<

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 644 $(TARGET) $(PAMDIR)/$(TARGET)

uninstall:
	rm -f $(PAMDIR)/$(TARGET)
	rm -rf /var/lib/pam_failguard

.PHONY: all clean install uninstall