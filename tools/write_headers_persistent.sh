#!/bin/bash
mkdir -p build_qt_headers/include/QtCore
mkdir -p build_qt_headers/include/QtGui
mkdir -p build_qt_headers/include/QtWidgets

cat << 'EOF' > build_qt_headers/include/QtCore/qconfig.h
#define QT_FEATURE_shared 1
#define QT_VERSION_STR "6.5.2"
#define QT_VERSION_MAJOR 6
#define QT_VERSION_MINOR 5
#define QT_VERSION_PATCH 2

// Core features
#define QT_FEATURE_shortcut 1
#define QT_FEATURE_action 1
#define QT_FEATURE_abstractbutton 1
#define QT_FEATURE_pushbutton 1
#define QT_FEATURE_label 1
#define QT_FEATURE_regularexpression 1
#define QT_FEATURE_textmarkdownwriter -1
#define QT_FEATURE_textmarkdownreader -1
#define QT_FEATURE_movie -1
#define QT_FEATURE_settings 1
#define QT_FEATURE_filesystemmodel 1
#define QT_FEATURE_filesystemwatcher 1
#define QT_FEATURE_properties 1
#define QT_FEATURE_animation 1
#define QT_FEATURE_library 1
#define QT_FEATURE_dynamicsortfilterproxymodel 1
#define QT_FEATURE_proxymodel 1
#define QT_FEATURE_itemmodel 1
#define QT_FEATURE_thread 1

// Avoid division by zero for other common features by defining them as disabled (-1)
#define QT_FEATURE_accessibility -1
#define QT_FEATURE_draganddrop -1
#define QT_FEATURE_clipboard -1
#define QT_FEATURE_im -1
#define QT_FEATURE_sessionmanager -1
#define QT_FEATURE_soundiness -1
#define QT_FEATURE_picture -1
#define QT_FEATURE_graphicsview -1
#define QT_FEATURE_graphicseffect -1
#define QT_FEATURE_sizegrip -1
#define QT_FEATURE_standarditemmodel -1
#define QT_FEATURE_dirtylifeline -1
#define QT_FEATURE_pdf -1
#define QT_FEATURE_xcb -1
#define QT_FEATURE_opengl -1
#define QT_FEATURE_vulkan -1
#define QT_FEATURE_egl -1
#define QT_FEATURE_systemtrayicon -1
#define QT_FEATURE_undocommand -1
#define QT_FEATURE_undostack -1
#define QT_FEATURE_undogroup -1
#define QT_FEATURE_undoview -1
#define QT_FEATURE_keysequenceedit -1
#define QT_FEATURE_widgetaction -1
#define QT_FEATURE_lineedit -1
#define QT_FEATURE_validator -1
#define QT_FEATURE_completer -1
#define QT_FEATURE_progressbar -1
#define QT_FEATURE_combobox -1
#define QT_FEATURE_spinbox -1
#define QT_FEATURE_scrollarea -1
#define QT_FEATURE_scrollbar -1
#define QT_FEATURE_slider -1
#define QT_FEATURE_dial -1
#define QT_FEATURE_groupbox -1
#define QT_FEATURE_splitter -1
#define QT_FEATURE_toolbox -1
#define QT_FEATURE_stackedwidget -1
#define QT_FEATURE_tabbar -1
#define QT_FEATURE_tabwidget -1
#define QT_FEATURE_statusbar -1
#define QT_FEATURE_menu -1
#define QT_FEATURE_menubar -1
#define QT_FEATURE_toolbar -1
#define QT_FEATURE_dockwidget -1
#define QT_FEATURE_dialog -1
#define QT_FEATURE_dialogbuttonbox -1
#define QT_FEATURE_messagebox -1
#define QT_FEATURE_filedialog -1
#define QT_FEATURE_fontdialog -1
#define QT_FEATURE_colordialog -1
#define QT_FEATURE_inputdialog -1
#define QT_FEATURE_errormessage -1
#define QT_FEATURE_progressdialog -1
#define QT_FEATURE_wizard -1
EOF

cat << 'EOF' > build_qt_headers/include/QtCore/qtcoreexports.h
#ifndef QTCOREEXPORTS_H
#define QTCOREEXPORTS_H
#define Q_CORE_EXPORT
#endif
EOF

cat << 'EOF' > build_qt_headers/include/QtGui/qtguiexports.h
#ifndef QTGUIEXPORTS_H
#define QTGUIEXPORTS_H
#define Q_GUI_EXPORT
#endif
EOF

cat << 'EOF' > build_qt_headers/include/QtWidgets/qtwidgetsexports.h
#ifndef QTWIDGETSEXPORTS_H
#define QTWIDGETSEXPORTS_H
#define Q_WIDGETS_EXPORT
#endif
EOF

# Define inline macros as empty to avoid compilation and circular dependency checks
cat << 'EOF' > build_qt_headers/include/QtCore/qtcore-config.h
#ifndef QT_CORE_CONFIG_H
#define QT_CORE_CONFIG_H
#define QT_CORE_INLINE_SINCE(major, minor)
#define QT_CORE_INLINE_IMPL_SINCE(major, minor) 0
#define QT_CORE_REMOVED_SINCE(major, minor) 0
#endif
EOF

cat << 'EOF' > build_qt_headers/include/QtGui/qtgui-config.h
#ifndef QT_GUI_CONFIG_H
#define QT_GUI_CONFIG_H
#define QT_GUI_INLINE_SINCE(major, minor)
#define QT_GUI_INLINE_IMPL_SINCE(major, minor) 0
#define QT_GUI_REMOVED_SINCE(major, minor) 0
#endif
EOF

cat << 'EOF' > build_qt_headers/include/QtWidgets/qtwidgets-config.h
#ifndef QT_WIDGETS_CONFIG_H
#define QT_WIDGETS_CONFIG_H
#define QT_WIDGETS_INLINE_SINCE(major, minor)
#define QT_WIDGETS_INLINE_IMPL_SINCE(major, minor) 0
#define QT_WIDGETS_REMOVED_SINCE(major, minor) 0
#endif
EOF
