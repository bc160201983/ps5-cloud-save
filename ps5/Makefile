ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to the installed ps5-payload SDK directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

.PHONY: all clean deploy
all: pscloud-probe.elf pscloud-export.elf
pscloud-probe.elf: probe.c common/log.c common/log.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ probe.c common/log.c
pscloud-export.elf: export.c common/log.c common/zip.c common/log.h common/zip.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ export.c common/log.c common/zip.c
deploy: pscloud-probe.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p 9021 $<
clean:
	rm -f pscloud-probe.elf pscloud-export.elf
