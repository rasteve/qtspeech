// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only

#include "qtexttospeech_sapi.h"

#include <windows.h>
#ifdef Q_CC_MSVC
#pragma warning(disable : 4996)
#endif
#include <sapi.h>
#include <sphelper.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

QT_BEGIN_NAMESPACE

#ifndef SPERR_NO_MORE_ITEMS
# define SPERR_NO_MORE_ITEMS MAKE_SAPI_ERROR(0x039)
#endif

#ifdef Q_CC_MINGW // from sphelper.h

static const GUID CLSD_SpVoice = {0x96749377, 0x3391, 0x11d2,{0x9e, 0xe3, 0x0, 0xc0, 0x4f, 0x79, 0x73, 0x96}};

static inline HRESULT SpGetTokenFromId(const WCHAR *pszTokenId, ISpObjectToken **cpToken, BOOL fCreateIfNotExist = FALSE)
{
    LPUNKNOWN pUnkOuter = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_SpObjectToken, pUnkOuter, CLSCTX_ALL,
                                    __uuidof(ISpObjectToken), reinterpret_cast<void **>(cpToken));
    if (SUCCEEDED(hr))
        hr = (*cpToken)->SetId(NULL, pszTokenId, fCreateIfNotExist);
    return hr;
}

static inline HRESULT SpCreateNewToken(const WCHAR *pszTokenId, ISpObjectToken **ppToken)
{
    // Forcefully create the token
    return SpGetTokenFromId(pszTokenId, ppToken, TRUE);
}

inline void SpClearEvent(SPEVENT *pe)
{
    switch (pe->elParamType) {
    case SPET_LPARAM_IS_TOKEN:
    case SPET_LPARAM_IS_OBJECT:
        reinterpret_cast<IUnknown *>(pe->lParam)->Release();
        break;
    case SPET_LPARAM_IS_POINTER:
    case SPET_LPARAM_IS_STRING:
        CoTaskMemFree(reinterpret_cast<void *>(pe->lParam));
        break;
    case SPET_LPARAM_IS_UNDEFINED:
        break;
    }
}

#endif // Q_CC_MINGW

QTextToSpeechEngineSapi::QTextToSpeechEngineSapi(const QVariantMap &, QObject *)
{
    if (FAILED(::CoInitialize(NULL))) {
        qWarning() << "Init of COM failed";
        return;
    }

    HRESULT hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void **)&m_voice);
    if (!SUCCEEDED(hr)) {
        setError(QTextToSpeech::ErrorReason::Initialization,
                 QCoreApplication::translate("QTextToSpeech",
                                             "Could not initialize text-to-speech engine."));
        return;
    }

    m_voice->SetInterest(SPFEI_ALL_TTS_EVENTS, SPFEI_ALL_TTS_EVENTS);
    m_voice->SetNotifyCallbackInterface(this, 0, 0);
    updateVoices();
    if (m_voices.isEmpty()) {
        setError(QTextToSpeech::ErrorReason::Configuration,
                 QCoreApplication::translate("QTextToSpeech", "No voices available."));
    } else {
        m_state = QTextToSpeech::Ready;
        m_errorReason = QTextToSpeech::ErrorReason::NoError;
    }
}

QTextToSpeechEngineSapi::~QTextToSpeechEngineSapi()
{
    if (m_voice)
        m_voice->Release();
    CoUninitialize();
}

bool QTextToSpeechEngineSapi::isSpeaking() const
{
    SPVOICESTATUS eventStatus;
    m_voice->GetStatus(&eventStatus, NULL);
    return eventStatus.dwRunningState == SPRS_IS_SPEAKING;
}

void QTextToSpeechEngineSapi::say(const QString &text)
{
    if (text.isEmpty())
        return;

    if (m_state != QTextToSpeech::Ready)
        stop(QTextToSpeech::BoundaryHint::Default);

    currentText = text;
    const QString prefix = u"<pitch absmiddle=\"%1\"/>"_qs.arg(m_pitch * 10);
    textOffset = prefix.length();
    currentText.prepend(prefix);

    HRESULT hr = m_voice->Speak(currentText.toStdWString().data(), SPF_ASYNC, NULL);
    if (!SUCCEEDED(hr))
        setError(QTextToSpeech::ErrorReason::Input,
                 QCoreApplication::translate("QTextToSpeech", "Speech synthesizing failure."));
}

void QTextToSpeechEngineSapi::stop(QTextToSpeech::BoundaryHint boundaryHint)
{
    Q_UNUSED(boundaryHint);
    if (m_state == QTextToSpeech::Paused || m_pauseRequested)
        resume();
    m_voice->Speak(NULL, SPF_PURGEBEFORESPEAK, 0);
    currentText.clear();
}

void QTextToSpeechEngineSapi::pause(QTextToSpeech::BoundaryHint boundaryHint)
{
    Q_UNUSED(boundaryHint);
    if (!isSpeaking())
        return;

    // SAPI voices count calls to Pause() and require an equal
    // number of calls to Resume(); we don't want that, so don't
    // call either more than once.
    if (!m_pauseRequested && m_state != QTextToSpeech::Paused) {
        m_pauseRequested = true;
        m_voice->Pause();
    }
}

void QTextToSpeechEngineSapi::resume()
{
    if (m_pauseRequested || m_state == QTextToSpeech::Paused) {
        m_pauseRequested = false;
        m_voice->Resume();
    }
}

bool QTextToSpeechEngineSapi::setPitch(double pitch)
{
    m_pitch = pitch;
    return true;
}

double QTextToSpeechEngineSapi::pitch() const
{
    return m_pitch;
}

bool QTextToSpeechEngineSapi::setRate(double rate)
{
    // -10 to 10
    m_voice->SetRate(long(rate*10));
    return true;
}

double QTextToSpeechEngineSapi::rate() const
{
    long rateValue;
    if (m_voice->GetRate(&rateValue) == S_OK)
        return rateValue / 10.0;
    return -1;
}

bool QTextToSpeechEngineSapi::setVolume(double volume)
{
    // 0 to 1
    m_voice->SetVolume(volume * 100);
    return true;
}

double QTextToSpeechEngineSapi::volume() const
{
    USHORT baseVolume;
    if (m_voice->GetVolume(&baseVolume) == S_OK)
        return baseVolume / 100.0;
    return 0.0;
}

QString QTextToSpeechEngineSapi::voiceId(ISpObjectToken *speechToken) const
{
    HRESULT hr = S_OK;
    LPWSTR vId = nullptr;
    hr = speechToken->GetId(&vId);
    if (FAILED(hr)) {
        qWarning() << "ISpObjectToken::GetId failed";
        return QString();
    }
    return QString::fromWCharArray(vId);
}

QMap<QString, QString> QTextToSpeechEngineSapi::voiceAttributes(ISpObjectToken *speechToken) const
{
    HRESULT hr = S_OK;
    QMap<QString, QString> result;

    ISpDataKey *pAttrKey = nullptr;
    hr = speechToken->OpenKey(L"Attributes", &pAttrKey);
    if (FAILED(hr)) {
        qWarning() << "ISpObjectToken::OpenKeys failed";
        return result;
    }

    // enumerate values
    for (ULONG v = 0; ; v++) {
        LPWSTR val = nullptr;
        hr = pAttrKey->EnumValues(v, &val);
        if (SPERR_NO_MORE_ITEMS == hr) {
            // done
            break;
        } else if (FAILED(hr)) {
            qWarning() << "ISpDataKey::EnumValues failed";
            continue;
        }

        // how do we know whether it's a string or a DWORD?
        LPWSTR data = nullptr;
        hr = pAttrKey->GetStringValue(val, &data);
        if (FAILED(hr)) {
            qWarning() << "ISpDataKey::GetStringValue failed";
            continue;
        }

        if (0 != wcscmp(val, L""))
            result[QString::fromWCharArray(val)] = QString::fromWCharArray(data);

        // FIXME: Do we need to free the memory here?
        CoTaskMemFree(val);
        CoTaskMemFree(data);
    }

    return result;
}

QLocale QTextToSpeechEngineSapi::lcidToLocale(const QString &lcid) const
{
    bool ok;
    LCID locale = lcid.toInt(&ok, 16);
    if (!ok) {
        qWarning() << "Could not convert language attribute to LCID";
        return QLocale();
    }
    const int nchars = GetLocaleInfoW(locale, LOCALE_SISO639LANGNAME, NULL, 0);
    QVarLengthArray<wchar_t, 12> languageCode(nchars);
    GetLocaleInfoW(locale, LOCALE_SISO639LANGNAME, languageCode.data(), nchars);
    return QLocale(QString::fromWCharArray(languageCode.data()));
}

void QTextToSpeechEngineSapi::updateVoices()
{
    HRESULT hr = S_OK;
    ISpObjectToken *cpVoiceToken = nullptr;
    IEnumSpObjectTokens *cpEnum = nullptr;
    ULONG ulCount = 0;

    hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
    if (SUCCEEDED(hr))
        hr = cpEnum->GetCount(&ulCount);

    // Loop through all voices
    while (SUCCEEDED(hr) && ulCount--) {
        if (cpVoiceToken) {
            cpVoiceToken->Release();
            cpVoiceToken = nullptr;
        }
        hr = cpEnum->Next(1, &cpVoiceToken, NULL);

        // Get attributes of the voice
        const QMap<QString, QString> vAttr = voiceAttributes(cpVoiceToken);

        // Transform Windows LCID to QLocale
        const QLocale vLocale = lcidToLocale(vAttr[u"Language"_qs]);

        // Create voice
        const QString name = vAttr[u"Name"_qs];
        const QVoice::Age age = vAttr[u"Age"_qs] == u"Adult"_qs ?  QVoice::Adult : QVoice::Other;
        const QVoice::Gender gender = vAttr[u"Gender"_qs] == u"Male"_qs ? QVoice::Male :
                                      vAttr[u"Gender"_qs] == u"Female"_qs ? QVoice::Female :
                                      QVoice::Unknown;
        // Getting the ID of the voice to set the voice later
        const QVoice voice = createVoice(name, vLocale, gender, age, voiceId(cpVoiceToken));
        m_voices.insert(vLocale, voice);
    }
    if (cpVoiceToken)
        cpVoiceToken->Release();
    cpEnum->Release();
}

QList<QLocale> QTextToSpeechEngineSapi::availableLocales() const
{
    return m_voices.uniqueKeys();
}

bool QTextToSpeechEngineSapi::setLocale(const QLocale &locale)
{
    const QList<QVoice> voicesForLocale = m_voices.values(locale);
    if (voicesForLocale.isEmpty()) {
        setError(QTextToSpeech::ErrorReason::Configuration,
                 QCoreApplication::translate("QTextToSpeech", "No voice found for locale %1.")
                    .arg(locale.bcp47Name()));
        return false;
    }

    setVoice(voicesForLocale[0]);
    return true;
}

QLocale QTextToSpeechEngineSapi::locale() const
{
    // Get current voice id
    ISpObjectToken *cpVoiceToken = nullptr;
    HRESULT hr = m_voice->GetVoice(&cpVoiceToken);
    if (FAILED(hr)) {
        qWarning() << "ISpObjectToken::GetVoice failed";
        return QLocale();
    }
    // read attributes
    const QMap<QString, QString> vAttr = voiceAttributes(cpVoiceToken);
    cpVoiceToken->Release();
    return lcidToLocale(vAttr.value(u"Language"_qs));
}

QList<QVoice> QTextToSpeechEngineSapi::availableVoices() const
{
    return m_voices.values(locale());
}

bool QTextToSpeechEngineSapi::setVoice(const QVoice &voice)
{
    // Convert voice id to null-terminated wide char string
    const QString vId = voiceData(voice).toString();
    QVarLengthArray<wchar_t, 255> tokenId(vId.size() + 1);
    vId.toWCharArray(tokenId.data());
    tokenId[vId.size()] = 0;

    // create the voice token via the id
    HRESULT hr = S_OK;
    ISpObjectToken *cpVoiceToken = nullptr;
    hr = SpCreateNewToken(tokenId.constData(), &cpVoiceToken);
    if (FAILED(hr)) {
        if (cpVoiceToken)
            cpVoiceToken->Release();
        setError(QTextToSpeech::ErrorReason::Configuration,
                 QCoreApplication::translate("QTextToSpeech", "Could not set voice."));
        return false;
    }

    if (m_state != QTextToSpeech::Ready) {
        m_state = QTextToSpeech::Ready;
        emit stateChanged(m_state);
    }

    m_voice->SetVoice(cpVoiceToken);
    cpVoiceToken->Release();
    return true;
}

QVoice QTextToSpeechEngineSapi::voice() const
{
    ISpObjectToken *cpVoiceToken = nullptr;
    HRESULT hr = m_voice->GetVoice(&cpVoiceToken);
    if (FAILED(hr)) {
        qWarning() << "ISpObjectToken::GetVoice failed";
        return QVoice();
    }
    QString vId = voiceId(cpVoiceToken);
    cpVoiceToken->Release();
    for (const QVoice &voice : m_voices) {
        if (voiceData(voice).toString() == vId)
            return voice;
    }
    return QVoice();
}

QTextToSpeech::State QTextToSpeechEngineSapi::state() const
{
    return m_state;
}

void QTextToSpeechEngineSapi::setError(QTextToSpeech::ErrorReason reason, const QString &string)
{
    m_errorReason = reason;
    m_errorString = string;
    if (reason != QTextToSpeech::ErrorReason::NoError)
        return;
    if (m_state != QTextToSpeech::Error) {
        m_state = QTextToSpeech::Error;
        emit stateChanged(m_state);
    }
    emit errorOccurred(m_errorReason, m_errorString);
}

QTextToSpeech::ErrorReason QTextToSpeechEngineSapi::errorReason() const
{
    return m_errorReason;
}

QString QTextToSpeechEngineSapi::errorString() const
{
    return m_errorString;
}

HRESULT QTextToSpeechEngineSapi::NotifyCallback(WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    const QTextToSpeech::State oldState = m_state;

    // we can't use CSpEvent here as it doesn't provide the time offset
    if (ISpEventSource2 *eventSource; S_OK == m_voice->QueryInterface(&eventSource)) {
        SPEVENTEX event;
        ULONG got;
        while (S_OK == eventSource->GetEventsEx(1, &event, &got)) {
            switch (event.eEventId) {
            case SPEI_START_INPUT_STREAM:
                m_state = QTextToSpeech::Speaking;
                break;
            case SPEI_END_INPUT_STREAM:
                m_state = QTextToSpeech::Ready;
                break;
            case SPEI_WORD_BOUNDARY:
                emit sayingWord(event.lParam - textOffset, event.wParam);
                break;
            // these are the other TTS events which might be interesting for us at some point
            case SPEI_SENTENCE_BOUNDARY:
            case SPEI_PHONEME:
            case SPEI_TTS_BOOKMARK:
            case SPEI_VISEME:
            case SPEI_VOICE_CHANGE:
            case SPEI_TTS_AUDIO_LEVEL:
                break;
            // the rest is uninteresting (speech recognition or engine-internal events).
            default:
                break;
            }
            // There is no SpClearEventEx, and while a SPEVENTEX is not a subclass of SPEVENT, the
            // two structs have identical memory layout, plus the extra ullAudioTimeOffset member.
            SpClearEvent(reinterpret_cast<SPEVENT*>(&event));
        }

        eventSource->Release();
    }

    // There are no explicit events for pause/resume, so we have to brute force this ourselves.
    // This means that we transition into pause prematurely, as SAPI typically only pauses in
    // a word- or sentence-break.
    if (m_pauseRequested)
        m_state = QTextToSpeech::Paused;
    else if (m_state == QTextToSpeech::Paused && isSpeaking())
        m_state = QTextToSpeech::Speaking;

    if (m_state != oldState)
        emit stateChanged(m_state);

    return S_OK;
}

QT_END_NAMESPACE
