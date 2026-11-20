# 7.13. Cleaning up the Temporary System (override)
# The book continues with an optional backup that runs outside the chroot.
# build-lfs.sh keeps its own progress, so only the cleanup runs here.
rm -rf /usr/share/{info,man,doc}/*
find /usr/{lib,libexec} -name \*.la -delete
rm -rf /tools
