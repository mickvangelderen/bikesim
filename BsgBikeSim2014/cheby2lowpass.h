#ifndef CHEBY2LOWPASS_H
#define CHEBY2LOWPASS_H

/*
 * This file is autogenerated. Please do not modify directly as changes may be
 * overwritten.
 */

class Cheby2Lowpass {
private:
    static const int _size = 4 + 1;
    double _x[_size]; // input
    double _y[_size]; // output
    int _n; // current array index

public:
    Cheby2Lowpass();
    float filter(float sample);
};

#endif // CHEBY2LOWPASS_H