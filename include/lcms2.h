#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t cmsTagSignature;
typedef uint32_t cmsUInt32Number;
typedef void* cmsHPROFILE;
typedef void* cmsHTRANSFORM;
typedef void* cmsToneCurve;

struct cmsCIEXYZ {
    double X;
    double Y;
    double Z;
};
typedef struct cmsCIEXYZ cmsCIEXYZ;

struct cmsCIExyY {
    double x;
    double y;
    double Y;
};
typedef struct cmsCIExyY cmsCIExyY;

struct cmsCIExyYTRIPLE {
    cmsCIExyY Red;
    cmsCIExyY Green;
    cmsCIExyY Blue;
};
typedef struct cmsCIExyYTRIPLE cmsCIExyYTRIPLE;

#define cmsSigRgbData 1
#define INTENT_RELATIVE_COLORIMETRIC 1
#define cmsFLAGS_BLACKPOINTCOMPENSATION 0x0001
#define cmsFLAGS_HIGHRESPRECALC 0x0002
#define TYPE_RGB_FLT 1
#define TYPE_XYZ_FLT 2

inline int cmsIsTag(cmsHPROFILE hProfile, cmsTagSignature sig) { return 0; }
inline cmsUInt32Number cmsReadRawTag(cmsHPROFILE hProfile, cmsTagSignature sig, void* Buffer, cmsUInt32Number BufferSize) { return 0; }
inline void cmsCloseProfile(cmsHPROFILE hProfile) {}
inline void cmsDeleteTransform(cmsHTRANSFORM hTransform) {}
inline cmsToneCurve* cmsBuildGamma(void* ContextID, double Gamma) { return 0; }
inline cmsHPROFILE cmsCreateRGBProfile(const cmsCIExyY* WhitePoint, const cmsCIExyYTRIPLE* Primaries, cmsToneCurve* const TransferFunction[3]) { return 0; }
inline void cmsFreeToneCurve(cmsToneCurve* Curve) {}
inline cmsHTRANSFORM cmsCreateTransform(cmsHPROFILE Input, cmsUInt32Number InputFormat, cmsHPROFILE Output, cmsUInt32Number OutputFormat, cmsUInt32Number Intent, cmsUInt32Number dwFlags) { return 0; }
inline void cmsDoTransform(cmsHTRANSFORM Transform, const void* InputBuffer, void* OutputBuffer, cmsUInt32Number Size) {}
inline void cmsXYZ2xyY(cmsCIExyY* Dest, const cmsCIEXYZ* Source) {}
inline cmsHPROFILE cmsCreateXYZProfile(void) { return 0; }
inline cmsHPROFILE cmsOpenProfileFromFile(const char* ICCProfile, const char* sAccess) { return 0; }
inline cmsUInt32Number cmsGetColorSpace(cmsHPROFILE hProfile) { return 0; }

#ifdef __cplusplus
}
#endif
