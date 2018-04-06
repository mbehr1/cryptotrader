#include "roundingdouble.h"
#include <cassert>
#include <cmath>
#include <QDebug>

RoundingDouble::RoundingDouble(const double &d, const QString &minNumber) :
    _val(d), _prec(0), _cacheValid(false)
{
    assert(_val >= 0.0); // not tested yet for neg. values

    // determine precision based on the minNumber string (e.g. 0.1 -> 1)
    // startsWith is only correct for cases where it ends with 0. e.g. 0.1000 instead of 0.1
    if (minNumber.startsWith(QStringLiteral("0.1"))) _prec = 1; else
    if (minNumber.startsWith(QStringLiteral("0.01"))) _prec = 2; else
    if (minNumber.startsWith(QStringLiteral("0.001"))) _prec = 3; else
    if (minNumber.startsWith(QStringLiteral("0.0001"))) _prec = 4; else
    if (minNumber.startsWith(QStringLiteral("0.00001"))) _prec = 5; else
    if (minNumber.startsWith(QStringLiteral("0.000001"))) _prec = 6; else
    if (minNumber.startsWith(QStringLiteral("0.0000001"))) _prec = 7; else
    if (minNumber.startsWith(QStringLiteral("0.00000001"))) _prec = 8; else
    if (minNumber.startsWith(QStringLiteral("0.000000001"))) _prec = 9; else
    if (minNumber.startsWith(QStringLiteral("0.0000000001"))) _prec = 10; else
    if (minNumber == QStringLiteral("1")) _prec = 0; else
    if (minNumber == QStringLiteral("10")) _prec = -1; else
    if (minNumber == QStringLiteral("100")) _prec = -2; else
    if (minNumber == QStringLiteral("1000")) _prec = -3; else
    if (minNumber == QStringLiteral("10000")) _prec = -4; else
    if (minNumber == QStringLiteral("100000")) _prec = -5; else

                {
                    qWarning() << __PRETTY_FUNCTION__ << "unsupported minNumber" << minNumber;
                    assert(false);
                }
}

RoundingDouble::RoundingDouble(const double &d, int prec) :
    _val(d), _prec(prec), _cacheValid(false)
{
    if (prec < -10) {
        qCritical() << __PRETTY_FUNCTION__ << "too small prec!" << prec;
        assert(false);
    }
    if (prec > 10) {
        qCritical() << __PRETTY_FUNCTION__ << "too big prec!" << prec;
        assert(false);
    }
}

RoundingDouble::operator double() const
{
    if (_cacheValid) return _cachedDouble;
    (void)operator QString();
    assert(_cacheValid); // we rely on operator QString() to set _cachedDouble as well!
    //_cachedDouble =  _cachedStr.toDouble();
    //_cacheValid = true;
    return _cachedDouble;
}

RoundingDouble::operator QString() const
{
    if (_cacheValid) return _cachedStr;

    if (_prec > 0) { // normal 0.1, ...
        _cachedStr = QString("%1").arg(_val, 0, 'f', _prec);
    } else { // e.g. prec = -1 -> "10"
        if (_prec == 0) {
           _cachedStr = QString("%1").arg(std::llround(_val)); // different qt versions can't handle the double 0 case
        } else {
            int loops = 0 - _prec;
            unsigned long div = 10;
            for (int i=1; i<loops; ++i) {
                div *= 10ul;
            }
            _cachedStr = QString("%1").arg(std::llround(_val/div));
            for (int i=0; i<loops; ++i)
                _cachedStr.append('0');
        }
    }
//    qDebug() << __PRETTY_FUNCTION__ << _val << _prec << temp;
    _cachedDouble = _cachedStr.toDouble();
    _cacheValid = true;
    return _cachedStr;
}

QDebug operator<<(QDebug d, const RoundingDouble &r)
{
    d << (QString)r;
    return d;
}
