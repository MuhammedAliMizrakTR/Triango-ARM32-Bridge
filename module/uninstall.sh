#!/system/bin/sh
# Triango ARM32 Bridge — module removal cleanup.
#
# binfmt_misc entry removal: "echo -1 > /proc/sys/fs/binfmt_misc/<name>"
# (documented in Documentation/admin-guide/binfmt-misc.rst).
BM=/proc/sys/fs/binfmt_misc
[ -e "$BM/triango-arm32" ] && echo -1 > "$BM/triango-arm32" 2>/dev/null

rm -rf /data/adb/triango
exit 0
