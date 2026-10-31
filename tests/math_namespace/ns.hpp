// A math namespace for the test: every function returns a value no real libm returns (the argument plus a marker), so the output shows which functions the generated code called.
#pragma once
namespace marker_math {
inline double sin(double x) { return x + 1000; }
inline double cos(double x) { return x + 2000; }
inline double tan(double x) { return x + 3000; }
inline double asin(double x) { return x + 4000; }
inline double acos(double x) { return x + 5000; }
inline double atan(double x) { return x + 6000; }
inline double atan2(double y, double x) { return y * 10 + x + 7000; }
inline double sinh(double x) { return x + 8000; }
inline double cosh(double x) { return x + 9000; }
inline double tanh(double x) { return x + 10000; }
inline double exp(double x) { return x + 11000; }
inline double log(double x) { return x + 12000; }
inline double pow(double x, double y) { return x * 100 + y + 13000; }
}  // namespace marker_math
