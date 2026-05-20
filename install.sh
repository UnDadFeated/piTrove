#!/usr/bin/env bash
# install.sh — PiTrove v7.1.0 installer
  echo "  PiTrove v7.1.0 Installer"
# piTrove Configuration File (v7.1.0)
 echo "  PiTrove v7.1.0 installation complete!"
echo "============================================"
echo

# Conditional next steps based on mount outcome
echo "  Next steps:"
if [[ "$storage_choice" -eq 2 ]]; then
    echo "    NAS: ✓ Local storage"
    echo "    1. Start UI: piTrove --config (runs TUI wizard)"
    echo "    2. Auto-start is enabled (reboot to test)"
elif [[ "$USE_NAS" -eq 1 || "$storage_choice" == "3" ]]; then
    if [[ "$NAS_MOUNT_SUCCESS" -eq 1 ]]; then
        echo "    NAS: ✓ Already mounted at /mnt/nas"
        echo "    1. Start UI: piTrove --config (runs TUI wizard)"
        echo "    2. Auto-start is enabled (reboot to test)"
    else
        echo "    NAS: ✗ Not mounted — configure manually"
        echo "    1. Add to /etc/fstab"
        echo "       Example: //192.168.4.111/Home/Archive /mnt/nas cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail 0 0"
        echo "    2. Run: sudo mount -a"
        echo "    3. Start UI: piTrove --config (runs TUI wizard)"
        echo "    4. Auto-start is enabled (reboot to test)"
    fi
fi
echo
echo "  Directories:"
echo "  Config:       $PRIMARY_HOME/piTrove/src/config/config.toml"
echo "  Source:       $PRIMARY_HOME/piTrove/src/"
echo "  Cache:        $PRIMARY_HOME/.cache/piTrove/"
echo "  Logs:         $PRIMARY_HOME/piTrove/logs/"
echo
