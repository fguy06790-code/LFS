# Begin /etc/profile.d/lfs-setup-hint.sh
# Remind root to run lfs-setup while the system has no regular user yet.
if [ "$(id -u)" = 0 ] && [ -t 1 ] && ! awk -F: '$3 >= 1000 && $3 < 60000 { found = 1 } END { exit !found }' /etc/passwd; then
    echo
    echo "  Welcome to Linux From Scratch! Run 'lfs-setup' to set the timezone,"
    echo "  language, keyboard, network and to create your user account."
    echo
fi
# End /etc/profile.d/lfs-setup-hint.sh
