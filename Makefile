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

$(BUILDDIR)/nativeb_mcp: $(SRCDIR)/main/mcp.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean mcp
