/*
 * This file is part of rxwspr.
 *
 * Copyright (C) 2020 Edson Pereira, PY2SDR, <py2sdr@gmail.com>
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
#ifndef RXWSPR_H
#define RXWSPR_H
#include <QObject>
#include <QCoreApplication>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QtMath>
#include <time.h>
#include "sunpropagator.h"

static const int MAX_UPLOAD_RETRIES = 10;

struct SpotUpload
{
    QString url;
    int retryCount;
};

class RXWSPR : public QObject
{
    Q_OBJECT
public:
    explicit RXWSPR(
        const QString &callsign,
        const QString &locator,
        const QString &frequency,
        const QString &multicastGroup,
        const QString &multicastPort,
        const QString &multicastInterface,
        const QString &logDir,
        int sampleRate = 12000,
        bool deepSearch = false,
        bool noUpload = false,
        bool silentStdout = false,
        QObject *parent = nullptr);

signals:
    void finished();

public slots:
    void timeTick();
    void aboutToQuitApp();
    void quit();
    bool hasError() const { return m_initError; }

private:
    QCoreApplication *m_app;
    QString m_myCall;
    QString m_myGrid;
    QString m_baseFrequency;
    QString m_baseFrequencyMHz;
    QString m_multicastGroup;
    QString m_multicastPort;
    QString m_multicastInterface;
    QString m_logDir;
    QString m_date;

    int m_sampleRate;
    int m_recSize;

    QUdpSocket m_rxSocket;
    QByteArray m_audioData;
    QByteArray m_waveFileHeader;
    QString m_wavFileName;

    QString m_WSPRdecoderProgram;
    QString m_FST4WdecoderProgram;
    QString m_WSPROutput;
    QString m_FST4WOutput;
    QProcess m_WSPRdecoderProcess;
    QProcess m_FST4WdecoderProcess;

    QTimer m_tickTimer;
    QTimer m_uploadTimer;

    QQueue<SpotUpload> m_urlQueue;
    SpotUpload m_currentUpload;
    QNetworkAccessManager m_networkManager;

    bool m_recTrigger;
    bool m_deepSearch;
    bool m_noUpload;
    bool m_silentStdout;
    bool m_initError;
    time_t m_time;
    time_t m_timeSave;
    float m_rawPower;

    SunPropagator m_sunLocal;
    SunPropagator m_sunDX;

    void compute_dB(const QByteArray *data);
    void writeToLog(const QString &line);
    void enqueueSpot(const QDateTime &dt, const QString &time, const QString &sig,
                     const QString &dtOffset, const QString &tqrg, const QString &drift,
                     const QString &tcall, const QString &tgrid, const QString &dbm,
                     const QString &mode);
    QString formatSpotLine(const QString &time, const QString &sig, const QString &dtOffset,
                           const QString &tqrg, const QString &drift, const QString &tcall,
                           const QString &tgrid, const QString &dbm, const QString &mode);
    float getLat(const QString &locatorString);
    float getLon(const QString &locatorString);
    int dxDistance(const QString &dxGrid);

private slots:
    void udpRead();
    void accumulateWSPROutput();
    void WSPRDecoderFinished(int code, QProcess::ExitStatus status);
    void WSPRDecoderError(QProcess::ProcessError error);
    void accumulateFST4WOutput();
    void FST4WDecoderFinished(int code, QProcess::ExitStatus status);
    void FST4WDecoderError(QProcess::ProcessError error);
    void uploadSpots();
    void networkReply(QNetworkReply *reply);
};
#endif

