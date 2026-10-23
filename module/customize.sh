#!/system/bin/sh
MODPATH=${0%/*}
ui_print "- com.carxtech.sr Control installing"
set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/zygisk/arm64-v8a.so 0 0 0644
ui_print "- done, reboot to take effect"
