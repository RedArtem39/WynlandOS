#!/bin/bash
mkdir -p /tmp/qt_build_tmp/build_qt/include/QtCore
mkdir -p /tmp/qt_build_tmp/build_qt/include/QtGui
mkdir -p /tmp/qt_build_tmp/build_qt/include/QtWidgets

cat << 'EOF' > /tmp/qt_build_tmp/build_qt/include/QtCore/qconfig.h
#define QT_FEATURE_shared 1
#define QT_VERSION_STR "6.5.2"
#define QT_VERSION_MAJOR 6
#define QT_VERSION_MINOR 5
#define QT_VERSION_PATCH 2
EOF

cat << 'EOF' > /tmp/qt_build_tmp/build_qt/include/QtCore/qtcoreexports.h
#ifndef QTCOREEXPORTS_H
#define QTCOREEXPORTS_H
#define Q_CORE_EXPORT
#endif
EOF

cat << 'EOF' > /tmp/qt_build_tmp/build_qt/include/QtGui/qtguiexports.h
#ifndef QTGUIEXPORTS_H
#define QTGUIEXPORTS_H
#define Q_GUI_EXPORT
#endif
EOF

cat << 'EOF' > /tmp/qt_build_tmp/build_qt/include/QtWidgets/qtwidgetsexports.h
#ifndef QTWIDGETSEXPORTS_H
#define QTWIDGETSEXPORTS_H
#define Q_WIDGETS_EXPORT
#endif
EOF
