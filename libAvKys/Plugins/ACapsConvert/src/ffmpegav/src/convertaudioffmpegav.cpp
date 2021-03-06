/* Webcamoid, webcam capture application.
 * Copyright (C) 2011-2017  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include "convertaudioffmpegav.h"

typedef QMap<AkAudioCaps::ChannelLayout, int64_t> ChannelLayoutsMap;

inline ChannelLayoutsMap initChannelFormatsMap()
{
    ChannelLayoutsMap channelLayouts = {
        {AkAudioCaps::Layout_mono  , AV_CH_LAYOUT_MONO  },
        {AkAudioCaps::Layout_stereo, AV_CH_LAYOUT_STEREO}
    };

    return channelLayouts;
}

Q_GLOBAL_STATIC_WITH_ARGS(ChannelLayoutsMap, channelLayouts, (initChannelFormatsMap()))

ConvertAudioFFmpegAV::ConvertAudioFFmpegAV(QObject *parent):
    ConvertAudio(parent)
{
    this->m_resampleContext = NULL;

#ifndef QT_DEBUG
    av_log_set_level(AV_LOG_QUIET);
#endif
}

ConvertAudioFFmpegAV::~ConvertAudioFFmpegAV()
{
    this->uninit();
}

bool ConvertAudioFFmpegAV::init(const AkAudioCaps &caps)
{
    QMutexLocker mutexLocker(&this->m_mutex);
    this->m_caps = caps;
    this->m_resampleContext = avresample_alloc_context();

    return true;
}

AkPacket ConvertAudioFFmpegAV::convert(const AkAudioPacket &packet)
{
    QMutexLocker mutexLocker(&this->m_mutex);

    if (!this->m_caps)
        return AkPacket();

    int64_t iSampleLayout = channelLayouts->value(packet.caps().layout(), 0);

    AVSampleFormat iSampleFormat =
            av_get_sample_fmt(AkAudioCaps::sampleFormatToString(packet.caps().format())
                              .toStdString().c_str());

    int iSampleRate = packet.caps().rate();
    int iNChannels = packet.caps().channels();
    int iNSamples = packet.caps().samples();

    int64_t oSampleLayout = channelLayouts->value(this->m_caps.layout(),
                                                  AV_CH_LAYOUT_STEREO);

    AVSampleFormat oSampleFormat =
            av_get_sample_fmt(AkAudioCaps::sampleFormatToString(this->m_caps.format())
                              .toStdString().c_str());

    int oSampleRate = this->m_caps.rate();
    int oNChannels = this->m_caps.channels();

    // Create input audio frame.
    static AVFrame iFrame;
    memset(&iFrame, 0, sizeof(AVFrame));
    iFrame.format = iSampleFormat;
    iFrame.channels = iNChannels;
    iFrame.channel_layout = uint64_t(iSampleLayout);
    iFrame.sample_rate = iSampleRate;
    iFrame.nb_samples = iNSamples;
    iFrame.pts = packet.pts();

    if (avcodec_fill_audio_frame(&iFrame,
                                 iFrame.channels,
                                 iSampleFormat,
                                 reinterpret_cast<const uint8_t *>(packet.buffer().constData()),
                                 packet.buffer().size(),
                                 1) < 0) {
        return AkPacket();
    }

    // Fill output audio frame.
    AVFrame oFrame;
    memset(&oFrame, 0, sizeof(AVFrame));
    oFrame.format = oSampleFormat;
    oFrame.channels = oNChannels;
    oFrame.channel_layout = uint64_t(oSampleLayout);
    oFrame.sample_rate = oSampleRate;
    oFrame.nb_samples = int(avresample_get_delay(this->m_resampleContext))
                        + iFrame.nb_samples
                        * oSampleRate
                        / iSampleRate
                        + 3;
    oFrame.pts = iFrame.pts * oSampleRate / iSampleRate;

    // Calculate the size of the audio buffer.
    int frameSize = av_samples_get_buffer_size(oFrame.linesize,
                                               oFrame.channels,
                                               oFrame.nb_samples,
                                               oSampleFormat,
                                               1);

    QByteArray oBuffer(frameSize, Qt::Uninitialized);

    if (avcodec_fill_audio_frame(&oFrame,
                                 oFrame.channels,
                                 oSampleFormat,
                                 reinterpret_cast<const uint8_t *>(oBuffer.constData()),
                                 oBuffer.size(),
                                 1) < 0) {
        return AkPacket();
    }

    // convert to destination format
    if (avresample_convert_frame(this->m_resampleContext,
                                 &oFrame,
                                 &iFrame) < 0)
        return AkPacket();

    frameSize = av_samples_get_buffer_size(oFrame.linesize,
                                           oFrame.channels,
                                           oFrame.nb_samples,
                                           oSampleFormat,
                                           1);

    oBuffer.resize(frameSize);

    AkAudioPacket oAudioPacket;
    oAudioPacket.caps() = this->m_caps;
    oAudioPacket.caps().samples() = oFrame.nb_samples;
    oAudioPacket.buffer() = oBuffer;
    oAudioPacket.pts() = oFrame.pts;
    oAudioPacket.timeBase() = AkFrac(1, this->m_caps.rate());
    oAudioPacket.index() = packet.index();
    oAudioPacket.id() = packet.id();

    return oAudioPacket.toPacket();
}

void ConvertAudioFFmpegAV::uninit()
{
    QMutexLocker mutexLocker(&this->m_mutex);
    this->m_caps = AkAudioCaps();

    if (this->m_resampleContext)
        avresample_free(&this->m_resampleContext);
}
