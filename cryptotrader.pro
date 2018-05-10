QT += core
QT += websockets
QT -= gui

CONFIG += c++11
CONFIG += warn_on

TARGET = cryptotrader
CONFIG += console
CONFIG -= app_bundle

TEMPLATE = app

include($$PWD/lib/QtTelegramBot/QtTelegramBot.pri)

HEADERS += tradestrategy.h \
    strategyexchgdelta.h \
    exchangebinance.h \
    exchangenam.h \
    strategyarbitrage.h \
    exchangehitbtc.h \
    roundingdouble.h
SOURCES += tradestrategy.cpp \
    strategyexchgdelta.cpp \
    exchangenam.cpp \
    exchangebinance.cpp \
    strategyarbitrage.cpp \
    exchangehitbtc.cpp \
    roundingdouble.cpp

SOURCES += main.cpp \
    exchangebitfinex.cpp \
    providercandles.cpp \
    channel.cpp \
    engine.cpp \
    strategyrsinoloss.cpp \
    channelaccountinfo.cpp \
    exchange.cpp \
    exchangebitflyer.cpp

HEADERS += \
    exchangebitfinex.h \
    providercandles.h \
    channel.h \
    engine.h \
    strategyrsinoloss.h \
    channelaccountinfo.h \
    exchange.h \
    exchangebitflyer.h

LIBS += -lta_lib
