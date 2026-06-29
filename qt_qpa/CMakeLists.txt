# ==============================================================================
# WynlandOS - CMake file for QWynlandFb platform plugin (Qt6)
# ==============================================================================

qt_internal_add_plugin(QWynlandFbPlugin
    OUTPUT_NAME qwynlandfb
    PLUGIN_TYPE platforms
    SOURCES
        qwynlandfbmain.cpp
        qwynlandfbintegration.h qwynlandfbintegration.cpp
        qwynlandfbscreen.h qwynlandfbscreen.cpp
        qwynlandfbinput.h qwynlandfbinput.cpp
    LIBRARIES
        Qt::CorePrivate
        Qt::GuiPrivate
)
