#include "roundingdouble.h"
#include <cassert>
#include <QDebug>

RoundingDouble::RoundingDouble(const double &d, const QString &minNumber) :
    _val(d), _prec(0)
{
    assert(_val >= 0.0); // not tested yet for neg. values

    // determine precision based on the minNumber string (e.g. 0.1 -> 1)
    if (minNumber == QStringLiteral("0.1")) _prec = 1; else
    if (minNumber == QStringLiteral("0.01")) _prec = 2; else
    if (minNumber == QStringLiteral("0.001")) _prec = 3; else
    if (minNumber == QStringLiteral("0.0001")) _prec = 4; else
    if (minNumber == QStringLiteral("0.00001")) _prec = 5; else
    if (minNumber == QStringLiteral("0.000001")) _prec = 6; else
    if (minNumber == QStringLiteral("0.0000001")) _prec = 7; else
    if (minNumber == QStringLiteral("0.00000001")) _prec = 8; else
    if (minNumber == QStringLiteral("0.000000001")) _prec = 9; else
    if (minNumber == QStringLiteral("0.0000000001")) _prec = 10; else
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

RoundingDouble::operator double() const
{
    QString temp = operator QString();
    return temp.toDouble();
}

RoundingDouble::operator QString() const
{
    QString temp;
    if (_prec >= 0) { // normal 0.1, ...
        temp = QString("%1").arg(_val, 0, 'f', _prec);
    } else { // e.g. prec = -1 -> "10"
        int loops = 0 - _prec;
        unsigned long div = 10;
        for (int i=1; i<loops; ++i) {
            div *= 10ul;
        }
        temp = QString("%1").arg(_val/div, 0, 'f', 0);
        for (int i=0; i<loops; ++i)
            temp.append('0');
    }
//    qDebug() << __PRETTY_FUNCTION__ << _val << _prec << temp;
    return temp;
}
