# SideETH.STX — STinG Ethernet driver for SideTNFS
#
# Requires m68k-atari-mint-gcc (or Pure-C with adjustments).
# Headers default to a sibling checkout of th-otto/STinG:
#   ../../Atari/STinG/include   (from this repo's parent "3rd party")
# Override with: make STING_INCLUDE=/path/to/STinG/include

STING_INCLUDE ?= ../../../Atari/STinG/include

CROSSPREFIX ?= m68k-atari-mint-
CC = $(CROSSPREFIX)gcc
AS = $(CC)

WARN = -Wall -W -Wstrict-prototypes -Wmissing-prototypes
CFLAGS = -O2 -fomit-frame-pointer $(WARN) -I"$(STING_INCLUDE)" -I.
LDFLAGS = -s

# Bisection (optional): make SIDEETH_NO_TX=1  or  SIDEETH_NO_RX=1
ifneq ($(SIDEETH_NO_TX),)
CFLAGS += -DSIDEETH_NO_TX
endif
ifneq ($(SIDEETH_NO_RX),)
CFLAGS += -DSIDEETH_NO_RX
endif

SRCS = sideeth.c cart.c arp.c tools.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: sideeth.stx

sideeth.stx: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) sideeth.stx
