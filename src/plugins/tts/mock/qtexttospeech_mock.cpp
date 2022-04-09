// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only


#include "qtexttospeech_mock.h"
#include <QtCore/QTimerEvent>
#include <QtCore/qregularexpression.h>

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

QTextToSpeechEngineMock::QTextToSpeechEngineMock(const QVariantMap &parameters, QObject *parent)
    : QTextToSpeechEngine(parent), m_parameters(parameters)
{
    m_locale = availableLocales().first();
    m_voice = availableVoices().first();
    m_state = QTextToSpeech::Ready;
    m_errorReason = QTextToSpeech::ErrorReason::NoError;
}

QTextToSpeechEngineMock::~QTextToSpeechEngineMock()
{
}

QList<QLocale> QTextToSpeechEngineMock::availableLocales() const
{
    QList<QLocale> locales;

    locales << QLocale(QLocale::English, QLocale::UnitedKingdom)
            << QLocale(QLocale::NorwegianBokmal, QLocale::Norway)
            << QLocale(QLocale::Finnish, QLocale::Finland);

    return locales;
}

QList<QVoice> QTextToSpeechEngineMock::availableVoices() const
{
    QList<QVoice> voices;

    const QString voiceData = m_locale.bcp47Name();
    switch (m_locale.language()) {
    case QLocale::English: {
        voices << createVoice("Bob", m_locale, QVoice::Male, QVoice::Adult, voiceData + QLatin1String("-1"))
               << createVoice("Anne", m_locale, QVoice::Female, QVoice::Adult, voiceData + QLatin1String("-2"));
        break;
    }
    case QLocale::NorwegianBokmal:
        voices << createVoice("Eivind", m_locale, QVoice::Male, QVoice::Adult, voiceData + QLatin1String("-1"))
               << createVoice("Kjersti", m_locale, QVoice::Female, QVoice::Adult, voiceData + QLatin1String("-2"));
        break;
    case QLocale::Finnish:
        voices << createVoice("Kari", m_locale, QVoice::Male, QVoice::Adult, voiceData + QLatin1String("-1"))
               << createVoice("Anneli", m_locale, QVoice::Female, QVoice::Adult, voiceData + QLatin1String("-2"));
        break;
    default:
        Q_ASSERT_X(false, "availableVoices", "Unsupported locale!");
        break;
    }
    return voices;
}

void QTextToSpeechEngineMock::say(const QString &text)
{
    m_text = text;
    m_currentIndex = 0;
    m_timer.start(wordTime(), Qt::PreciseTimer, this);
    m_state = QTextToSpeech::Speaking;
    emit stateChanged(m_state);
}

void QTextToSpeechEngineMock::stop(QTextToSpeech::BoundaryHint boundaryHint)
{
    Q_UNUSED(boundaryHint);
    if (m_state == QTextToSpeech::Ready || m_state == QTextToSpeech::Error)
        return;

    Q_ASSERT(m_state == QTextToSpeech::Paused || m_timer.isActive());
    // finish immediately
    m_text.clear();
    m_currentIndex = -1;
    m_timer.stop();

    m_state = QTextToSpeech::Ready;
    emit stateChanged(m_state);
}

void QTextToSpeechEngineMock::pause(QTextToSpeech::BoundaryHint boundaryHint)
{
    Q_UNUSED(boundaryHint);
    if (m_state != QTextToSpeech::Speaking)
        return;

    // implement "pause after word end"
    m_pauseRequested = true;
}

void QTextToSpeechEngineMock::resume()
{
    if (m_state != QTextToSpeech::Paused)
        return;

    m_timer.start(wordTime(), Qt::PreciseTimer, this);
    m_state = QTextToSpeech::Speaking;
    emit stateChanged(m_state);
}

void QTextToSpeechEngineMock::timerEvent(QTimerEvent *e)
{
    if (e->timerId() != m_timer.timerId()) {
        QTextToSpeechEngine::timerEvent(e);
        return;
    }

    Q_ASSERT(m_state == QTextToSpeech::Speaking);
    Q_ASSERT(m_text.length());

    // Find start of next word, skipping punctuations. This is good enough for testing.
    QRegularExpressionMatch match;
    qsizetype nextSpace = m_text.indexOf(QRegularExpression(u"\\W+"_s), m_currentIndex, &match);
    if (nextSpace == -1)
        nextSpace = m_text.length();
    const QString word = m_text.sliced(m_currentIndex, nextSpace - m_currentIndex);
    sayingWord(m_currentIndex, nextSpace - m_currentIndex);
    m_currentIndex = nextSpace + match.captured().length();

    if (m_currentIndex >= m_text.length()) {
        // done speaking all words
        m_timer.stop();
        m_state = QTextToSpeech::Ready;
        m_currentIndex = -1;
        emit stateChanged(m_state);
    } else if (m_pauseRequested) {
        m_timer.stop();
        m_state = QTextToSpeech::Paused;
        emit stateChanged(m_state);
    }
    m_pauseRequested = false;
}

double QTextToSpeechEngineMock::rate() const
{
    return m_rate;
}

bool QTextToSpeechEngineMock::setRate(double rate)
{
    m_rate = rate;
    if (m_timer.isActive()) {
        m_timer.stop();
        m_timer.start(wordTime(), Qt::PreciseTimer, this);
    }
    return true;
}

double QTextToSpeechEngineMock::pitch() const
{
    return m_pitch;
}

bool QTextToSpeechEngineMock::setPitch(double pitch)
{
    m_pitch = pitch;
    return true;
}

QLocale QTextToSpeechEngineMock::locale() const
{
    return m_locale;
}

bool QTextToSpeechEngineMock::setLocale(const QLocale &locale)
{
    if (!availableLocales().contains(locale))
        return false;
    m_locale = locale;
    const auto voices = availableVoices();
    if (!voices.contains(m_voice))
        m_voice = voices.isEmpty() ? QVoice() : voices.first();
    return true;
}

double QTextToSpeechEngineMock::volume() const
{
    return m_volume;
}

bool QTextToSpeechEngineMock::setVolume(double volume)
{
    if (volume < 0.0 || volume > 1.0)
        return false;

    m_volume = volume;
    return true;
}

QVoice QTextToSpeechEngineMock::voice() const
{
    return m_voice;
}

bool QTextToSpeechEngineMock::setVoice(const QVoice &voice)
{
    const QString voiceId = voiceData(voice).toString();
    const QLocale voiceLocale = QLocale(voiceId.left(voiceId.lastIndexOf("-")));
    if (!availableLocales().contains(voiceLocale)) {
        qWarning("Engine does not support voice's locale %s",
                 qPrintable(voiceLocale.bcp47Name()));
        return false;
    }
    m_locale = voiceLocale;
    if (!availableVoices().contains(voice)) {
        qWarning("Engine does not support voice %s in the locale %s",
                 qPrintable(voice.name()), qPrintable(voiceLocale.bcp47Name()));
        return false;
    }
    m_voice = voice;
    return true;
}

QTextToSpeech::State QTextToSpeechEngineMock::state() const
{
    return m_state;
}

QTextToSpeech::ErrorReason QTextToSpeechEngineMock::errorReason() const
{
    return m_errorReason;
}

QString QTextToSpeechEngineMock::errorString() const
{
    return m_errorString;
}

QT_END_NAMESPACE
