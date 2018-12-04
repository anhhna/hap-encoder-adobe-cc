
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <numeric>

#define __STDC_CONSTANT_MACROS

extern "C" {
#include <libavutil/channel_layout.h>
}


#include "movie_reader.hpp"


#undef av_err2str
static std::string av_err2str(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, errnum);
    return buffer;
}

extern "C" {
    extern AVInputFormat ff_mov_demuxer;
}
// =======================================================
MovieReader::MovieReader(
    VideoFormat videoFormat,
    MovieReadCallback onRead,
    MovieSeekCallback onSeek,
    MovieErrorCallback onError)
    : videoStreamIdx_(-1), audioStreamIdx_(-1),
      onRead_(onRead), onSeek_(onSeek), onError_(onError)
{
    int ret;

    uint8_t* buffer = (uint8_t*)av_malloc(cAVIOBufferSize);
    if (!buffer)
        throw std::runtime_error("couldn't allocate write buffer");
    AVIOContext *ioContext = avio_alloc_context(
        buffer,               // unsigned char *buffer,
        (int)cAVIOBufferSize, // int buffer_size,
        0,                    // int write_flag,
        this,                 // void *opaque,
        c_onRead,             // int(*read_packet)(void *opaque, uint8_t *buf, int buf_size),
        nullptr,              // int(*write_packet)(void *opaque, uint8_t *buf, int buf_size),
        c_onSeek);            // int64_t(*seek)(void *opaque, int64_t offset, int whence));
    if (!ioContext)
    {
        av_free(buffer);  // not owned by anyone yet :(
        throw std::runtime_error("couldn't allocate io context");
    }
    ioContext_.reset(ioContext);

    /* allocate the output media context */
    AVFormatContext *formatContext = avformat_alloc_context();
    formatContext->iformat = &ff_mov_demuxer;
    formatContext->pb = ioContext_.get();

    ret = avformat_open_input(&formatContext, NULL, NULL, NULL);
    if (ret < 0) {
        throw std::runtime_error("format could not open input");
    }

    formatContext_.reset(formatContext); // and own it

    if ((ret = avformat_find_stream_info(formatContext_.get(), 0)) < 0) {
        throw std::runtime_error("Failed to retrieve input stream information");
    }


    //!!!
    //!!!av_dump_format(formatContext_.get(), 0, "input file", 0);

    //frameRateNumerator_ = formatContext_->
    //frameRateDenominator_ = formatContext_->iformat->

    auto desiredVideoTag = MKTAG(videoFormat[0], videoFormat[1], videoFormat[2], videoFormat[3]);

    for (size_t i = 0; i < formatContext_->nb_streams; i++) {
        AVStream *stream = formatContext_->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;

        switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            audioStreamIdx_ = (int)i;
            break;
        case AVMEDIA_TYPE_VIDEO:
            if (codecpar->codec_tag == desiredVideoTag) {
                videoStreamIdx_       = (int)i;
                width_                = codecpar->width;
                height_               = codecpar->height;
                frameRateNumerator_   = stream->avg_frame_rate.num;
                frameRateDenominator_ = stream->avg_frame_rate.den;
                numFrames_            = stream->nb_frames;
            }
            break;
        default:
            break;
        }
    }

    if (videoStreamIdx_ == -1)
        throw std::runtime_error("could not find video stream");

}

void MovieReader::readVideoFrame(double tFrame, std::vector<uint8_t>& frame)
{
    int64_t timestamp = (int64_t)(tFrame * formatContext_->streams[videoStreamIdx_]->avg_frame_rate.num);
    int ret = av_seek_frame(formatContext_.get(), videoStreamIdx_, timestamp, AVSEEK_FLAG_ANY);
    if (ret < 0)
        throw std::runtime_error(std::string("could not seek to read frame " + std::to_string(tFrame) + " - " + av_err2str(ret)));

    AVPacket pkt;
    ret = av_read_frame(formatContext_.get(), &pkt);
    if (ret < 0)
        throw std::runtime_error(std::string("could not read frame " + std::to_string(tFrame) + " - " + av_err2str(ret)));

    frame.resize(pkt.size);
    std::copy(pkt.data, pkt.data + pkt.size, &frame[0]);

    av_packet_unref(&pkt);
}

MovieReader::~MovieReader()
{
}


int MovieReader::c_onRead(void *context, uint8_t *data, int size)
{
    MovieReader *obj = reinterpret_cast<MovieReader*>(context);
    try
    {
        return (int)obj->onRead_(data, size);
    }
    catch (const std::exception &ex)
    {
        obj->onError_(ex.what());
        return -1;
    }
    catch (...)
    {
        obj->onError_("unhandled exception while writing");
        return -1;
    }
    return size;
}

int64_t MovieReader::c_onSeek(void *context, int64_t seekPos, int whence)
{
    MovieReader *obj = reinterpret_cast<MovieReader*>(context);
    try
    {
        return obj->onSeek_(seekPos, whence);
    }
    catch (const std::exception &ex)
    {
        obj->onError_(ex.what());
        return -1;
    }
    catch (...)
    {
        obj->onError_("unhandled exception while seeking");
        return -1;
    }
}
