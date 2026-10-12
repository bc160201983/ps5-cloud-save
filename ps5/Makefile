ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to the installed ps5-payload SDK directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

.PHONY: all clean deploy upload recovery backup dashboard share createtest writetest memdump
all: pscloud-probe.elf pscloud-export.elf pscloud-ps.elf
pscloud-ps.elf: ps-probe.c
	$(CC) -O2 -Wall -Wextra -Werror -o $@ ps-probe.c -lkernel
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
share: pscloud-share.elf pscloud-share-game.elf
pscloud-share-game.elf: share-game.c backup.c common/sharing.c common/sharing.h common/mount.c common/restore.c common/zip.c common/log.c common/snapshot.c common/savemeta.c common/appmeta.c common/slotcreate.c common/slotcreate.h sqlite3.o
	$(CC) -O2 -Wall -Wextra -Werror -DPSCLOUD_BACKUP_EMBEDDED $$($(CURL_CONFIG) --cflags) -I$(SQLITE_DIR) -o $@ share-game.c backup.c common/sharing.c common/slotcreate.c sqlite3.o common/mount.c common/restore.c common/zip.c common/log.c common/snapshot.c common/savemeta.c common/appmeta.c $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh
pscloud-share.elf: share.c backup.c common/portable.c common/portable.h common/mount.c common/restore.c common/zip.c common/log.c common/snapshot.c common/savemeta.c
	$(CC) -O2 -Wall -Wextra -Werror -DPSCLOUD_BACKUP_EMBEDDED $$($(CURL_CONFIG) --cflags) -o $@ share.c backup.c common/portable.c common/mount.c common/restore.c common/zip.c common/log.c common/snapshot.c common/savemeta.c $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh
ui.h: ui.html ../VERSION ../tools/embed_ui.py
	python3 ../tools/embed_ui.py ui.html ui.h ../VERSION
pscloud-dashboard.elf: dashboard.c ui.h backup.c download.c ../src/worker.c common/managed.c common/managed.h common/mount.c common/restore.c common/zip.c common/log.c common/cloud.c common/snapshot.c common/appmeta.c common/appmeta.h common/bundle.c common/bundle.h common/savemeta.c common/savemeta.h common/google.c common/google.h common/transfer.h common/sharing.c common/sharing.h common/slotcreate.c common/slotcreate.h sqlite3.o
	$(CC) -O2 -Wall -Wextra -Werror -DPSCLOUD_BACKUP_EMBEDDED -DPSCLOUD_DOWNLOAD_EMBEDDED -DPSCLOUD_EMBEDDED $$($(CURL_CONFIG) --cflags) -I$(SQLITE_DIR) -o $@ dashboard.c backup.c download.c ../src/worker.c common/managed.c common/mount.c common/restore.c common/zip.c common/log.c common/cloud.c common/snapshot.c common/appmeta.c common/bundle.c common/savemeta.c common/google.c common/sharing.c common/slotcreate.c sqlite3.o $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh -lSceSystemService -lpthread
# Test only: makes and reopens a new save container inside /data/pscloud/create-test.
createtest: pscloud-create-test.elf
pscloud-create-test.elf: create-test.c common/mount.c common/mount.h common/log.c common/appmeta.c common/appmeta.h common/restore.c common/zip.c common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ create-test.c common/mount.c common/log.c common/appmeta.c common/restore.c common/zip.c common/snapshot.c $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh
# Test only: adds two slots to PPSA10595 (and removes them on the next run).
SQLITE_DIR ?= third_party/sqlite
writetest: pscloud-write-test.elf
sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	$(CC) -O2 -w -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -c -o $@ $(SQLITE_DIR)/sqlite3.c
pscloud-write-test.elf: write-test.c sqlite3.o common/mount.c common/mount.h common/log.c
	$(CC) -O2 -Wall -Wextra -Werror -I$(SQLITE_DIR) -o $@ write-test.c sqlite3.o common/mount.c common/log.c -lkernel -lSceFsInternalForVsh
# Diagnostic: copies the running game's program image to /data/pscloud/dump (read only).
memdump: pscloud-memdump.elf
pscloud-memdump.elf: memdump.c
	$(CC) -O2 -Wall -Wextra -Werror -o $@ memdump.c -lkernel
pscloud-backup.elf: backup.c common/mount.c common/mount.h common/restore.c common/zip.c common/log.c common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ backup.c common/mount.c common/restore.c common/zip.c common/log.c common/snapshot.c $$($(CURL_CONFIG) --static-libs) -lkernel -lSceFsInternalForVsh
pscloud-download.elf: download.c common/cloud.c common/restore.c common/log.c common/restore.h common/cloud.h common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ download.c common/cloud.c common/restore.c common/log.c common/snapshot.c $$($(CURL_CONFIG) --static-libs)
pscloud-restore.elf: restore.c common/restore.c common/zip.c common/log.c common/restore.h common/snapshot.c
	$(CC) -O2 -Wall -Wextra -Werror $$($(CURL_CONFIG) --cflags) -o $@ restore.c common/restore.c common/zip.c common/log.c common/snapshot.c $$($(CURL_CONFIG) --static-libs)
deploy: pscloud-probe.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p 9021 $<
clean:
	rm -f pscloud-probe.elf pscloud-export.elf pscloud-ps.elf pscloud-upload.elf pscloud-download.elf pscloud-restore.elf pscloud-backup.elf pscloud-dashboard.elf pscloud-create-test.elf pscloud-write-test.elf pscloud-memdump.elf sqlite3.o ui.h
