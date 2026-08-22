# ==============================================================================
# WynlandOS - CMake file for QWynlandFb platform plugin (Qt6)
# ==============================================================================

qt_internal_add_plugin(QWynlandFbPlugin
    OUTPUT_NAME qwynlandfb
    PLUGIN_TYPE platforms
    STATIC # this OS has exactly one QPA plugin and no runtime plugin
           # directory scanning need -- link it straight into every Zerp
           # Qt app via Q_IMPORT_PLUGIN instead
    SOURCES
        qwynlandfbmain.cpp
        qwynlandfb_zerpargs.h qwynlandfb_zerpargs.cpp
        qwynlandfbintegration.h qwynlandfbintegration.cpp
        qwynlandfbscreen.h qwynlandfbscreen.cpp
        qwynlandfbinput.h qwynlandfbinput.cpp
    LIBRARIES
        Qt::CorePrivate
        Qt::GuiPrivate
)
