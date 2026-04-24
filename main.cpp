/*
 * This file is part of rxwspr.
 *
 * Copyright (C) 2020 by Edson Pereira, PY2SDR
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rxwspr. If not, see <http://www.gnu.org/licenses/>.
 */
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include "rxwspr.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    a.setApplicationVersion("4.00");

    // Define command-line options
    QCommandLineParser parser;
    parser.setApplicationDescription("PY2SDR WSPR/FST4W Receiver");
    parser.addHelpOption();
    parser.addVersionOption();

    // Required options
    QCommandLineOption callsign(QStringList() << "c" << "callsign", "Station callsign", "callsign");
    QCommandLineOption locator(QStringList() << "l" << "locator", "Grid locator", "locator");
    QCommandLineOption frequency(QStringList() << "f" << "freq", "RX Frequency in Hz", "freq");
    QCommandLineOption multicastGroup(QStringList() << "g" << "group", "Multicast group", "group");
    QCommandLineOption multicastPort(QStringList() << "p" << "port", "Multicast port", "port");
    QCommandLineOption multicastInterface(QStringList() << "i" << "iface", "Multicast network interface", "iface");

    // Optional settings
    QCommandLineOption sampleRate(QStringList() << "s" << "samplerate", "Input sample rate: 48000 or 12000", "samplerate", "12000");
    QCommandLineOption deepSearch("deep", "Enable wsprd deep search mode");
    QCommandLineOption noUpload("no-upload", "Disable spot upload to WSPRNet");
    QCommandLineOption silentStdout(QStringList() << "q" << "quiet", "Do not print messages to STDOUT");
    QCommandLineOption logDir(QStringList() << "d" << "logdir", "Log file directory", "logdir", QDir::homePath());

    parser.addOption(callsign);
    parser.addOption(locator);
    parser.addOption(frequency);
    parser.addOption(multicastGroup);
    parser.addOption(multicastPort);
    parser.addOption(multicastInterface);
    parser.addOption(sampleRate);
    parser.addOption(deepSearch);
    parser.addOption(noUpload);
    parser.addOption(silentStdout);
    parser.addOption(logDir);

    parser.process(a);

    // --- Validate required parameters ---

    if (parser.value(callsign).isEmpty())
    {
        qInfo().noquote() << "Invalid callsign";
        parser.showHelp();
        return 1;
    }

    if (parser.value(locator).isEmpty())
    {
        qInfo().noquote() << "Invalid QTH locator";
        parser.showHelp();
        return 1;
    }

    // Frequency must be a positive integer in Hz.
    // Uses qint64 to support frequencies above 2 GHz (e.g. 10 GHz band).
    if (parser.value(frequency).isEmpty())
    {
        qInfo().noquote() << "Invalid frequency";
        parser.showHelp();
        return 1;
    }

    bool freqValid = false;
    qint64 freqHz = parser.value(frequency).toLongLong(&freqValid);
    Q_UNUSED(freqHz)
    if (!freqValid || freqHz <= 0)
    {
        qInfo().noquote() << "Frequency must be a positive integer in Hz";
        parser.showHelp();
        return 1;
    }

    if (parser.value(multicastGroup).isEmpty())
    {
        qInfo().noquote() << "Invalid multicast group";
        parser.showHelp();
        return 1;
    }

    // Port must be in unprivileged range (1025-65535)
    if (parser.value(multicastPort).isEmpty())
    {
        qInfo().noquote() << "Invalid multicast port";
        parser.showHelp();
        return 1;
    }

    bool portValid = false;
    int port = parser.value(multicastPort).toInt(&portValid);
    Q_UNUSED(port)
    if (!portValid || port < 1025 || port > 65535)
    {
        qInfo().noquote() << "Multicast port must be an integer between 1025 and 65535";
        parser.showHelp();
        return 1;
    }

    if (parser.value(multicastInterface).isEmpty())
    {
        qInfo().noquote() << "Invalid multicast interface";
        parser.showHelp();
        return 1;
    }

    // Verify log directory exists and is writable
    QDir logDirPath(parser.value(logDir));
    if (!logDirPath.exists())
    {
        qInfo().noquote() << "Log directory does not exist:" << parser.value(logDir);
        parser.showHelp();
        return 1;
    }

    QFileInfo logDirInfo(parser.value(logDir));
    if (!logDirInfo.isWritable())
    {
        qInfo().noquote() << "Log directory is not writable:" << parser.value(logDir);
        parser.showHelp();
        return 1;
    }

    // Sample rate must be 48000 or 12000
    int rate = parser.value(sampleRate).toInt();
    if (rate != 48000 && rate != 12000)
    {
        qInfo().noquote() << "Sample rate must be 48000 or 12000";
        parser.showHelp();
        return 1;
    }

    // --- Create and start the WSPR/FST4W receiver ---

    RXWSPR rxwspr(
        parser.value(callsign),
        parser.value(locator),
        parser.value(frequency),
        parser.value(multicastGroup),
        parser.value(multicastPort),
        parser.value(multicastInterface),
        parser.value(logDir),
        rate,
        parser.isSet(deepSearch),
        parser.isSet(noUpload),
        parser.isSet(silentStdout)
    );

    if (rxwspr.hasError())
    {
        return 1;
    }

    // Connect quit signals for clean shutdown
    QObject::connect(&rxwspr, &RXWSPR::finished, &a, &QCoreApplication::quit);
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &rxwspr, &RXWSPR::aboutToQuitApp);

    return a.exec();
}
