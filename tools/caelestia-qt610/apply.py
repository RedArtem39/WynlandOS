#!/usr/bin/env python3
# WynlandOS - Caelestia uses Qt 6.11's QML; the image has Ubuntu's Qt 6.10.
# What it uses of 6.11, made of 6.10 here (tools/stage_caelestia.sh runs
# this on the checked-out source). Drop it with Qt 6.11.
#
# usage: apply.py CAELESTIA_SRC
import os
import shutil
import sys

src = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))


def edit(rel, old, new, count=1):
    path = os.path.join(src, rel)
    s = open(path, encoding="utf-8").read()
    if s.count(old) != count:
        sys.exit("caelestia-qt610: %s: expected %d of %r, found %d" % (rel, count, old, s.count(old)))
    open(path, "w", encoding="utf-8").write(s.replace(old, new))


# Templates.DoubleSpinBox (6.11): a Control with its API
for f in ("SpinBoxShim.qml", "SpinButtonShim.qml"):
    shutil.copy(os.path.join(here, f), os.path.join(src, "components/controls", f))
edit("components/controls/StyledSpinBox.qml", "\nDoubleSpinBox {", "\nSpinBoxShim {")

# RectangularShadow's per-corner radii (6.11): accepted, the shadow's
# corners all take `radius`
edit("components/effects/Elevation.qml", "RectangularShadow {\n",
     "RectangularShadow {\n    property real topLeftRadius: radius\n    property real topRightRadius: radius\n"
     "    property real bottomLeftRadius: radius\n    property real bottomRightRadius: radius\n")

# `char` is a reserved word to Qt 6.10's QML/JS parser (not to 6.11's)
path = os.path.join(src, "modules/lock/center/InputField.qml")
s = open(path, encoding="utf-8").read()
import re
n = len(re.findall(r"\bchar\b", s))
if n < 2:
    sys.exit("caelestia-qt610: InputField.qml: no `char` id")
open(path, "w", encoding="utf-8").write(re.sub(r"\bchar\b", "charItem", s))

print("  CAE        Qt 6.10 shims applied")
