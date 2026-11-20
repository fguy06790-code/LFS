
# Added by build-lfs.sh: boot into the graphical login (runlevel 5), where the
# sddm boot script runs. The LFS inittab defaults to the text console (3).
sed -i 's/^id:3:initdefault:/id:5:initdefault:/' /etc/inittab
