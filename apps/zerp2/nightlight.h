// Zerp 2.0 - night light (nightlight.cpp): the screen warmer after sunset.
#pragma once
class QQuickWindow;
// the session's window: its frames get the night's colour when it is due
void nightlightAttach(QQuickWindow *win);
