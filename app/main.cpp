#include "utils/error.h"
#include "utils/queue.h"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <istream>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <vector>
// FFmpeg
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

#define STREAM_FRAME_RATE 25
#define STREAM_DURATION 10.0
#define STREAM_CODEC AV_CODEC_ID_H264
std::condition_variable cVar;
std::vector<std::thread> thread_pool;
struct membuf : std::streambuf
{
	membuf(char const *base, size_t size)
	{
		char *p(const_cast<char *>(base));
		this->setg(p, p, p + size);
	}
};

struct imemstream : virtual membuf, std::istream
{
	imemstream(char const *base, size_t size)
			: membuf(base, size), std::istream(static_cast<std::streambuf *>(this)) {}
};

typedef struct ReadInterval
{
	int id;							///< identifier
	int64_t start, end; ///< start, end in second/AV_TIME_BASE units
	int has_start, has_end;
	int start_is_offset, end_is_offset;
	int duration_frames;
} ReadInterval;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt,
											 const char *tag)
{
	AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

	printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s "
				 "duration_time:%s stream_index:%d\n",
				 tag, av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
				 av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
				 av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
				 pkt->stream_index);
}

int secondsSince(std::__1::chrono::system_clock::time_point since)
{
	auto current = std::chrono::system_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(current - since).count();
}

static int read_interval_packets(AVCodecContext *codecCtx,
																 AVFormatContext *inFmtCtx, AVStream *in_stream,
																 ReadInterval *interval, int64_t *cur_ts,
																 std::function<void(AVPacket)> callback)
{
	AVFormatContext *fmt_ctx = inFmtCtx;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	int ret = 0, i = 0, frame_count = 0;
	int64_t start = -INT64_MAX, end = interval->end;
	int has_start = 0, has_end = interval->has_end && !interval->end_is_offset;
	int width, height;
	width = in_stream->codecpar->width;
	height = in_stream->codecpar->height;

	av_log(NULL, AV_LOG_VERBOSE, "Processing read interval ");

	if (interval->has_start)
	{
		int64_t target;
		if (interval->start_is_offset)
		{
			if (*cur_ts == AV_NOPTS_VALUE)
			{
				av_log(NULL, AV_LOG_ERROR,
							 "Could not seek to relative position since current "
							 "timestamp is not defined\n");
				ret = AVERROR(EINVAL);
				goto end;
			}
			target = *cur_ts + interval->start;
		}
		else
		{
			target = interval->start;
		}
		if ((ret = avformat_seek_file(fmt_ctx, -1, -INT64_MAX, target, INT64_MAX,
																	0)) < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Could not seek to position %" PRId64 ": %s\n",
						 interval->start, av_err2str(ret));
			goto end;
		}
	}

	pkt = av_packet_alloc();
	if (!pkt)
	{
		ret = AVERROR(ENOMEM);
		goto end;
	}

	while (!av_read_frame(fmt_ctx, pkt))
	{
		auto start_ts = std::chrono::system_clock::now();
		if (pkt->stream_index == in_stream->index)
		{
			AVRational tb = in_stream->time_base;
			int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (pts != AV_NOPTS_VALUE)
				*cur_ts = av_rescale_q(pts, tb, AV_TIME_BASE_Q);

			if (!has_start && *cur_ts != AV_NOPTS_VALUE)
			{
				start = *cur_ts;
				has_start = 1;
			}

			if (has_start && !has_end && interval->end_is_offset)
			{
				end = start + interval->end;
				has_end = 1;
			}

			if (interval->end_is_offset && interval->duration_frames)
			{
				if (frame_count >= interval->end)
					break;
			}
			else if (has_end && *cur_ts != AV_NOPTS_VALUE && *cur_ts >= end)
			{
				break;
			}

			frame_count++;
			std::cout << "decode" << std::endl;
			std::unique_ptr<AVPacket> ptr = std::make_unique<AVPacket>(*pkt);
			thread_pool.emplace_back(std::thread(callback, *ptr));
			std::cout << "readed, " << secondsSince(start_ts) << "ms\n";
		}
	}
	av_packet_unref(pkt);
	// Flush remaining frames that are cached in the decoder

end:
	av_frame_free(&frame);
	av_packet_free(&pkt);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Could not read packets in interval ");
	}
	return ret;
}

typedef struct Input
{
	const char *inFile;
	AVFormatContext *inFmtCtx;
	const AVInputFormat *inFmt;
	const AVCodec *inCodec;
	AVStream *in_stream;
	AVCodecContext *inCodecCtx;
} Input;

int readloop(Input *input, std::function<void(AVPacket)> callback)
{
	int ret;
	int64_t cur_ts;
	ReadInterval interval = (ReadInterval){.has_start = 0, .has_end = 0};
	const AVCodec *pCodec;
	AVCodecContext *pCodecCtx;
	int width, height;
	width = input->in_stream->codecpar->width;
	height = input->in_stream->codecpar->height;

	ret = av_read_play(input->inFmtCtx);
	if (ret < 0)
	{
		HandleError(ret, "cannot start read from input");
		goto exitLoop;
	}
	pCodec = avcodec_find_decoder(input->in_stream->codecpar->codec_id);
	pCodecCtx = avcodec_alloc_context3(pCodec);
	ret = avcodec_parameters_to_context(pCodecCtx, input->in_stream->codecpar);
	if (ret < 0)
	{
		std::cout << ret << std::endl;
		HandleError(ret, "cannot avcodec_parameters_to_context");
		goto exitLoop;
	}

	ret = avcodec_open2(pCodecCtx, pCodec, nullptr);
	if (ret < 0)
	{
		std::cout << ret << std::endl;
		HandleError(ret, "cannot avcodec_open");
		goto exitLoop;
	}

	cur_ts = input->in_stream->start_time;
	ret = read_interval_packets(pCodecCtx, input->inFmtCtx, input->in_stream,
															&interval, &cur_ts, callback);
	if (ret < 0)
	{
		HandleError(ret, "cannot read_interval_packets");
		goto exitLoop;
	}
	ret = av_read_pause(input->inFmtCtx);

exitLoop:
	avformat_close_input(&input->inFmtCtx);
	std::cout << "exit loop\n";
	cVar.notify_one();
	return 0;
}

typedef struct Output
{
	const char *outFile;
	AVFormatContext *outFmtCtx;
	const AVOutputFormat *outFmt;
	const AVCodec *outCodec;
	AVCodecContext *outCodecCtx;
	AVStream *out_stream;
} Output;

void writeLoop(Output *output, concurrent_queue<AVPacket> *buffCh)
{
	int ret;

	std::cout << "start hls writer...\n";
	while (1)
	{
		std::cout << "waiting...\n";
		AVPacket pkt;
		buffCh->wait_and_pop(pkt);
		pkt.stream_index = output->out_stream->index;

		ret = av_interleaved_write_frame(output->outFmtCtx, &pkt);
		if (ret < 0)
		{
			HandleError(ret, "cannot av_write_frame");
		}
	}
};

int init_input(const char *infile, Input &input)
{
	AVFormatContext *inFmtCtx;
	const AVInputFormat *inFmt;
	const AVCodec *inCodec;
	AVStream *in_stream;
	AVCodecContext *inCodecCtx;
	AVDictionary *option = nullptr;
	int ret;

	avformat_network_init();

	inFmtCtx = avformat_alloc_context();

	av_dict_set(&option, "rtsp_transport", "tcp", 0);
	av_dict_set(&option, "re", "", 0);

	inFmt = av_find_input_format("rtsp");
	// open input file context
	ret = avformat_open_input(&inFmtCtx, infile, inFmt, &option);
	if (ret < 0)
	{
		return HandleError(ret, "fail to avforamt_open_input");
	}

	inFmtCtx->flags |= AVFMT_FLAG_GENPTS;
	// retrive input stream information
	ret = avformat_find_stream_info(inFmtCtx, NULL);
	if (ret < 0)
	{
		return HandleError(ret, "fail to avformat_find_stream_info");
	}
	av_dump_format(inFmtCtx, 0, infile, 0);

	// find primary video stream
	ret = av_find_best_stream(inFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &inCodec, 1);
	if (ret < -2)
	{
		std::cerr << "fail to av_find_best_stream: ret=" << ret;
		return ret;
	}
	std::cout << "first video stream at index " << ret << " found\n";

	// in_stream is video stream of input;
	in_stream = inFmtCtx->streams[ret];
	inCodec = avcodec_find_decoder(in_stream->codecpar->codec_id);
	inCodecCtx = avcodec_alloc_context3(inCodec);
	ret = avcodec_parameters_to_context(inCodecCtx, in_stream->codecpar);
	if (ret < 0)
	{
		return HandleError(ret, "cannot avcodec_open2 inCodecCtx");
	}

	ret = avcodec_open2(inCodecCtx, inCodec, NULL);
	if (ret < 0)
	{
		return HandleError(ret, "cannot avcodec_open2 inCodecCtx");
	}

	input = {
			.inFile = infile,
			.inFmtCtx = inFmtCtx,
			.inFmt = inFmt,
			.inCodec = inCodec,
			.in_stream = in_stream,
			.inCodecCtx = inCodecCtx,
	};
	return 0;
}

int init_output(const char *outFile, Output &output, Input *input)
{
	AVFormatContext *outFmtCtx = avformat_alloc_context();
	const AVOutputFormat *outFmt;
	const AVCodec *outCodec;
	AVCodecContext *outCodecCtx;
	AVStream *out_stream;
	int ret;

	outCodec = avcodec_find_encoder(STREAM_CODEC);

	outFmt = av_guess_format("hls", outFile, NULL);
	ret = avformat_alloc_output_context2(&outFmtCtx, outFmt, "hls", outFile);
	if (ret < 0)
	{
		return HandleError(ret, "failed to avformat_alloc_output_context2");
	}
	if (!outFmtCtx)
	{
		std::cout << "could not create output context\n";
		return 2;
	}

	outFmtCtx->video_codec_id = STREAM_CODEC;
	out_stream = avformat_new_stream(outFmtCtx, outCodec);
	if (!out_stream)
	{
		std::cout << "failed to allocating output stream\n";
		return 1;
	}
	else
	{
		out_stream->time_base = (AVRational){1, 90000};
	}

	ret = avcodec_parameters_copy(out_stream->codecpar, input->in_stream->codecpar);
	if (ret < 0)
	{
		std::cout << "failed to copy codec parameters\n";
		return 1;
	}

	av_dump_format(outFmtCtx, 0, outFile, 1);
	if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&outFmtCtx->pb, outFile, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			return HandleError(ret, "failed to avio_open");
		}
	}

	outCodecCtx = avcodec_alloc_context3(outCodec);
	outCodecCtx->pix_fmt = AV_PIX_FMT_YUV422P;
	outCodecCtx->time_base = (AVRational){1, 90000};
	outCodecCtx->height = input->in_stream->codecpar->height;
	outCodecCtx->width = input->in_stream->codecpar->width;
	outCodecCtx->framerate = input->in_stream->codecpar->framerate;

	ret = avcodec_open2(outCodecCtx, outCodec, NULL);
	if (ret < 0)
	{
		return HandleError(ret, "failed to avcodec_open2, outCodecCtx");
	}
	ret = avcodec_parameters_to_context(outCodecCtx, out_stream->codecpar);
	if (ret < 0)
	{
		return HandleError(ret,
											 "failed to avcodec_parameters_to_context, outCodecCtx");
	}

	AVDictionary *outOption = nullptr;
	av_dict_set(&outOption, "hls_flags", "delete_segments", AV_DICT_APPEND);
	ret = avformat_write_header(outFmtCtx, &outOption);
	if (ret < 0)
	{
		return HandleError(ret, "failed to avformat_write_header");
	}

	output = {
			.outFile = outFile,
			.outFmtCtx = outFmtCtx,
			.outFmt = outFmt,
			.outCodec = outCodec,
			.outCodecCtx = outCodecCtx,
			.out_stream = out_stream,
	};
}

void clean_input(Input *input)
{
	avformat_free_context(input->inFmtCtx);
	avformat_close_input(&input->inFmtCtx);
	avcodec_free_context(&input->inCodecCtx);
}
void clean_output(Output *output)
{
	avformat_free_context(output->outFmtCtx);
	avcodec_free_context(&output->outCodecCtx);
}

void print_input_metadata(Input input)
{
	std::cout << "infile: " << input.inFile << "\n"
						<< "format: " << input.inFmtCtx->iformat->name << "\n"
						<< "vcodec: " << input.inCodec->name << "\n"
						<< "size:   " << input.in_stream->codecpar->width << 'x'
						<< input.in_stream->codecpar->height << "\n"
						<< "stream index" << input.in_stream->index << "\n"
						<< "fps:    " << av_q2d(input.in_stream->codecpar->framerate)
						<< "\n"
						<< "length: "
						<< av_rescale_q(input.in_stream->duration,
														input.in_stream->time_base, {1, 1000}) /
									 1000.
						<< " [sec]\n"
						<< "frame:  " << input.in_stream->nb_frames << "\n"
						<< std::flush;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		std::cout << "Usage:" << argv[0] << "<in_rtsp_url>" << std::endl;
		return 0;
	}
	int ret;

	// ########################### INIT INPUT ######################

	Input input;
	ret = init_input(argv[1], input);
	if (ret < 0)
	{
		return ret;
	}
	// ############################################################

	// ################## INIT OUTPUT #############################

	Output output;
	ret = init_output(argv[2], output, &input);
	if (ret < 0)
	{
		return ret;
	}

	// ############################################################

	concurrent_queue<AVPacket> buffCh;
	concurrent_counter counter;

	auto callback = [&buffCh](AVPacket pkt) -> void
	{ buffCh.push(pkt); };

	std::thread readRTSPThread(readloop, &input, callback);
	std::thread writeHLSThread(writeLoop, &output, &buffCh);

	std::cout << "waiting for end\n";
	for (auto &th : thread_pool)
	{
		th.join();
	}

	readRTSPThread.join();
	writeHLSThread.join();

	clean_input(&input);
	clean_output(&output);
	std::terminate();
	return 1;
}
