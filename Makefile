# Makefile — Syscall Throttling LKM + userspace tools

# Nome del modulo kernel (.ko prodotto)
MODULE  := throttleDriver

# Tool userspace
CLIENT  := throttleClient
TEST    := throttleTest
TEST2   := throttleTest2

# Directory sorgente del kernel in esecuzione
KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

# Target principale

obj-m += $(MODULE).o
$(MODULE)-objs := throttle_mem.o throttle_discovery.o throttle_hook.o \
                  throttle_core.o throttle_dev.o throttle_main.o

all: module userspace

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace: $(CLIENT) $(TEST) $(TEST2)

$(CLIENT): $(CLIENT).c
	gcc -Wall -o $@ $<

$(TEST): $(TEST).c
	gcc -Wall -o $@ $< -lpthread

$(TEST2): $(TEST2).c
	gcc -Wall -o $@ $< -lpthread

# Per inserimento/rimozione modulo 
load: module
	sudo insmod $(MODULE).ko
	sudo mknod /dev/$(MODULE) c $$(awk '$$2=="$(MODULE)" {print $$1}' /proc/devices) 0
	sudo chmod 0666 /dev/$(MODULE)

unload:
	@if ! lsmod | grep -q "^$(MODULE) "; then \
	    echo "$(MODULE) non è caricato, nulla da fare."; \
	    sudo rm -f /dev/$(MODULE); \
	    exit 0; \
	fi
	@MAJOR=$$(awk '$$2=="$(MODULE)"{print $$1}' /proc/devices 2>/dev/null); \
	 if [ -n "$$MAJOR" ] && [ ! -c /dev/$(MODULE) ]; then \
	     sudo mknod /dev/$(MODULE) c $$MAJOR 0; \
	     sudo chmod 0666 /dev/$(MODULE); \
	 fi
	-sudo $(PWD)/$(CLIENT) monitor 0
	sleep 0.2
	sudo rmmod $(MODULE) || true
	sudo rm -f /dev/$(MODULE)

reload: unload load

# Pulizia
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(CLIENT) $(TEST) $(TEST2)

.PHONY: all module userspace load unload reload clean
