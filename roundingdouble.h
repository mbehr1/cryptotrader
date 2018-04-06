#ifndef ROUNDINGDOUBLE_H
#define ROUNDINGDOUBLE_H

#include <QString>
#include <QDebug>

class RoundingDouble
{
public:
    RoundingDouble(const double &d, const QString &minNumber); // e.g. 0.05 "0.1"
    RoundingDouble(const double &d, int prec); // e.g. 0.05 1
    RoundingDouble &operator =(const double &v) { _val = v; _cacheValid = false; return *this;}
    operator QString () const; // "0.1"
    operator double() const; // returns as rounded value 0.1
private:
    double _val;
    int _prec;

    // we cached both the operator QString() and double()
    mutable bool _cacheValid;
    mutable QString _cachedStr; // for operator QString()
    mutable double _cachedDouble; // for operator double()
};

QDebug operator<< (QDebug d, const RoundingDouble &r);

#endif // ROUNDINGDOUBLE_H
