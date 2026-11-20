# 9.5. General Network Configuration (override)
# The book's values are placeholders. These defaults match QEMU's user-mode
# network; change NET_* and HOST_NAME in lfs.conf for other setups.
. /sources/lfs-build/config.env

cd /etc/sysconfig/
cat > ifconfig.eth0 << EOF
ONBOOT=yes
IFACE=eth0
SERVICE=ipv4-static
IP=$NET_IP
GATEWAY=$NET_GATEWAY
PREFIX=$NET_PREFIX
BROADCAST=$NET_BROADCAST
EOF

cat > /etc/resolv.conf << EOF
# Begin /etc/resolv.conf

nameserver $NET_DNS

# End /etc/resolv.conf
EOF

echo "$HOST_NAME" > /etc/hostname

cat > /etc/hosts << EOF
# Begin /etc/hosts

127.0.0.1 localhost.localdomain localhost
127.0.1.1 $HOST_NAME.localdomain $HOST_NAME
::1       localhost ip6-localhost ip6-loopback
ff02::1   ip6-allnodes
ff02::2   ip6-allrouters

# End /etc/hosts
EOF
