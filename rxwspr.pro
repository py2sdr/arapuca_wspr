QT += core network
QT -= gui
TARGET = rxwspr
CONFIG += console
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += main.cpp \
    rxwspr.cpp \
    sunpropagator.cpp

HEADERS += rxwspr.h \
    sunpropagator.h

target.path = /usr/local/bin
INSTALLS += target
