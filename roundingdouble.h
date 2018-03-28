#ifndef ROUNDINGDOUBLE_H
#define ROUNDINGDOUBLE_H

#include <QString>

class RoundingDouble
{
public:
    RoundingDouble(const double &d, const QString &minNumber); // e.g. 0.05 "0.1"
    operator QString () const; // "0.1"
    operator double() const; // returns as rounded value 0.1
private:
    double _val;
    int _prec;
};

#endif // ROUNDINGDOUBLE_H
