# ratox version
VERSION = 0.2.2
# paths
PREFIX = /usr
MANPREFIX = $(PREFIX)/share/man
CC = cc
LD = $(CC)
CPPFLAGS = -DVERSION=\"${VERSION}\"
CFLAGS   = -g -I/usr/include -Wall -Wunused $(CPPFLAGS) $(shell dpkg-buildflags --get CFLAGS)
LDFLAGS  = -g -L/usr/lib -ltoxcore -ltoxav -ltoxencryptsave -lrt $(shell dpkg-buildflags --get LDFLAGS)