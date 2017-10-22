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

SOURCES += main.cpp \
    exchangebitfinex.cpp \
    providercandles.cpp \
    channel.cpp \
    engine.cpp \
    strategyrsinoloss.cpp \
    channelaccountinfo.cpp

HEADERS += \
    exchangebitfinex.h \
    providercandles.h \
    channel.h \
    engine.h \
    strategyrsinoloss.h \
    channelaccountinfo.h

LIBS += -lta_lib
