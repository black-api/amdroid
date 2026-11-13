#ifndef AMDROID_KMATH_H
#define AMDROID_KMATH_H
#include "amdroid/types.h"
/* Freestanding math for the kernel (no libm). */
double kfabs(double x);
double ktrunc(double x);
double kfloor(double x);
double kceil(double x);
double kfmod(double a, double b);
double ksqrt(double x);
double kexp(double x);
double klog(double x);
double kpow(double a, double b);
double ksin(double x);
double kcos(double x);
double ktan(double x);
double katan(double x);
double katan2(double y, double x);
/* Parses a decimal number like strtod. Returns 1 on success, sets *end. */
int kstrtod(const char *s, double *out, const char **end);
/* Formats a double with up to 15 significant digits. Returns the length. */
int kfmt_double(char *buf, int cap, double d);
#endif
