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

INCLUDEPATH += $$PWD/lib/PubNubQtSync/PubNubQtSync
INCLUDEPATH += $$PWD/lib/PubNubQtSync/c-core/core
DEPENDPATH += $$PWD/lib/PubNubQtSync/c-core/core
HEADERS += $$PWD/lib/PubNubQtSync/PubNubQtSync/pubnub_qt.h \
    tradestrategy.h \
    strategyexchgdelta.h \
    exchangebinance.h \
    exchangenam.h
SOURCES += $$PWD/lib/PubNubQtSync/PubNubQtSync/pubnub_qt.cpp $$PWD/lib/PubNubQtSync/c-core/core/pubnub_ccore.c $$PWD/lib/PubNubQtSync/c-core/core/pubnub_assert_std.c $$PWD/lib/PubNubQtSync/c-core/core/pubnub_json_parse.c $$PWD/lib/PubNubQtSync/c-core/core/pubnub_helper.c \
    tradestrategy.cpp \
    strategyexchgdelta.cpp \
    exchangenam.cpp \
    exchangebinance.cpp


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
