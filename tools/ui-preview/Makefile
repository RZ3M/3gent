CC ?= cc
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g \
          -Ishim -I../../client-3ds/include -I.
LDLIBS := -lm

SOURCES := preview.c svg_backend.c ../../client-3ds/source/ui.c

.PHONY: all run clean

all: preview

preview: $(SOURCES) shim/3ds.h shim/citro2d.h svg_backend.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

run: preview
	@mkdir -p out
	./preview

clean:
	rm -rf preview preview.dSYM out
