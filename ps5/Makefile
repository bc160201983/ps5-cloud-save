ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to the installed ps5-payload SDK directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

.PHONY: all clean deploy upload
all: pscloud-probe.elf pscloud-export.elf
pscloud-probe.elf: probe.c common/log.c common/log.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ probe.c common/log.c
pscloud-export.elf: export.c common/log.c common/zip.c common/log.h common/zip.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ export.c common/log.c common/zip.c
# Requires the separately installed PS5 curl/OpenSSL libraries. Keep existing
# diagnostic builds usable with the base SDK alone.
upload: pscloud-upload.elf
CURL_CONFIG ?= $(PS5_PAYLOAD_SDK)/bin/prospero-curl-config
pscloud-upload.elf: upload.c ../src/worker.c common/log.c common/log.h
	@test -x "$(CURL_CONFIG)" || { echo "Install PS5 curl/OpenSSL libraries first"; exit 1; }
	$(CC) -O2 -Wall -Wextra -Werror -DPSCLOUD_EMBEDDED $$($(CURL_CONFIG) --cflags) -o $@ upload.c ../src/worker.c common/log.c $$($(CURL_CONFIG) --static-libs)
deploy: pscloud-probe.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p 9021 $<
clean:
	rm -f pscloud-probe.elf pscloud-export.elf pscloud-upload.elf
