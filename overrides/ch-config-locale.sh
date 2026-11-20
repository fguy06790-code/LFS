# 9.7. Configuring the System Locale (override)
# The book's first commands only display locale information, and /etc/profile
# has a placeholder. This writes /etc/profile with LFS_LANG from lfs.conf.
. /sources/lfs-build/config.env

# Make sure the chosen locale exists (glibc installs common ones in chapter 8).
if ! locale -a 2>/dev/null | grep -qix "$(echo "$LFS_LANG" | sed 's/UTF-8/utf8/I')"; then
    localedef -i "${LFS_LANG%%.*}" -f UTF-8 "$LFS_LANG" || echo "warning: could not create locale $LFS_LANG"
fi

cat > /etc/profile << "EOF"
# Begin /etc/profile

for i in $(locale); do
  unset ${i%=*}
done

if [[ "$TERM" = linux ]]; then
  export LANG=C.UTF-8
else
  export LANG=@LFS_LANG@
fi

# End /etc/profile
EOF
sed -i "s|@LFS_LANG@|$LFS_LANG|" /etc/profile
