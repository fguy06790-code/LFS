
# Added by build-lfs.sh: the /usr variant of the KF6 additions to qt6.sh, from
# "KDE Frameworks 6 Pre-installation Configuration" in BLFS. They must follow
# Qt 6, since this appends to the qt6.sh that Qt's own section creates.
cat >> /etc/profile.d/qt6.sh << "EOF"
# Begin kf6 extension for /etc/profile.d/qt6.sh

pathappend /usr/lib/plugins        QT_PLUGIN_PATH
pathappend $QT6DIR/lib/plugins     QT_PLUGIN_PATH

pathappend /usr/lib/qt6/qml        QML2_IMPORT_PATH
pathappend $QT6DIR/lib/qml         QML2_IMPORT_PATH

# End extension for /etc/profile.d/qt6.sh
EOF
