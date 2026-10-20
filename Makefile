ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to your installed ps5-payload-sdk directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -O2 -Wall -Wextra -Werror
LDLIBS := -lSceFsInternalForVsh -lSceNotification -lpthread

all: backup.elf

backup.elf: main.c notification_icon.h backup_progress.h
	$(CC) $(CFLAGS) main.c $(LDLIBS) -o $@

.PHONY: all clean
clean:
	rm -f backup.elf
