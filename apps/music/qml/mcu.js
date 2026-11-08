.pragma library
// Google's material-color-utilities 0.4.0 (Apache-2.0, https://github.com/material-foundation/material-color-utilities),
// bundled by esbuild into one script for the QML engine: MCU.Hct, MCU.Score, MCU.Scheme*.
var MCU = (() => {
  var __defProp = Object.defineProperty;
  var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
  var __getOwnPropNames = Object.getOwnPropertyNames, __getOwnPropSymbols = Object.getOwnPropertySymbols;
  var __hasOwnProp = Object.prototype.hasOwnProperty, __propIsEnum = Object.prototype.propertyIsEnumerable;
  var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: !0, configurable: !0, writable: !0, value }) : obj[key] = value, __spreadValues = (a, b) => {
    for (var prop in b || (b = {}))
      __hasOwnProp.call(b, prop) && __defNormalProp(a, prop, b[prop]);
    if (__getOwnPropSymbols)
      for (var prop of __getOwnPropSymbols(b))
        __propIsEnum.call(b, prop) && __defNormalProp(a, prop, b[prop]);
    return a;
  };
  var __export = (target, all) => {
    for (var name in all)
      __defProp(target, name, { get: all[name], enumerable: !0 });
  }, __copyProps = (to, from, except, desc) => {
    if (from && typeof from == "object" || typeof from == "function")
      for (let key of __getOwnPropNames(from))
        !__hasOwnProp.call(to, key) && key !== except && __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
    return to;
  };
  var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: !0 }), mod);

  // entry.js
  var entry_exports = {};
  __export(entry_exports, {
    DynamicScheme: () => DynamicScheme,
    Hct: () => Hct,
    SchemeContent: () => SchemeContent,
    SchemeExpressive: () => SchemeExpressive,
    SchemeFidelity: () => SchemeFidelity,
    SchemeFruitSalad: () => SchemeFruitSalad,
    SchemeMonochrome: () => SchemeMonochrome,
    SchemeNeutral: () => SchemeNeutral,
    SchemeRainbow: () => SchemeRainbow,
    SchemeTonalSpot: () => SchemeTonalSpot,
    SchemeVibrant: () => SchemeVibrant,
    Score: () => Score,
    argbFromRgb: () => argbFromRgb
  });

  // node_modules/@material/material-color-utilities/utils/math_utils.js
  function signum(num) {
    return num < 0 ? -1 : num === 0 ? 0 : 1;
  }
  function lerp(start, stop, amount) {
    return (1 - amount) * start + amount * stop;
  }
  function clampInt(min, max, input) {
    return input < min ? min : input > max ? max : input;
  }
  function clampDouble(min, max, input) {
    return input < min ? min : input > max ? max : input;
  }
  function sanitizeDegreesInt(degrees) {
    return degrees = degrees % 360, degrees < 0 && (degrees = degrees + 360), degrees;
  }
  function sanitizeDegreesDouble(degrees) {
    return degrees = degrees % 360, degrees < 0 && (degrees = degrees + 360), degrees;
  }
  function differenceDegrees(a, b) {
    return 180 - Math.abs(Math.abs(a - b) - 180);
  }
  function matrixMultiply(row, matrix) {
    let a = row[0] * matrix[0][0] + row[1] * matrix[0][1] + row[2] * matrix[0][2], b = row[0] * matrix[1][0] + row[1] * matrix[1][1] + row[2] * matrix[1][2], c = row[0] * matrix[2][0] + row[1] * matrix[2][1] + row[2] * matrix[2][2];
    return [a, b, c];
  }

  // node_modules/@material/material-color-utilities/utils/color_utils.js
  var SRGB_TO_XYZ = [
    [0.41233895, 0.35762064, 0.18051042],
    [0.2126, 0.7152, 0.0722],
    [0.01932141, 0.11916382, 0.95034478]
  ], XYZ_TO_SRGB = [
    [
      3.2413774792388685,
      -1.5376652402851851,
      -0.49885366846268053
    ],
    [
      -0.9691452513005321,
      1.8758853451067872,
      0.04156585616912061
    ],
    [
      0.05562093689691305,
      -0.20395524564742123,
      1.0571799111220335
    ]
  ], WHITE_POINT_D65 = [95.047, 100, 108.883];
  function argbFromRgb(red, green, blue) {
    return (255 << 24 | (red & 255) << 16 | (green & 255) << 8 | blue & 255) >>> 0;
  }
  function argbFromLinrgb(linrgb) {
    let r = delinearized(linrgb[0]), g = delinearized(linrgb[1]), b = delinearized(linrgb[2]);
    return argbFromRgb(r, g, b);
  }
  function redFromArgb(argb) {
    return argb >> 16 & 255;
  }
  function greenFromArgb(argb) {
    return argb >> 8 & 255;
  }
  function blueFromArgb(argb) {
    return argb & 255;
  }
  function argbFromXyz(x, y, z) {
    let matrix = XYZ_TO_SRGB, linearR = matrix[0][0] * x + matrix[0][1] * y + matrix[0][2] * z, linearG = matrix[1][0] * x + matrix[1][1] * y + matrix[1][2] * z, linearB = matrix[2][0] * x + matrix[2][1] * y + matrix[2][2] * z, r = delinearized(linearR), g = delinearized(linearG), b = delinearized(linearB);
    return argbFromRgb(r, g, b);
  }
  function xyzFromArgb(argb) {
    let r = linearized(redFromArgb(argb)), g = linearized(greenFromArgb(argb)), b = linearized(blueFromArgb(argb));
    return matrixMultiply([r, g, b], SRGB_TO_XYZ);
  }
  function labFromArgb(argb) {
    let linearR = linearized(redFromArgb(argb)), linearG = linearized(greenFromArgb(argb)), linearB = linearized(blueFromArgb(argb)), matrix = SRGB_TO_XYZ, x = matrix[0][0] * linearR + matrix[0][1] * linearG + matrix[0][2] * linearB, y = matrix[1][0] * linearR + matrix[1][1] * linearG + matrix[1][2] * linearB, z = matrix[2][0] * linearR + matrix[2][1] * linearG + matrix[2][2] * linearB, whitePoint = WHITE_POINT_D65, xNormalized = x / whitePoint[0], yNormalized = y / whitePoint[1], zNormalized = z / whitePoint[2], fx = labF(xNormalized), fy = labF(yNormalized), fz = labF(zNormalized), l = 116 * fy - 16, a = 500 * (fx - fy), b = 200 * (fy - fz);
    return [l, a, b];
  }
  function argbFromLstar(lstar) {
    let y = yFromLstar(lstar), component = delinearized(y);
    return argbFromRgb(component, component, component);
  }
  function lstarFromArgb(argb) {
    let y = xyzFromArgb(argb)[1];
    return 116 * labF(y / 100) - 16;
  }
  function yFromLstar(lstar) {
    return 100 * labInvf((lstar + 16) / 116);
  }
  function lstarFromY(y) {
    return labF(y / 100) * 116 - 16;
  }
  function linearized(rgbComponent) {
    let normalized = rgbComponent / 255;
    return normalized <= 0.040449936 ? normalized / 12.92 * 100 : Math.pow((normalized + 0.055) / 1.055, 2.4) * 100;
  }
  function delinearized(rgbComponent) {
    let normalized = rgbComponent / 100, delinearized2 = 0;
    return normalized <= 31308e-7 ? delinearized2 = normalized * 12.92 : delinearized2 = 1.055 * Math.pow(normalized, 1 / 2.4) - 0.055, clampInt(0, 255, Math.round(delinearized2 * 255));
  }
  function whitePointD65() {
    return WHITE_POINT_D65;
  }
  function labF(t) {
    let e = 0.008856451679035631, kappa = 24389 / 27;
    return t > e ? Math.pow(t, 1 / 3) : (kappa * t + 16) / 116;
  }
  function labInvf(ft) {
    let e = 0.008856451679035631, kappa = 24389 / 27, ft3 = ft * ft * ft;
    return ft3 > e ? ft3 : (116 * ft - 16) / kappa;
  }

  // node_modules/@material/material-color-utilities/hct/viewing_conditions.js
  var ViewingConditions = class _ViewingConditions {
    /**
     * Create ViewingConditions from a simple, physically relevant, set of
     * parameters.
     *
     * @param whitePoint White point, measured in the XYZ color space.
     *     default = D65, or sunny day afternoon
     * @param adaptingLuminance The luminance of the adapting field. Informally,
     *     how bright it is in the room where the color is viewed. Can be
     *     calculated from lux by multiplying lux by 0.0586. default = 11.72,
     *     or 200 lux.
     * @param backgroundLstar The lightness of the area surrounding the color.
     *     measured by L* in L*a*b*. default = 50.0
     * @param surround A general description of the lighting surrounding the
     *     color. 0 is pitch dark, like watching a movie in a theater. 1.0 is a
     *     dimly light room, like watching TV at home at night. 2.0 means there
     *     is no difference between the lighting on the color and around it.
     *     default = 2.0
     * @param discountingIlluminant Whether the eye accounts for the tint of the
     *     ambient lighting, such as knowing an apple is still red in green light.
     *     default = false, the eye does not perform this process on
     *       self-luminous objects like displays.
     */
    static make(whitePoint = whitePointD65(), adaptingLuminance = 200 / Math.PI * yFromLstar(50) / 100, backgroundLstar = 50, surround = 2, discountingIlluminant = !1) {
      let xyz = whitePoint, rW = xyz[0] * 0.401288 + xyz[1] * 0.650173 + xyz[2] * -0.051461, gW = xyz[0] * -0.250268 + xyz[1] * 1.204414 + xyz[2] * 0.045854, bW = xyz[0] * -2079e-6 + xyz[1] * 0.048952 + xyz[2] * 0.953127, f = 0.8 + surround / 10, c = f >= 0.9 ? lerp(0.59, 0.69, (f - 0.9) * 10) : lerp(0.525, 0.59, (f - 0.8) * 10), d = discountingIlluminant ? 1 : f * (1 - 1 / 3.6 * Math.exp((-adaptingLuminance - 42) / 92));
      d = d > 1 ? 1 : d < 0 ? 0 : d;
      let nc = f, rgbD = [
        d * (100 / rW) + 1 - d,
        d * (100 / gW) + 1 - d,
        d * (100 / bW) + 1 - d
      ], k = 1 / (5 * adaptingLuminance + 1), k4 = k * k * k * k, k4F = 1 - k4, fl = k4 * adaptingLuminance + 0.1 * k4F * k4F * Math.cbrt(5 * adaptingLuminance), n = yFromLstar(backgroundLstar) / whitePoint[1], z = 1.48 + Math.sqrt(n), nbb = 0.725 / Math.pow(n, 0.2), ncb = nbb, rgbAFactors = [
        Math.pow(fl * rgbD[0] * rW / 100, 0.42),
        Math.pow(fl * rgbD[1] * gW / 100, 0.42),
        Math.pow(fl * rgbD[2] * bW / 100, 0.42)
      ], rgbA = [
        400 * rgbAFactors[0] / (rgbAFactors[0] + 27.13),
        400 * rgbAFactors[1] / (rgbAFactors[1] + 27.13),
        400 * rgbAFactors[2] / (rgbAFactors[2] + 27.13)
      ], aw = (2 * rgbA[0] + rgbA[1] + 0.05 * rgbA[2]) * nbb;
      return new _ViewingConditions(n, aw, nbb, ncb, c, nc, rgbD, fl, Math.pow(fl, 0.25), z);
    }
    /**
     * Parameters are intermediate values of the CAM16 conversion process. Their
     * names are shorthand for technical color science terminology, this class
     * would not benefit from documenting them individually. A brief overview
     * is available in the CAM16 specification, and a complete overview requires
     * a color science textbook, such as Fairchild's Color Appearance Models.
     */
    constructor(n, aw, nbb, ncb, c, nc, rgbD, fl, fLRoot, z) {
      this.n = n, this.aw = aw, this.nbb = nbb, this.ncb = ncb, this.c = c, this.nc = nc, this.rgbD = rgbD, this.fl = fl, this.fLRoot = fLRoot, this.z = z;
    }
  };
  ViewingConditions.DEFAULT = ViewingConditions.make();

  // node_modules/@material/material-color-utilities/hct/cam16.js
  var Cam16 = class _Cam16 {
    /**
     * All of the CAM16 dimensions can be calculated from 3 of the dimensions, in
     * the following combinations:
     *      -  {j or q} and {c, m, or s} and hue
     *      - jstar, astar, bstar
     * Prefer using a static method that constructs from 3 of those dimensions.
     * This constructor is intended for those methods to use to return all
     * possible dimensions.
     *
     * @param hue
     * @param chroma informally, colorfulness / color intensity. like saturation
     *     in HSL, except perceptually accurate.
     * @param j lightness
     * @param q brightness; ratio of lightness to white point's lightness
     * @param m colorfulness
     * @param s saturation; ratio of chroma to white point's chroma
     * @param jstar CAM16-UCS J coordinate
     * @param astar CAM16-UCS a coordinate
     * @param bstar CAM16-UCS b coordinate
     */
    constructor(hue, chroma, j, q, m, s, jstar, astar, bstar) {
      this.hue = hue, this.chroma = chroma, this.j = j, this.q = q, this.m = m, this.s = s, this.jstar = jstar, this.astar = astar, this.bstar = bstar;
    }
    /**
     * CAM16 instances also have coordinates in the CAM16-UCS space, called J*,
     * a*, b*, or jstar, astar, bstar in code. CAM16-UCS is included in the CAM16
     * specification, and is used to measure distances between colors.
     */
    distance(other) {
      let dJ = this.jstar - other.jstar, dA = this.astar - other.astar, dB = this.bstar - other.bstar, dEPrime = Math.sqrt(dJ * dJ + dA * dA + dB * dB);
      return 1.41 * Math.pow(dEPrime, 0.63);
    }
    /**
     * @param argb ARGB representation of a color.
     * @return CAM16 color, assuming the color was viewed in default viewing
     *     conditions.
     */
    static fromInt(argb) {
      return _Cam16.fromIntInViewingConditions(argb, ViewingConditions.DEFAULT);
    }
    /**
     * @param argb ARGB representation of a color.
     * @param viewingConditions Information about the environment where the color
     *     was observed.
     * @return CAM16 color.
     */
    static fromIntInViewingConditions(argb, viewingConditions) {
      let red = (argb & 16711680) >> 16, green = (argb & 65280) >> 8, blue = argb & 255, redL = linearized(red), greenL = linearized(green), blueL = linearized(blue), x = 0.41233895 * redL + 0.35762064 * greenL + 0.18051042 * blueL, y = 0.2126 * redL + 0.7152 * greenL + 0.0722 * blueL, z = 0.01932141 * redL + 0.11916382 * greenL + 0.95034478 * blueL, rC = 0.401288 * x + 0.650173 * y - 0.051461 * z, gC = -0.250268 * x + 1.204414 * y + 0.045854 * z, bC = -2079e-6 * x + 0.048952 * y + 0.953127 * z, rD = viewingConditions.rgbD[0] * rC, gD = viewingConditions.rgbD[1] * gC, bD = viewingConditions.rgbD[2] * bC, rAF = Math.pow(viewingConditions.fl * Math.abs(rD) / 100, 0.42), gAF = Math.pow(viewingConditions.fl * Math.abs(gD) / 100, 0.42), bAF = Math.pow(viewingConditions.fl * Math.abs(bD) / 100, 0.42), rA = signum(rD) * 400 * rAF / (rAF + 27.13), gA = signum(gD) * 400 * gAF / (gAF + 27.13), bA = signum(bD) * 400 * bAF / (bAF + 27.13), a = (11 * rA + -12 * gA + bA) / 11, b = (rA + gA - 2 * bA) / 9, u = (20 * rA + 20 * gA + 21 * bA) / 20, p2 = (40 * rA + 20 * gA + bA) / 20, atanDegrees = Math.atan2(b, a) * 180 / Math.PI, hue = sanitizeDegreesDouble(atanDegrees), hueRadians = hue * Math.PI / 180, ac = p2 * viewingConditions.nbb, j = 100 * Math.pow(ac / viewingConditions.aw, viewingConditions.c * viewingConditions.z), q = 4 / viewingConditions.c * Math.sqrt(j / 100) * (viewingConditions.aw + 4) * viewingConditions.fLRoot, huePrime = hue < 20.14 ? hue + 360 : hue, eHue = 0.25 * (Math.cos(huePrime * Math.PI / 180 + 2) + 3.8), t = 5e4 / 13 * eHue * viewingConditions.nc * viewingConditions.ncb * Math.sqrt(a * a + b * b) / (u + 0.305), alpha = Math.pow(t, 0.9) * Math.pow(1.64 - Math.pow(0.29, viewingConditions.n), 0.73), c = alpha * Math.sqrt(j / 100), m = c * viewingConditions.fLRoot, s = 50 * Math.sqrt(alpha * viewingConditions.c / (viewingConditions.aw + 4)), jstar = (1 + 100 * 7e-3) * j / (1 + 7e-3 * j), mstar = 1 / 0.0228 * Math.log(1 + 0.0228 * m), astar = mstar * Math.cos(hueRadians), bstar = mstar * Math.sin(hueRadians);
      return new _Cam16(hue, c, j, q, m, s, jstar, astar, bstar);
    }
    /**
     * @param j CAM16 lightness
     * @param c CAM16 chroma
     * @param h CAM16 hue
     */
    static fromJch(j, c, h) {
      return _Cam16.fromJchInViewingConditions(j, c, h, ViewingConditions.DEFAULT);
    }
    /**
     * @param j CAM16 lightness
     * @param c CAM16 chroma
     * @param h CAM16 hue
     * @param viewingConditions Information about the environment where the color
     *     was observed.
     */
    static fromJchInViewingConditions(j, c, h, viewingConditions) {
      let q = 4 / viewingConditions.c * Math.sqrt(j / 100) * (viewingConditions.aw + 4) * viewingConditions.fLRoot, m = c * viewingConditions.fLRoot, alpha = c / Math.sqrt(j / 100), s = 50 * Math.sqrt(alpha * viewingConditions.c / (viewingConditions.aw + 4)), hueRadians = h * Math.PI / 180, jstar = (1 + 100 * 7e-3) * j / (1 + 7e-3 * j), mstar = 1 / 0.0228 * Math.log(1 + 0.0228 * m), astar = mstar * Math.cos(hueRadians), bstar = mstar * Math.sin(hueRadians);
      return new _Cam16(h, c, j, q, m, s, jstar, astar, bstar);
    }
    /**
     * @param jstar CAM16-UCS lightness.
     * @param astar CAM16-UCS a dimension. Like a* in L*a*b*, it is a Cartesian
     *     coordinate on the Y axis.
     * @param bstar CAM16-UCS b dimension. Like a* in L*a*b*, it is a Cartesian
     *     coordinate on the X axis.
     */
    static fromUcs(jstar, astar, bstar) {
      return _Cam16.fromUcsInViewingConditions(jstar, astar, bstar, ViewingConditions.DEFAULT);
    }
    /**
     * @param jstar CAM16-UCS lightness.
     * @param astar CAM16-UCS a dimension. Like a* in L*a*b*, it is a Cartesian
     *     coordinate on the Y axis.
     * @param bstar CAM16-UCS b dimension. Like a* in L*a*b*, it is a Cartesian
     *     coordinate on the X axis.
     * @param viewingConditions Information about the environment where the color
     *     was observed.
     */
    static fromUcsInViewingConditions(jstar, astar, bstar, viewingConditions) {
      let a = astar, b = bstar, m = Math.sqrt(a * a + b * b), c = (Math.exp(m * 0.0228) - 1) / 0.0228 / viewingConditions.fLRoot, h = Math.atan2(b, a) * (180 / Math.PI);
      h < 0 && (h += 360);
      let j = jstar / (1 - (jstar - 100) * 7e-3);
      return _Cam16.fromJchInViewingConditions(j, c, h, viewingConditions);
    }
    /**
     *  @return ARGB representation of color, assuming the color was viewed in
     *     default viewing conditions, which are near-identical to the default
     *     viewing conditions for sRGB.
     */
    toInt() {
      return this.viewed(ViewingConditions.DEFAULT);
    }
    /**
     * @param viewingConditions Information about the environment where the color
     *     will be viewed.
     * @return ARGB representation of color
     */
    viewed(viewingConditions) {
      let alpha = this.chroma === 0 || this.j === 0 ? 0 : this.chroma / Math.sqrt(this.j / 100), t = Math.pow(alpha / Math.pow(1.64 - Math.pow(0.29, viewingConditions.n), 0.73), 1 / 0.9), hRad = this.hue * Math.PI / 180, eHue = 0.25 * (Math.cos(hRad + 2) + 3.8), ac = viewingConditions.aw * Math.pow(this.j / 100, 1 / viewingConditions.c / viewingConditions.z), p1 = eHue * (5e4 / 13) * viewingConditions.nc * viewingConditions.ncb, p2 = ac / viewingConditions.nbb, hSin = Math.sin(hRad), hCos = Math.cos(hRad), gamma = 23 * (p2 + 0.305) * t / (23 * p1 + 11 * t * hCos + 108 * t * hSin), a = gamma * hCos, b = gamma * hSin, rA = (460 * p2 + 451 * a + 288 * b) / 1403, gA = (460 * p2 - 891 * a - 261 * b) / 1403, bA = (460 * p2 - 220 * a - 6300 * b) / 1403, rCBase = Math.max(0, 27.13 * Math.abs(rA) / (400 - Math.abs(rA))), rC = signum(rA) * (100 / viewingConditions.fl) * Math.pow(rCBase, 1 / 0.42), gCBase = Math.max(0, 27.13 * Math.abs(gA) / (400 - Math.abs(gA))), gC = signum(gA) * (100 / viewingConditions.fl) * Math.pow(gCBase, 1 / 0.42), bCBase = Math.max(0, 27.13 * Math.abs(bA) / (400 - Math.abs(bA))), bC = signum(bA) * (100 / viewingConditions.fl) * Math.pow(bCBase, 1 / 0.42), rF = rC / viewingConditions.rgbD[0], gF = gC / viewingConditions.rgbD[1], bF = bC / viewingConditions.rgbD[2], x = 1.86206786 * rF - 1.01125463 * gF + 0.14918677 * bF, y = 0.38752654 * rF + 0.62144744 * gF - 897398e-8 * bF, z = -0.0158415 * rF - 0.03412294 * gF + 1.04996444 * bF;
      return argbFromXyz(x, y, z);
    }
    /// Given color expressed in XYZ and viewed in [viewingConditions], convert to
    /// CAM16.
    static fromXyzInViewingConditions(x, y, z, viewingConditions) {
      let rC = 0.401288 * x + 0.650173 * y - 0.051461 * z, gC = -0.250268 * x + 1.204414 * y + 0.045854 * z, bC = -2079e-6 * x + 0.048952 * y + 0.953127 * z, rD = viewingConditions.rgbD[0] * rC, gD = viewingConditions.rgbD[1] * gC, bD = viewingConditions.rgbD[2] * bC, rAF = Math.pow(viewingConditions.fl * Math.abs(rD) / 100, 0.42), gAF = Math.pow(viewingConditions.fl * Math.abs(gD) / 100, 0.42), bAF = Math.pow(viewingConditions.fl * Math.abs(bD) / 100, 0.42), rA = signum(rD) * 400 * rAF / (rAF + 27.13), gA = signum(gD) * 400 * gAF / (gAF + 27.13), bA = signum(bD) * 400 * bAF / (bAF + 27.13), a = (11 * rA + -12 * gA + bA) / 11, b = (rA + gA - 2 * bA) / 9, u = (20 * rA + 20 * gA + 21 * bA) / 20, p2 = (40 * rA + 20 * gA + bA) / 20, atanDegrees = Math.atan2(b, a) * 180 / Math.PI, hue = atanDegrees < 0 ? atanDegrees + 360 : atanDegrees >= 360 ? atanDegrees - 360 : atanDegrees, hueRadians = hue * Math.PI / 180, ac = p2 * viewingConditions.nbb, J = 100 * Math.pow(ac / viewingConditions.aw, viewingConditions.c * viewingConditions.z), Q = 4 / viewingConditions.c * Math.sqrt(J / 100) * (viewingConditions.aw + 4) * viewingConditions.fLRoot, huePrime = hue < 20.14 ? hue + 360 : hue, eHue = 1 / 4 * (Math.cos(huePrime * Math.PI / 180 + 2) + 3.8), t = 5e4 / 13 * eHue * viewingConditions.nc * viewingConditions.ncb * Math.sqrt(a * a + b * b) / (u + 0.305), alpha = Math.pow(t, 0.9) * Math.pow(1.64 - Math.pow(0.29, viewingConditions.n), 0.73), C = alpha * Math.sqrt(J / 100), M = C * viewingConditions.fLRoot, s = 50 * Math.sqrt(alpha * viewingConditions.c / (viewingConditions.aw + 4)), jstar = (1 + 100 * 7e-3) * J / (1 + 7e-3 * J), mstar = Math.log(1 + 0.0228 * M) / 0.0228, astar = mstar * Math.cos(hueRadians), bstar = mstar * Math.sin(hueRadians);
      return new _Cam16(hue, C, J, Q, M, s, jstar, astar, bstar);
    }
    /// XYZ representation of CAM16 seen in [viewingConditions].
    xyzInViewingConditions(viewingConditions) {
      let alpha = this.chroma === 0 || this.j === 0 ? 0 : this.chroma / Math.sqrt(this.j / 100), t = Math.pow(alpha / Math.pow(1.64 - Math.pow(0.29, viewingConditions.n), 0.73), 1 / 0.9), hRad = this.hue * Math.PI / 180, eHue = 0.25 * (Math.cos(hRad + 2) + 3.8), ac = viewingConditions.aw * Math.pow(this.j / 100, 1 / viewingConditions.c / viewingConditions.z), p1 = eHue * (5e4 / 13) * viewingConditions.nc * viewingConditions.ncb, p2 = ac / viewingConditions.nbb, hSin = Math.sin(hRad), hCos = Math.cos(hRad), gamma = 23 * (p2 + 0.305) * t / (23 * p1 + 11 * t * hCos + 108 * t * hSin), a = gamma * hCos, b = gamma * hSin, rA = (460 * p2 + 451 * a + 288 * b) / 1403, gA = (460 * p2 - 891 * a - 261 * b) / 1403, bA = (460 * p2 - 220 * a - 6300 * b) / 1403, rCBase = Math.max(0, 27.13 * Math.abs(rA) / (400 - Math.abs(rA))), rC = signum(rA) * (100 / viewingConditions.fl) * Math.pow(rCBase, 1 / 0.42), gCBase = Math.max(0, 27.13 * Math.abs(gA) / (400 - Math.abs(gA))), gC = signum(gA) * (100 / viewingConditions.fl) * Math.pow(gCBase, 1 / 0.42), bCBase = Math.max(0, 27.13 * Math.abs(bA) / (400 - Math.abs(bA))), bC = signum(bA) * (100 / viewingConditions.fl) * Math.pow(bCBase, 1 / 0.42), rF = rC / viewingConditions.rgbD[0], gF = gC / viewingConditions.rgbD[1], bF = bC / viewingConditions.rgbD[2], x = 1.86206786 * rF - 1.01125463 * gF + 0.14918677 * bF, y = 0.38752654 * rF + 0.62144744 * gF - 897398e-8 * bF, z = -0.0158415 * rF - 0.03412294 * gF + 1.04996444 * bF;
      return [x, y, z];
    }
  };

  // node_modules/@material/material-color-utilities/hct/hct_solver.js
  var HctSolver = class _HctSolver {
    /**
     * Sanitizes a small enough angle in radians.
     *
     * @param angle An angle in radians; must not deviate too much
     * from 0.
     * @return A coterminal angle between 0 and 2pi.
     */
    static sanitizeRadians(angle) {
      return (angle + Math.PI * 8) % (Math.PI * 2);
    }
    /**
     * Delinearizes an RGB component, returning a floating-point
     * number.
     *
     * @param rgbComponent 0.0 <= rgb_component <= 100.0, represents
     * linear R/G/B channel
     * @return 0.0 <= output <= 255.0, color channel converted to
     * regular RGB space
     */
    static trueDelinearized(rgbComponent) {
      let normalized = rgbComponent / 100, delinearized2 = 0;
      return normalized <= 31308e-7 ? delinearized2 = normalized * 12.92 : delinearized2 = 1.055 * Math.pow(normalized, 1 / 2.4) - 0.055, delinearized2 * 255;
    }
    static chromaticAdaptation(component) {
      let af = Math.pow(Math.abs(component), 0.42);
      return signum(component) * 400 * af / (af + 27.13);
    }
    /**
     * Returns the hue of a linear RGB color in CAM16.
     *
     * @param linrgb The linear RGB coordinates of a color.
     * @return The hue of the color in CAM16, in radians.
     */
    static hueOf(linrgb) {
      let scaledDiscount = matrixMultiply(linrgb, _HctSolver.SCALED_DISCOUNT_FROM_LINRGB), rA = _HctSolver.chromaticAdaptation(scaledDiscount[0]), gA = _HctSolver.chromaticAdaptation(scaledDiscount[1]), bA = _HctSolver.chromaticAdaptation(scaledDiscount[2]), a = (11 * rA + -12 * gA + bA) / 11, b = (rA + gA - 2 * bA) / 9;
      return Math.atan2(b, a);
    }
    static areInCyclicOrder(a, b, c) {
      let deltaAB = _HctSolver.sanitizeRadians(b - a), deltaAC = _HctSolver.sanitizeRadians(c - a);
      return deltaAB < deltaAC;
    }
    /**
     * Solves the lerp equation.
     *
     * @param source The starting number.
     * @param mid The number in the middle.
     * @param target The ending number.
     * @return A number t such that lerp(source, target, t) = mid.
     */
    static intercept(source, mid, target) {
      return (mid - source) / (target - source);
    }
    static lerpPoint(source, t, target) {
      return [
        source[0] + (target[0] - source[0]) * t,
        source[1] + (target[1] - source[1]) * t,
        source[2] + (target[2] - source[2]) * t
      ];
    }
    /**
     * Intersects a segment with a plane.
     *
     * @param source The coordinates of point A.
     * @param coordinate The R-, G-, or B-coordinate of the plane.
     * @param target The coordinates of point B.
     * @param axis The axis the plane is perpendicular with. (0: R, 1:
     * G, 2: B)
     * @return The intersection point of the segment AB with the plane
     * R=coordinate, G=coordinate, or B=coordinate
     */
    static setCoordinate(source, coordinate, target, axis) {
      let t = _HctSolver.intercept(source[axis], coordinate, target[axis]);
      return _HctSolver.lerpPoint(source, t, target);
    }
    static isBounded(x) {
      return 0 <= x && x <= 100;
    }
    /**
     * Returns the nth possible vertex of the polygonal intersection.
     *
     * @param y The Y value of the plane.
     * @param n The zero-based index of the point. 0 <= n <= 11.
     * @return The nth possible vertex of the polygonal intersection
     * of the y plane and the RGB cube, in linear RGB coordinates, if
     * it exists. If this possible vertex lies outside of the cube,
     * [-1.0, -1.0, -1.0] is returned.
     */
    static nthVertex(y, n) {
      let kR = _HctSolver.Y_FROM_LINRGB[0], kG = _HctSolver.Y_FROM_LINRGB[1], kB = _HctSolver.Y_FROM_LINRGB[2], coordA = n % 4 <= 1 ? 0 : 100, coordB = n % 2 === 0 ? 0 : 100;
      if (n < 4) {
        let g = coordA, b = coordB, r = (y - g * kG - b * kB) / kR;
        return _HctSolver.isBounded(r) ? [r, g, b] : [-1, -1, -1];
      } else if (n < 8) {
        let b = coordA, r = coordB, g = (y - r * kR - b * kB) / kG;
        return _HctSolver.isBounded(g) ? [r, g, b] : [-1, -1, -1];
      } else {
        let r = coordA, g = coordB, b = (y - r * kR - g * kG) / kB;
        return _HctSolver.isBounded(b) ? [r, g, b] : [-1, -1, -1];
      }
    }
    /**
     * Finds the segment containing the desired color.
     *
     * @param y The Y value of the color.
     * @param targetHue The hue of the color.
     * @return A list of two sets of linear RGB coordinates, each
     * corresponding to an endpoint of the segment containing the
     * desired color.
     */
    static bisectToSegment(y, targetHue) {
      let left = [-1, -1, -1], right = left, leftHue = 0, rightHue = 0, initialized = !1, uncut = !0;
      for (let n = 0; n < 12; n++) {
        let mid = _HctSolver.nthVertex(y, n);
        if (mid[0] < 0)
          continue;
        let midHue = _HctSolver.hueOf(mid);
        if (!initialized) {
          left = mid, right = mid, leftHue = midHue, rightHue = midHue, initialized = !0;
          continue;
        }
        (uncut || _HctSolver.areInCyclicOrder(leftHue, midHue, rightHue)) && (uncut = !1, _HctSolver.areInCyclicOrder(leftHue, targetHue, midHue) ? (right = mid, rightHue = midHue) : (left = mid, leftHue = midHue));
      }
      return [left, right];
    }
    static midpoint(a, b) {
      return [
        (a[0] + b[0]) / 2,
        (a[1] + b[1]) / 2,
        (a[2] + b[2]) / 2
      ];
    }
    static criticalPlaneBelow(x) {
      return Math.floor(x - 0.5);
    }
    static criticalPlaneAbove(x) {
      return Math.ceil(x - 0.5);
    }
    /**
     * Finds a color with the given Y and hue on the boundary of the
     * cube.
     *
     * @param y The Y value of the color.
     * @param targetHue The hue of the color.
     * @return The desired color, in linear RGB coordinates.
     */
    static bisectToLimit(y, targetHue) {
      let segment = _HctSolver.bisectToSegment(y, targetHue), left = segment[0], leftHue = _HctSolver.hueOf(left), right = segment[1];
      for (let axis = 0; axis < 3; axis++)
        if (left[axis] !== right[axis]) {
          let lPlane = -1, rPlane = 255;
          left[axis] < right[axis] ? (lPlane = _HctSolver.criticalPlaneBelow(_HctSolver.trueDelinearized(left[axis])), rPlane = _HctSolver.criticalPlaneAbove(_HctSolver.trueDelinearized(right[axis]))) : (lPlane = _HctSolver.criticalPlaneAbove(_HctSolver.trueDelinearized(left[axis])), rPlane = _HctSolver.criticalPlaneBelow(_HctSolver.trueDelinearized(right[axis])));
          for (let i = 0; i < 8 && !(Math.abs(rPlane - lPlane) <= 1); i++) {
            let mPlane = Math.floor((lPlane + rPlane) / 2), midPlaneCoordinate = _HctSolver.CRITICAL_PLANES[mPlane], mid = _HctSolver.setCoordinate(left, midPlaneCoordinate, right, axis), midHue = _HctSolver.hueOf(mid);
            _HctSolver.areInCyclicOrder(leftHue, targetHue, midHue) ? (right = mid, rPlane = mPlane) : (left = mid, leftHue = midHue, lPlane = mPlane);
          }
        }
      return _HctSolver.midpoint(left, right);
    }
    static inverseChromaticAdaptation(adapted) {
      let adaptedAbs = Math.abs(adapted), base = Math.max(0, 27.13 * adaptedAbs / (400 - adaptedAbs));
      return signum(adapted) * Math.pow(base, 1 / 0.42);
    }
    /**
     * Finds a color with the given hue, chroma, and Y.
     *
     * @param hueRadians The desired hue in radians.
     * @param chroma The desired chroma.
     * @param y The desired Y.
     * @return The desired color as a hexadecimal integer, if found; 0
     * otherwise.
     */
    static findResultByJ(hueRadians, chroma, y) {
      let j = Math.sqrt(y) * 11, viewingConditions = ViewingConditions.DEFAULT, tInnerCoeff = 1 / Math.pow(1.64 - Math.pow(0.29, viewingConditions.n), 0.73), p1 = 0.25 * (Math.cos(hueRadians + 2) + 3.8) * (5e4 / 13) * viewingConditions.nc * viewingConditions.ncb, hSin = Math.sin(hueRadians), hCos = Math.cos(hueRadians);
      for (let iterationRound = 0; iterationRound < 5; iterationRound++) {
        let jNormalized = j / 100, alpha = chroma === 0 || j === 0 ? 0 : chroma / Math.sqrt(jNormalized), t = Math.pow(alpha * tInnerCoeff, 1 / 0.9), p2 = viewingConditions.aw * Math.pow(jNormalized, 1 / viewingConditions.c / viewingConditions.z) / viewingConditions.nbb, gamma = 23 * (p2 + 0.305) * t / (23 * p1 + 11 * t * hCos + 108 * t * hSin), a = gamma * hCos, b = gamma * hSin, rA = (460 * p2 + 451 * a + 288 * b) / 1403, gA = (460 * p2 - 891 * a - 261 * b) / 1403, bA = (460 * p2 - 220 * a - 6300 * b) / 1403, rCScaled = _HctSolver.inverseChromaticAdaptation(rA), gCScaled = _HctSolver.inverseChromaticAdaptation(gA), bCScaled = _HctSolver.inverseChromaticAdaptation(bA), linrgb = matrixMultiply([rCScaled, gCScaled, bCScaled], _HctSolver.LINRGB_FROM_SCALED_DISCOUNT);
        if (linrgb[0] < 0 || linrgb[1] < 0 || linrgb[2] < 0)
          return 0;
        let kR = _HctSolver.Y_FROM_LINRGB[0], kG = _HctSolver.Y_FROM_LINRGB[1], kB = _HctSolver.Y_FROM_LINRGB[2], fnj = kR * linrgb[0] + kG * linrgb[1] + kB * linrgb[2];
        if (fnj <= 0)
          return 0;
        if (iterationRound === 4 || Math.abs(fnj - y) < 2e-3)
          return linrgb[0] > 100.01 || linrgb[1] > 100.01 || linrgb[2] > 100.01 ? 0 : argbFromLinrgb(linrgb);
        j = j - (fnj - y) * j / (2 * fnj);
      }
      return 0;
    }
    /**
     * Finds an sRGB color with the given hue, chroma, and L*, if
     * possible.
     *
     * @param hueDegrees The desired hue, in degrees.
     * @param chroma The desired chroma.
     * @param lstar The desired L*.
     * @return A hexadecimal representing the sRGB color. The color
     * has sufficiently close hue, chroma, and L* to the desired
     * values, if possible; otherwise, the hue and L* will be
     * sufficiently close, and chroma will be maximized.
     */
    static solveToInt(hueDegrees, chroma, lstar) {
      if (chroma < 1e-4 || lstar < 1e-4 || lstar > 99.9999)
        return argbFromLstar(lstar);
      hueDegrees = sanitizeDegreesDouble(hueDegrees);
      let hueRadians = hueDegrees / 180 * Math.PI, y = yFromLstar(lstar), exactAnswer = _HctSolver.findResultByJ(hueRadians, chroma, y);
      if (exactAnswer !== 0)
        return exactAnswer;
      let linrgb = _HctSolver.bisectToLimit(y, hueRadians);
      return argbFromLinrgb(linrgb);
    }
    /**
     * Finds an sRGB color with the given hue, chroma, and L*, if
     * possible.
     *
     * @param hueDegrees The desired hue, in degrees.
     * @param chroma The desired chroma.
     * @param lstar The desired L*.
     * @return An CAM16 object representing the sRGB color. The color
     * has sufficiently close hue, chroma, and L* to the desired
     * values, if possible; otherwise, the hue and L* will be
     * sufficiently close, and chroma will be maximized.
     */
    static solveToCam(hueDegrees, chroma, lstar) {
      return Cam16.fromInt(_HctSolver.solveToInt(hueDegrees, chroma, lstar));
    }
  };
  HctSolver.SCALED_DISCOUNT_FROM_LINRGB = [
    [
      0.001200833568784504,
      0.002389694492170889,
      2795742885861124e-19
    ],
    [
      5891086651375999e-19,
      0.0029785502573438758,
      3270666104008398e-19
    ],
    [
      10146692491640572e-20,
      5364214359186694e-19,
      0.0032979401770712076
    ]
  ];
  HctSolver.LINRGB_FROM_SCALED_DISCOUNT = [
    [
      1373.2198709594231,
      -1100.4251190754821,
      -7.278681089101213
    ],
    [
      -271.815969077903,
      559.6580465940733,
      -32.46047482791194
    ],
    [
      1.9622899599665666,
      -57.173814538844006,
      308.7233197812385
    ]
  ];
  HctSolver.Y_FROM_LINRGB = [0.2126, 0.7152, 0.0722];
  HctSolver.CRITICAL_PLANES = [
    0.015176349177441876,
    0.045529047532325624,
    0.07588174588720938,
    0.10623444424209313,
    0.13658714259697685,
    0.16693984095186062,
    0.19729253930674434,
    0.2276452376616281,
    0.2579979360165119,
    0.28835063437139563,
    0.3188300904430532,
    0.350925934958123,
    0.3848314933096426,
    0.42057480301049466,
    0.458183274052838,
    0.4976837250274023,
    0.5391024159806381,
    0.5824650784040898,
    0.6277969426914107,
    0.6751227633498623,
    0.7244668422128921,
    0.775853049866786,
    0.829304845476233,
    0.8848452951698498,
    0.942497089126609,
    1.0022825574869039,
    1.0642236851973577,
    1.1283421258858297,
    1.1946592148522128,
    1.2631959812511864,
    1.3339731595349034,
    1.407011200216447,
    1.4823302800086415,
    1.5599503113873272,
    1.6398909516233677,
    1.7221716113234105,
    1.8068114625156377,
    1.8938294463134073,
    1.9832442801866852,
    2.075074464868551,
    2.1693382909216234,
    2.2660538449872063,
    2.36523901573795,
    2.4669114995532007,
    2.5710888059345764,
    2.6777882626779785,
    2.7870270208169257,
    2.898822059350997,
    3.0131901897720907,
    3.1301480604002863,
    3.2497121605402226,
    3.3718988244681087,
    3.4967242352587946,
    3.624204428461639,
    3.754355295633311,
    3.887192587735158,
    4.022731918402185,
    4.160988767090289,
    4.301978482107941,
    4.445716283538092,
    4.592217266055746,
    4.741496401646282,
    4.893568542229298,
    5.048448422192488,
    5.20615066083972,
    5.3666897647573375,
    5.5300801301023865,
    5.696336044816294,
    5.865471690767354,
    6.037501145825082,
    6.212438385869475,
    6.390297286737924,
    6.571091626112461,
    6.7548350853498045,
    6.941541251256611,
    7.131223617812143,
    7.323895587840543,
    7.5195704746346665,
    7.7182615035334345,
    7.919981813454504,
    8.124744458384042,
    8.332562408825165,
    8.543448553206703,
    8.757415699253682,
    8.974476575321063,
    9.194643831691977,
    9.417930041841839,
    9.644347703669503,
    9.873909240696694,
    10.106627003236781,
    10.342513269534024,
    10.58158024687427,
    10.8238400726681,
    11.069304815507364,
    11.317986476196008,
    11.569896988756009,
    11.825048221409341,
    12.083451977536606,
    12.345119996613247,
    12.610063955123938,
    12.878295467455942,
    13.149826086772048,
    13.42466730586372,
    13.702830557985108,
    13.984327217668513,
    14.269168601521828,
    14.55736596900856,
    14.848930523210871,
    15.143873411576273,
    15.44220572664832,
    15.743938506781891,
    16.04908273684337,
    16.35764934889634,
    16.66964922287304,
    16.985093187232053,
    17.30399201960269,
    17.62635644741625,
    17.95219714852476,
    18.281524751807332,
    18.614349837764564,
    18.95068293910138,
    19.290534541298456,
    19.633915083172692,
    19.98083495742689,
    20.331304511189067,
    20.685334046541502,
    21.042933821039977,
    21.404114048223256,
    21.76888489811322,
    22.137256497705877,
    22.50923893145328,
    22.884842241736916,
    23.264076429332462,
    23.6469514538663,
    24.033477234264016,
    24.42366364919083,
    24.817520537484558,
    25.21505769858089,
    25.61628489293138,
    26.021211842414342,
    26.429848230738664,
    26.842203703840827,
    27.258287870275353,
    27.678110301598522,
    28.10168053274597,
    28.529008062403893,
    28.96010235337422,
    29.39497283293396,
    29.83362889318845,
    30.276079891419332,
    30.722335150426627,
    31.172403958865512,
    31.62629557157785,
    32.08401920991837,
    32.54558406207592,
    33.010999283389665,
    33.4802739966603,
    33.953417292456834,
    34.430438229418264,
    34.911345834551085,
    35.39614910352207,
    35.88485700094671,
    36.37747846067349,
    36.87402238606382,
    37.37449765026789,
    37.87891309649659,
    38.38727753828926,
    38.89959975977785,
    39.41588851594697,
    39.93615253289054,
    40.460400508064545,
    40.98864111053629,
    41.520882981230194,
    42.05713473317016,
    42.597404951718396,
    43.141702194811224,
    43.6900349931913,
    44.24241185063697,
    44.798841244188324,
    45.35933162437017,
    45.92389141541209,
    46.49252901546552,
    47.065252796817916,
    47.64207110610409,
    48.22299226451468,
    48.808024568002054,
    49.3971762874833,
    49.9904556690408,
    50.587870934119984,
    51.189430279724725,
    51.79514187861014,
    52.40501387947288,
    53.0190544071392,
    53.637271562750364,
    54.259673423945976,
    54.88626804504493,
    55.517063457223934,
    56.15206766869424,
    56.79128866487574,
    57.43473440856916,
    58.08241284012621,
    58.734331877617365,
    59.39049941699807,
    60.05092333227251,
    60.715611475655585,
    61.38457167773311,
    62.057811747619894,
    62.7353394731159,
    63.417162620860914,
    64.10328893648692,
    64.79372614476921,
    65.48848194977529,
    66.18756403501224,
    66.89098006357258,
    67.59873767827808,
    68.31084450182222,
    69.02730813691093,
    69.74813616640164,
    70.47333615344107,
    71.20291564160104,
    71.93688215501312,
    72.67524319850172,
    73.41800625771542,
    74.16517879925733,
    74.9167682708136,
    75.67278210128072,
    76.43322770089146,
    77.1981124613393,
    77.96744375590167,
    78.74122893956174,
    79.51947534912904,
    80.30219030335869,
    81.08938110306934,
    81.88105503125999,
    82.67721935322541,
    83.4778813166706,
    84.28304815182372,
    85.09272707154808,
    85.90692527145302,
    86.72564993000343,
    87.54890820862819,
    88.3767072518277,
    89.2090541872801,
    90.04595612594655,
    90.88742016217518,
    91.73345337380438,
    92.58406282226491,
    93.43925555268066,
    94.29903859396902,
    95.16341895893969,
    96.03240364439274,
    96.9059996312159,
    97.78421388448044,
    98.6670533535366,
    99.55452497210776
  ];

  // node_modules/@material/material-color-utilities/hct/hct.js
  var Hct = class _Hct {
    static from(hue, chroma, tone) {
      return new _Hct(HctSolver.solveToInt(hue, chroma, tone));
    }
    /**
     * @param argb ARGB representation of a color.
     * @return HCT representation of a color in default viewing conditions
     */
    static fromInt(argb) {
      return new _Hct(argb);
    }
    toInt() {
      return this.argb;
    }
    /**
     * A number, in degrees, representing ex. red, orange, yellow, etc.
     * Ranges from 0 <= hue < 360.
     */
    get hue() {
      return this.internalHue;
    }
    /**
     * @param newHue 0 <= newHue < 360; invalid values are corrected.
     * Chroma may decrease because chroma has a different maximum for any given
     * hue and tone.
     */
    set hue(newHue) {
      this.setInternalState(HctSolver.solveToInt(newHue, this.internalChroma, this.internalTone));
    }
    get chroma() {
      return this.internalChroma;
    }
    /**
     * @param newChroma 0 <= newChroma < ?
     * Chroma may decrease because chroma has a different maximum for any given
     * hue and tone.
     */
    set chroma(newChroma) {
      this.setInternalState(HctSolver.solveToInt(this.internalHue, newChroma, this.internalTone));
    }
    /** Lightness. Ranges from 0 to 100. */
    get tone() {
      return this.internalTone;
    }
    /**
     * @param newTone 0 <= newTone <= 100; invalid valids are corrected.
     * Chroma may decrease because chroma has a different maximum for any given
     * hue and tone.
     */
    set tone(newTone) {
      this.setInternalState(HctSolver.solveToInt(this.internalHue, this.internalChroma, newTone));
    }
    /** Sets a property of the Hct object. */
    setValue(propertyName, value) {
      this[propertyName] = value;
    }
    toString() {
      return `HCT(${this.hue.toFixed(0)}, ${this.chroma.toFixed(0)}, ${this.tone.toFixed(0)})`;
    }
    static isBlue(hue) {
      return hue >= 250 && hue < 270;
    }
    static isYellow(hue) {
      return hue >= 105 && hue < 125;
    }
    static isCyan(hue) {
      return hue >= 170 && hue < 207;
    }
    constructor(argb) {
      this.argb = argb;
      let cam = Cam16.fromInt(argb);
      this.internalHue = cam.hue, this.internalChroma = cam.chroma, this.internalTone = lstarFromArgb(argb), this.argb = argb;
    }
    setInternalState(argb) {
      let cam = Cam16.fromInt(argb);
      this.internalHue = cam.hue, this.internalChroma = cam.chroma, this.internalTone = lstarFromArgb(argb), this.argb = argb;
    }
    /**
     * Translates a color into different [ViewingConditions].
     *
     * Colors change appearance. They look different with lights on versus off,
     * the same color, as in hex code, on white looks different when on black.
     * This is called color relativity, most famously explicated by Josef Albers
     * in Interaction of Color.
     *
     * In color science, color appearance models can account for this and
     * calculate the appearance of a color in different settings. HCT is based on
     * CAM16, a color appearance model, and uses it to make these calculations.
     *
     * See [ViewingConditions.make] for parameters affecting color appearance.
     */
    inViewingConditions(vc) {
      let viewedInVc = Cam16.fromInt(this.toInt()).xyzInViewingConditions(vc), recastInVc = Cam16.fromXyzInViewingConditions(viewedInVc[0], viewedInVc[1], viewedInVc[2], ViewingConditions.make());
      return _Hct.from(recastInVc.hue, recastInVc.chroma, lstarFromY(viewedInVc[1]));
    }
  };

  // node_modules/@material/material-color-utilities/contrast/contrast.js
  var Contrast = class _Contrast {
    /**
     * Returns a contrast ratio, which ranges from 1 to 21.
     *
     * @param toneA Tone between 0 and 100. Values outside will be clamped.
     * @param toneB Tone between 0 and 100. Values outside will be clamped.
     */
    static ratioOfTones(toneA, toneB) {
      return toneA = clampDouble(0, 100, toneA), toneB = clampDouble(0, 100, toneB), _Contrast.ratioOfYs(yFromLstar(toneA), yFromLstar(toneB));
    }
    static ratioOfYs(y1, y2) {
      let lighter = y1 > y2 ? y1 : y2, darker = lighter === y2 ? y1 : y2;
      return (lighter + 5) / (darker + 5);
    }
    /**
     * Returns a tone >= tone parameter that ensures ratio parameter.
     * Return value is between 0 and 100.
     * Returns -1 if ratio cannot be achieved with tone parameter.
     *
     * @param tone Tone return value must contrast with.
     * Range is 0 to 100. Invalid values will result in -1 being returned.
     * @param ratio Contrast ratio of return value and tone.
     * Range is 1 to 21, invalid values have undefined behavior.
     */
    static lighter(tone, ratio) {
      if (tone < 0 || tone > 100)
        return -1;
      let darkY = yFromLstar(tone), lightY = ratio * (darkY + 5) - 5, realContrast = _Contrast.ratioOfYs(lightY, darkY), delta = Math.abs(realContrast - ratio);
      if (realContrast < ratio && delta > 0.04)
        return -1;
      let returnValue = lstarFromY(lightY) + 0.4;
      return returnValue < 0 || returnValue > 100 ? -1 : returnValue;
    }
    /**
     * Returns a tone <= tone parameter that ensures ratio parameter.
     * Return value is between 0 and 100.
     * Returns -1 if ratio cannot be achieved with tone parameter.
     *
     * @param tone Tone return value must contrast with.
     * Range is 0 to 100. Invalid values will result in -1 being returned.
     * @param ratio Contrast ratio of return value and tone.
     * Range is 1 to 21, invalid values have undefined behavior.
     */
    static darker(tone, ratio) {
      if (tone < 0 || tone > 100)
        return -1;
      let lightY = yFromLstar(tone), darkY = (lightY + 5) / ratio - 5, realContrast = _Contrast.ratioOfYs(lightY, darkY), delta = Math.abs(realContrast - ratio);
      if (realContrast < ratio && delta > 0.04)
        return -1;
      let returnValue = lstarFromY(darkY) - 0.4;
      return returnValue < 0 || returnValue > 100 ? -1 : returnValue;
    }
    /**
     * Returns a tone >= tone parameter that ensures ratio parameter.
     * Return value is between 0 and 100.
     * Returns 100 if ratio cannot be achieved with tone parameter.
     *
     * This method is unsafe because the returned value is guaranteed to be in
     * bounds for tone, i.e. between 0 and 100. However, that value may not reach
     * the ratio with tone. For example, there is no color lighter than T100.
     *
     * @param tone Tone return value must contrast with.
     * Range is 0 to 100. Invalid values will result in 100 being returned.
     * @param ratio Desired contrast ratio of return value and tone parameter.
     * Range is 1 to 21, invalid values have undefined behavior.
     */
    static lighterUnsafe(tone, ratio) {
      let lighterSafe = _Contrast.lighter(tone, ratio);
      return lighterSafe < 0 ? 100 : lighterSafe;
    }
    /**
     * Returns a tone >= tone parameter that ensures ratio parameter.
     * Return value is between 0 and 100.
     * Returns 100 if ratio cannot be achieved with tone parameter.
     *
     * This method is unsafe because the returned value is guaranteed to be in
     * bounds for tone, i.e. between 0 and 100. However, that value may not reach
     * the [ratio with [tone]. For example, there is no color darker than T0.
     *
     * @param tone Tone return value must contrast with.
     * Range is 0 to 100. Invalid values will result in 0 being returned.
     * @param ratio Desired contrast ratio of return value and tone parameter.
     * Range is 1 to 21, invalid values have undefined behavior.
     */
    static darkerUnsafe(tone, ratio) {
      let darkerSafe = _Contrast.darker(tone, ratio);
      return darkerSafe < 0 ? 0 : darkerSafe;
    }
  };

  // node_modules/@material/material-color-utilities/dislike/dislike_analyzer.js
  var DislikeAnalyzer = class _DislikeAnalyzer {
    /**
     * Returns true if a color is disliked.
     *
     * @param hct A color to be judged.
     * @return Whether the color is disliked.
     *
     * Disliked is defined as a dark yellow-green that is not neutral.
     */
    static isDisliked(hct) {
      let huePasses = Math.round(hct.hue) >= 90 && Math.round(hct.hue) <= 111, chromaPasses = Math.round(hct.chroma) > 16, tonePasses = Math.round(hct.tone) < 65;
      return huePasses && chromaPasses && tonePasses;
    }
    /**
     * If a color is disliked, lighten it to make it likable.
     *
     * @param hct A color to be judged.
     * @return A new color if the original color is disliked, or the original
     *   color if it is acceptable.
     */
    static fixIfDisliked(hct) {
      return _DislikeAnalyzer.isDisliked(hct) ? Hct.from(hct.hue, hct.chroma, 70) : hct;
    }
  };

  // node_modules/@material/material-color-utilities/dynamiccolor/dynamic_color.js
  function validateExtendedColor(originalColor, specVersion, extendedColor) {
    if (originalColor.name !== extendedColor.name)
      throw new Error(`Attempting to extend color ${originalColor.name} with color ${extendedColor.name} of different name for spec version ${specVersion}.`);
    if (originalColor.isBackground !== extendedColor.isBackground)
      throw new Error(`Attempting to extend color ${originalColor.name} as a ${originalColor.isBackground ? "background" : "foreground"} with color ${extendedColor.name} as a ${extendedColor.isBackground ? "background" : "foreground"} for spec version ${specVersion}.`);
  }
  function extendSpecVersion(originlColor, specVersion, extendedColor) {
    return validateExtendedColor(originlColor, specVersion, extendedColor), DynamicColor.fromPalette({
      name: originlColor.name,
      palette: (s) => s.specVersion === specVersion ? extendedColor.palette(s) : originlColor.palette(s),
      tone: (s) => s.specVersion === specVersion ? extendedColor.tone(s) : originlColor.tone(s),
      isBackground: originlColor.isBackground,
      chromaMultiplier: (s) => {
        let chromaMultiplier = s.specVersion === specVersion ? extendedColor.chromaMultiplier : originlColor.chromaMultiplier;
        return chromaMultiplier !== void 0 ? chromaMultiplier(s) : 1;
      },
      background: (s) => {
        let background = s.specVersion === specVersion ? extendedColor.background : originlColor.background;
        return background !== void 0 ? background(s) : void 0;
      },
      secondBackground: (s) => {
        let secondBackground = s.specVersion === specVersion ? extendedColor.secondBackground : originlColor.secondBackground;
        return secondBackground !== void 0 ? secondBackground(s) : void 0;
      },
      contrastCurve: (s) => {
        let contrastCurve = s.specVersion === specVersion ? extendedColor.contrastCurve : originlColor.contrastCurve;
        return contrastCurve !== void 0 ? contrastCurve(s) : void 0;
      },
      toneDeltaPair: (s) => {
        let toneDeltaPair = s.specVersion === specVersion ? extendedColor.toneDeltaPair : originlColor.toneDeltaPair;
        return toneDeltaPair !== void 0 ? toneDeltaPair(s) : void 0;
      }
    });
  }
  var DynamicColor = class _DynamicColor {
    /**
     * Create a DynamicColor defined by a TonalPalette and HCT tone.
     *
     * @param args Functions with DynamicScheme as input. Must provide a palette
     *     and tone. May provide a background DynamicColor and ToneDeltaPair.
     */
    static fromPalette(args) {
      var _a, _b, _c;
      return new _DynamicColor((_a = args.name) != null ? _a : "", args.palette, (_b = args.tone) != null ? _b : _DynamicColor.getInitialToneFromBackground(args.background), (_c = args.isBackground) != null ? _c : !1, args.chromaMultiplier, args.background, args.secondBackground, args.contrastCurve, args.toneDeltaPair);
    }
    static getInitialToneFromBackground(background) {
      return background === void 0 ? (s) => 50 : (s) => background(s) ? background(s).getTone(s) : 50;
    }
    /**
     * The base constructor for DynamicColor.
     *
     * _Strongly_ prefer using one of the convenience constructors. This class is
     * arguably too flexible to ensure it can support any scenario. Functional
     * arguments allow  overriding without risks that come with subclasses.
     *
     * For example, the default behavior of adjust tone at max contrast
     * to be at a 7.0 ratio with its background is principled and
     * matches accessibility guidance. That does not mean it's the desired
     * approach for _every_ design system, and every color pairing,
     * always, in every case.
     *
     * @param name The name of the dynamic color. Defaults to empty.
     * @param palette Function that provides a TonalPalette given DynamicScheme. A
     *     TonalPalette is defined by a hue and chroma, so this replaces the need
     *     to specify hue/chroma. By providing a tonal palette, when contrast
     *     adjustments are made, intended chroma can be preserved.
     * @param tone Function that provides a tone, given a DynamicScheme.
     * @param isBackground Whether this dynamic color is a background, with some
     *     other color as the foreground. Defaults to false.
     * @param chromaMultiplier A factor that multiplies the chroma for this color.
     * @param background The background of the dynamic color (as a function of a
     *     `DynamicScheme`), if it exists.
     * @param secondBackground A second background of the dynamic color (as a
     *     function of a `DynamicScheme`), if it exists.
     * @param contrastCurve A `ContrastCurve` object specifying how its contrast
     *     against its background should behave in various contrast levels
     *     options.
     * @param toneDeltaPair A `ToneDeltaPair` object specifying a tone delta
     *     constraint between two colors. One of them must be the color being
     *     constructed.
     */
    constructor(name, palette, tone, isBackground, chromaMultiplier, background, secondBackground, contrastCurve, toneDeltaPair) {
      if (this.name = name, this.palette = palette, this.tone = tone, this.isBackground = isBackground, this.chromaMultiplier = chromaMultiplier, this.background = background, this.secondBackground = secondBackground, this.contrastCurve = contrastCurve, this.toneDeltaPair = toneDeltaPair, this.hctCache = /* @__PURE__ */ new Map(), !background && secondBackground)
        throw new Error(`Color ${name} has secondBackgrounddefined, but background is not defined.`);
      if (!background && contrastCurve)
        throw new Error(`Color ${name} has contrastCurvedefined, but background is not defined.`);
      if (background && !contrastCurve)
        throw new Error(`Color ${name} has backgrounddefined, but contrastCurve is not defined.`);
    }
    /**
     * Returns a deep copy of this DynamicColor.
     */
    clone() {
      return _DynamicColor.fromPalette({
        name: this.name,
        palette: this.palette,
        tone: this.tone,
        isBackground: this.isBackground,
        chromaMultiplier: this.chromaMultiplier,
        background: this.background,
        secondBackground: this.secondBackground,
        contrastCurve: this.contrastCurve,
        toneDeltaPair: this.toneDeltaPair
      });
    }
    /**
     * Clears the cache of HCT values for this color. For testing or debugging
     * purposes.
     */
    clearCache() {
      this.hctCache.clear();
    }
    /**
     * Returns a ARGB integer (i.e. a hex code).
     *
     * @param scheme Defines the conditions of the user interface, for example,
     *     whether or not it is dark mode or light mode, and what the desired
     *     contrast level is.
     */
    getArgb(scheme) {
      return this.getHct(scheme).toInt();
    }
    /**
     * Returns a color, expressed in the HCT color space, that this
     * DynamicColor is under the conditions in scheme.
     *
     * @param scheme Defines the conditions of the user interface, for example,
     *     whether or not it is dark mode or light mode, and what the desired
     *     contrast level is.
     */
    getHct(scheme) {
      let cachedAnswer = this.hctCache.get(scheme);
      if (cachedAnswer != null)
        return cachedAnswer;
      let answer = getSpec(scheme.specVersion).getHct(scheme, this);
      return this.hctCache.size > 4 && this.hctCache.clear(), this.hctCache.set(scheme, answer), answer;
    }
    /**
     * Returns a tone, T in the HCT color space, that this DynamicColor is under
     * the conditions in scheme.
     *
     * @param scheme Defines the conditions of the user interface, for example,
     *     whether or not it is dark mode or light mode, and what the desired
     *     contrast level is.
     */
    getTone(scheme) {
      return getSpec(scheme.specVersion).getTone(scheme, this);
    }
    /**
     * Given a background tone, finds a foreground tone, while ensuring they reach
     * a contrast ratio that is as close to [ratio] as possible.
     *
     * @param bgTone Tone in HCT. Range is 0 to 100, undefined behavior when it
     *     falls outside that range.
     * @param ratio The contrast ratio desired between bgTone and the return
     *     value.
     */
    static foregroundTone(bgTone, ratio) {
      let lighterTone = Contrast.lighterUnsafe(bgTone, ratio), darkerTone = Contrast.darkerUnsafe(bgTone, ratio), lighterRatio = Contrast.ratioOfTones(lighterTone, bgTone), darkerRatio = Contrast.ratioOfTones(darkerTone, bgTone);
      if (_DynamicColor.tonePrefersLightForeground(bgTone)) {
        let negligibleDifference = Math.abs(lighterRatio - darkerRatio) < 0.1 && lighterRatio < ratio && darkerRatio < ratio;
        return lighterRatio >= ratio || lighterRatio >= darkerRatio || negligibleDifference ? lighterTone : darkerTone;
      } else
        return darkerRatio >= ratio || darkerRatio >= lighterRatio ? darkerTone : lighterTone;
    }
    /**
     * Returns whether [tone] prefers a light foreground.
     *
     * People prefer white foregrounds on ~T60-70. Observed over time, and also
     * by Andrew Somers during research for APCA.
     *
     * T60 used as to create the smallest discontinuity possible when skipping
     * down to T49 in order to ensure light foregrounds.
     * Since `tertiaryContainer` in dark monochrome scheme requires a tone of
     * 60, it should not be adjusted. Therefore, 60 is excluded here.
     */
    static tonePrefersLightForeground(tone) {
      return Math.round(tone) < 60;
    }
    /**
     * Returns whether [tone] can reach a contrast ratio of 4.5 with a lighter
     * color.
     */
    static toneAllowsLightForeground(tone) {
      return Math.round(tone) <= 49;
    }
    /**
     * Adjusts a tone such that white has 4.5 contrast, if the tone is
     * reasonably close to supporting it.
     */
    static enableLightForeground(tone) {
      return _DynamicColor.tonePrefersLightForeground(tone) && !_DynamicColor.toneAllowsLightForeground(tone) ? 49 : tone;
    }
  }, ColorCalculationDelegateImpl2021 = class {
    getHct(scheme, color) {
      let tone = color.getTone(scheme);
      return color.palette(scheme).getHct(tone);
    }
    getTone(scheme, color) {
      let decreasingContrast = scheme.contrastLevel < 0, toneDeltaPair = color.toneDeltaPair ? color.toneDeltaPair(scheme) : void 0;
      if (toneDeltaPair) {
        let roleA = toneDeltaPair.roleA, roleB = toneDeltaPair.roleB, delta = toneDeltaPair.delta, polarity = toneDeltaPair.polarity, stayTogether = toneDeltaPair.stayTogether, aIsNearer = polarity === "nearer" || polarity === "lighter" && !scheme.isDark || polarity === "darker" && scheme.isDark, nearer = aIsNearer ? roleA : roleB, farther = aIsNearer ? roleB : roleA, amNearer = color.name === nearer.name, expansionDir = scheme.isDark ? 1 : -1, nTone = nearer.tone(scheme), fTone = farther.tone(scheme);
        if (color.background && nearer.contrastCurve && farther.contrastCurve) {
          let bg = color.background(scheme), nContrastCurve = nearer.contrastCurve(scheme), fContrastCurve = farther.contrastCurve(scheme);
          if (bg && nContrastCurve && fContrastCurve) {
            let bgTone = bg.getTone(scheme), nContrast = nContrastCurve.get(scheme.contrastLevel), fContrast = fContrastCurve.get(scheme.contrastLevel);
            Contrast.ratioOfTones(bgTone, nTone) < nContrast && (nTone = DynamicColor.foregroundTone(bgTone, nContrast)), Contrast.ratioOfTones(bgTone, fTone) < fContrast && (fTone = DynamicColor.foregroundTone(bgTone, fContrast)), decreasingContrast && (nTone = DynamicColor.foregroundTone(bgTone, nContrast), fTone = DynamicColor.foregroundTone(bgTone, fContrast));
          }
        }
        return (fTone - nTone) * expansionDir < delta && (fTone = clampDouble(0, 100, nTone + delta * expansionDir), (fTone - nTone) * expansionDir >= delta || (nTone = clampDouble(0, 100, fTone - delta * expansionDir))), 50 <= nTone && nTone < 60 ? expansionDir > 0 ? (nTone = 60, fTone = Math.max(fTone, nTone + delta * expansionDir)) : (nTone = 49, fTone = Math.min(fTone, nTone + delta * expansionDir)) : 50 <= fTone && fTone < 60 && (stayTogether ? expansionDir > 0 ? (nTone = 60, fTone = Math.max(fTone, nTone + delta * expansionDir)) : (nTone = 49, fTone = Math.min(fTone, nTone + delta * expansionDir)) : expansionDir > 0 ? fTone = 60 : fTone = 49), amNearer ? nTone : fTone;
      } else {
        let answer = color.tone(scheme);
        if (color.background == null || color.background(scheme) === void 0 || color.contrastCurve == null || color.contrastCurve(scheme) === void 0)
          return answer;
        let bgTone = color.background(scheme).getTone(scheme), desiredRatio = color.contrastCurve(scheme).get(scheme.contrastLevel);
        if (Contrast.ratioOfTones(bgTone, answer) >= desiredRatio || (answer = DynamicColor.foregroundTone(bgTone, desiredRatio)), decreasingContrast && (answer = DynamicColor.foregroundTone(bgTone, desiredRatio)), color.isBackground && 50 <= answer && answer < 60 && (Contrast.ratioOfTones(49, bgTone) >= desiredRatio ? answer = 49 : answer = 60), color.secondBackground == null || color.secondBackground(scheme) === void 0)
          return answer;
        let [bg1, bg2] = [color.background, color.secondBackground], [bgTone1, bgTone2] = [bg1(scheme).getTone(scheme), bg2(scheme).getTone(scheme)], [upper, lower] = [Math.max(bgTone1, bgTone2), Math.min(bgTone1, bgTone2)];
        if (Contrast.ratioOfTones(upper, answer) >= desiredRatio && Contrast.ratioOfTones(lower, answer) >= desiredRatio)
          return answer;
        let lightOption = Contrast.lighter(upper, desiredRatio), darkOption = Contrast.darker(lower, desiredRatio), availables = [];
        return lightOption !== -1 && availables.push(lightOption), darkOption !== -1 && availables.push(darkOption), DynamicColor.tonePrefersLightForeground(bgTone1) || DynamicColor.tonePrefersLightForeground(bgTone2) ? lightOption < 0 ? 100 : lightOption : availables.length === 1 ? availables[0] : darkOption < 0 ? 0 : darkOption;
      }
    }
  }, ColorCalculationDelegateImpl2025 = class {
    getHct(scheme, color) {
      let palette = color.palette(scheme), tone = color.getTone(scheme), hue = palette.hue, chroma = palette.chroma * (color.chromaMultiplier ? color.chromaMultiplier(scheme) : 1);
      return Hct.from(hue, chroma, tone);
    }
    getTone(scheme, color) {
      let toneDeltaPair = color.toneDeltaPair ? color.toneDeltaPair(scheme) : void 0;
      if (toneDeltaPair) {
        let roleA = toneDeltaPair.roleA, roleB = toneDeltaPair.roleB, polarity = toneDeltaPair.polarity, constraint = toneDeltaPair.constraint, absoluteDelta = polarity === "darker" || polarity === "relative_lighter" && scheme.isDark || polarity === "relative_darker" && !scheme.isDark ? -toneDeltaPair.delta : toneDeltaPair.delta, amRoleA = color.name === roleA.name, selfRole = amRoleA ? roleA : roleB, refRole = amRoleA ? roleB : roleA, selfTone = selfRole.tone(scheme), refTone = refRole.getTone(scheme), relativeDelta = absoluteDelta * (amRoleA ? 1 : -1);
        if (constraint === "exact" ? selfTone = clampDouble(0, 100, refTone + relativeDelta) : constraint === "nearer" ? relativeDelta > 0 ? selfTone = clampDouble(0, 100, clampDouble(refTone, refTone + relativeDelta, selfTone)) : selfTone = clampDouble(0, 100, clampDouble(refTone + relativeDelta, refTone, selfTone)) : constraint === "farther" && (relativeDelta > 0 ? selfTone = clampDouble(refTone + relativeDelta, 100, selfTone) : selfTone = clampDouble(0, refTone + relativeDelta, selfTone)), color.background && color.contrastCurve) {
          let background = color.background(scheme), contrastCurve = color.contrastCurve(scheme);
          if (background && contrastCurve) {
            let bgTone = background.getTone(scheme), selfContrast = contrastCurve.get(scheme.contrastLevel);
            selfTone = Contrast.ratioOfTones(bgTone, selfTone) >= selfContrast && scheme.contrastLevel >= 0 ? selfTone : DynamicColor.foregroundTone(bgTone, selfContrast);
          }
        }
        return color.isBackground && !color.name.endsWith("_fixed_dim") && (selfTone >= 57 ? selfTone = clampDouble(65, 100, selfTone) : selfTone = clampDouble(0, 49, selfTone)), selfTone;
      } else {
        let answer = color.tone(scheme);
        if (color.background == null || color.background(scheme) === void 0 || color.contrastCurve == null || color.contrastCurve(scheme) === void 0)
          return answer;
        let bgTone = color.background(scheme).getTone(scheme), desiredRatio = color.contrastCurve(scheme).get(scheme.contrastLevel);
        if (answer = Contrast.ratioOfTones(bgTone, answer) >= desiredRatio && scheme.contrastLevel >= 0 ? answer : DynamicColor.foregroundTone(bgTone, desiredRatio), color.isBackground && !color.name.endsWith("_fixed_dim") && (answer >= 57 ? answer = clampDouble(65, 100, answer) : answer = clampDouble(0, 49, answer)), color.secondBackground == null || color.secondBackground(scheme) === void 0)
          return answer;
        let [bg1, bg2] = [color.background, color.secondBackground], [bgTone1, bgTone2] = [bg1(scheme).getTone(scheme), bg2(scheme).getTone(scheme)], [upper, lower] = [Math.max(bgTone1, bgTone2), Math.min(bgTone1, bgTone2)];
        if (Contrast.ratioOfTones(upper, answer) >= desiredRatio && Contrast.ratioOfTones(lower, answer) >= desiredRatio)
          return answer;
        let lightOption = Contrast.lighter(upper, desiredRatio), darkOption = Contrast.darker(lower, desiredRatio), availables = [];
        return lightOption !== -1 && availables.push(lightOption), darkOption !== -1 && availables.push(darkOption), DynamicColor.tonePrefersLightForeground(bgTone1) || DynamicColor.tonePrefersLightForeground(bgTone2) ? lightOption < 0 ? 100 : lightOption : availables.length === 1 ? availables[0] : darkOption < 0 ? 0 : darkOption;
      }
    }
  }, spec2021 = new ColorCalculationDelegateImpl2021(), spec2025 = new ColorCalculationDelegateImpl2025();
  function getSpec(specVersion) {
    return specVersion === "2025" ? spec2025 : spec2021;
  }

  // node_modules/@material/material-color-utilities/palettes/tonal_palette.js
  var TonalPalette = class _TonalPalette {
    /**
     * @param argb ARGB representation of a color
     * @return Tones matching that color's hue and chroma.
     */
    static fromInt(argb) {
      let hct = Hct.fromInt(argb);
      return _TonalPalette.fromHct(hct);
    }
    /**
     * @param hct Hct
     * @return Tones matching that color's hue and chroma.
     */
    static fromHct(hct) {
      return new _TonalPalette(hct.hue, hct.chroma, hct);
    }
    /**
     * @param hue HCT hue
     * @param chroma HCT chroma
     * @return Tones matching hue and chroma.
     */
    static fromHueAndChroma(hue, chroma) {
      let keyColor = new KeyColor(hue, chroma).create();
      return new _TonalPalette(hue, chroma, keyColor);
    }
    constructor(hue, chroma, keyColor) {
      this.hue = hue, this.chroma = chroma, this.keyColor = keyColor, this.cache = /* @__PURE__ */ new Map();
    }
    /**
     * @param tone HCT tone, measured from 0 to 100.
     * @return ARGB representation of a color with that tone.
     */
    tone(tone) {
      let argb = this.cache.get(tone);
      return argb === void 0 && (tone == 99 && Hct.isYellow(this.hue) ? argb = this.averageArgb(this.tone(98), this.tone(100)) : argb = Hct.from(this.hue, this.chroma, tone).toInt(), this.cache.set(tone, argb)), argb;
    }
    /**
     * @param tone HCT tone.
     * @return HCT representation of a color with that tone.
     */
    getHct(tone) {
      return Hct.fromInt(this.tone(tone));
    }
    averageArgb(argb1, argb2) {
      let red1 = argb1 >>> 16 & 255, green1 = argb1 >>> 8 & 255, blue1 = argb1 & 255, red2 = argb2 >>> 16 & 255, green2 = argb2 >>> 8 & 255, blue2 = argb2 & 255, red = Math.round((red1 + red2) / 2), green = Math.round((green1 + green2) / 2), blue = Math.round((blue1 + blue2) / 2);
      return (255 << 24 | (red & 255) << 16 | (green & 255) << 8 | blue & 255) >>> 0;
    }
  }, KeyColor = class {
    constructor(hue, requestedChroma) {
      this.hue = hue, this.requestedChroma = requestedChroma, this.chromaCache = /* @__PURE__ */ new Map(), this.maxChromaValue = 200;
    }
    /**
     * Creates a key color from a [hue] and a [chroma].
     * The key color is the first tone, starting from T50, matching the given hue
     * and chroma.
     *
     * @return Key color [Hct]
     */
    create() {
      let lowerTone = 0, upperTone = 100;
      for (; lowerTone < upperTone; ) {
        let midTone = Math.floor((lowerTone + upperTone) / 2), isAscending = this.maxChroma(midTone) < this.maxChroma(midTone + 1);
        if (this.maxChroma(midTone) >= this.requestedChroma - 0.01)
          if (Math.abs(lowerTone - 50) < Math.abs(upperTone - 50))
            upperTone = midTone;
          else {
            if (lowerTone === midTone)
              return Hct.from(this.hue, this.requestedChroma, lowerTone);
            lowerTone = midTone;
          }
        else
          isAscending ? lowerTone = midTone + 1 : upperTone = midTone;
      }
      return Hct.from(this.hue, this.requestedChroma, lowerTone);
    }
    // Find the maximum chroma for a given tone
    maxChroma(tone) {
      if (this.chromaCache.has(tone))
        return this.chromaCache.get(tone);
      let chroma = Hct.from(this.hue, this.maxChromaValue, tone).chroma;
      return this.chromaCache.set(tone, chroma), chroma;
    }
  };

  // node_modules/@material/material-color-utilities/temperature/temperature_cache.js
  var TemperatureCache = class _TemperatureCache {
    constructor(input) {
      this.input = input, this.hctsByTempCache = [], this.hctsByHueCache = [], this.tempsByHctCache = /* @__PURE__ */ new Map(), this.inputRelativeTemperatureCache = -1, this.complementCache = null;
    }
    get hctsByTemp() {
      if (this.hctsByTempCache.length > 0)
        return this.hctsByTempCache;
      let hcts = this.hctsByHue.concat([this.input]), temperaturesByHct = this.tempsByHct;
      return hcts.sort((a, b) => temperaturesByHct.get(a) - temperaturesByHct.get(b)), this.hctsByTempCache = hcts, hcts;
    }
    get warmest() {
      return this.hctsByTemp[this.hctsByTemp.length - 1];
    }
    get coldest() {
      return this.hctsByTemp[0];
    }
    /**
     * A set of colors with differing hues, equidistant in temperature.
     *
     * In art, this is usually described as a set of 5 colors on a color wheel
     * divided into 12 sections. This method allows provision of either of those
     * values.
     *
     * Behavior is undefined when [count] or [divisions] is 0.
     * When divisions < count, colors repeat.
     *
     * [count] The number of colors to return, includes the input color.
     * [divisions] The number of divisions on the color wheel.
     */
    analogous(count = 5, divisions = 12) {
      let startHue = Math.round(this.input.hue), startHct = this.hctsByHue[startHue], lastTemp = this.relativeTemperature(startHct), allColors = [startHct], absoluteTotalTempDelta = 0;
      for (let i = 0; i < 360; i++) {
        let hue = sanitizeDegreesInt(startHue + i), hct = this.hctsByHue[hue], temp = this.relativeTemperature(hct), tempDelta = Math.abs(temp - lastTemp);
        lastTemp = temp, absoluteTotalTempDelta += tempDelta;
      }
      let hueAddend = 1, tempStep = absoluteTotalTempDelta / divisions, totalTempDelta = 0;
      for (lastTemp = this.relativeTemperature(startHct); allColors.length < divisions; ) {
        let hue = sanitizeDegreesInt(startHue + hueAddend), hct = this.hctsByHue[hue], temp = this.relativeTemperature(hct), tempDelta = Math.abs(temp - lastTemp);
        totalTempDelta += tempDelta;
        let desiredTotalTempDeltaForIndex = allColors.length * tempStep, indexSatisfied = totalTempDelta >= desiredTotalTempDeltaForIndex, indexAddend = 1;
        for (; indexSatisfied && allColors.length < divisions; ) {
          allColors.push(hct);
          let desiredTotalTempDeltaForIndex2 = (allColors.length + indexAddend) * tempStep;
          indexSatisfied = totalTempDelta >= desiredTotalTempDeltaForIndex2, indexAddend++;
        }
        if (lastTemp = temp, hueAddend++, hueAddend > 360) {
          for (; allColors.length < divisions; )
            allColors.push(hct);
          break;
        }
      }
      let answers = [this.input], increaseHueCount = Math.floor((count - 1) / 2);
      for (let i = 1; i < increaseHueCount + 1; i++) {
        let index = 0 - i;
        for (; index < 0; )
          index = allColors.length + index;
        index >= allColors.length && (index = index % allColors.length), answers.splice(0, 0, allColors[index]);
      }
      let decreaseHueCount = count - increaseHueCount - 1;
      for (let i = 1; i < decreaseHueCount + 1; i++) {
        let index = i;
        for (; index < 0; )
          index = allColors.length + index;
        index >= allColors.length && (index = index % allColors.length), answers.push(allColors[index]);
      }
      return answers;
    }
    /**
     * A color that complements the input color aesthetically.
     *
     * In art, this is usually described as being across the color wheel.
     * History of this shows intent as a color that is just as cool-warm as the
     * input color is warm-cool.
     */
    get complement() {
      if (this.complementCache != null)
        return this.complementCache;
      let coldestHue = this.coldest.hue, coldestTemp = this.tempsByHct.get(this.coldest), warmestHue = this.warmest.hue, range = this.tempsByHct.get(this.warmest) - coldestTemp, startHueIsColdestToWarmest = _TemperatureCache.isBetween(this.input.hue, coldestHue, warmestHue), startHue = startHueIsColdestToWarmest ? warmestHue : coldestHue, endHue = startHueIsColdestToWarmest ? coldestHue : warmestHue, directionOfRotation = 1, smallestError = 1e3, answer = this.hctsByHue[Math.round(this.input.hue)], complementRelativeTemp = 1 - this.inputRelativeTemperature;
      for (let hueAddend = 0; hueAddend <= 360; hueAddend += 1) {
        let hue = sanitizeDegreesDouble(startHue + directionOfRotation * hueAddend);
        if (!_TemperatureCache.isBetween(hue, startHue, endHue))
          continue;
        let possibleAnswer = this.hctsByHue[Math.round(hue)], relativeTemp = (this.tempsByHct.get(possibleAnswer) - coldestTemp) / range, error = Math.abs(complementRelativeTemp - relativeTemp);
        error < smallestError && (smallestError = error, answer = possibleAnswer);
      }
      return this.complementCache = answer, this.complementCache;
    }
    /**
     * Temperature relative to all colors with the same chroma and tone.
     * Value on a scale from 0 to 1.
     */
    relativeTemperature(hct) {
      let range = this.tempsByHct.get(this.warmest) - this.tempsByHct.get(this.coldest), differenceFromColdest = this.tempsByHct.get(hct) - this.tempsByHct.get(this.coldest);
      return range === 0 ? 0.5 : differenceFromColdest / range;
    }
    /** Relative temperature of the input color. See [relativeTemperature]. */
    get inputRelativeTemperature() {
      return this.inputRelativeTemperatureCache >= 0 ? this.inputRelativeTemperatureCache : (this.inputRelativeTemperatureCache = this.relativeTemperature(this.input), this.inputRelativeTemperatureCache);
    }
    /** A Map with keys of HCTs in [hctsByTemp], values of raw temperature. */
    get tempsByHct() {
      if (this.tempsByHctCache.size > 0)
        return this.tempsByHctCache;
      let allHcts = this.hctsByHue.concat([this.input]), temperaturesByHct = /* @__PURE__ */ new Map();
      for (let e of allHcts)
        temperaturesByHct.set(e, _TemperatureCache.rawTemperature(e));
      return this.tempsByHctCache = temperaturesByHct, temperaturesByHct;
    }
    /**
     * HCTs for all hues, with the same chroma/tone as the input.
     * Sorted ascending, hue 0 to 360.
     */
    get hctsByHue() {
      if (this.hctsByHueCache.length > 0)
        return this.hctsByHueCache;
      let hcts = [];
      for (let hue = 0; hue <= 360; hue += 1) {
        let colorAtHue = Hct.from(hue, this.input.chroma, this.input.tone);
        hcts.push(colorAtHue);
      }
      return this.hctsByHueCache = hcts, this.hctsByHueCache;
    }
    /** Determines if an angle is between two other angles, rotating clockwise. */
    static isBetween(angle, a, b) {
      return a < b ? a <= angle && angle <= b : a <= angle || angle <= b;
    }
    /**
     * Value representing cool-warm factor of a color.
     * Values below 0 are considered cool, above, warm.
     *
     * Color science has researched emotion and harmony, which art uses to select
     * colors. Warm-cool is the foundation of analogous and complementary colors.
     * See:
     * - Li-Chen Ou's Chapter 19 in Handbook of Color Psychology (2015).
     * - Josef Albers' Interaction of Color chapters 19 and 21.
     *
     * Implementation of Ou, Woodcock and Wright's algorithm, which uses
     * L*a*b* / LCH color space.
     * Return value has these properties:
     * - Values below 0 are cool, above 0 are warm.
     * - Lower bound: -0.52 - (chroma ^ 1.07 / 20). L*a*b* chroma is infinite.
     *   Assuming max of 130 chroma, -9.66.
     * - Upper bound: -0.52 + (chroma ^ 1.07 / 20). L*a*b* chroma is infinite.
     *   Assuming max of 130 chroma, 8.61.
     */
    static rawTemperature(color) {
      let lab = labFromArgb(color.toInt()), hue = sanitizeDegreesDouble(Math.atan2(lab[2], lab[1]) * 180 / Math.PI), chroma = Math.sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
      return -0.5 + 0.02 * Math.pow(chroma, 1.07) * Math.cos(sanitizeDegreesDouble(hue - 50) * Math.PI / 180);
    }
  };

  // node_modules/@material/material-color-utilities/dynamiccolor/contrast_curve.js
  var ContrastCurve = class {
    /**
     * Creates a `ContrastCurve` object.
     *
     * @param low Value for contrast level -1.0
     * @param normal Value for contrast level 0.0
     * @param medium Value for contrast level 0.5
     * @param high Value for contrast level 1.0
     */
    constructor(low, normal, medium, high) {
      this.low = low, this.normal = normal, this.medium = medium, this.high = high;
    }
    /**
     * Returns the value at a given contrast level.
     *
     * @param contrastLevel The contrast level. 0.0 is the default (normal); -1.0
     *     is the lowest; 1.0 is the highest.
     * @return The value. For contrast ratios, a number between 1.0 and 21.0.
     */
    get(contrastLevel) {
      return contrastLevel <= -1 ? this.low : contrastLevel < 0 ? lerp(this.low, this.normal, (contrastLevel - -1) / 1) : contrastLevel < 0.5 ? lerp(this.normal, this.medium, (contrastLevel - 0) / 0.5) : contrastLevel < 1 ? lerp(this.medium, this.high, (contrastLevel - 0.5) / 0.5) : this.high;
    }
  };

  // node_modules/@material/material-color-utilities/dynamiccolor/tone_delta_pair.js
  var ToneDeltaPair = class {
    /**
     * Documents a constraint in tone distance between two DynamicColors.
     *
     * The polarity is an adjective that describes "A", compared to "B".
     *
     * For instance, ToneDeltaPair(A, B, 15, 'darker', 'exact') states that
     * A's tone should be exactly 15 darker than B's.
     *
     * 'relative_darker' and 'relative_lighter' describes the tone adjustment
     * relative to the surface color trend (white in light mode; black in dark
     * mode). For instance, ToneDeltaPair(A, B, 10, 'relative_lighter',
     * 'farther') states that A should be at least 10 lighter than B in light
     * mode, and at least 10 darker than B in dark mode.
     *
     * @param roleA The first role in a pair.
     * @param roleB The second role in a pair.
     * @param delta Required difference between tones. Absolute value, negative
     * values have undefined behavior.
     * @param polarity The relative relation between tones of roleA and roleB,
     * as described above.
     * @param constraint How to fulfill the tone delta pair constraint.
     * @param stayTogether Whether these two roles should stay on the same side
     * of the "awkward zone" (T50-59). This is necessary for certain cases where
     * one role has two backgrounds.
     */
    constructor(roleA, roleB, delta, polarity, stayTogether, constraint) {
      this.roleA = roleA, this.roleB = roleB, this.delta = delta, this.polarity = polarity, this.stayTogether = stayTogether, this.constraint = constraint, this.constraint = constraint != null ? constraint : "exact";
    }
  };

  // node_modules/@material/material-color-utilities/dynamiccolor/variant.js
  var Variant;
  (function(Variant2) {
    Variant2[Variant2.MONOCHROME = 0] = "MONOCHROME", Variant2[Variant2.NEUTRAL = 1] = "NEUTRAL", Variant2[Variant2.TONAL_SPOT = 2] = "TONAL_SPOT", Variant2[Variant2.VIBRANT = 3] = "VIBRANT", Variant2[Variant2.EXPRESSIVE = 4] = "EXPRESSIVE", Variant2[Variant2.FIDELITY = 5] = "FIDELITY", Variant2[Variant2.CONTENT = 6] = "CONTENT", Variant2[Variant2.RAINBOW = 7] = "RAINBOW", Variant2[Variant2.FRUIT_SALAD = 8] = "FRUIT_SALAD";
  })(Variant || (Variant = {}));

  // node_modules/@material/material-color-utilities/dynamiccolor/color_spec_2021.js
  function isFidelity(scheme) {
    return scheme.variant === Variant.FIDELITY || scheme.variant === Variant.CONTENT;
  }
  function isMonochrome(scheme) {
    return scheme.variant === Variant.MONOCHROME;
  }
  function findDesiredChromaByTone(hue, chroma, tone, byDecreasingTone) {
    let answer = tone, closestToChroma = Hct.from(hue, chroma, tone);
    if (closestToChroma.chroma < chroma) {
      let chromaPeak = closestToChroma.chroma;
      for (; closestToChroma.chroma < chroma; ) {
        answer += byDecreasingTone ? -1 : 1;
        let potentialSolution = Hct.from(hue, chroma, answer);
        if (chromaPeak > potentialSolution.chroma || Math.abs(potentialSolution.chroma - chroma) < 0.4)
          break;
        let potentialDelta = Math.abs(potentialSolution.chroma - chroma), currentDelta = Math.abs(closestToChroma.chroma - chroma);
        potentialDelta < currentDelta && (closestToChroma = potentialSolution), chromaPeak = Math.max(chromaPeak, potentialSolution.chroma);
      }
    }
    return answer;
  }
  var ColorSpecDelegateImpl2021 = class {
    ////////////////////////////////////////////////////////////////
    // Main Palettes                                              //
    ////////////////////////////////////////////////////////////////
    primaryPaletteKeyColor() {
      return DynamicColor.fromPalette({
        name: "primary_palette_key_color",
        palette: (s) => s.primaryPalette,
        tone: (s) => s.primaryPalette.keyColor.tone
      });
    }
    secondaryPaletteKeyColor() {
      return DynamicColor.fromPalette({
        name: "secondary_palette_key_color",
        palette: (s) => s.secondaryPalette,
        tone: (s) => s.secondaryPalette.keyColor.tone
      });
    }
    tertiaryPaletteKeyColor() {
      return DynamicColor.fromPalette({
        name: "tertiary_palette_key_color",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => s.tertiaryPalette.keyColor.tone
      });
    }
    neutralPaletteKeyColor() {
      return DynamicColor.fromPalette({
        name: "neutral_palette_key_color",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.neutralPalette.keyColor.tone
      });
    }
    neutralVariantPaletteKeyColor() {
      return DynamicColor.fromPalette({
        name: "neutral_variant_palette_key_color",
        palette: (s) => s.neutralVariantPalette,
        tone: (s) => s.neutralVariantPalette.keyColor.tone
      });
    }
    errorPaletteKeyColor() {
      return DynamicColor.fromPalette({
        name: "error_palette_key_color",
        palette: (s) => s.errorPalette,
        tone: (s) => s.errorPalette.keyColor.tone
      });
    }
    ////////////////////////////////////////////////////////////////
    // Surfaces [S]                                               //
    ////////////////////////////////////////////////////////////////
    background() {
      return DynamicColor.fromPalette({
        name: "background",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 6 : 98,
        isBackground: !0
      });
    }
    onBackground() {
      return DynamicColor.fromPalette({
        name: "on_background",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 90 : 10,
        background: (s) => this.background(),
        contrastCurve: (s) => new ContrastCurve(3, 3, 4.5, 7)
      });
    }
    surface() {
      return DynamicColor.fromPalette({
        name: "surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 6 : 98,
        isBackground: !0
      });
    }
    surfaceDim() {
      return DynamicColor.fromPalette({
        name: "surface_dim",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 6 : new ContrastCurve(87, 87, 80, 75).get(s.contrastLevel),
        isBackground: !0
      });
    }
    surfaceBright() {
      return DynamicColor.fromPalette({
        name: "surface_bright",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? new ContrastCurve(24, 24, 29, 34).get(s.contrastLevel) : 98,
        isBackground: !0
      });
    }
    surfaceContainerLowest() {
      return DynamicColor.fromPalette({
        name: "surface_container_lowest",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? new ContrastCurve(4, 4, 2, 0).get(s.contrastLevel) : 100,
        isBackground: !0
      });
    }
    surfaceContainerLow() {
      return DynamicColor.fromPalette({
        name: "surface_container_low",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? new ContrastCurve(10, 10, 11, 12).get(s.contrastLevel) : new ContrastCurve(96, 96, 96, 95).get(s.contrastLevel),
        isBackground: !0
      });
    }
    surfaceContainer() {
      return DynamicColor.fromPalette({
        name: "surface_container",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? new ContrastCurve(12, 12, 16, 20).get(s.contrastLevel) : new ContrastCurve(94, 94, 92, 90).get(s.contrastLevel),
        isBackground: !0
      });
    }
    surfaceContainerHigh() {
      return DynamicColor.fromPalette({
        name: "surface_container_high",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? new ContrastCurve(17, 17, 21, 25).get(s.contrastLevel) : new ContrastCurve(92, 92, 88, 85).get(s.contrastLevel),
        isBackground: !0
      });
    }
    surfaceContainerHighest() {
      return DynamicColor.fromPalette({
        name: "surface_container_highest",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? new ContrastCurve(22, 22, 26, 30).get(s.contrastLevel) : new ContrastCurve(90, 90, 84, 80).get(s.contrastLevel),
        isBackground: !0
      });
    }
    onSurface() {
      return DynamicColor.fromPalette({
        name: "on_surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 90 : 10,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    surfaceVariant() {
      return DynamicColor.fromPalette({
        name: "surface_variant",
        palette: (s) => s.neutralVariantPalette,
        tone: (s) => s.isDark ? 30 : 90,
        isBackground: !0
      });
    }
    onSurfaceVariant() {
      return DynamicColor.fromPalette({
        name: "on_surface_variant",
        palette: (s) => s.neutralVariantPalette,
        tone: (s) => s.isDark ? 80 : 30,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    inverseSurface() {
      return DynamicColor.fromPalette({
        name: "inverse_surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 90 : 20,
        isBackground: !0
      });
    }
    inverseOnSurface() {
      return DynamicColor.fromPalette({
        name: "inverse_on_surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 20 : 95,
        background: (s) => this.inverseSurface(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    outline() {
      return DynamicColor.fromPalette({
        name: "outline",
        palette: (s) => s.neutralVariantPalette,
        tone: (s) => s.isDark ? 60 : 50,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1.5, 3, 4.5, 7)
      });
    }
    outlineVariant() {
      return DynamicColor.fromPalette({
        name: "outline_variant",
        palette: (s) => s.neutralVariantPalette,
        tone: (s) => s.isDark ? 30 : 80,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5)
      });
    }
    shadow() {
      return DynamicColor.fromPalette({
        name: "shadow",
        palette: (s) => s.neutralPalette,
        tone: (s) => 0
      });
    }
    scrim() {
      return DynamicColor.fromPalette({
        name: "scrim",
        palette: (s) => s.neutralPalette,
        tone: (s) => 0
      });
    }
    surfaceTint() {
      return DynamicColor.fromPalette({
        name: "surface_tint",
        palette: (s) => s.primaryPalette,
        tone: (s) => s.isDark ? 80 : 40,
        isBackground: !0
      });
    }
    ////////////////////////////////////////////////////////////////
    // Primary [P].                                               //
    ////////////////////////////////////////////////////////////////
    primary() {
      return DynamicColor.fromPalette({
        name: "primary",
        palette: (s) => s.primaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 100 : 0 : s.isDark ? 80 : 40,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 7),
        toneDeltaPair: (s) => new ToneDeltaPair(this.primaryContainer(), this.primary(), 10, "nearer", !1)
      });
    }
    primaryDim() {
    }
    onPrimary() {
      return DynamicColor.fromPalette({
        name: "on_primary",
        palette: (s) => s.primaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 10 : 90 : s.isDark ? 20 : 100,
        background: (s) => this.primary(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    primaryContainer() {
      return DynamicColor.fromPalette({
        name: "primary_container",
        palette: (s) => s.primaryPalette,
        tone: (s) => isFidelity(s) ? s.sourceColorHct.tone : isMonochrome(s) ? s.isDark ? 85 : 25 : s.isDark ? 30 : 90,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.primaryContainer(), this.primary(), 10, "nearer", !1)
      });
    }
    onPrimaryContainer() {
      return DynamicColor.fromPalette({
        name: "on_primary_container",
        palette: (s) => s.primaryPalette,
        tone: (s) => isFidelity(s) ? DynamicColor.foregroundTone(this.primaryContainer().tone(s), 4.5) : isMonochrome(s) ? s.isDark ? 0 : 100 : s.isDark ? 90 : 30,
        background: (s) => this.primaryContainer(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    inversePrimary() {
      return DynamicColor.fromPalette({
        name: "inverse_primary",
        palette: (s) => s.primaryPalette,
        tone: (s) => s.isDark ? 40 : 80,
        background: (s) => this.inverseSurface(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 7)
      });
    }
    /////////////////////////////////////////////////////////////////
    // Secondary [Q].                                              //
    /////////////////////////////////////////////////////////////////
    secondary() {
      return DynamicColor.fromPalette({
        name: "secondary",
        palette: (s) => s.secondaryPalette,
        tone: (s) => s.isDark ? 80 : 40,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 7),
        toneDeltaPair: (s) => new ToneDeltaPair(this.secondaryContainer(), this.secondary(), 10, "nearer", !1)
      });
    }
    secondaryDim() {
    }
    onSecondary() {
      return DynamicColor.fromPalette({
        name: "on_secondary",
        palette: (s) => s.secondaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 10 : 100 : s.isDark ? 20 : 100,
        background: (s) => this.secondary(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    secondaryContainer() {
      return DynamicColor.fromPalette({
        name: "secondary_container",
        palette: (s) => s.secondaryPalette,
        tone: (s) => {
          let initialTone = s.isDark ? 30 : 90;
          return isMonochrome(s) ? s.isDark ? 30 : 85 : isFidelity(s) ? findDesiredChromaByTone(s.secondaryPalette.hue, s.secondaryPalette.chroma, initialTone, !s.isDark) : initialTone;
        },
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.secondaryContainer(), this.secondary(), 10, "nearer", !1)
      });
    }
    onSecondaryContainer() {
      return DynamicColor.fromPalette({
        name: "on_secondary_container",
        palette: (s) => s.secondaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 90 : 10 : isFidelity(s) ? DynamicColor.foregroundTone(this.secondaryContainer().tone(s), 4.5) : s.isDark ? 90 : 30,
        background: (s) => this.secondaryContainer(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    /////////////////////////////////////////////////////////////////
    // Tertiary [T].                                               //
    /////////////////////////////////////////////////////////////////
    tertiary() {
      return DynamicColor.fromPalette({
        name: "tertiary",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 90 : 25 : s.isDark ? 80 : 40,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 7),
        toneDeltaPair: (s) => new ToneDeltaPair(this.tertiaryContainer(), this.tertiary(), 10, "nearer", !1)
      });
    }
    tertiaryDim() {
    }
    onTertiary() {
      return DynamicColor.fromPalette({
        name: "on_tertiary",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 10 : 90 : s.isDark ? 20 : 100,
        background: (s) => this.tertiary(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    tertiaryContainer() {
      return DynamicColor.fromPalette({
        name: "tertiary_container",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => {
          if (isMonochrome(s))
            return s.isDark ? 60 : 49;
          if (!isFidelity(s))
            return s.isDark ? 30 : 90;
          let proposedHct = s.tertiaryPalette.getHct(s.sourceColorHct.tone);
          return DislikeAnalyzer.fixIfDisliked(proposedHct).tone;
        },
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.tertiaryContainer(), this.tertiary(), 10, "nearer", !1)
      });
    }
    onTertiaryContainer() {
      return DynamicColor.fromPalette({
        name: "on_tertiary_container",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 0 : 100 : isFidelity(s) ? DynamicColor.foregroundTone(this.tertiaryContainer().tone(s), 4.5) : s.isDark ? 90 : 30,
        background: (s) => this.tertiaryContainer(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    //////////////////////////////////////////////////////////////////
    // Error [E].                                                   //
    //////////////////////////////////////////////////////////////////
    error() {
      return DynamicColor.fromPalette({
        name: "error",
        palette: (s) => s.errorPalette,
        tone: (s) => s.isDark ? 80 : 40,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 7),
        toneDeltaPair: (s) => new ToneDeltaPair(this.errorContainer(), this.error(), 10, "nearer", !1)
      });
    }
    errorDim() {
    }
    onError() {
      return DynamicColor.fromPalette({
        name: "on_error",
        palette: (s) => s.errorPalette,
        tone: (s) => s.isDark ? 20 : 100,
        background: (s) => this.error(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    errorContainer() {
      return DynamicColor.fromPalette({
        name: "error_container",
        palette: (s) => s.errorPalette,
        tone: (s) => s.isDark ? 30 : 90,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.errorContainer(), this.error(), 10, "nearer", !1)
      });
    }
    onErrorContainer() {
      return DynamicColor.fromPalette({
        name: "on_error_container",
        palette: (s) => s.errorPalette,
        tone: (s) => isMonochrome(s) ? s.isDark ? 90 : 10 : s.isDark ? 90 : 30,
        background: (s) => this.errorContainer(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    //////////////////////////////////////////////////////////////////
    // Primary Fixed [PF]                                           //
    //////////////////////////////////////////////////////////////////
    primaryFixed() {
      return DynamicColor.fromPalette({
        name: "primary_fixed",
        palette: (s) => s.primaryPalette,
        tone: (s) => isMonochrome(s) ? 40 : 90,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.primaryFixed(), this.primaryFixedDim(), 10, "lighter", !0)
      });
    }
    primaryFixedDim() {
      return DynamicColor.fromPalette({
        name: "primary_fixed_dim",
        palette: (s) => s.primaryPalette,
        tone: (s) => isMonochrome(s) ? 30 : 80,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.primaryFixed(), this.primaryFixedDim(), 10, "lighter", !0)
      });
    }
    onPrimaryFixed() {
      return DynamicColor.fromPalette({
        name: "on_primary_fixed",
        palette: (s) => s.primaryPalette,
        tone: (s) => isMonochrome(s) ? 100 : 10,
        background: (s) => this.primaryFixedDim(),
        secondBackground: (s) => this.primaryFixed(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    onPrimaryFixedVariant() {
      return DynamicColor.fromPalette({
        name: "on_primary_fixed_variant",
        palette: (s) => s.primaryPalette,
        tone: (s) => isMonochrome(s) ? 90 : 30,
        background: (s) => this.primaryFixedDim(),
        secondBackground: (s) => this.primaryFixed(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    ///////////////////////////////////////////////////////////////////
    // Secondary Fixed [QF]                                          //
    ///////////////////////////////////////////////////////////////////
    secondaryFixed() {
      return DynamicColor.fromPalette({
        name: "secondary_fixed",
        palette: (s) => s.secondaryPalette,
        tone: (s) => isMonochrome(s) ? 80 : 90,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.secondaryFixed(), this.secondaryFixedDim(), 10, "lighter", !0)
      });
    }
    secondaryFixedDim() {
      return DynamicColor.fromPalette({
        name: "secondary_fixed_dim",
        palette: (s) => s.secondaryPalette,
        tone: (s) => isMonochrome(s) ? 70 : 80,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.secondaryFixed(), this.secondaryFixedDim(), 10, "lighter", !0)
      });
    }
    onSecondaryFixed() {
      return DynamicColor.fromPalette({
        name: "on_secondary_fixed",
        palette: (s) => s.secondaryPalette,
        tone: (s) => 10,
        background: (s) => this.secondaryFixedDim(),
        secondBackground: (s) => this.secondaryFixed(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    onSecondaryFixedVariant() {
      return DynamicColor.fromPalette({
        name: "on_secondary_fixed_variant",
        palette: (s) => s.secondaryPalette,
        tone: (s) => isMonochrome(s) ? 25 : 30,
        background: (s) => this.secondaryFixedDim(),
        secondBackground: (s) => this.secondaryFixed(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    /////////////////////////////////////////////////////////////////
    // Tertiary Fixed [TF]                                         //
    /////////////////////////////////////////////////////////////////
    tertiaryFixed() {
      return DynamicColor.fromPalette({
        name: "tertiary_fixed",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? 40 : 90,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.tertiaryFixed(), this.tertiaryFixedDim(), 10, "lighter", !0)
      });
    }
    tertiaryFixedDim() {
      return DynamicColor.fromPalette({
        name: "tertiary_fixed_dim",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? 30 : 80,
        isBackground: !0,
        background: (s) => this.highestSurface(s),
        contrastCurve: (s) => new ContrastCurve(1, 1, 3, 4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.tertiaryFixed(), this.tertiaryFixedDim(), 10, "lighter", !0)
      });
    }
    onTertiaryFixed() {
      return DynamicColor.fromPalette({
        name: "on_tertiary_fixed",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? 100 : 10,
        background: (s) => this.tertiaryFixedDim(),
        secondBackground: (s) => this.tertiaryFixed(),
        contrastCurve: (s) => new ContrastCurve(4.5, 7, 11, 21)
      });
    }
    onTertiaryFixedVariant() {
      return DynamicColor.fromPalette({
        name: "on_tertiary_fixed_variant",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => isMonochrome(s) ? 90 : 30,
        background: (s) => this.tertiaryFixedDim(),
        secondBackground: (s) => this.tertiaryFixed(),
        contrastCurve: (s) => new ContrastCurve(3, 4.5, 7, 11)
      });
    }
    ////////////////////////////////////////////////////////////////
    // Other                                                      //
    ////////////////////////////////////////////////////////////////
    highestSurface(s) {
      return s.isDark ? this.surfaceBright() : this.surfaceDim();
    }
  };

  // node_modules/@material/material-color-utilities/dynamiccolor/color_spec_2025.js
  function tMaxC(palette, lowerBound = 0, upperBound = 100, chromaMultiplier = 1) {
    let answer = findBestToneForChroma(palette.hue, palette.chroma * chromaMultiplier, 100, !0);
    return clampDouble(lowerBound, upperBound, answer);
  }
  function tMinC(palette, lowerBound = 0, upperBound = 100) {
    let answer = findBestToneForChroma(palette.hue, palette.chroma, 0, !1);
    return clampDouble(lowerBound, upperBound, answer);
  }
  function findBestToneForChroma(hue, chroma, tone, byDecreasingTone) {
    let answer = tone, bestCandidate = Hct.from(hue, chroma, answer);
    for (; bestCandidate.chroma < chroma && !(tone < 0 || tone > 100); ) {
      tone += byDecreasingTone ? -1 : 1;
      let newCandidate = Hct.from(hue, chroma, tone);
      bestCandidate.chroma < newCandidate.chroma && (bestCandidate = newCandidate, answer = tone);
    }
    return answer;
  }
  function getCurve(defaultContrast) {
    return defaultContrast === 1.5 ? new ContrastCurve(1.5, 1.5, 3, 5.5) : defaultContrast === 3 ? new ContrastCurve(3, 3, 4.5, 7) : defaultContrast === 4.5 ? new ContrastCurve(4.5, 4.5, 7, 11) : defaultContrast === 6 ? new ContrastCurve(6, 6, 7, 11) : defaultContrast === 7 ? new ContrastCurve(7, 7, 11, 21) : defaultContrast === 9 ? new ContrastCurve(9, 9, 11, 21) : defaultContrast === 11 ? new ContrastCurve(11, 11, 21, 21) : defaultContrast === 21 ? new ContrastCurve(21, 21, 21, 21) : new ContrastCurve(defaultContrast, defaultContrast, 7, 21);
  }
  var ColorSpecDelegateImpl2025 = class extends ColorSpecDelegateImpl2021 {
    ////////////////////////////////////////////////////////////////
    // Surfaces [S]                                               //
    ////////////////////////////////////////////////////////////////
    surface() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => (super.surface().tone(s), s.platform === "phone" ? s.isDark ? 4 : Hct.isYellow(s.neutralPalette.hue) ? 99 : s.variant === Variant.VIBRANT ? 97 : 98 : 0),
        isBackground: !0
      });
      return extendSpecVersion(super.surface(), "2025", color2025);
    }
    surfaceDim() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_dim",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 4 : Hct.isYellow(s.neutralPalette.hue) ? 90 : s.variant === Variant.VIBRANT ? 85 : 87,
        isBackground: !0,
        chromaMultiplier: (s) => {
          if (!s.isDark) {
            if (s.variant === Variant.NEUTRAL)
              return 2.5;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.7;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? 2.7 : 1.75;
            if (s.variant === Variant.VIBRANT)
              return 1.36;
          }
          return 1;
        }
      });
      return extendSpecVersion(super.surfaceDim(), "2025", color2025);
    }
    surfaceBright() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_bright",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 18 : Hct.isYellow(s.neutralPalette.hue) ? 99 : s.variant === Variant.VIBRANT ? 97 : 98,
        isBackground: !0,
        chromaMultiplier: (s) => {
          if (s.isDark) {
            if (s.variant === Variant.NEUTRAL)
              return 2.5;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.7;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? 2.7 : 1.75;
            if (s.variant === Variant.VIBRANT)
              return 1.36;
          }
          return 1;
        }
      });
      return extendSpecVersion(super.surfaceBright(), "2025", color2025);
    }
    surfaceContainerLowest() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_container_lowest",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 0 : 100,
        isBackground: !0
      });
      return extendSpecVersion(super.surfaceContainerLowest(), "2025", color2025);
    }
    surfaceContainerLow() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_container_low",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.platform === "phone" ? s.isDark ? 6 : Hct.isYellow(s.neutralPalette.hue) ? 98 : s.variant === Variant.VIBRANT ? 95 : 96 : 15,
        isBackground: !0,
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 1.3;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.25;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? 1.3 : 1.15;
            if (s.variant === Variant.VIBRANT)
              return 1.08;
          }
          return 1;
        }
      });
      return extendSpecVersion(super.surfaceContainerLow(), "2025", color2025);
    }
    surfaceContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_container",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.platform === "phone" ? s.isDark ? 9 : Hct.isYellow(s.neutralPalette.hue) ? 96 : s.variant === Variant.VIBRANT ? 92 : 94 : 20,
        isBackground: !0,
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 1.6;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.4;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? 1.6 : 1.3;
            if (s.variant === Variant.VIBRANT)
              return 1.15;
          }
          return 1;
        }
      });
      return extendSpecVersion(super.surfaceContainer(), "2025", color2025);
    }
    surfaceContainerHigh() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_container_high",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.platform === "phone" ? s.isDark ? 12 : Hct.isYellow(s.neutralPalette.hue) ? 94 : s.variant === Variant.VIBRANT ? 90 : 92 : 25,
        isBackground: !0,
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 1.9;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.5;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? 1.95 : 1.45;
            if (s.variant === Variant.VIBRANT)
              return 1.22;
          }
          return 1;
        }
      });
      return extendSpecVersion(super.surfaceContainerHigh(), "2025", color2025);
    }
    surfaceContainerHighest() {
      let color2025 = DynamicColor.fromPalette({
        name: "surface_container_highest",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 15 : Hct.isYellow(s.neutralPalette.hue) ? 92 : s.variant === Variant.VIBRANT ? 88 : 90,
        isBackground: !0,
        chromaMultiplier: (s) => s.variant === Variant.NEUTRAL ? 2.2 : s.variant === Variant.TONAL_SPOT ? 1.7 : s.variant === Variant.EXPRESSIVE ? Hct.isYellow(s.neutralPalette.hue) ? 2.3 : 1.6 : s.variant === Variant.VIBRANT ? 1.29 : 1
      });
      return extendSpecVersion(super.surfaceContainerHighest(), "2025", color2025);
    }
    onSurface() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.variant === Variant.VIBRANT ? tMaxC(s.neutralPalette, 0, 100, 1.1) : DynamicColor.getInitialToneFromBackground((s2) => s2.platform === "phone" ? this.highestSurface(s2) : this.surfaceContainerHigh())(s),
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 2.2;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.7;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? s.isDark ? 3 : 2.3 : 1.6;
          }
          return 1;
        },
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.isDark && s.platform === "phone" ? getCurve(11) : getCurve(9)
      });
      return extendSpecVersion(super.onSurface(), "2025", color2025);
    }
    onSurfaceVariant() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_surface_variant",
        palette: (s) => s.neutralPalette,
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 2.2;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.7;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? s.isDark ? 3 : 2.3 : 1.6;
          }
          return 1;
        },
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? s.isDark ? getCurve(6) : getCurve(4.5) : getCurve(7)
      });
      return extendSpecVersion(super.onSurfaceVariant(), "2025", color2025);
    }
    outline() {
      let color2025 = DynamicColor.fromPalette({
        name: "outline",
        palette: (s) => s.neutralPalette,
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 2.2;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.7;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? s.isDark ? 3 : 2.3 : 1.6;
          }
          return 1;
        },
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(3) : getCurve(4.5)
      });
      return extendSpecVersion(super.outline(), "2025", color2025);
    }
    outlineVariant() {
      let color2025 = DynamicColor.fromPalette({
        name: "outline_variant",
        palette: (s) => s.neutralPalette,
        chromaMultiplier: (s) => {
          if (s.platform === "phone") {
            if (s.variant === Variant.NEUTRAL)
              return 2.2;
            if (s.variant === Variant.TONAL_SPOT)
              return 1.7;
            if (s.variant === Variant.EXPRESSIVE)
              return Hct.isYellow(s.neutralPalette.hue) ? s.isDark ? 3 : 2.3 : 1.6;
          }
          return 1;
        },
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(1.5) : getCurve(3)
      });
      return extendSpecVersion(super.outlineVariant(), "2025", color2025);
    }
    inverseSurface() {
      let color2025 = DynamicColor.fromPalette({
        name: "inverse_surface",
        palette: (s) => s.neutralPalette,
        tone: (s) => s.isDark ? 98 : 4,
        isBackground: !0
      });
      return extendSpecVersion(super.inverseSurface(), "2025", color2025);
    }
    inverseOnSurface() {
      let color2025 = DynamicColor.fromPalette({
        name: "inverse_on_surface",
        palette: (s) => s.neutralPalette,
        background: (s) => this.inverseSurface(),
        contrastCurve: (s) => getCurve(7)
      });
      return extendSpecVersion(super.inverseOnSurface(), "2025", color2025);
    }
    ////////////////////////////////////////////////////////////////
    // Primaries [P]                                              //
    ////////////////////////////////////////////////////////////////
    primary() {
      let color2025 = DynamicColor.fromPalette({
        name: "primary",
        palette: (s) => s.primaryPalette,
        tone: (s) => s.variant === Variant.NEUTRAL ? s.platform === "phone" ? s.isDark ? 80 : 40 : 90 : s.variant === Variant.TONAL_SPOT ? s.platform === "phone" ? s.isDark ? 80 : tMaxC(s.primaryPalette) : tMaxC(s.primaryPalette, 0, 90) : s.variant === Variant.EXPRESSIVE ? s.platform === "phone" ? tMaxC(s.primaryPalette, 0, Hct.isYellow(s.primaryPalette.hue) ? 25 : Hct.isCyan(s.primaryPalette.hue) ? 88 : 98) : tMaxC(s.primaryPalette) : s.platform === "phone" ? tMaxC(s.primaryPalette, 0, Hct.isCyan(s.primaryPalette.hue) ? 88 : 98) : tMaxC(s.primaryPalette),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(4.5) : getCurve(7),
        toneDeltaPair: (s) => s.platform === "phone" ? new ToneDeltaPair(this.primaryContainer(), this.primary(), 5, "relative_lighter", !0, "farther") : void 0
      });
      return extendSpecVersion(super.primary(), "2025", color2025);
    }
    primaryDim() {
      return DynamicColor.fromPalette({
        name: "primary_dim",
        palette: (s) => s.primaryPalette,
        tone: (s) => s.variant === Variant.NEUTRAL ? 85 : s.variant === Variant.TONAL_SPOT ? tMaxC(s.primaryPalette, 0, 90) : tMaxC(s.primaryPalette),
        isBackground: !0,
        background: (s) => this.surfaceContainerHigh(),
        contrastCurve: (s) => getCurve(4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.primaryDim(), this.primary(), 5, "darker", !0, "farther")
      });
    }
    onPrimary() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_primary",
        palette: (s) => s.primaryPalette,
        background: (s) => s.platform === "phone" ? this.primary() : this.primaryDim(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onPrimary(), "2025", color2025);
    }
    primaryContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "primary_container",
        palette: (s) => s.primaryPalette,
        tone: (s) => s.platform === "watch" ? 30 : s.variant === Variant.NEUTRAL ? s.isDark ? 30 : 90 : s.variant === Variant.TONAL_SPOT ? s.isDark ? tMinC(s.primaryPalette, 35, 93) : tMaxC(s.primaryPalette, 0, 90) : s.variant === Variant.EXPRESSIVE ? s.isDark ? tMaxC(s.primaryPalette, 30, 93) : tMaxC(s.primaryPalette, 78, Hct.isCyan(s.primaryPalette.hue) ? 88 : 90) : s.isDark ? tMinC(s.primaryPalette, 66, 93) : tMaxC(s.primaryPalette, 66, Hct.isCyan(s.primaryPalette.hue) ? 88 : 93),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        toneDeltaPair: (s) => s.platform === "phone" ? void 0 : new ToneDeltaPair(this.primaryContainer(), this.primaryDim(), 10, "darker", !0, "farther"),
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.primaryContainer(), "2025", color2025);
    }
    onPrimaryContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_primary_container",
        palette: (s) => s.primaryPalette,
        background: (s) => this.primaryContainer(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onPrimaryContainer(), "2025", color2025);
    }
    primaryFixed() {
      let color2025 = DynamicColor.fromPalette({
        name: "primary_fixed",
        palette: (s) => s.primaryPalette,
        tone: (s) => {
          let tempS = Object.assign({}, s, { isDark: !1, contrastLevel: 0 });
          return this.primaryContainer().getTone(tempS);
        },
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.primaryFixed(), "2025", color2025);
    }
    primaryFixedDim() {
      let color2025 = DynamicColor.fromPalette({
        name: "primary_fixed_dim",
        palette: (s) => s.primaryPalette,
        tone: (s) => this.primaryFixed().getTone(s),
        isBackground: !0,
        toneDeltaPair: (s) => new ToneDeltaPair(this.primaryFixedDim(), this.primaryFixed(), 5, "darker", !0, "exact")
      });
      return extendSpecVersion(super.primaryFixedDim(), "2025", color2025);
    }
    onPrimaryFixed() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_primary_fixed",
        palette: (s) => s.primaryPalette,
        background: (s) => this.primaryFixedDim(),
        contrastCurve: (s) => getCurve(7)
      });
      return extendSpecVersion(super.onPrimaryFixed(), "2025", color2025);
    }
    onPrimaryFixedVariant() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_primary_fixed_variant",
        palette: (s) => s.primaryPalette,
        background: (s) => this.primaryFixedDim(),
        contrastCurve: (s) => getCurve(4.5)
      });
      return extendSpecVersion(super.onPrimaryFixedVariant(), "2025", color2025);
    }
    inversePrimary() {
      let color2025 = DynamicColor.fromPalette({
        name: "inverse_primary",
        palette: (s) => s.primaryPalette,
        tone: (s) => tMaxC(s.primaryPalette),
        background: (s) => this.inverseSurface(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.inversePrimary(), "2025", color2025);
    }
    ////////////////////////////////////////////////////////////////
    // Secondaries [Q]                                            //
    ////////////////////////////////////////////////////////////////
    secondary() {
      let color2025 = DynamicColor.fromPalette({
        name: "secondary",
        palette: (s) => s.secondaryPalette,
        tone: (s) => s.platform === "watch" ? s.variant === Variant.NEUTRAL ? 90 : tMaxC(s.secondaryPalette, 0, 90) : s.variant === Variant.NEUTRAL ? s.isDark ? tMinC(s.secondaryPalette, 0, 98) : tMaxC(s.secondaryPalette) : s.variant === Variant.VIBRANT ? tMaxC(s.secondaryPalette, 0, s.isDark ? 90 : 98) : s.isDark ? 80 : tMaxC(s.secondaryPalette),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(4.5) : getCurve(7),
        toneDeltaPair: (s) => s.platform === "phone" ? new ToneDeltaPair(this.secondaryContainer(), this.secondary(), 5, "relative_lighter", !0, "farther") : void 0
      });
      return extendSpecVersion(super.secondary(), "2025", color2025);
    }
    secondaryDim() {
      return DynamicColor.fromPalette({
        name: "secondary_dim",
        palette: (s) => s.secondaryPalette,
        tone: (s) => s.variant === Variant.NEUTRAL ? 85 : tMaxC(s.secondaryPalette, 0, 90),
        isBackground: !0,
        background: (s) => this.surfaceContainerHigh(),
        contrastCurve: (s) => getCurve(4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.secondaryDim(), this.secondary(), 5, "darker", !0, "farther")
      });
    }
    onSecondary() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_secondary",
        palette: (s) => s.secondaryPalette,
        background: (s) => s.platform === "phone" ? this.secondary() : this.secondaryDim(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onSecondary(), "2025", color2025);
    }
    secondaryContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "secondary_container",
        palette: (s) => s.secondaryPalette,
        tone: (s) => s.platform === "watch" ? 30 : s.variant === Variant.VIBRANT ? s.isDark ? tMinC(s.secondaryPalette, 30, 40) : tMaxC(s.secondaryPalette, 84, 90) : s.variant === Variant.EXPRESSIVE ? s.isDark ? 15 : tMaxC(s.secondaryPalette, 90, 95) : s.isDark ? 25 : 90,
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        toneDeltaPair: (s) => s.platform === "watch" ? new ToneDeltaPair(this.secondaryContainer(), this.secondaryDim(), 10, "darker", !0, "farther") : void 0,
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.secondaryContainer(), "2025", color2025);
    }
    onSecondaryContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_secondary_container",
        palette: (s) => s.secondaryPalette,
        background: (s) => this.secondaryContainer(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onSecondaryContainer(), "2025", color2025);
    }
    secondaryFixed() {
      let color2025 = DynamicColor.fromPalette({
        name: "secondary_fixed",
        palette: (s) => s.secondaryPalette,
        tone: (s) => {
          let tempS = Object.assign({}, s, { isDark: !1, contrastLevel: 0 });
          return this.secondaryContainer().getTone(tempS);
        },
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.secondaryFixed(), "2025", color2025);
    }
    secondaryFixedDim() {
      let color2025 = DynamicColor.fromPalette({
        name: "secondary_fixed_dim",
        palette: (s) => s.secondaryPalette,
        tone: (s) => this.secondaryFixed().getTone(s),
        isBackground: !0,
        toneDeltaPair: (s) => new ToneDeltaPair(this.secondaryFixedDim(), this.secondaryFixed(), 5, "darker", !0, "exact")
      });
      return extendSpecVersion(super.secondaryFixedDim(), "2025", color2025);
    }
    onSecondaryFixed() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_secondary_fixed",
        palette: (s) => s.secondaryPalette,
        background: (s) => this.secondaryFixedDim(),
        contrastCurve: (s) => getCurve(7)
      });
      return extendSpecVersion(super.onSecondaryFixed(), "2025", color2025);
    }
    onSecondaryFixedVariant() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_secondary_fixed_variant",
        palette: (s) => s.secondaryPalette,
        background: (s) => this.secondaryFixedDim(),
        contrastCurve: (s) => getCurve(4.5)
      });
      return extendSpecVersion(super.onSecondaryFixedVariant(), "2025", color2025);
    }
    ////////////////////////////////////////////////////////////////
    // Tertiaries [T]                                             //
    ////////////////////////////////////////////////////////////////
    tertiary() {
      let color2025 = DynamicColor.fromPalette({
        name: "tertiary",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => s.platform === "watch" ? s.variant === Variant.TONAL_SPOT ? tMaxC(s.tertiaryPalette, 0, 90) : tMaxC(s.tertiaryPalette) : s.variant === Variant.EXPRESSIVE || s.variant === Variant.VIBRANT ? tMaxC(s.tertiaryPalette, 0, Hct.isCyan(s.tertiaryPalette.hue) ? 88 : s.isDark ? 98 : 100) : s.isDark ? tMaxC(s.tertiaryPalette, 0, 98) : tMaxC(s.tertiaryPalette),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(4.5) : getCurve(7),
        toneDeltaPair: (s) => s.platform === "phone" ? new ToneDeltaPair(this.tertiaryContainer(), this.tertiary(), 5, "relative_lighter", !0, "farther") : void 0
      });
      return extendSpecVersion(super.tertiary(), "2025", color2025);
    }
    tertiaryDim() {
      return DynamicColor.fromPalette({
        name: "tertiary_dim",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => s.variant === Variant.TONAL_SPOT ? tMaxC(s.tertiaryPalette, 0, 90) : tMaxC(s.tertiaryPalette),
        isBackground: !0,
        background: (s) => this.surfaceContainerHigh(),
        contrastCurve: (s) => getCurve(4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.tertiaryDim(), this.tertiary(), 5, "darker", !0, "farther")
      });
    }
    onTertiary() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_tertiary",
        palette: (s) => s.tertiaryPalette,
        background: (s) => s.platform === "phone" ? this.tertiary() : this.tertiaryDim(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onTertiary(), "2025", color2025);
    }
    tertiaryContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "tertiary_container",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => s.platform === "watch" ? s.variant === Variant.TONAL_SPOT ? tMaxC(s.tertiaryPalette, 0, 90) : tMaxC(s.tertiaryPalette) : s.variant === Variant.NEUTRAL ? s.isDark ? tMaxC(s.tertiaryPalette, 0, 93) : tMaxC(s.tertiaryPalette, 0, 96) : s.variant === Variant.TONAL_SPOT ? tMaxC(s.tertiaryPalette, 0, s.isDark ? 93 : 100) : s.variant === Variant.EXPRESSIVE ? tMaxC(s.tertiaryPalette, 75, Hct.isCyan(s.tertiaryPalette.hue) ? 88 : s.isDark ? 93 : 100) : s.isDark ? tMaxC(s.tertiaryPalette, 0, 93) : tMaxC(s.tertiaryPalette, 72, 100),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        toneDeltaPair: (s) => s.platform === "watch" ? new ToneDeltaPair(this.tertiaryContainer(), this.tertiaryDim(), 10, "darker", !0, "farther") : void 0,
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.tertiaryContainer(), "2025", color2025);
    }
    onTertiaryContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_tertiary_container",
        palette: (s) => s.tertiaryPalette,
        background: (s) => this.tertiaryContainer(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onTertiaryContainer(), "2025", color2025);
    }
    tertiaryFixed() {
      let color2025 = DynamicColor.fromPalette({
        name: "tertiary_fixed",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => {
          let tempS = Object.assign({}, s, { isDark: !1, contrastLevel: 0 });
          return this.tertiaryContainer().getTone(tempS);
        },
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.tertiaryFixed(), "2025", color2025);
    }
    tertiaryFixedDim() {
      let color2025 = DynamicColor.fromPalette({
        name: "tertiary_fixed_dim",
        palette: (s) => s.tertiaryPalette,
        tone: (s) => this.tertiaryFixed().getTone(s),
        isBackground: !0,
        toneDeltaPair: (s) => new ToneDeltaPair(this.tertiaryFixedDim(), this.tertiaryFixed(), 5, "darker", !0, "exact")
      });
      return extendSpecVersion(super.tertiaryFixedDim(), "2025", color2025);
    }
    onTertiaryFixed() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_tertiary_fixed",
        palette: (s) => s.tertiaryPalette,
        background: (s) => this.tertiaryFixedDim(),
        contrastCurve: (s) => getCurve(7)
      });
      return extendSpecVersion(super.onTertiaryFixed(), "2025", color2025);
    }
    onTertiaryFixedVariant() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_tertiary_fixed_variant",
        palette: (s) => s.tertiaryPalette,
        background: (s) => this.tertiaryFixedDim(),
        contrastCurve: (s) => getCurve(4.5)
      });
      return extendSpecVersion(super.onTertiaryFixedVariant(), "2025", color2025);
    }
    ////////////////////////////////////////////////////////////////
    // Errors [E]                                                 //
    ////////////////////////////////////////////////////////////////
    error() {
      let color2025 = DynamicColor.fromPalette({
        name: "error",
        palette: (s) => s.errorPalette,
        tone: (s) => s.platform === "phone" ? s.isDark ? tMinC(s.errorPalette, 0, 98) : tMaxC(s.errorPalette) : tMinC(s.errorPalette),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : this.surfaceContainerHigh(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(4.5) : getCurve(7),
        toneDeltaPair: (s) => s.platform === "phone" ? new ToneDeltaPair(this.errorContainer(), this.error(), 5, "relative_lighter", !0, "farther") : void 0
      });
      return extendSpecVersion(super.error(), "2025", color2025);
    }
    errorDim() {
      return DynamicColor.fromPalette({
        name: "error_dim",
        palette: (s) => s.errorPalette,
        tone: (s) => tMinC(s.errorPalette),
        isBackground: !0,
        background: (s) => this.surfaceContainerHigh(),
        contrastCurve: (s) => getCurve(4.5),
        toneDeltaPair: (s) => new ToneDeltaPair(this.errorDim(), this.error(), 5, "darker", !0, "farther")
      });
    }
    onError() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_error",
        palette: (s) => s.errorPalette,
        background: (s) => s.platform === "phone" ? this.error() : this.errorDim(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(6) : getCurve(7)
      });
      return extendSpecVersion(super.onError(), "2025", color2025);
    }
    errorContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "error_container",
        palette: (s) => s.errorPalette,
        tone: (s) => s.platform === "watch" ? 30 : s.isDark ? tMinC(s.errorPalette, 30, 93) : tMaxC(s.errorPalette, 0, 90),
        isBackground: !0,
        background: (s) => s.platform === "phone" ? this.highestSurface(s) : void 0,
        toneDeltaPair: (s) => s.platform === "watch" ? new ToneDeltaPair(this.errorContainer(), this.errorDim(), 10, "darker", !0, "farther") : void 0,
        contrastCurve: (s) => s.platform === "phone" && s.contrastLevel > 0 ? getCurve(1.5) : void 0
      });
      return extendSpecVersion(super.errorContainer(), "2025", color2025);
    }
    onErrorContainer() {
      let color2025 = DynamicColor.fromPalette({
        name: "on_error_container",
        palette: (s) => s.errorPalette,
        background: (s) => this.errorContainer(),
        contrastCurve: (s) => s.platform === "phone" ? getCurve(4.5) : getCurve(7)
      });
      return extendSpecVersion(super.onErrorContainer(), "2025", color2025);
    }
    /////////////////////////////////////////////////////////////////
    // Remapped Colors                                             //
    /////////////////////////////////////////////////////////////////
    surfaceVariant() {
      let color2025 = Object.assign(this.surfaceContainerHighest().clone(), { name: "surface_variant" });
      return extendSpecVersion(super.surfaceVariant(), "2025", color2025);
    }
    surfaceTint() {
      let color2025 = Object.assign(this.primary().clone(), { name: "surface_tint" });
      return extendSpecVersion(super.surfaceTint(), "2025", color2025);
    }
    background() {
      let color2025 = Object.assign(this.surface().clone(), { name: "background" });
      return extendSpecVersion(super.background(), "2025", color2025);
    }
    onBackground() {
      let color2025 = Object.assign(this.onSurface().clone(), {
        name: "on_background",
        tone: (s) => s.platform === "watch" ? 100 : this.onSurface().getTone(s)
      });
      return extendSpecVersion(super.onBackground(), "2025", color2025);
    }
  };

  // node_modules/@material/material-color-utilities/dynamiccolor/material_dynamic_colors.js
  var MaterialDynamicColors = class _MaterialDynamicColors {
    constructor() {
      this.allColors = [
        this.background(),
        this.onBackground(),
        this.surface(),
        this.surfaceDim(),
        this.surfaceBright(),
        this.surfaceContainerLowest(),
        this.surfaceContainerLow(),
        this.surfaceContainer(),
        this.surfaceContainerHigh(),
        this.surfaceContainerHighest(),
        this.onSurface(),
        this.onSurfaceVariant(),
        this.outline(),
        this.outlineVariant(),
        this.inverseSurface(),
        this.inverseOnSurface(),
        this.primary(),
        this.primaryDim(),
        this.onPrimary(),
        this.primaryContainer(),
        this.onPrimaryContainer(),
        this.primaryFixed(),
        this.primaryFixedDim(),
        this.onPrimaryFixed(),
        this.onPrimaryFixedVariant(),
        this.inversePrimary(),
        this.secondary(),
        this.secondaryDim(),
        this.onSecondary(),
        this.secondaryContainer(),
        this.onSecondaryContainer(),
        this.secondaryFixed(),
        this.secondaryFixedDim(),
        this.onSecondaryFixed(),
        this.onSecondaryFixedVariant(),
        this.tertiary(),
        this.tertiaryDim(),
        this.onTertiary(),
        this.tertiaryContainer(),
        this.onTertiaryContainer(),
        this.tertiaryFixed(),
        this.tertiaryFixedDim(),
        this.onTertiaryFixed(),
        this.onTertiaryFixedVariant(),
        this.error(),
        this.errorDim(),
        this.onError(),
        this.errorContainer(),
        this.onErrorContainer()
      ].filter((c) => c !== void 0);
    }
    highestSurface(s) {
      return _MaterialDynamicColors.colorSpec.highestSurface(s);
    }
    ////////////////////////////////////////////////////////////////
    // Main Palettes                                              //
    ////////////////////////////////////////////////////////////////
    primaryPaletteKeyColor() {
      return _MaterialDynamicColors.colorSpec.primaryPaletteKeyColor();
    }
    secondaryPaletteKeyColor() {
      return _MaterialDynamicColors.colorSpec.secondaryPaletteKeyColor();
    }
    tertiaryPaletteKeyColor() {
      return _MaterialDynamicColors.colorSpec.tertiaryPaletteKeyColor();
    }
    neutralPaletteKeyColor() {
      return _MaterialDynamicColors.colorSpec.neutralPaletteKeyColor();
    }
    neutralVariantPaletteKeyColor() {
      return _MaterialDynamicColors.colorSpec.neutralVariantPaletteKeyColor();
    }
    errorPaletteKeyColor() {
      return _MaterialDynamicColors.colorSpec.errorPaletteKeyColor();
    }
    ////////////////////////////////////////////////////////////////
    // Surfaces [S]                                               //
    ////////////////////////////////////////////////////////////////
    background() {
      return _MaterialDynamicColors.colorSpec.background();
    }
    onBackground() {
      return _MaterialDynamicColors.colorSpec.onBackground();
    }
    surface() {
      return _MaterialDynamicColors.colorSpec.surface();
    }
    surfaceDim() {
      return _MaterialDynamicColors.colorSpec.surfaceDim();
    }
    surfaceBright() {
      return _MaterialDynamicColors.colorSpec.surfaceBright();
    }
    surfaceContainerLowest() {
      return _MaterialDynamicColors.colorSpec.surfaceContainerLowest();
    }
    surfaceContainerLow() {
      return _MaterialDynamicColors.colorSpec.surfaceContainerLow();
    }
    surfaceContainer() {
      return _MaterialDynamicColors.colorSpec.surfaceContainer();
    }
    surfaceContainerHigh() {
      return _MaterialDynamicColors.colorSpec.surfaceContainerHigh();
    }
    surfaceContainerHighest() {
      return _MaterialDynamicColors.colorSpec.surfaceContainerHighest();
    }
    onSurface() {
      return _MaterialDynamicColors.colorSpec.onSurface();
    }
    surfaceVariant() {
      return _MaterialDynamicColors.colorSpec.surfaceVariant();
    }
    onSurfaceVariant() {
      return _MaterialDynamicColors.colorSpec.onSurfaceVariant();
    }
    outline() {
      return _MaterialDynamicColors.colorSpec.outline();
    }
    outlineVariant() {
      return _MaterialDynamicColors.colorSpec.outlineVariant();
    }
    inverseSurface() {
      return _MaterialDynamicColors.colorSpec.inverseSurface();
    }
    inverseOnSurface() {
      return _MaterialDynamicColors.colorSpec.inverseOnSurface();
    }
    shadow() {
      return _MaterialDynamicColors.colorSpec.shadow();
    }
    scrim() {
      return _MaterialDynamicColors.colorSpec.scrim();
    }
    surfaceTint() {
      return _MaterialDynamicColors.colorSpec.surfaceTint();
    }
    ////////////////////////////////////////////////////////////////
    // Primaries [P]                                              //
    ////////////////////////////////////////////////////////////////
    primary() {
      return _MaterialDynamicColors.colorSpec.primary();
    }
    primaryDim() {
      return _MaterialDynamicColors.colorSpec.primaryDim();
    }
    onPrimary() {
      return _MaterialDynamicColors.colorSpec.onPrimary();
    }
    primaryContainer() {
      return _MaterialDynamicColors.colorSpec.primaryContainer();
    }
    onPrimaryContainer() {
      return _MaterialDynamicColors.colorSpec.onPrimaryContainer();
    }
    inversePrimary() {
      return _MaterialDynamicColors.colorSpec.inversePrimary();
    }
    /////////////////////////////////////////////////////////////////
    // Primary Fixed [PF]                                          //
    /////////////////////////////////////////////////////////////////
    primaryFixed() {
      return _MaterialDynamicColors.colorSpec.primaryFixed();
    }
    primaryFixedDim() {
      return _MaterialDynamicColors.colorSpec.primaryFixedDim();
    }
    onPrimaryFixed() {
      return _MaterialDynamicColors.colorSpec.onPrimaryFixed();
    }
    onPrimaryFixedVariant() {
      return _MaterialDynamicColors.colorSpec.onPrimaryFixedVariant();
    }
    ////////////////////////////////////////////////////////////////
    // Secondaries [Q]                                            //
    ////////////////////////////////////////////////////////////////
    secondary() {
      return _MaterialDynamicColors.colorSpec.secondary();
    }
    secondaryDim() {
      return _MaterialDynamicColors.colorSpec.secondaryDim();
    }
    onSecondary() {
      return _MaterialDynamicColors.colorSpec.onSecondary();
    }
    secondaryContainer() {
      return _MaterialDynamicColors.colorSpec.secondaryContainer();
    }
    onSecondaryContainer() {
      return _MaterialDynamicColors.colorSpec.onSecondaryContainer();
    }
    /////////////////////////////////////////////////////////////////
    // Secondary Fixed [QF]                                        //
    /////////////////////////////////////////////////////////////////
    secondaryFixed() {
      return _MaterialDynamicColors.colorSpec.secondaryFixed();
    }
    secondaryFixedDim() {
      return _MaterialDynamicColors.colorSpec.secondaryFixedDim();
    }
    onSecondaryFixed() {
      return _MaterialDynamicColors.colorSpec.onSecondaryFixed();
    }
    onSecondaryFixedVariant() {
      return _MaterialDynamicColors.colorSpec.onSecondaryFixedVariant();
    }
    ////////////////////////////////////////////////////////////////
    // Tertiaries [T]                                             //
    ////////////////////////////////////////////////////////////////
    tertiary() {
      return _MaterialDynamicColors.colorSpec.tertiary();
    }
    tertiaryDim() {
      return _MaterialDynamicColors.colorSpec.tertiaryDim();
    }
    onTertiary() {
      return _MaterialDynamicColors.colorSpec.onTertiary();
    }
    tertiaryContainer() {
      return _MaterialDynamicColors.colorSpec.tertiaryContainer();
    }
    onTertiaryContainer() {
      return _MaterialDynamicColors.colorSpec.onTertiaryContainer();
    }
    /////////////////////////////////////////////////////////////////
    // Tertiary Fixed [TF]                                         //
    /////////////////////////////////////////////////////////////////
    tertiaryFixed() {
      return _MaterialDynamicColors.colorSpec.tertiaryFixed();
    }
    tertiaryFixedDim() {
      return _MaterialDynamicColors.colorSpec.tertiaryFixedDim();
    }
    onTertiaryFixed() {
      return _MaterialDynamicColors.colorSpec.onTertiaryFixed();
    }
    onTertiaryFixedVariant() {
      return _MaterialDynamicColors.colorSpec.onTertiaryFixedVariant();
    }
    ////////////////////////////////////////////////////////////////
    // Errors [E]                                                 //
    ////////////////////////////////////////////////////////////////
    error() {
      return _MaterialDynamicColors.colorSpec.error();
    }
    errorDim() {
      return _MaterialDynamicColors.colorSpec.errorDim();
    }
    onError() {
      return _MaterialDynamicColors.colorSpec.onError();
    }
    errorContainer() {
      return _MaterialDynamicColors.colorSpec.errorContainer();
    }
    onErrorContainer() {
      return _MaterialDynamicColors.colorSpec.onErrorContainer();
    }
    // Static variables are deprecated. Use the instance methods to get correct
    // specs based on request.
    /** @deprecated Use highestSurface() instead. */
    static highestSurface(s) {
      return _MaterialDynamicColors.colorSpec.highestSurface(s);
    }
  };
  MaterialDynamicColors.contentAccentToneDelta = 15;
  MaterialDynamicColors.colorSpec = new ColorSpecDelegateImpl2025();
  MaterialDynamicColors.primaryPaletteKeyColor = MaterialDynamicColors.colorSpec.primaryPaletteKeyColor();
  MaterialDynamicColors.secondaryPaletteKeyColor = MaterialDynamicColors.colorSpec.secondaryPaletteKeyColor();
  MaterialDynamicColors.tertiaryPaletteKeyColor = MaterialDynamicColors.colorSpec.tertiaryPaletteKeyColor();
  MaterialDynamicColors.neutralPaletteKeyColor = MaterialDynamicColors.colorSpec.neutralPaletteKeyColor();
  MaterialDynamicColors.neutralVariantPaletteKeyColor = MaterialDynamicColors.colorSpec.neutralVariantPaletteKeyColor();
  MaterialDynamicColors.background = MaterialDynamicColors.colorSpec.background();
  MaterialDynamicColors.onBackground = MaterialDynamicColors.colorSpec.onBackground();
  MaterialDynamicColors.surface = MaterialDynamicColors.colorSpec.surface();
  MaterialDynamicColors.surfaceDim = MaterialDynamicColors.colorSpec.surfaceDim();
  MaterialDynamicColors.surfaceBright = MaterialDynamicColors.colorSpec.surfaceBright();
  MaterialDynamicColors.surfaceContainerLowest = MaterialDynamicColors.colorSpec.surfaceContainerLowest();
  MaterialDynamicColors.surfaceContainerLow = MaterialDynamicColors.colorSpec.surfaceContainerLow();
  MaterialDynamicColors.surfaceContainer = MaterialDynamicColors.colorSpec.surfaceContainer();
  MaterialDynamicColors.surfaceContainerHigh = MaterialDynamicColors.colorSpec.surfaceContainerHigh();
  MaterialDynamicColors.surfaceContainerHighest = MaterialDynamicColors.colorSpec.surfaceContainerHighest();
  MaterialDynamicColors.onSurface = MaterialDynamicColors.colorSpec.onSurface();
  MaterialDynamicColors.surfaceVariant = MaterialDynamicColors.colorSpec.surfaceVariant();
  MaterialDynamicColors.onSurfaceVariant = MaterialDynamicColors.colorSpec.onSurfaceVariant();
  MaterialDynamicColors.inverseSurface = MaterialDynamicColors.colorSpec.inverseSurface();
  MaterialDynamicColors.inverseOnSurface = MaterialDynamicColors.colorSpec.inverseOnSurface();
  MaterialDynamicColors.outline = MaterialDynamicColors.colorSpec.outline();
  MaterialDynamicColors.outlineVariant = MaterialDynamicColors.colorSpec.outlineVariant();
  MaterialDynamicColors.shadow = MaterialDynamicColors.colorSpec.shadow();
  MaterialDynamicColors.scrim = MaterialDynamicColors.colorSpec.scrim();
  MaterialDynamicColors.surfaceTint = MaterialDynamicColors.colorSpec.surfaceTint();
  MaterialDynamicColors.primary = MaterialDynamicColors.colorSpec.primary();
  MaterialDynamicColors.onPrimary = MaterialDynamicColors.colorSpec.onPrimary();
  MaterialDynamicColors.primaryContainer = MaterialDynamicColors.colorSpec.primaryContainer();
  MaterialDynamicColors.onPrimaryContainer = MaterialDynamicColors.colorSpec.onPrimaryContainer();
  MaterialDynamicColors.inversePrimary = MaterialDynamicColors.colorSpec.inversePrimary();
  MaterialDynamicColors.secondary = MaterialDynamicColors.colorSpec.secondary();
  MaterialDynamicColors.onSecondary = MaterialDynamicColors.colorSpec.onSecondary();
  MaterialDynamicColors.secondaryContainer = MaterialDynamicColors.colorSpec.secondaryContainer();
  MaterialDynamicColors.onSecondaryContainer = MaterialDynamicColors.colorSpec.onSecondaryContainer();
  MaterialDynamicColors.tertiary = MaterialDynamicColors.colorSpec.tertiary();
  MaterialDynamicColors.onTertiary = MaterialDynamicColors.colorSpec.onTertiary();
  MaterialDynamicColors.tertiaryContainer = MaterialDynamicColors.colorSpec.tertiaryContainer();
  MaterialDynamicColors.onTertiaryContainer = MaterialDynamicColors.colorSpec.onTertiaryContainer();
  MaterialDynamicColors.error = MaterialDynamicColors.colorSpec.error();
  MaterialDynamicColors.onError = MaterialDynamicColors.colorSpec.onError();
  MaterialDynamicColors.errorContainer = MaterialDynamicColors.colorSpec.errorContainer();
  MaterialDynamicColors.onErrorContainer = MaterialDynamicColors.colorSpec.onErrorContainer();
  MaterialDynamicColors.primaryFixed = MaterialDynamicColors.colorSpec.primaryFixed();
  MaterialDynamicColors.primaryFixedDim = MaterialDynamicColors.colorSpec.primaryFixedDim();
  MaterialDynamicColors.onPrimaryFixed = MaterialDynamicColors.colorSpec.onPrimaryFixed();
  MaterialDynamicColors.onPrimaryFixedVariant = MaterialDynamicColors.colorSpec.onPrimaryFixedVariant();
  MaterialDynamicColors.secondaryFixed = MaterialDynamicColors.colorSpec.secondaryFixed();
  MaterialDynamicColors.secondaryFixedDim = MaterialDynamicColors.colorSpec.secondaryFixedDim();
  MaterialDynamicColors.onSecondaryFixed = MaterialDynamicColors.colorSpec.onSecondaryFixed();
  MaterialDynamicColors.onSecondaryFixedVariant = MaterialDynamicColors.colorSpec.onSecondaryFixedVariant();
  MaterialDynamicColors.tertiaryFixed = MaterialDynamicColors.colorSpec.tertiaryFixed();
  MaterialDynamicColors.tertiaryFixedDim = MaterialDynamicColors.colorSpec.tertiaryFixedDim();
  MaterialDynamicColors.onTertiaryFixed = MaterialDynamicColors.colorSpec.onTertiaryFixed();
  MaterialDynamicColors.onTertiaryFixedVariant = MaterialDynamicColors.colorSpec.onTertiaryFixedVariant();

  // node_modules/@material/material-color-utilities/dynamiccolor/dynamic_scheme.js
  var DynamicScheme = class _DynamicScheme {
    static maybeFallbackSpecVersion(specVersion, variant) {
      switch (variant) {
        case Variant.EXPRESSIVE:
        case Variant.VIBRANT:
        case Variant.TONAL_SPOT:
        case Variant.NEUTRAL:
          return specVersion;
        default:
          return "2021";
      }
    }
    constructor(args) {
      var _a, _b, _c, _d, _e, _f, _g, _h, _i;
      this.sourceColorArgb = args.sourceColorHct.toInt(), this.variant = args.variant, this.contrastLevel = args.contrastLevel, this.isDark = args.isDark, this.platform = (_a = args.platform) != null ? _a : "phone", this.specVersion = _DynamicScheme.maybeFallbackSpecVersion((_b = args.specVersion) != null ? _b : "2021", this.variant), this.sourceColorHct = args.sourceColorHct, this.primaryPalette = (_c = args.primaryPalette) != null ? _c : getSpec2(this.specVersion).getPrimaryPalette(this.variant, args.sourceColorHct, this.isDark, this.platform, this.contrastLevel), this.secondaryPalette = (_d = args.secondaryPalette) != null ? _d : getSpec2(this.specVersion).getSecondaryPalette(this.variant, args.sourceColorHct, this.isDark, this.platform, this.contrastLevel), this.tertiaryPalette = (_e = args.tertiaryPalette) != null ? _e : getSpec2(this.specVersion).getTertiaryPalette(this.variant, args.sourceColorHct, this.isDark, this.platform, this.contrastLevel), this.neutralPalette = (_f = args.neutralPalette) != null ? _f : getSpec2(this.specVersion).getNeutralPalette(this.variant, args.sourceColorHct, this.isDark, this.platform, this.contrastLevel), this.neutralVariantPalette = (_g = args.neutralVariantPalette) != null ? _g : getSpec2(this.specVersion).getNeutralVariantPalette(this.variant, args.sourceColorHct, this.isDark, this.platform, this.contrastLevel), this.errorPalette = (_i = (_h = args.errorPalette) != null ? _h : getSpec2(this.specVersion).getErrorPalette(this.variant, args.sourceColorHct, this.isDark, this.platform, this.contrastLevel)) != null ? _i : TonalPalette.fromHueAndChroma(25, 84), this.colors = new MaterialDynamicColors();
    }
    toString() {
      return `Scheme: variant=${Variant[this.variant]}, mode=${this.isDark ? "dark" : "light"}, platform=${this.platform}, contrastLevel=${this.contrastLevel.toFixed(1)}, seed=${this.sourceColorHct.toString()}, specVersion=${this.specVersion}`;
    }
    /**
     * Returns a new hue based on a piecewise function and input color hue.
     *
     * For example, for the following function:
     * result = 26 if 0 <= hue < 101
     * result = 39 if 101 <= hue < 210
     * result = 28 if 210 <= hue < 360
     *
     * call the function as:
     *
     * const hueBreakpoints = [0, 101, 210, 360];
     * const hues = [26, 39, 28];
     * const result = scheme.piecewise(hue, hueBreakpoints, hues);
     *
     * @param sourceColorHct The input value.
     * @param hueBreakpoints The breakpoints, in sorted order. No default lower or
     *     upper bounds are assumed.
     * @param hues The hues that should be applied when source color's hue is >=
     *     the same index in hueBrakpoints array, and < the hue at the next index
     *     in hueBrakpoints array. Otherwise, the source color's hue is returned.
     */
    static getPiecewiseHue(sourceColorHct, hueBreakpoints, hues) {
      let size = Math.min(hueBreakpoints.length - 1, hues.length), sourceHue = sourceColorHct.hue;
      for (let i = 0; i < size; i++)
        if (sourceHue >= hueBreakpoints[i] && sourceHue < hueBreakpoints[i + 1])
          return sanitizeDegreesDouble(hues[i]);
      return sourceHue;
    }
    /**
     * Returns a shifted hue based on a piecewise function and input color hue.
     *
     * For example, for the following function:
     * result = hue + 26 if 0 <= hue < 101
     * result = hue - 39 if 101 <= hue < 210
     * result = hue + 28 if 210 <= hue < 360
     *
     * call the function as:
     *
     * const hueBreakpoints = [0, 101, 210, 360];
     * const hues = [26, -39, 28];
     * const result = scheme.getRotatedHue(hue, hueBreakpoints, hues);
     *
     * @param sourceColorHct the source color of the theme, in HCT.
     * @param hueBreakpoints The "breakpoints", i.e. the hues at which a rotation
     *     should be apply. No default lower or upper bounds are assumed.
     * @param rotations The rotation that should be applied when source color's
     *     hue is >= the same index in hues array, and < the hue at the next
     *     index in hues array. Otherwise, the source color's hue is returned.
     */
    static getRotatedHue(sourceColorHct, hueBreakpoints, rotations) {
      let rotation = _DynamicScheme.getPiecewiseHue(sourceColorHct, hueBreakpoints, rotations);
      return Math.min(hueBreakpoints.length - 1, rotations.length) <= 0 && (rotation = 0), sanitizeDegreesDouble(sourceColorHct.hue + rotation);
    }
    getArgb(dynamicColor) {
      return dynamicColor.getArgb(this);
    }
    getHct(dynamicColor) {
      return dynamicColor.getHct(this);
    }
    // Palette key colors
    get primaryPaletteKeyColor() {
      return this.getArgb(this.colors.primaryPaletteKeyColor());
    }
    get secondaryPaletteKeyColor() {
      return this.getArgb(this.colors.secondaryPaletteKeyColor());
    }
    get tertiaryPaletteKeyColor() {
      return this.getArgb(this.colors.tertiaryPaletteKeyColor());
    }
    get neutralPaletteKeyColor() {
      return this.getArgb(this.colors.neutralPaletteKeyColor());
    }
    get neutralVariantPaletteKeyColor() {
      return this.getArgb(this.colors.neutralVariantPaletteKeyColor());
    }
    get errorPaletteKeyColor() {
      return this.getArgb(this.colors.errorPaletteKeyColor());
    }
    // Surface colors
    get background() {
      return this.getArgb(this.colors.background());
    }
    get onBackground() {
      return this.getArgb(this.colors.onBackground());
    }
    get surface() {
      return this.getArgb(this.colors.surface());
    }
    get surfaceDim() {
      return this.getArgb(this.colors.surfaceDim());
    }
    get surfaceBright() {
      return this.getArgb(this.colors.surfaceBright());
    }
    get surfaceContainerLowest() {
      return this.getArgb(this.colors.surfaceContainerLowest());
    }
    get surfaceContainerLow() {
      return this.getArgb(this.colors.surfaceContainerLow());
    }
    get surfaceContainer() {
      return this.getArgb(this.colors.surfaceContainer());
    }
    get surfaceContainerHigh() {
      return this.getArgb(this.colors.surfaceContainerHigh());
    }
    get surfaceContainerHighest() {
      return this.getArgb(this.colors.surfaceContainerHighest());
    }
    get onSurface() {
      return this.getArgb(this.colors.onSurface());
    }
    get surfaceVariant() {
      return this.getArgb(this.colors.surfaceVariant());
    }
    get onSurfaceVariant() {
      return this.getArgb(this.colors.onSurfaceVariant());
    }
    get inverseSurface() {
      return this.getArgb(this.colors.inverseSurface());
    }
    get inverseOnSurface() {
      return this.getArgb(this.colors.inverseOnSurface());
    }
    get outline() {
      return this.getArgb(this.colors.outline());
    }
    get outlineVariant() {
      return this.getArgb(this.colors.outlineVariant());
    }
    get shadow() {
      return this.getArgb(this.colors.shadow());
    }
    get scrim() {
      return this.getArgb(this.colors.scrim());
    }
    get surfaceTint() {
      return this.getArgb(this.colors.surfaceTint());
    }
    // Primary colors
    get primary() {
      return this.getArgb(this.colors.primary());
    }
    get primaryDim() {
      let primaryDim = this.colors.primaryDim();
      if (primaryDim === void 0)
        throw new Error("`primaryDim` color is undefined prior to 2025 spec.");
      return this.getArgb(primaryDim);
    }
    get onPrimary() {
      return this.getArgb(this.colors.onPrimary());
    }
    get primaryContainer() {
      return this.getArgb(this.colors.primaryContainer());
    }
    get onPrimaryContainer() {
      return this.getArgb(this.colors.onPrimaryContainer());
    }
    get primaryFixed() {
      return this.getArgb(this.colors.primaryFixed());
    }
    get primaryFixedDim() {
      return this.getArgb(this.colors.primaryFixedDim());
    }
    get onPrimaryFixed() {
      return this.getArgb(this.colors.onPrimaryFixed());
    }
    get onPrimaryFixedVariant() {
      return this.getArgb(this.colors.onPrimaryFixedVariant());
    }
    get inversePrimary() {
      return this.getArgb(this.colors.inversePrimary());
    }
    // Secondary colors
    get secondary() {
      return this.getArgb(this.colors.secondary());
    }
    get secondaryDim() {
      let secondaryDim = this.colors.secondaryDim();
      if (secondaryDim === void 0)
        throw new Error("`secondaryDim` color is undefined prior to 2025 spec.");
      return this.getArgb(secondaryDim);
    }
    get onSecondary() {
      return this.getArgb(this.colors.onSecondary());
    }
    get secondaryContainer() {
      return this.getArgb(this.colors.secondaryContainer());
    }
    get onSecondaryContainer() {
      return this.getArgb(this.colors.onSecondaryContainer());
    }
    get secondaryFixed() {
      return this.getArgb(this.colors.secondaryFixed());
    }
    get secondaryFixedDim() {
      return this.getArgb(this.colors.secondaryFixedDim());
    }
    get onSecondaryFixed() {
      return this.getArgb(this.colors.onSecondaryFixed());
    }
    get onSecondaryFixedVariant() {
      return this.getArgb(this.colors.onSecondaryFixedVariant());
    }
    // Tertiary colors
    get tertiary() {
      return this.getArgb(this.colors.tertiary());
    }
    get tertiaryDim() {
      let tertiaryDim = this.colors.tertiaryDim();
      if (tertiaryDim === void 0)
        throw new Error("`tertiaryDim` color is undefined prior to 2025 spec.");
      return this.getArgb(tertiaryDim);
    }
    get onTertiary() {
      return this.getArgb(this.colors.onTertiary());
    }
    get tertiaryContainer() {
      return this.getArgb(this.colors.tertiaryContainer());
    }
    get onTertiaryContainer() {
      return this.getArgb(this.colors.onTertiaryContainer());
    }
    get tertiaryFixed() {
      return this.getArgb(this.colors.tertiaryFixed());
    }
    get tertiaryFixedDim() {
      return this.getArgb(this.colors.tertiaryFixedDim());
    }
    get onTertiaryFixed() {
      return this.getArgb(this.colors.onTertiaryFixed());
    }
    get onTertiaryFixedVariant() {
      return this.getArgb(this.colors.onTertiaryFixedVariant());
    }
    // Error colors
    get error() {
      return this.getArgb(this.colors.error());
    }
    get errorDim() {
      let errorDim = this.colors.errorDim();
      if (errorDim === void 0)
        throw new Error("`errorDim` color is undefined prior to 2025 spec.");
      return this.getArgb(errorDim);
    }
    get onError() {
      return this.getArgb(this.colors.onError());
    }
    get errorContainer() {
      return this.getArgb(this.colors.errorContainer());
    }
    get onErrorContainer() {
      return this.getArgb(this.colors.onErrorContainer());
    }
  };
  DynamicScheme.DEFAULT_SPEC_VERSION = "2021";
  DynamicScheme.DEFAULT_PLATFORM = "phone";
  var DynamicSchemePalettesDelegateImpl2021 = class {
    //////////////////////////////////////////////////////////////////
    // Scheme Palettes                                              //
    //////////////////////////////////////////////////////////////////
    getPrimaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.CONTENT:
        case Variant.FIDELITY:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, sourceColorHct.chroma);
        case Variant.FRUIT_SALAD:
          return TonalPalette.fromHueAndChroma(sanitizeDegreesDouble(sourceColorHct.hue - 50), 48);
        case Variant.MONOCHROME:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 12);
        case Variant.RAINBOW:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 48);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 36);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(sanitizeDegreesDouble(sourceColorHct.hue + 240), 40);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 200);
        default:
          throw new Error(`Unsupported variant: ${variant}`);
      }
    }
    getSecondaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.CONTENT:
        case Variant.FIDELITY:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, Math.max(sourceColorHct.chroma - 32, sourceColorHct.chroma * 0.5));
        case Variant.FRUIT_SALAD:
          return TonalPalette.fromHueAndChroma(sanitizeDegreesDouble(sourceColorHct.hue - 50), 36);
        case Variant.MONOCHROME:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 8);
        case Variant.RAINBOW:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 16);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 16);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 21, 51, 121, 151, 191, 271, 321, 360], [45, 95, 45, 20, 45, 90, 45, 45, 45]), 24);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 41, 61, 101, 131, 181, 251, 301, 360], [18, 15, 10, 12, 15, 18, 15, 12, 12]), 24);
        default:
          throw new Error(`Unsupported variant: ${variant}`);
      }
    }
    getTertiaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.CONTENT:
          return TonalPalette.fromHct(DislikeAnalyzer.fixIfDisliked(new TemperatureCache(sourceColorHct).analogous(
            /* count= */
            3,
            /* divisions= */
            6
          )[2]));
        case Variant.FIDELITY:
          return TonalPalette.fromHct(DislikeAnalyzer.fixIfDisliked(new TemperatureCache(sourceColorHct).complement));
        case Variant.FRUIT_SALAD:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 36);
        case Variant.MONOCHROME:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 16);
        case Variant.RAINBOW:
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sanitizeDegreesDouble(sourceColorHct.hue + 60), 24);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 21, 51, 121, 151, 191, 271, 321, 360], [120, 120, 20, 45, 20, 15, 20, 120, 120]), 32);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 41, 61, 101, 131, 181, 251, 301, 360], [35, 30, 20, 25, 30, 35, 30, 25, 25]), 32);
        default:
          throw new Error(`Unsupported variant: ${variant}`);
      }
    }
    getNeutralPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.CONTENT:
        case Variant.FIDELITY:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, sourceColorHct.chroma / 8);
        case Variant.FRUIT_SALAD:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 10);
        case Variant.MONOCHROME:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 2);
        case Variant.RAINBOW:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 6);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(sanitizeDegreesDouble(sourceColorHct.hue + 15), 8);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 10);
        default:
          throw new Error(`Unsupported variant: ${variant}`);
      }
    }
    getNeutralVariantPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.CONTENT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, sourceColorHct.chroma / 8 + 4);
        case Variant.FIDELITY:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, sourceColorHct.chroma / 8 + 4);
        case Variant.FRUIT_SALAD:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 16);
        case Variant.MONOCHROME:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 2);
        case Variant.RAINBOW:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 0);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 8);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(sanitizeDegreesDouble(sourceColorHct.hue + 15), 12);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 12);
        default:
          throw new Error(`Unsupported variant: ${variant}`);
      }
    }
    getErrorPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
    }
  }, DynamicSchemePalettesDelegateImpl2025 = class _DynamicSchemePalettesDelegateImpl2025 extends DynamicSchemePalettesDelegateImpl2021 {
    //////////////////////////////////////////////////////////////////
    // Scheme Palettes                                              //
    //////////////////////////////////////////////////////////////////
    getPrimaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" ? Hct.isBlue(sourceColorHct.hue) ? 12 : 8 : Hct.isBlue(sourceColorHct.hue) ? 16 : 12);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" && isDark ? 26 : 32);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" ? isDark ? 36 : 48 : 40);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" ? 74 : 56);
        default:
          return super.getPrimaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel);
      }
    }
    getSecondaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" ? Hct.isBlue(sourceColorHct.hue) ? 6 : 4 : Hct.isBlue(sourceColorHct.hue) ? 10 : 6);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, 16);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 105, 140, 204, 253, 278, 300, 333, 360], [-160, 155, -100, 96, -96, -156, -165, -160]), platform === "phone" && isDark ? 16 : 24);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 38, 105, 140, 333, 360], [-14, 10, -14, 10, -14]), platform === "phone" ? 56 : 36);
        default:
          return super.getSecondaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel);
      }
    }
    getTertiaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 38, 105, 161, 204, 278, 333, 360], [-32, 26, 10, -39, 24, -15, -32]), platform === "phone" ? 20 : 36);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 20, 71, 161, 333, 360], [-40, 48, -32, 40, -32]), platform === "phone" ? 28 : 32);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 105, 140, 204, 253, 278, 300, 333, 360], [-165, 160, -105, 101, -101, -160, -170, -165]), 48);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(DynamicScheme.getRotatedHue(sourceColorHct, [0, 38, 71, 105, 140, 161, 253, 333, 360], [-72, 35, 24, -24, 62, 50, 62, -72]), 56);
        default:
          return super.getTertiaryPalette(variant, sourceColorHct, isDark, platform, contrastLevel);
      }
    }
    static getExpressiveNeutralHue(sourceColorHct) {
      return DynamicScheme.getRotatedHue(sourceColorHct, [0, 71, 124, 253, 278, 300, 360], [10, 0, 10, 0, 10, 0]);
    }
    static getExpressiveNeutralChroma(sourceColorHct, isDark, platform) {
      let neutralHue = _DynamicSchemePalettesDelegateImpl2025.getExpressiveNeutralHue(sourceColorHct);
      return platform === "phone" ? isDark ? Hct.isYellow(neutralHue) ? 6 : 14 : 18 : 12;
    }
    static getVibrantNeutralHue(sourceColorHct) {
      return DynamicScheme.getRotatedHue(sourceColorHct, [0, 38, 105, 140, 333, 360], [-14, 10, -14, 10, -14]);
    }
    static getVibrantNeutralChroma(sourceColorHct, platform) {
      let neutralHue = _DynamicSchemePalettesDelegateImpl2025.getVibrantNeutralHue(sourceColorHct);
      return platform === "phone" || Hct.isBlue(neutralHue) ? 28 : 20;
    }
    getNeutralPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" ? 1.4 : 6);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, platform === "phone" ? 5 : 10);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(_DynamicSchemePalettesDelegateImpl2025.getExpressiveNeutralHue(sourceColorHct), _DynamicSchemePalettesDelegateImpl2025.getExpressiveNeutralChroma(sourceColorHct, isDark, platform));
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(_DynamicSchemePalettesDelegateImpl2025.getVibrantNeutralHue(sourceColorHct), _DynamicSchemePalettesDelegateImpl2025.getVibrantNeutralChroma(sourceColorHct, platform));
        default:
          return super.getNeutralPalette(variant, sourceColorHct, isDark, platform, contrastLevel);
      }
    }
    getNeutralVariantPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      switch (variant) {
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, (platform === "phone" ? 1.4 : 6) * 2.2);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(sourceColorHct.hue, (platform === "phone" ? 5 : 10) * 1.7);
        case Variant.EXPRESSIVE:
          let expressiveNeutralHue = _DynamicSchemePalettesDelegateImpl2025.getExpressiveNeutralHue(sourceColorHct), expressiveNeutralChroma = _DynamicSchemePalettesDelegateImpl2025.getExpressiveNeutralChroma(sourceColorHct, isDark, platform);
          return TonalPalette.fromHueAndChroma(expressiveNeutralHue, expressiveNeutralChroma * (expressiveNeutralHue >= 105 && expressiveNeutralHue < 125 ? 1.6 : 2.3));
        case Variant.VIBRANT:
          let vibrantNeutralHue = _DynamicSchemePalettesDelegateImpl2025.getVibrantNeutralHue(sourceColorHct), vibrantNeutralChroma = _DynamicSchemePalettesDelegateImpl2025.getVibrantNeutralChroma(sourceColorHct, platform);
          return TonalPalette.fromHueAndChroma(vibrantNeutralHue, vibrantNeutralChroma * 1.29);
        default:
          return super.getNeutralVariantPalette(variant, sourceColorHct, isDark, platform, contrastLevel);
      }
    }
    getErrorPalette(variant, sourceColorHct, isDark, platform, contrastLevel) {
      let errorHue = DynamicScheme.getPiecewiseHue(sourceColorHct, [0, 3, 13, 23, 33, 43, 153, 273, 360], [12, 22, 32, 12, 22, 32, 22, 12]);
      switch (variant) {
        case Variant.NEUTRAL:
          return TonalPalette.fromHueAndChroma(errorHue, platform === "phone" ? 50 : 40);
        case Variant.TONAL_SPOT:
          return TonalPalette.fromHueAndChroma(errorHue, platform === "phone" ? 60 : 48);
        case Variant.EXPRESSIVE:
          return TonalPalette.fromHueAndChroma(errorHue, platform === "phone" ? 64 : 48);
        case Variant.VIBRANT:
          return TonalPalette.fromHueAndChroma(errorHue, platform === "phone" ? 80 : 60);
        default:
          return super.getErrorPalette(variant, sourceColorHct, isDark, platform, contrastLevel);
      }
    }
  }, spec20212 = new DynamicSchemePalettesDelegateImpl2021(), spec20252 = new DynamicSchemePalettesDelegateImpl2025();
  function getSpec2(specVersion) {
    return specVersion === "2025" ? spec20252 : spec20212;
  }

  // node_modules/@material/material-color-utilities/scheme/scheme_content.js
  var SchemeContent = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.CONTENT,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_expressive.js
  var SchemeExpressive = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.EXPRESSIVE,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_fidelity.js
  var SchemeFidelity = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.FIDELITY,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_fruit_salad.js
  var SchemeFruitSalad = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.FRUIT_SALAD,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_monochrome.js
  var SchemeMonochrome = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.MONOCHROME,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_neutral.js
  var SchemeNeutral = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.NEUTRAL,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_rainbow.js
  var SchemeRainbow = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.RAINBOW,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_tonal_spot.js
  var SchemeTonalSpot = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.TONAL_SPOT,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/scheme/scheme_vibrant.js
  var SchemeVibrant = class extends DynamicScheme {
    constructor(sourceColorHct, isDark, contrastLevel, specVersion = DynamicScheme.DEFAULT_SPEC_VERSION, platform = DynamicScheme.DEFAULT_PLATFORM) {
      super({
        sourceColorHct,
        variant: Variant.VIBRANT,
        contrastLevel,
        isDark,
        platform,
        specVersion
      });
    }
  };

  // node_modules/@material/material-color-utilities/score/score.js
  var SCORE_OPTION_DEFAULTS = {
    desired: 4,
    fallbackColorARGB: 4282549748,
    filter: !0
    // Avoid unsuitable colors.
  };
  function compare(a, b) {
    return a.score > b.score ? -1 : a.score < b.score ? 1 : 0;
  }
  var Score = class _Score {
    constructor() {
    }
    /**
     * Given a map with keys of colors and values of how often the color appears,
     * rank the colors based on suitability for being used for a UI theme.
     *
     * @param colorsToPopulation map with keys of colors and values of how often
     *     the color appears, usually from a source image.
     * @param {ScoreOptions} options optional parameters.
     * @return Colors sorted by suitability for a UI theme. The most suitable
     *     color is the first item, the least suitable is the last. There will
     *     always be at least one color returned. If all the input colors
     *     were not suitable for a theme, a default fallback color will be
     *     provided, Google Blue.
     */
    static score(colorsToPopulation, options) {
      let { desired, fallbackColorARGB, filter } = __spreadValues(__spreadValues({}, SCORE_OPTION_DEFAULTS), options), colorsHct = [], huePopulation = new Array(360).fill(0), populationSum = 0;
      for (let [argb, population] of colorsToPopulation.entries()) {
        let hct = Hct.fromInt(argb);
        colorsHct.push(hct);
        let hue = Math.floor(hct.hue);
        huePopulation[hue] += population, populationSum += population;
      }
      let hueExcitedProportions = new Array(360).fill(0);
      for (let hue = 0; hue < 360; hue++) {
        let proportion = huePopulation[hue] / populationSum;
        for (let i = hue - 14; i < hue + 16; i++) {
          let neighborHue = sanitizeDegreesInt(i);
          hueExcitedProportions[neighborHue] += proportion;
        }
      }
      let scoredHct = new Array();
      for (let hct of colorsHct) {
        let hue = sanitizeDegreesInt(Math.round(hct.hue)), proportion = hueExcitedProportions[hue];
        if (filter && (hct.chroma < _Score.CUTOFF_CHROMA || proportion <= _Score.CUTOFF_EXCITED_PROPORTION))
          continue;
        let proportionScore = proportion * 100 * _Score.WEIGHT_PROPORTION, chromaWeight = hct.chroma < _Score.TARGET_CHROMA ? _Score.WEIGHT_CHROMA_BELOW : _Score.WEIGHT_CHROMA_ABOVE, chromaScore = (hct.chroma - _Score.TARGET_CHROMA) * chromaWeight, score = proportionScore + chromaScore;
        scoredHct.push({ hct, score });
      }
      scoredHct.sort(compare);
      let chosenColors = [];
      for (let differenceDegrees2 = 90; differenceDegrees2 >= 15; differenceDegrees2--) {
        chosenColors.length = 0;
        for (let { hct } of scoredHct)
          if (chosenColors.find((chosenHct) => differenceDegrees(hct.hue, chosenHct.hue) < differenceDegrees2) || chosenColors.push(hct), chosenColors.length >= desired)
            break;
        if (chosenColors.length >= desired)
          break;
      }
      let colors = [];
      chosenColors.length === 0 && colors.push(fallbackColorARGB);
      for (let chosenHct of chosenColors)
        colors.push(chosenHct.toInt());
      return colors;
    }
  };
  Score.TARGET_CHROMA = 48;
  Score.WEIGHT_PROPORTION = 0.7;
  Score.WEIGHT_CHROMA_ABOVE = 0.3;
  Score.WEIGHT_CHROMA_BELOW = 0.1;
  Score.CUTOFF_CHROMA = 5;
  Score.CUTOFF_EXCITED_PROPORTION = 0.01;
  return __toCommonJS(entry_exports);
})();
/*! Bundled license information:

@material/material-color-utilities/utils/math_utils.js:
@material/material-color-utilities/utils/color_utils.js:
@material/material-color-utilities/hct/viewing_conditions.js:
@material/material-color-utilities/hct/cam16.js:
@material/material-color-utilities/hct/hct_solver.js:
@material/material-color-utilities/hct/hct.js:
@material/material-color-utilities/blend/blend.js:
@material/material-color-utilities/palettes/tonal_palette.js:
@material/material-color-utilities/palettes/core_palette.js:
@material/material-color-utilities/quantize/lab_point_provider.js:
@material/material-color-utilities/quantize/quantizer_wsmeans.js:
@material/material-color-utilities/quantize/quantizer_map.js:
@material/material-color-utilities/quantize/quantizer_wu.js:
@material/material-color-utilities/quantize/quantizer_celebi.js:
@material/material-color-utilities/scheme/scheme.js:
@material/material-color-utilities/scheme/scheme_android.js:
@material/material-color-utilities/score/score.js:
@material/material-color-utilities/utils/string_utils.js:
@material/material-color-utilities/utils/image_utils.js:
@material/material-color-utilities/utils/theme_utils.js:
@material/material-color-utilities/index.js:
  (**
   * @license
   * Copyright 2021 Google LLC
   *
   * Licensed under the Apache License, Version 2.0 (the "License");
   * you may not use this file except in compliance with the License.
   * You may obtain a copy of the License at
   *
   *      http://www.apache.org/licenses/LICENSE-2.0
   *
   * Unless required by applicable law or agreed to in writing, software
   * distributed under the License is distributed on an "AS IS" BASIS,
   * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   * See the License for the specific language governing permissions and
   * limitations under the License.
   *)

@material/material-color-utilities/contrast/contrast.js:
@material/material-color-utilities/dynamiccolor/dynamic_color.js:
@material/material-color-utilities/dynamiccolor/variant.js:
@material/material-color-utilities/dynamiccolor/material_dynamic_colors.js:
@material/material-color-utilities/dynamiccolor/dynamic_scheme.js:
@material/material-color-utilities/scheme/scheme_expressive.js:
@material/material-color-utilities/scheme/scheme_fruit_salad.js:
@material/material-color-utilities/scheme/scheme_monochrome.js:
@material/material-color-utilities/scheme/scheme_neutral.js:
@material/material-color-utilities/scheme/scheme_rainbow.js:
@material/material-color-utilities/scheme/scheme_tonal_spot.js:
@material/material-color-utilities/scheme/scheme_vibrant.js:
  (**
   * @license
   * Copyright 2022 Google LLC
   *
   * Licensed under the Apache License, Version 2.0 (the "License");
   * you may not use this file except in compliance with the License.
   * You may obtain a copy of the License at
   *
   *      http://www.apache.org/licenses/LICENSE-2.0
   *
   * Unless required by applicable law or agreed to in writing, software
   * distributed under the License is distributed on an "AS IS" BASIS,
   * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   * See the License for the specific language governing permissions and
   * limitations under the License.
   *)

@material/material-color-utilities/dislike/dislike_analyzer.js:
@material/material-color-utilities/temperature/temperature_cache.js:
@material/material-color-utilities/dynamiccolor/contrast_curve.js:
@material/material-color-utilities/dynamiccolor/tone_delta_pair.js:
@material/material-color-utilities/scheme/scheme_content.js:
@material/material-color-utilities/scheme/scheme_fidelity.js:
  (**
   * @license
   * Copyright 2023 Google LLC
   *
   * Licensed under the Apache License, Version 2.0 (the "License");
   * you may not use this file except in compliance with the License.
   * You may obtain a copy of the License at
   *
   *      http://www.apache.org/licenses/LICENSE-2.0
   *
   * Unless required by applicable law or agreed to in writing, software
   * distributed under the License is distributed on an "AS IS" BASIS,
   * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   * See the License for the specific language governing permissions and
   * limitations under the License.
   *)

@material/material-color-utilities/dynamiccolor/color_spec_2021.js:
@material/material-color-utilities/dynamiccolor/color_spec_2025.js:
  (**
   * @license
   * Copyright 2025 Google LLC
   *
   * Licensed under the Apache License, Version 2.0 (the "License");
   * you may not use this file except in compliance with the License.
   * You may obtain a copy of the License at
   *
   *      http://www.apache.org/licenses/LICENSE-2.0
   *
   * Unless required by applicable law or agreed to in writing, software
   * distributed under the License is distributed on an "AS IS" BASIS,
   * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   * See the License for the specific language governing permissions and
   * limitations under the License.
   *)
*/
