CC ?= cc
QUIRC := ../../client-3ds/third_party/quirc
CFLAGS := -std=gnu11 -O2 -g -I$(QUIRC)
LDLIBS :=

SOURCES := check.c $(QUIRC)/quirc.c $(QUIRC)/decode.c $(QUIRC)/identify.c \
           $(QUIRC)/version_db.c

BRIDGE := ../../bridge
SAMPLE ?= 3gent://pair?v=1&h=192.168.1.42&p=8080&q=8081&c=K7M2QX4T9BWF&n=jm-mbp

.PHONY: all run clean

all: qrcheck

qrcheck: $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

# Encodes with the bridge and decodes with the handheld's decoder.
run: qrcheck
	@cd $(BRIDGE) && npm run --silent build
	@node $(BRIDGE)/dist/src/qr-matrix-cli.js '$(SAMPLE)' | ./qrcheck 4

clean:
	rm -rf qrcheck qrcheck.dSYM
