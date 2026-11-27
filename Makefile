# SPDX-License-Identifier: MIT
#
# make                  module, shsctl and libshs into build/
# sudo make install     DKMS module, tools, library and headers; loads at boot
# sudo make uninstall   undoes install

MOD		:= strix_halo_smu
VERSION		:= $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
BOARD		:= GZ302EA

KVER		?= $(shell uname -r)
KDIR		?= /lib/modules/$(KVER)/build

BUILD		:= $(CURDIR)/build
MODOUT		:= $(BUILD)/module
USROUT		:= $(BUILD)/user

PREFIX		?= /usr/local
BINDIR		:= $(DESTDIR)$(PREFIX)/bin
LIBDIR		:= $(DESTDIR)$(PREFIX)/lib
INCDIR		:= $(DESTDIR)$(PREFIX)/include
DKMS_SRC	:= $(DESTDIR)/usr/src/$(MOD)-$(VERSION)
LOAD_CONF	:= $(DESTDIR)/etc/modules-load.d/$(MOD).conf
PARAM_CONF	:= /etc/modprobe.d/$(MOD).conf

SONAME		:= libshs.so.0
CFLAGS		?= -O2
SHS_CFLAGS	:= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -I$(CURDIR) -fPIC
LIB_DEPS	:= user/libshs.c user/shs.h strix_halo_smu.h

# Prints the package-manager command for $(1) (dkms or headers) on Arch, Fedora and Debian families.
define pkg_hint
for f in /etc/os-release /usr/lib/os-release; do [ -r $$f ] && . $$f && break; done; \
case " $$ID $$ID_LIKE " in \
*" arch "*) k=$$(cat /usr/lib/modules/$(KVER)/pkgbase 2>/dev/null || echo linux); \
	[ $(1) = dkms ] && echo "  sudo pacman -S dkms" || echo "  sudo pacman -S $$k-headers";; \
*" fedora "*) [ $(1) = dkms ] && echo "  sudo dnf install dkms" || echo "  sudo dnf install kernel-devel-$(KVER)";; \
*" debian "*) [ $(1) = dkms ] && echo "  sudo apt install dkms" || echo "  sudo apt install linux-headers-$(KVER)";; \
*) echo "  install the $(1) package of your distribution";; \
esac
endef

define need_root
[ "$$(id -u)" = 0 ] || { echo "$@: run as root: sudo make $@"; exit 1; }
endef

.PHONY: all module tools clean install install-check install-dkms install-tools install-boot uninstall

all: module tools

module:
	@mkdir -p $(MODOUT)
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(MODOUT) modules

tools: $(USROUT)/shsctl $(USROUT)/$(SONAME)

$(USROUT)/$(SONAME): $(LIB_DEPS)
	@mkdir -p $(USROUT)
	$(CC) $(CFLAGS) $(SHS_CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(SONAME) -o $@ user/libshs.c -lm

$(USROUT)/shsctl: user/shsctl.c $(LIB_DEPS)
	@mkdir -p $(USROUT)
	$(CC) $(CFLAGS) $(SHS_CFLAGS) $(LDFLAGS) -o $@ user/shsctl.c user/libshs.c -lm

clean:
	rm -rf $(BUILD)

install: tools
	@$(MAKE) --no-print-directory install-check
	@$(MAKE) --no-print-directory install-dkms
	@$(MAKE) --no-print-directory install-tools
	@$(MAKE) --no-print-directory install-boot
	@if grep -q "^$(MOD) " /proc/modules; then modprobe -r $(MOD) || echo "warning: could not unload the running $(MOD); the new one loads after reboot"; fi
	@if modprobe $(MOD); then echo "$(MOD) $(VERSION) installed and loaded"; \
	else echo "warning: installed, but $(MOD) refused to load; see: sudo dmesg | grep $(MOD)"; fi

# FORCE=1 skips the board check, for packaging and CI on other machines.
install-check:
	@$(need_root)
	@[ "$(FORCE)" = 1 ] || [ "$$(cat /sys/class/dmi/id/board_name 2>/dev/null)" = $(BOARD) ] || \
		{ echo "this machine is not a $(BOARD); $(MOD) would refuse to load (FORCE=1 to install anyway)"; exit 1; }
	@command -v dkms >/dev/null || { echo "dkms is not installed:"; $(call pkg_hint,dkms); exit 1; }
	@[ -d $(KDIR) ] || { echo "kernel headers for $(KVER) are missing:"; $(call pkg_hint,headers); exit 1; }

install-dkms:
	@$(need_root)
	@if dkms status -m $(MOD) -v $(VERSION) | grep -q .; then dkms remove $(MOD)/$(VERSION) --all; fi
	install -d $(DKMS_SRC)
	install -m 0644 Kbuild dkms.conf $(MOD).c $(MOD).h $(DKMS_SRC)/
	dkms add $(MOD)/$(VERSION)
	dkms build $(MOD)/$(VERSION) -k $(KVER)
	dkms install $(MOD)/$(VERSION) -k $(KVER)

install-tools: tools
	install -Dm 0755 $(USROUT)/shsctl $(BINDIR)/shsctl
	install -Dm 0755 $(USROUT)/$(SONAME) $(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(LIBDIR)/libshs.so
	install -Dm 0644 user/shs.h $(INCDIR)/shs.h
	install -Dm 0644 $(MOD).h $(INCDIR)/$(MOD).h
	@[ -n "$(DESTDIR)" ] || ldconfig

install-boot:
	install -Dm 0644 /dev/null $(LOAD_CONF)
	echo $(MOD) > $(LOAD_CONF)

uninstall:
	@$(need_root)
	@if grep -q "^$(MOD) " /proc/modules; then modprobe -r $(MOD) || echo "warning: $(MOD) is in use; it stays loaded until reboot"; fi
	@for v in $$(dkms status -m $(MOD) 2>/dev/null | sed 's#^[^/]*/\([^,:]*\).*#\1#' | sort -u); do \
		echo "dkms remove $(MOD)/$$v --all"; dkms remove $(MOD)/$$v --all; done
	rm -rf $(DESTDIR)/usr/src/$(MOD)-*
	rm -f $(LOAD_CONF) $(BINDIR)/shsctl $(LIBDIR)/$(SONAME) $(LIBDIR)/libshs.so
	rm -f $(INCDIR)/shs.h $(INCDIR)/$(MOD).h
	@[ -n "$(DESTDIR)" ] || ldconfig
	@[ ! -e $(PARAM_CONF) ] || echo "note: kept $(PARAM_CONF) (your module parameters); remove it by hand if unwanted"
