/*
 * rxwspr - WSPR/FST4W Receiver
 *
 * Copyright (C) 2020 by Edson Pereira, PY2SDR <py2sdr@gmail.com>
 *
 * The program listens to 12 kHz or 48 kHz 16 bit samples from IP multicast,
 * records two minutes of samples time aligned to even minutes,
 * calls the wsprd and jt9 processes to decode WSPR and FST4W spots,
 * receives the decoder results and sends the spots to WSPRNet.
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
 * <http://www.gnu.org/licenses/>.
 */

#include <QDebug>
#include <QDataStream>
#include <QProcess>
#include <QFileInfo>
#include <QtEndian>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <math.h>
#include <time.h>
#include "rxwspr.h"

RXWSPR::RXWSPR(
    const QString &callsign,
    const QString &locator,
    const QString &frequency,
    const QString &multicastGroup,
    const QString &multicastPort,
    const QString &multicastInterface,
    const QString &logDir,
    int sampleRate,
    bool deepSearch,
    bool noUpload,
    bool silentStdout,
    QObject *parent) : QObject(parent)
{
    m_app = QCoreApplication::instance();
    m_initError = false;

    m_WSPRdecoderProgram = QStandardPaths::findExecutable("wsprd");
    if (m_WSPRdecoderProgram.isEmpty())
    {
        qCritical() << "wsprd not found in PATH";
        m_initError = true;
        return;
    }

    m_FST4WdecoderProgram = QStandardPaths::findExecutable("jt9");
    if (m_FST4WdecoderProgram.isEmpty())
    {
        qCritical() << "jt9 not found in PATH";
        m_initError = true;
        return;
    }

    m_myCall = callsign.toUpper();
    m_myGrid = locator.toUpper();
    m_baseFrequency = frequency;
    m_baseFrequencyMHz = QString::number(frequency.toLongLong() / 1e6, 'f', 6);
    m_multicastGroup = multicastGroup;
    m_multicastPort = multicastPort;
    m_multicastInterface = multicastInterface;
    m_logDir = logDir;
    m_sampleRate = sampleRate;
    m_recSize = 115 * m_sampleRate * 2; // 115 sec * sample rate * 2 bytes
    m_recTrigger = false;
    m_deepSearch = deepSearch;
    m_noUpload = noUpload;
    m_silentStdout = silentStdout;

    // Enable FST4W spectral spread measurements
    QFile plotspecFile("plotspec");
    plotspecFile.open(QIODevice::WriteOnly);
    plotspecFile.close();

    connect(&m_networkManager, &QNetworkAccessManager::finished, this, &RXWSPR::networkReply);

    // Prepare the wave file header
    QDataStream ds(&m_waveFileHeader, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData("RIFF", 4);     // Literal RIFF
    ds << quint32(2880000 + 36);    // ChunkSize: data length + 36 remaining header bytes
    ds.writeRawData("WAVE", 4);     // Literal WAVE
    ds.writeRawData("fmt ", 4);     // Literal fmt
    ds << quint32(16);              // fmt header length
    ds << quint16(1);               // PCM format
    ds << quint16(1);               // Channel count
    ds << quint32(12000);           // Sample rate
    ds << quint32(24000);           // Data rate
    ds << quint16(2);               // Bytes per sample
    ds << quint16(16);              // SampleBits
    ds.writeRawData("data", 4);     // Literal data
    ds << quint32(2880000);         // Data length. 120 sec at 12 kHz

    // WSPR decoder process
    connect(&m_WSPRdecoderProcess, &QProcess::readyReadStandardOutput,
            this, &RXWSPR::accumulateWSPROutput);
    connect(&m_WSPRdecoderProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RXWSPR::WSPRDecoderFinished);
    connect(&m_WSPRdecoderProcess, &QProcess::errorOccurred,
            this, &RXWSPR::WSPRDecoderError);

    // FST4W decoder process
    connect(&m_FST4WdecoderProcess, &QProcess::readyReadStandardOutput,
            this, &RXWSPR::accumulateFST4WOutput);
    connect(&m_FST4WdecoderProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RXWSPR::FST4WDecoderFinished);
    connect(&m_FST4WdecoderProcess, &QProcess::errorOccurred,
            this, &RXWSPR::FST4WDecoderError);

    // Setup Multicast UDP socket for receiving audio samples
    connect(&m_rxSocket, &QUdpSocket::readyRead, this, &RXWSPR::udpRead);
    if (!m_rxSocket.bind(QHostAddress::AnyIPv4,
                           m_multicastPort.toInt(), QUdpSocket::ShareAddress))
    {
        qInfo() << "IPv4 multicast socket bind failed";
        m_initError = true;
        return;
    }

    QNetworkInterface iface = QNetworkInterface::interfaceFromName(m_multicastInterface);
    if (!m_rxSocket.joinMulticastGroup(QHostAddress(m_multicastGroup), iface))
    {
        qInfo() << "IPv4 multicast group join failed";
        m_initError = true;
        return;
    }

    // 50 ms time tick for audio recording sync
    connect(&m_tickTimer, &QTimer::timeout, this, &RXWSPR::timeTick);
    m_tickTimer.start(50);

    // WSPRNet upload timer
    connect(&m_uploadTimer, &QTimer::timeout, this, &RXWSPR::uploadSpots);
    m_uploadTimer.start(3000);
}

void RXWSPR::quit()
{
    emit finished();
}

void RXWSPR::aboutToQuitApp()
{

}

void RXWSPR::timeTick()
{
    time_t seconds;
    seconds = time(NULL);

    if (!m_recTrigger && seconds % 120 == 0)
    {
        m_time = seconds;
        m_audioData.clear();
        m_recTrigger = true;
    }
}

// Multicast socket callback
void RXWSPR::udpRead()
{
    QByteArray datagram;
    unsigned char *data;

    // Process incoming datagrams
    while (m_rxSocket.hasPendingDatagrams())
    {
        // Read audio from the multicast socket
        datagram.resize(m_rxSocket.pendingDatagramSize());
        m_rxSocket.readDatagram(datagram.data(), datagram.size());
        data = (unsigned char *)datagram.constData();

        if (m_recTrigger)
        {
            m_audioData.append((const char *)data, datagram.size());
        }

        if (m_recTrigger && m_audioData.size() > m_recSize)
        {
            QByteArray pcmAudioData;
            pcmAudioData.reserve(2880000); // 120 sec * 12 kHz * 2 bytes

            if (m_sampleRate == 48000)
            {
                // Downsample from 48 to 12 kHz and prepare buffer of LE16 ints.
                // No filtering is done here since audio is already filtered.
                for (int i = 0; i < m_recSize; i+=8)
                {
                    pcmAudioData.append(m_audioData.at(i));
                    pcmAudioData.append(m_audioData.at(i+1));
                }
            }
            else
            {
                // Already at 12 kHz, copy samples directly
                pcmAudioData.append(m_audioData.constData(), m_recSize);
            }

            // Fill up wave file to 120 seconds of samples
            int remainingBytes = 2880000 - pcmAudioData.size();
            if (remainingBytes > 0)
            {
                pcmAudioData.append(QByteArray(remainingBytes, '\0'));
            }

            m_wavFileName = QDateTime::fromSecsSinceEpoch(m_time, Qt::OffsetFromUTC).toString("yyMMdd_hhmm") + ".wav";
            m_date = QDateTime::fromSecsSinceEpoch(m_time, Qt::OffsetFromUTC).toString("yyMMdd");

            QFile f(m_wavFileName);
            if (!f.open(QIODevice::WriteOnly))
            {
                qWarning() << "Failed to open WAV file for writing:" << m_wavFileName;
                m_recTrigger = false;
                return;
            }
            if (f.write(m_waveFileHeader) == -1 || f.write(pcmAudioData) == -1)
            {
                qWarning() << "Failed to write WAV file:" << m_wavFileName;
                f.close();
                QFile::remove(m_wavFileName);
                m_recTrigger = false;
                return;
            }
            f.close();

            // Start WSPR decoder
            QStringList wsprArgs;
            wsprArgs << "-w";
            if (m_deepSearch) wsprArgs << "-d";
            wsprArgs << "-f" << m_baseFrequencyMHz << m_wavFileName;
            m_WSPRdecoderProcess.start(m_WSPRdecoderProgram, wsprArgs);

            // Start FST4W decoder
            QStringList fst4wArgs;
            fst4wArgs << "-W" << "-p" << "120" << "-f" << "1500" << "-F" << "400" << m_wavFileName;
            m_FST4WdecoderProcess.start(m_FST4WdecoderProgram, fst4wArgs);

            compute_dB(&pcmAudioData);

            m_sunLocal.propagate(m_time, getLat(m_myGrid), getLon(m_myGrid));

            QString separator;
            separator = m_wavFileName.mid(0, 11).replace("_", " ");
            separator += " -----------------------------------------";
            separator += QString("%1").arg(m_sunLocal.getEl(), 6, 'f', 1, ' ');
            separator += QString("%1 dB").arg(m_rawPower, 6, 'f', 1, ' ');

            QTextStream stdOut(stdout);
            if (!m_silentStdout) stdOut << separator << Qt::endl;
            writeToLog(separator);

            m_recTrigger = false;
            m_timeSave = m_time;
        }
    }
}

// Enqueue a decoded spot for upload to WSPRNet
void RXWSPR::enqueueSpot(const QDateTime &dt, const QString &time, const QString &sig,
                         const QString &dtOffset, const QString &tqrg, const QString &drift,
                         const QString &tcall, const QString &tgrid, const QString &dbm,
                         const QString &mode)
{

    QUrl url("https://wsprnet.org/post");
    QUrlQuery query;
    query.addQueryItem("function", "wspr");
    query.addQueryItem("rcall", m_myCall);
    query.addQueryItem("rgrid", m_myGrid);
    query.addQueryItem("rqrg",  m_baseFrequencyMHz);
    query.addQueryItem("date",  dt.toUTC().toString("yyMMdd"));
    query.addQueryItem("time",  time);
    query.addQueryItem("sig",   sig);
    query.addQueryItem("dt",    dtOffset);
    query.addQueryItem("tqrg",  tqrg);
    query.addQueryItem("drift", drift);
    query.addQueryItem("tcall", tcall);
    query.addQueryItem("tgrid", tgrid);
    query.addQueryItem("dbm",   dbm);
    query.addQueryItem("version", m_app->applicationVersion());
    query.addQueryItem("mode", mode);
    url.setQuery(query);
    m_urlQueue.enqueue({url.toString(), 0});
}

// Format a spot line for log file and stdout
QString RXWSPR::formatSpotLine(const QString &time, const QString &sig, const QString &dtOffset,
                               const QString &tqrg, const QString &drift, const QString &tcall,
                               const QString &tgrid, const QString &dbm, const QString &mode)
{

    m_sunDX.propagate(m_timeSave + 60, getLat(tgrid), getLon(tgrid));
    QString sunElevation = QString("%1").arg(m_sunDX.getEl(), 6, 'f', 1, ' ');
    QString dxDist = QString("%1 km").arg(dxDistance(tgrid), 6);

    QString s1;
    s1 = m_date;
    s1 += QString(" %1").arg(time, 4);
    s1 += QString(" %1").arg(sig, 3);
    s1 += QString(" %1").arg(dtOffset, 4);
    s1 += QString(" %1").arg(tqrg, 13);
    s1 += QString(" %1").arg(drift, 2);
    s1 += QString("  %1").arg(tcall, -6);
    s1 += QString(" %1").arg(tgrid, 4);
    s1 += QString(" %1").arg(dbm, 2);
    QChar firstChar = tcall.at(0);
    bool isTelemetry = (firstChar == '0' || firstChar == '1' || firstChar == 'Q');
    if (!isTelemetry)
    {
        s1 += sunElevation;
        s1 += dxDist;
    }
    else
    {
        // Balloon telemetry: fixed padding to align mode indicator
        // 6 (sun elevation) + 9 (" XXXXX km") = 15 chars
        s1 += QString(15, ' ');
    }
    s1 += QString("  %1").arg(mode);
    return s1;
}

// Accumulate decoder output as it arrives
void RXWSPR::accumulateWSPROutput()
{
    m_WSPROutput.append(m_WSPRdecoderProcess.readAllStandardOutput());
}

void RXWSPR::accumulateFST4WOutput()
{
    m_FST4WOutput.append(m_FST4WdecoderProcess.readAllStandardOutput());
}

// Process WSPR spots after decoder has finished
void RXWSPR::WSPRDecoderFinished(int code, QProcess::ExitStatus status)
{
    Q_UNUSED(code)
    Q_UNUSED(status)

    QDateTime dt = QDateTime::fromSecsSinceEpoch(m_timeSave, Qt::UTC);
    QTextStream stdOut(stdout);

    // Regular expression for extracting the wsprd spots
    QRegularExpression rx("(^\\d{4})\\s+([+-]?\\d+)\\s+([+-]?\\d+\\.\\d+)\\s+(\\d+\\.\\d+)\\s+([+-]?\\d+)\\s+([A-Z0-9]{3,6})\\s+([A-Z]{2}\\d{2})\\s+(\\d+)");

    // Parse decoder results
    QStringList lines = m_WSPROutput.split("\n");
    for (const QString &line : lines)
    {
        QRegularExpressionMatch match = rx.match(line);
        if(match.hasMatch())
        {
            enqueueSpot(
                        dt,                  // Timestamp
                        match.captured(1),  // Time
                        match.captured(2),  // Signal strength
                        match.captured(3),  // DT offset
                        match.captured(4),  // TX frequency
                        match.captured(5),  // Drift
                        match.captured(6),  // TX callsign
                        match.captured(7),  // TX grid
                        match.captured(8),  // TX power dBm
                        "2");               // Mode (WSPR)

            QString s1 = formatSpotLine(
                        match.captured(1),  // Time
                        match.captured(2),  // Signal strength
                        match.captured(3),  // DT offset
                        match.captured(4),  // TX frequency
                        match.captured(5),  // Drift
                        match.captured(6),  // TX callsign
                        match.captured(7),  // TX grid
                        match.captured(8),  // TX power dBm
                        "!");               // Mode (WSPR)

            if (!m_silentStdout) stdOut << s1 << Qt::endl;
            writeToLog(s1);
        }
    }
    m_WSPROutput.clear();

    // Whichever decoder finishes last removes the WAV file
    if (m_FST4WdecoderProcess.state() == QProcess::NotRunning)
    {
        if (!QFileInfo::exists("keepwav"))
        {
            QFile::remove(m_wavFileName);
        }
    }
}

// Process FST4W spots after decoder has finished
void RXWSPR::FST4WDecoderFinished(int code, QProcess::ExitStatus status)
{
    Q_UNUSED(code)
    Q_UNUSED(status)

    QDateTime dt = QDateTime::fromSecsSinceEpoch(m_timeSave, Qt::UTC);
    QTextStream stdOut(stdout);

    // Regular expression for extracting the FST4W spots
    QRegularExpression rx("(^\\d{4})\\s+([+-]?\\d+)\\s+([+-]?\\d+\\.\\d+)\\s+(\\d+)\\s+(.)\\s+([A-Z0-9]{3,6})\\s+([A-Z]{2}\\d{2})\\s+(\\d+)\\s+(\\d+\\.\\d+)");

    // Parse decoder results
    QStringList lines = m_FST4WOutput.split("\n");
    for (const QString &line : lines)
    {
        QRegularExpressionMatch match = rx.match(line);
        if(match.hasMatch())
        {
            double qrg = m_baseFrequencyMHz.toDouble() * 1E6;
            qrg += match.captured(4).toDouble();
            qrg /= 1E6;
            QString qrgString = QString("%1").arg(qrg, 10, 'f', 6);

            enqueueSpot(
                        dt,                  // Timestamp
                        match.captured(1),  // Time
                        match.captured(2),  // Signal strength
                        match.captured(3),  // DT offset
                        qrgString,          // TX frequency
                        "0",                // Drift (not available)
                        match.captured(6),  // TX callsign
                        match.captured(7),  // TX grid
                        match.captured(8),  // TX power dBm
                        "3");               // Mode (FST4W)

            QString s1 = formatSpotLine(
                        match.captured(1),  // Time
                        match.captured(2),  // Signal strength
                        match.captured(3),  // DT offset
                        qrgString,          // TX frequency
                        match.captured(5),  // Drift
                        match.captured(6),  // TX callsign
                        match.captured(7),  // TX grid
                        match.captured(8),  // TX power dBm
                        "@");               // Mode (FST4W)
            s1 += QString(" %1").arg(match.captured(9), 6);

            if (!m_silentStdout) stdOut << s1 << Qt::endl;
            writeToLog(s1);
        }
    }
    m_FST4WOutput.clear();

    // Whichever decoder finishes last removes the WAV file
    if (m_WSPRdecoderProcess.state() == QProcess::NotRunning)
    {
        if (!QFileInfo::exists("keepwav"))
        {
            QFile::remove(m_wavFileName);
        }
    }
}

// Handle decoder process errors
void RXWSPR::WSPRDecoderError(QProcess::ProcessError error)
{
    qWarning() << "WSPR decoder process error:" << error;
}

void RXWSPR::FST4WDecoderError(QProcess::ProcessError error)
{
    qWarning() << "FST4W decoder process error:" << error;
}

// Write a line to the log file
void RXWSPR::writeToLog(const QString &line)
{
    QFile logFile(m_logDir + "/rxwspr_" + m_baseFrequency + ".log");
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        qWarning() << "Failed to open log file:" << logFile.fileName();
        return;
    }
    QTextStream out(&logFile);
    out << line << Qt::endl;
}

// Compute power of audio buffer
void RXWSPR::compute_dB(const QByteArray *data)
{
    int numSamples = data->size() / 2;
    if (numSamples == 0)
    {
        m_rawPower = -99.0;
        return;
    }

    const int16_t *raw = reinterpret_cast<const int16_t*>(data->constData());

    double sum = 0;
    for (int i = 0; i < numSamples; i++)
    {
        int16_t sample = qFromLittleEndian(raw[i]);
        sum += (double)sample * sample;
    }
    m_rawPower = 10 * log10(sum / (double)numSamples);
}

// Send the spot to WSPRNet
void RXWSPR::uploadSpots()
{
    if (m_noUpload || m_urlQueue.isEmpty())
        return;

    m_currentUpload = m_urlQueue.dequeue();
    QUrl url(m_currentUpload.url);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "RXWSPR/" + m_app->applicationVersion().toLatin1());
    m_networkManager.get(request);
}

// HTTP reply
void RXWSPR::networkReply(QNetworkReply *reply)
{
    QString serverResponse = reply->readAll();

    // Check for network errors
    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "Network Error:" << reply->error();
        if (m_currentUpload.retryCount < MAX_UPLOAD_RETRIES)
        {
            m_currentUpload.retryCount++;
            qDebug() << "Retrying upload, attempt" << m_currentUpload.retryCount
                     << "of" << MAX_UPLOAD_RETRIES;
            m_urlQueue.enqueue(m_currentUpload);
        }
        else
        {
            qWarning() << "Upload failed after" << MAX_UPLOAD_RETRIES << "retries, spot lost";
        }
        reply->deleteLater();
        return;
    }

    // Check for the expected reply from the server
    if (!serverResponse.contains("spot(s) added"))
    {
        qDebug() << "Server did not return expected response";
        if (m_currentUpload.retryCount < MAX_UPLOAD_RETRIES)
        {
            m_currentUpload.retryCount++;
            qDebug() << "Retrying upload, attempt" << m_currentUpload.retryCount
                     << "of" << MAX_UPLOAD_RETRIES;
            m_urlQueue.enqueue(m_currentUpload);
        }
        else
        {
            qWarning() << "Upload failed after" << MAX_UPLOAD_RETRIES << "retries, spot lost";
        }
    }

    reply->deleteLater();
}

float RXWSPR::getLat(const QString &locatorString)
{
    QString padded = locatorString.toUpper() + "ll";
    QByteArray latin = padded.toLatin1();
    const char *locator = latin.constData();

    float fieldLat = (float)(locator[1] - 65) * 10;
    float squareLat = (float)(locator[3] - 48);
    float subsquareLat = ((float)(locator[5] - 65) + 0.5) / 24;

    return fieldLat + squareLat + subsquareLat - 90;
}

float RXWSPR::getLon(const QString &locatorString)
{
    QString padded = locatorString.toUpper() + "ll";
    QByteArray latin = padded.toLatin1();
    const char *locator = latin.constData();

    float fieldLng = (float)(locator[0] - 65) * 20;
    float squareLng = (float)(locator[2] - 48) * 2;
    float subsquareLng = ((float)(locator[4] - 65) + 0.5) / 12;

    return fieldLng + squareLng + subsquareLng - 180;
}

int RXWSPR::dxDistance(const QString &dxGrid)
{
    float myLat = getLat(m_myGrid);
    float myLon = getLon(m_myGrid);
    float dxLat = getLat(dxGrid);
    float dxLon = getLon(dxGrid);

    float dLat = qDegreesToRadians(myLat - dxLat);
    float dLon = qDegreesToRadians(myLon - dxLon);
    float fromLat = qDegreesToRadians(myLat);
    float toLat = qDegreesToRadians(dxLat);
    float a = qPow(qSin(dLat / 2), 2) + qPow(qSin(dLon / 2), 2) * qCos(fromLat) * qCos(toLat);
    float b = 2 * qAtan2(qSqrt(a), qSqrt(1 - a));
    float y = dLon * qCos(fromLat) * qCos(toLat);
    float x = qSin(toLat) - qSin(fromLat) * qCos(b);
    float az = qAtan2(y, x);

    if (az < 0)
        az += 2 * M_PI;

    az = qRadiansToDegrees(az);

    return b * 6371;
}
