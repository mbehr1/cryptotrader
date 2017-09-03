QT += core
QT += websockets
QT -= gui

CONFIG += c++11

TARGET = cryptotrader
CONFIG += console
CONFIG -= app_bundle

TEMPLATE = app

SOURCES += main.cpp \
    exchangebitfinex.cpp

HEADERS += \
    exchangebitfinex.h
