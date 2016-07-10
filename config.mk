# ratox version
VERSION = 0.2.1

# paths
PREFIX = /usr/
MANPREFIX = $(PREFIX)/share/man

CC = cc
LD = $(CC)
CPPFLAGS = -DVERSION=\"${VERSION}\"
CFLAGS   = -g -I/usr/local/include -Wall -Wunused $(CPPFLAGS)
LDFLAGS  = -g -L/usr/local/lib -ltoxcore -ltoxav -ltoxencryptsave
