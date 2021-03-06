#ifndef BUTTERLOWPASS_H
#define BUTTERLOWPASS_H

/*
 * This file is autogenerated. Please do not modify directly as changes may be
 * overwritten.
 */

class ButterLowpass {
private:
    static const int _size = 2 + 1;
    double _x[_size]; // input
    double _y[_size]; // output
    int _n; // current array index

public:
    ButterLowpass();
    float filter(float sample);
};

#endif // BUTTERLOWPASS_H