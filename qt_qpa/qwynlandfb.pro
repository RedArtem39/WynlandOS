TARGET = qwynlandfb
TEMPLATE = lib
CONFIG += plugin c++11

QT += core-private gui-private

HEADERS += \
    qwynlandfbintegration.h \
    qwynlandfbscreen.h \
    qwynlandfbinput.h

SOURCES += \
    qwynlandfbmain.cpp \
    qwynlandfbintegration.cpp \
    qwynlandfbscreen.cpp \
    qwynlandfbinput.cpp

OTHER_FILES += qwynlandfb.json

target.path = $$[QT_INSTALL_PLUGINS]/platforms
INSTALLS += target
