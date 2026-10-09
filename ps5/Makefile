ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to the installed ps5-payload SDK directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

.PHONY: all clean deploy upload recovery backup dashboard
all: pscloud-probe.elf pscloud-export.elf
pscloud-probe.elf: probe.c common/log.c common/log.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ probe.c common/log.c
pscloud-export.elf: export.c common/log.c common/zip.c common/log.h common/zip.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ export.c common/log.c common/zip.c
# Requires the separately installed PS5 curl/OpenSSL libraries. Keep existing
# diagnostic builds usable with the base SDK alone.
upload: pscloud-upload.elf
CURL_CONFIG ?= $(PS5_PAYLOAD_SDK)/bin/prospero-curl-config
pscloud-upload.elf: upload.c ../src/worker.c common/log.c common/log.h common/cloud.c common/cloud.h common/snapshot.c common/snapshot.h
	@test -x "$(CURL_CONFIG)" || { echo "Install PS5 curl/OpenSSL libraries first"; exit 1; }
	$(CC) -O2 -Wall -Wextra -Werror -DPSCLOUD_EMBEDDED $$($(CURL_CONFIG) --cflags) -o $@ upload.c ../src/worker.c common/log.c common/cloud.c common/snapshot.c $$($(CURL_CONFIG) --static-libs)
recovery: pscloud-download.elf pscloud-restore.elf
backup: pscloud-backup.elf
dashboard: pscloud-dashboard.elf
ui.h: ui.html ../VERSION ../tools/embed_ui.py
	python3 ../tools/embed_ui.py ui.html ui.h ../VERSION
pscloud-dashboard.elf: dashboard.c ui.h backup.c download.c ../src/worker.c common/managed.c common/managed.h common/mount.c common/restore.c common/zip.c common/log.c common/cloud.c common/snapshot.c common/appmeta.c common/appmeta.h common/bundle.c common/bundle.h common/savemeta.c common/savemeta.h common/google.c common/google.h common/transfer.h
	$(CC) -O2 -Wall -Wextra -Werror -DPSCLOUD_BACKUP_EMBEDDED -DPSCLOUD_DOWNLOAD_EMBEDDED -DPSCLOUD_EMBEDDED $$($(CURL_CONFIG) --cflags) -o $@ dashboard.c backup.c download.c ../src/worker.c common/managed.c common/mount.c common/restore.c common/zip.c common/log.c common/cloud.c common/snapshot.c common/appmeta.c common/bundle.c common/savemeta.c common/google.c $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh -lpthread
pscloud-backup.elf: backup.c common/mount.c common/mount.h common/restore.c common/zip.c common/log.c common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ backup.c common/mount.c common/restore.c common/zip.c common/log.c common/snapshot.c $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh
pscloud-download.elf: download.c common/cloud.c common/restore.c common/log.c common/restore.h common/cloud.h common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ download.c common/cloud.c common/restore.c common/log.c common/snapshot.c $$($(CURL_CONFIG) --static-libs)
pscloud-restore.elf: restore.c common/restore.c common/zip.c common/log.c common/restore.h common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ restore.c common/restore.c common/zip.c common/log.c common/snapshot.c $$($(CURL_CONFIG) --static-libs)
deploy: pscloud-probe.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p 9021 $<
clean:
	rm -f pscloud-probe.elf pscloud-export.elf pscloud-upload.elf pscloud-download.elf pscloud-restore.elf pscloud-backup.elf pscloud-dashboard.elf ui.h
