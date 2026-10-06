# A6L r6e (1 Oct 2026, docs/rom-r6e-20261001.md): file modes for vendor files outside the standard bin dirs.
# /vendor/a6l/radio/bin/* (rmtfs, tqftpserv, diag-router, qrtr-lookup, iw + their libs) are PRODUCT_COPY_FILES; without an
# entry here the build gives them the default 0644, and every r6d boot logged "/vendor/a6l/radio/bin/rmtfs: can't execute:
# Permission denied" (rmtfs/tqftpserv/diag-router restarted every 5 s -> the modem never got its EFS/firmware services).
[vendor/a6l/radio/bin/*]
mode: 0755
user: AID_ROOT
group: AID_SHELL
caps: 0
