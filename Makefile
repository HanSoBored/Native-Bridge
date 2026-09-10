CC ?= clang
# C23 standard with strict warnings and static analysis
CFLAGS = -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion \
         -Wnull-dereference -Wformat=2 -Wstrict-prototypes -Wold-style-definition \
         -Wmissing-prototypes -Wimplicit-fallthrough -Wvla \
         -Werror=implicit-function-declaration -O2 -pthread \
         -I$(SRCDIR)/common -fstack-protector-strong
LDFLAGS ?= -static

SRCDIR = src
BUILDDIR = build

all: $(BUILDDIR) $(BUILDDIR)/nativeb_server $(BUILDDIR)/andro

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/nativeb_server: $(SRCDIR)/main/server.c $(SRCDIR)/common/input.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -DDIRECT_INPUT $^ -o $@

$(BUILDDIR)/andro: $(SRCDIR)/main/client.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

# Target MCP
mcp: $(BUILDDIR)/nativeb_mcp

# lodepng.c is vendored third-party (pre-C23, conversion-heavy); silence
# all warnings for that TU only — own code stays under strict CFLAGS.
$(BUILDDIR)/lodepng.o: $(SRCDIR)/common/lodepng.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -w -c $< -o $@

# Explicit rule so screenshot.o uses project CFLAGS before linking into nativeb_mcp
$(BUILDDIR)/screenshot.o: $(SRCDIR)/common/screenshot.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/nativeb_mcp: $(SRCDIR)/main/mcp.c $(SRCDIR)/common/config.c $(BUILDDIR)/screenshot.o $(BUILDDIR)/lodepng.o | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILDDIR)/test_config: tests/test_config.c $(SRCDIR)/common/config.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILDDIR)/test_config
	$(BUILDDIR)/test_config

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean mcp test
