/* kmath.c — freestanding floating-point math and number conversion.
 * The kernel has no libm, so these are small series-based versions.
 * Accuracy is good to roughly 1e-12 for ordinary arguments. */
#include "amdroid/kmath.h"
#include <string.h>

#define LN2   0.693147180559945309417232121458
#define PI_2  1.570796326794896619231321691640
#define TWO_PI 6.283185307179586476925286766559

double kfabs(double x){ return x < 0 ? -x : x; }

double ktrunc(double x){
    if(x != x || x > 4.5e15 || x < -4.5e15) return x;
    return (double)(long long)x;
}

double kfloor(double x){ double t = ktrunc(x); if(t > x) t -= 1.0; return t; }
double kceil(double x){ double t = ktrunc(x); if(t < x) t += 1.0; return t; }
double kfmod(double a, double b){ double q = ktrunc(a / b); return a - q * b; }

double ksqrt(double x){
    double r;
    __asm__ volatile("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double kexp(double x){
    if(x > 709.0) return 1.7e308;
    if(x < -745.0) return 0.0;
    double k = kfloor(x / LN2 + 0.5);
    double r = x - k * LN2;
    double term = 1.0, sum = 1.0;
    for(int i = 1; i < 30; i++){ term *= r / i; sum += term; }
    int ki = (int)k;
    while(ki > 0){ sum *= 2.0; ki--; }
    while(ki < 0){ sum *= 0.5; ki++; }
    return sum;
}

double klog(double x){
    if(x == 0.0) return -1.7e308;
    if(x < 0.0) return 0.0 / 0.0;
    int e = 0;
    double m = x;
    while(m >= 2.0){ m *= 0.5; e++; }
    while(m < 1.0){ m *= 2.0; e--; }
    /* log(m) = 2 atanh((m-1)/(m+1)) */
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, term = s, sum = 0.0;
    for(int i = 1; i < 200; i += 2){ sum += term / i; term *= s2; }
    return 2.0 * sum + e * LN2;
}

double kpow(double a, double b){
    if(b == 0.0) return 1.0;
    if(a == 0.0) return b > 0.0 ? 0.0 : 1.7e308;
    if(b == kfloor(b) && kfabs(b) < 2147483648.0){
        long n = (long)b;
        int neg = n < 0;
        if(neg) n = -n;
        double r = 1.0, base = a;
        while(n){
            if(n & 1) r *= base;
            base *= base;
            n >>= 1;
        }
        return neg ? 1.0 / r : r;
    }
    if(a < 0.0) return 0.0 / 0.0;
    return kexp(b * klog(a));
}

static double reduce_angle(double x){
    double k = kfloor(x / TWO_PI + 0.5);
    return x - k * TWO_PI;
}

double ksin(double x){
    double r = reduce_angle(x), r2 = r * r, term = r, sum = r;
    for(int i = 1; i < 20; i++){
        term *= -r2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

double kcos(double x){
    double r = reduce_angle(x), r2 = r * r, term = 1.0, sum = 1.0;
    for(int i = 1; i < 20; i++){
        term *= -r2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

double ktan(double x){ return ksin(x) / kcos(x); }

/* atan of a small argument by its Taylor series */
static double atan_series(double z){
    double term = z, sum = z, z2 = z * z;
    for(int i = 1; i < 60; i++){
        term *= -z2;
        sum += term / (2 * i + 1);
    }
    return sum;
}

double katan(double z){
    int neg = z < 0.0;
    if(neg) z = -z;
    int inv = 0;
    if(z > 1.0){ z = 1.0 / z; inv = 1; }
    /* atan(z) = 2 atan(z / (1 + sqrt(1 + z^2))): halve until the series converges fast */
    int h = 0;
    while(z > 0.2){ z = z / (1.0 + ksqrt(1.0 + z * z)); h++; }
    double res = atan_series(z) * (double)(1 << h);
    if(inv) res = PI_2 - res;
    return neg ? -res : res;
}

double katan2(double y, double x){
    if(x > 0.0) return katan(y / x);
    if(x < 0.0) return y >= 0.0 ? katan(y / x) + 2 * PI_2 : katan(y / x) - 2 * PI_2;
    if(y > 0.0) return PI_2;
    if(y < 0.0) return -PI_2;
    return 0.0;
}

int kstrtod(const char *s, double *out, const char **end){
    const char *p = s;
    int neg = 0, any = 0;
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if(*p == '+' || *p == '-'){ neg = (*p == '-'); p++; }
    double v = 0.0;
    while(*p >= '0' && *p <= '9'){ v = v * 10.0 + (*p - '0'); p++; any = 1; }
    if(*p == '.'){
        p++;
        double f = 0.0, scale = 1.0;
        while(*p >= '0' && *p <= '9'){ f = f * 10.0 + (*p - '0'); scale *= 10.0; p++; any = 1; }
        v += f / scale;
    }
    if(!any){ if(end) *end = s; return 0; }
    if(*p == 'e' || *p == 'E'){
        const char *q = p + 1;
        int eneg = 0;
        if(*q == '+' || *q == '-'){ eneg = (*q == '-'); q++; }
        if(*q >= '0' && *q <= '9'){
            int ex = 0;
            while(*q >= '0' && *q <= '9'){ if(ex < 400) ex = ex * 10 + (*q - '0'); q++; }
            p = q;
            double m = 1.0;
            for(int i = 0; i < ex; i++) m *= 10.0;
            v = eneg ? v / m : v * m;
        }
    }
    *out = neg ? -v : v;
    if(end) *end = p;
    return 1;
}

int kfmt_double(char *buf, int cap, double d){
    char tmp[64];
    int n = 0;
    if(d != d){ strcpy(tmp, "nan"); n = 3; }
    else if(d > 1.7e308){ strcpy(tmp, "inf"); n = 3; }
    else if(d < -1.7e308){ strcpy(tmp, "-inf"); n = 4; }
    else {
        if(d < 0){ tmp[n++] = '-'; d = -d; }
        if(d == 0){
            tmp[n++] = '0';
        } else if(d < 9.0e15 && d == kfloor(d)){
            /* exact integer */
            unsigned long long v = (unsigned long long)d;
            char dg[24];
            int k = 0;
            while(v){ dg[k++] = (char)('0' + v % 10); v /= 10; }
            while(k) tmp[n++] = dg[--k];
        } else {
            int e = 0;
            double m = d;
            while(m >= 10.0){ m /= 10.0; e++; }
            while(m < 1.0){ m *= 10.0; e--; }
            unsigned long long D = (unsigned long long)(m * 1e14 + 0.5);
            if(D >= 1000000000000000ULL){ D /= 10; e++; }
            char dg[16];
            for(int i = 14; i >= 0; i--){ dg[i] = (char)('0' + D % 10); D /= 10; }
            int nd = 15;
            while(nd > 1 && dg[nd - 1] == '0') nd--;
            if(e >= 15 || e < -4){
                tmp[n++] = dg[0];
                if(nd > 1){ tmp[n++] = '.'; for(int i = 1; i < nd; i++) tmp[n++] = dg[i]; }
                tmp[n++] = 'e';
                tmp[n++] = e < 0 ? '-' : '+';
                int ae = e < 0 ? -e : e;
                if(ae >= 100) tmp[n++] = (char)('0' + ae / 100);
                if(ae >= 10) tmp[n++] = (char)('0' + (ae / 10) % 10);
                tmp[n++] = (char)('0' + ae % 10);
            } else if(e >= 0){
                for(int i = 0; i <= e; i++) tmp[n++] = i < nd ? dg[i] : '0';
                if(nd > e + 1){ tmp[n++] = '.'; for(int i = e + 1; i < nd; i++) tmp[n++] = dg[i]; }
            } else {
                tmp[n++] = '0';
                tmp[n++] = '.';
                for(int i = 0; i < -e - 1; i++) tmp[n++] = '0';
                for(int i = 0; i < nd; i++) tmp[n++] = dg[i];
            }
        }
    }
    if(n >= cap) n = cap - 1;
    if(n < 0) n = 0;
    for(int i = 0; i < n; i++) buf[i] = tmp[i];
    buf[n] = 0;
    return n;
}
