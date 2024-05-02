// FFmpeg
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
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
#include <unistd.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <streambuf>
#include <string>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <vector>

#define STREAM_FRAME_RATE 10
#define STREAM_CODEC AV_CODEC_ID_H264

std::condition_variable cVar;
std::vector<std::thread> thread_pool;
concurrent_logger logger;

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

int microSince(std::__1::chrono::system_clock::time_point since)
{
	auto current = std::chrono::system_clock::now();
	return std::chrono::duration_cast<std::chrono::microseconds>(current - since)
			.count();
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

void clean_input(Input *input)
{
	avformat_free_context(input->inFmtCtx);
	avformat_close_input(&input->inFmtCtx);
	avcodec_free_context(&input->inCodecCtx);
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

int read_loop(Input *input, std::function<void(AVFrame)> callback)
{
	int ret;
	AVPacket *pkt = NULL;
	int frame_count = 0;
	int log_count = 0;
	int num_looped = 0;
	// int64_t first_pts = AV_NOPTS_VALUE;
	// int64_t last_pts = AV_NOPTS_VALUE;

	auto start_ts = std::chrono::system_clock::now();

	// ########## Start network-related read ##########
	/* ret = av_read_play(input->inFmtCtx); */
	/* if (ret < 0) */
	/* { */
	/* 	HandleError(ret, "cannot start read from input"); */
	/* 	goto end; */
	// ###############################################
	/* } */

	std::cout << "Processing read interval\n";

	// ############################ READ THE STREAM
	// #######################################
	pkt = av_packet_alloc();
	if (!pkt)
	{
		ret = AVERROR(ENOMEM);
		goto end;
	}

	// ################### READ AVFRAME #####################

	while (true)
	{
		ret = av_read_frame(input->inFmtCtx, pkt);
		if (ret < 0)
		{
			avio_seek(input->inFmtCtx->pb, 0, SEEK_SET);
			avformat_seek_file(input->inFmtCtx, input->in_stream->index, 0, 0, input->in_stream->duration, 0);
			num_looped += 1;
			continue;
		}

		if (pkt->stream_index != input->in_stream->index)
		{
			continue;
		}
		pkt->dts = pkt->dts + num_looped * input->in_stream->duration;
		pkt->pts = pkt->pts + num_looped * input->in_stream->duration;

		ret = avcodec_send_packet(input->inCodecCtx, pkt);
		if (ret < 0)
		{
			if (ret == AVERROR(EAGAIN))
			{
				//  input is not accepted in the current state - user must read output with avcodec_receive_frame()
				// (once all output is read, the packet should be resent, and the call will not fail with EAGAIN)
				continue;
			}
			else if (ret == AVERROR(EOF))
			{
				break;
			}
			HandleError(ret, "[IN] failed to avcodec_send_packet");
			goto end;
		}

		AVFrame *frame;
		frame = av_frame_alloc();
		if (!frame)
		{
			std::cout << "[IN] failed to alloc frame\n";
		}

		frame->format = input->inCodecCtx->pix_fmt;
		frame->width = input->inCodecCtx->width;
		frame->height = input->inCodecCtx->height;
		ret = av_frame_get_buffer(frame, 0);
		if (ret < 0)
		{
			HandleError(ret, "[IN] failed to av_frame_get_buffer");
			exit(1);
		}

		while (ret >= 0)
		{
			auto start_read = std::chrono::system_clock::now();
			ret = av_frame_make_writable(frame);
			if (ret < 0)
			{
				HandleError(ret, "[IN] cannot av_frame_get_buffer");
				break;
			}

			ret = avcodec_receive_frame(input->inCodecCtx, frame);
			if (ret < 0)
			{
				if (ret == AVERROR_EOF)
					goto end;

				if (ret == AVERROR(EAGAIN))
				{
					av_frame_unref(frame);
					av_freep(frame);
					break;
				}

				HandleError(ret, "[IN] failed to avcodec_receive_frame");
				goto end;
			}

			callback(*frame);

			frame_count += 1;
			// ############## send the log to concurrent logger ##############
			int length = snprintf(nullptr, 0, "[IN] %s---%ffps", input->inFile,
														float(frame_count * 1e6 / (microSince(start_ts))));
			char *fps = new char[length + 1];
			snprintf(fps, length + 1, "[IN] %s---%ffps", input->inFile,
							 float(frame_count * 1e6 / (microSince(start_ts))));
			logger.write(std::move(fps));
			// ###############################################################

			// since read time is smaller than frame time, we should sleep  an amount of time to achive ops equal to fps
			// frame_time = 1/ fps;
			// sleep_time = 1/fps - time_since(start_read);
			auto ms_time_sleep = (frame->duration ? (frame->duration * 1000 / STREAM_FRAME_RATE) : (1e6 / (STREAM_FRAME_RATE))) - microSince(start_read);
			usleep(ms_time_sleep);
		}
	}

	// ####################################################################################

end:
	std::cout << "exit loop\n";
	av_packet_unref(pkt);
	av_packet_free(&pkt);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "[IN] Could not read packets in interval ");
	}
	return ret;
	if (ret < 0)
	{
		HandleError(ret, "[IN] cannot read_interval_packets");
		goto end;
	}
	// ########## Stop network-related read ##########
	// ret = av_read_pause(input->inFmtCtx);
	// ###############################################
	cVar.notify_one();
	clean_input(input);
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

void clean_output(Output *output)
{
	avformat_free_context(output->outFmtCtx);
	avcodec_free_context(&output->outCodecCtx);
}

void write_loop(Output *output, concurrent_queue<AVFrame> *buffCh)
{
	int ret;
	auto start_ts = std::chrono::system_clock::now();
	int frame_count;
	int log_count = 0;
	int64_t first_pts = AV_NOPTS_VALUE;
	int64_t last_pts = AV_NOPTS_VALUE;

	// ############# READ FRAME #############
	int next_pts = 0;
	while (true)
	{
		AVFrame frame;
		buffCh->wait_and_pop(frame);

		AVFrame *decodec_frame = av_frame_alloc();
		decodec_frame->width = frame.width;
		decodec_frame->height = frame.height;
		decodec_frame->format = frame.format;
		ret = av_frame_copy_props(decodec_frame, &frame);
		if (ret < 0)
		{
			HandleError(ret, "[OUT] cannot av_frame_copy_props");
		}
		ret = av_frame_get_buffer(decodec_frame, 0);
		if (ret < 0)
		{
			HandleError(ret, "[OUT] cannot make frame writeable av_frame_make_writeable");
		};
		ret = av_frame_make_writable(decodec_frame);
		if (ret < 0)
		{
			HandleError(ret, "[OUT] cannot make frame writeable av_frame_make_writeable");
		}
		ret = av_frame_copy(decodec_frame, &frame);
		if (ret < 0)
		{
			HandleError(ret, "[OUT] cannot make frame writeable av_frame_make_writeable");
		}
		decodec_frame->pts = next_pts++;

		if (decodec_frame->duration == 0)
		{
			continue;
			// exit(1);
		}

		// TODO:  return -22 code with this handler (frame or outCodecCtx)
		ret = avcodec_send_frame(output->outCodecCtx, decodec_frame);
		if (ret < 0)
		{
			if (ret == AVERROR(EOF))
			{
				HandleError(ret, "[OUT] end of file in avcodec_send_frame");
				break;
			}
			if (ret == AVERROR(EAGAIN))
			{
				continue;
			}
			HandleError(ret, "[OUT] failed to avcodec_send_frame");
			break;
		}

		while (ret >= 0)
		{
			auto start_write = std::chrono::system_clock::now();
			AVPacket *pkt = av_packet_alloc();
			ret = avcodec_receive_packet(output->outCodecCtx, pkt);
			if (ret < 0)
			{
				if (ret == AVERROR(EOF))
				{
					std::cout << "[OUT] outCodecCtx is flushed\n";
					goto end;
				}

				if (ret == AVERROR(EAGAIN))
				{
					break;
				}
				HandleError(ret, "[OUT] failed to avcodec_receive_packet");
				goto end;
			}

			if (first_pts == AV_NOPTS_VALUE)
			{
				start_ts = std::chrono::system_clock::now();
				first_pts = pkt->pts;
			}
			frame_count += 1;

			pkt->stream_index = output->out_stream->index;
			av_packet_rescale_ts(pkt,
													 output->outCodecCtx->time_base,
													 output->out_stream->time_base);

			// FIXME: pkt duration is 0
			if (pkt->duration == 0)
			{
				pkt->duration = 1000;
			}
			ret = av_interleaved_write_frame(output->outFmtCtx, pkt);
			if (ret < 0)
			{
				HandleError(ret, "[OUT] cannot av_write_frame");
				av_packet_unref(pkt);
				continue;
			}

			// ############## send the log to concurrent logger ##############
			int length = snprintf(nullptr, 0, "[OUT] %s---%ffps", output->outFile,
														float(frame_count * 1e6 / (microSince(start_ts))));
			char *fps = new char[length + 1];
			snprintf(fps, length + 1, "[OUT] %s---%ffps", output->outFile,
							 float(frame_count * 1e6 / (microSince(start_ts))));
			logger.write(std::move(fps));
			// ###############################################################
			av_packet_unref(pkt);

			// since the processing time is smaller than frame time, we could sleep an amount of time
			// frame_time 1 / fps;
			// sleep_time = 1/fps - time_since(start_write)
			// auto ms_time_sleep = 1e6 / STREAM_FRAME_RATE - microSince(start_write);
			// usleep(ms_time_sleep);

			auto ms_time_sleep = (pkt->duration ? (pkt->duration * 1000 / STREAM_FRAME_RATE) : (1e6 / (STREAM_FRAME_RATE))) - microSince(start_write);
			usleep(ms_time_sleep);
		}
	}

	// ######################################
end:
	clean_output(output);
};

int init_input(const char *infile, const char *format, Input &input)
{
	AVFormatContext *inFmtCtx;
	const AVInputFormat *inFmt;
	const AVCodec *inCodec;
	AVStream *in_stream;
	AVCodecContext *inCodecCtx;
	AVDictionary *option = nullptr;
	int ret;

	// ########### Network-relate bootstrap ###########
	// avformat_network_init();
	// ################################################

	inFmtCtx = avformat_alloc_context();
	// ################# Init-Option #################
	//
	// av_dict_set(&option, "rtsp_transport", "tcp", 0);
	av_dict_set(&option, "framerate", "10", 0);
	av_dict_set(&option, "streamloop", "-1", 0);
	// ###############################################
	inFmt = av_find_input_format(format);
	// open input file context
	// ret = avformat_open_input(&inFmtCtx, infile, nullptr, nullptr);
	ret = avformat_open_input(&inFmtCtx, infile, inFmt, &option);
	if (ret < 0)
	{
		return HandleError(ret, "[IN] fail to avforamt_open_input");
	}
	inFmtCtx->flags |= AVFMT_FLAG_GENPTS;
	// retrive input stream information
	ret = avformat_find_stream_info(inFmtCtx, NULL);
	if (ret < 0)
	{
		return HandleError(ret, "[IN] fail to avformat_find_stream_info");
	}

	// ####################### find primary video stream #######################
	ret = av_find_best_stream(inFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &inCodec, 1);
	if (ret < -2)
	{
		std::cerr << "[IN] fail to av_find_best_stream: ret=" << ret;
		return ret;
	}
	// in_stream is video stream of input;
	in_stream = inFmtCtx->streams[ret];
	// ##############################################

	// ####################### INIT CONTEXT  #######################
	inCodec = avcodec_find_decoder(in_stream->codecpar->codec_id);
	inCodecCtx = avcodec_alloc_context3(inCodec);
	ret = avcodec_parameters_to_context(inCodecCtx, in_stream->codecpar);
	if (ret < 0)
	{
		return HandleError(ret, "[IN] cannot avcodec_parameters_to_context");
	}

	inCodecCtx->framerate = av_guess_frame_rate(inFmtCtx, in_stream, NULL);
	inCodecCtx->pkt_timebase = in_stream->time_base;
	inCodecCtx->time_base = in_stream->time_base;

	ret = avcodec_open2(inCodecCtx, inCodec, NULL);
	if (ret < 0)
	{
		return HandleError(ret, "[IN] cannot avcodec_open2");
	}
	// ##############################################

	input = {
			.inFile = infile,
			.inFmtCtx = inFmtCtx,
			.inFmt = inFmt,
			.inCodec = inCodec,
			.in_stream = in_stream,
			.inCodecCtx = inCodecCtx,
	};
	print_input_metadata(input);
	return 0;
}

int init_output(const char *outFile, const char *format, Output &output,
								Input *input)
{
	AVFormatContext *outFmtCtx = avformat_alloc_context();
	const AVOutputFormat *outFmt;
	const AVCodec *outCodec;
	AVCodecContext *outCodecCtx;
	AVStream *out_stream;
	int ret;

	// ################# INIT CONTEXT #################
	outCodec = avcodec_find_encoder(input->inCodecCtx->codec_id);
	outFmt = av_guess_format(format, outFile, NULL);
	ret = avformat_alloc_output_context2(&outFmtCtx, outFmt, format, outFile);
	if (ret < 0)
	{
		return HandleError(ret, "[OUT] failed to avformat_alloc_output_context2");
	}
	if (!outFmtCtx)
	{
		std::cout << "[OUT] could not create output context\n";
		return 2;
	}

	outFmtCtx->video_codec_id = STREAM_CODEC;
	out_stream = avformat_new_stream(outFmtCtx, outCodec);
	if (!out_stream)
	{
		std::cout << "[OUT] failed to allocating output stream\n";
		return 1;
	}
	else
	{
		out_stream->time_base = (AVRational){1, 90000};
	}

	ret =
			avcodec_parameters_copy(out_stream->codecpar, input->in_stream->codecpar);
	if (ret < 0)
	{
		std::cout << "[OUT] failed to copy codec parameters\n";
		return 1;
	}

	// av_dump_format(outFmtCtx, 0, outFile, 1);
	if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&outFmtCtx->pb, outFile, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			return HandleError(ret, "[OUT] failed to avio_open");
		}
	}

	outCodecCtx = avcodec_alloc_context3(outCodec);
	ret = avcodec_parameters_to_context(outCodecCtx, input->in_stream->codecpar);
	if (ret < 0)
	{
		return HandleError(ret, "[OUT] cannot avcodec_parameters_to_context");
	}
	if (input->inCodecCtx->codec_type == AVMEDIA_TYPE_VIDEO)
	{
		outCodecCtx->codec_id = outFmtCtx->video_codec_id;
		outCodecCtx->sample_rate = input->inCodecCtx->sample_rate;
		outCodecCtx->height = input->inCodecCtx->height;
		outCodecCtx->width = input->inCodecCtx->width;
		outCodecCtx->sample_aspect_ratio = input->inCodecCtx->sample_aspect_ratio;
		outCodecCtx->pix_fmt = input->inCodecCtx->pix_fmt;
		outCodecCtx->sample_fmt = (input->inCodec)->sample_fmts ? (input->inCodec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		outCodecCtx->bit_rate = 400000;
		outCodecCtx->time_base = (AVRational){1, STREAM_FRAME_RATE};
		outCodecCtx->gop_size = 12; /* emit one intra frame every twelve frames at most */

		if (outFmtCtx->video_codec_id == AV_CODEC_ID_H264)
		{

			av_opt_set(outCodecCtx->priv_data, "preset", "slow", 0);
		}

		out_stream->time_base = (AVRational){1, STREAM_FRAME_RATE};
	}

	if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
		outCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	// ############# init out stream option ############
	// #################################################

	ret = avcodec_open2(outCodecCtx, outCodec, NULL);
	if (ret < 0)
	{
		return HandleError(ret, "[OUT] failed to avcodec_open2");
	}

	ret = avcodec_parameters_from_context(out_stream->codecpar, outCodecCtx);
	if (ret < 0)
	{
		return HandleError(ret,
											 "[OUT] failed to avcodec_parameters_to_context, outCodecCtx");
	}

	if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&outFmtCtx->pb, outFile, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			HandleError(ret, "[OUT] Could not open output file");
			return ret;
		}
	}

	AVDictionary *outOption = nullptr;
	av_dict_set(&outOption, "hls_flags", "delete_segments", AV_DICT_APPEND);

	ret = avformat_write_header(outFmtCtx, &outOption);
	if (ret < 0)
	{
		return HandleError(ret, "[OUT] failed to avformat_write_header");
	}
	// ################################################

	output = {
			.outFile = outFile,
			.outFmtCtx = outFmtCtx,
			.outFmt = outFmt,
			.outCodec = outCodec,
			.outCodecCtx = outCodecCtx,
			.out_stream = out_stream,
	};
	return 0;
}

int main(int argc, char *argv[])
{
	std::vector<std::thread> threads;
	std::thread log_thread(
			[]() -> void
			{
				while (true)
				{
					auto log = logger.read();
					std::cout << log << std::endl;
				}
			});

	switch (argc)
	{
	case 4:
	{
		// in this case we only read the pkt without encoded it

		/* av_log_set_level(AV_LOG_QUIET); */
		int num_thread = std::atoi(argv[1]);
		const char *in_format = argv[2];
		const char *in_file = argv[3];

		std ::cout
				<< "##################### INFORMATION #####################"
				<< "\nnum_thread: " << num_thread
				<< "\nin_format: " << in_format
				<< "\nin_file: " << in_file
				<< "\n#########################################################"
				<< std::endl;

		// start logger thread
		for (int i = 0; i < num_thread; i++)
		{
			std::thread scope_thread(
					[&in_file, &in_format, i]() -> void
					{
						// ########################### INIT INPUT ######################
						Input input;
						int ret;
						ret = init_input(in_file, in_format, input);
						if (ret < 0)
						{
							return;
						}
						// ############################################################

						auto callback = [](AVFrame frame) mutable -> void
						{
							return;
						};

						std::thread readThread(read_loop, &input, callback);
						readThread.join();
					});

			threads.emplace_back(std::move(scope_thread));
		}

		// ################### WAIT-ALL-THREADS ###################
		std::cout << "waiting for end\n";
		for (auto &th : thread_pool)
			th.join();

		for (auto &th : threads)
			th.join();
		// ######################################

		std::terminate();
		return 1;
	}
	case 6:
	{
		// in this case we read the pkt and encoded it to another format

		/* av_log_set_level(AV_LOG_QUIET); */
		int num_thread = std::atoi(argv[1]);
		const char *in_format = argv[2];
		const char *in_file = argv[3];
		const char *out_format = argv[4];
		const char *out_file = argv[5];

		std ::cout
				<< "##################### INFORMATION #####################"
				<< "\nnum_thread: " << num_thread
				<< "\nin_format: " << in_format
				<< "\nin_file: " << in_file
				<< "\nout_format: " << out_format
				<< "\nout_file: " << out_file
				<< "\n#########################################################"
				<< std::endl;

		// start logger thread
		for (int i = 0; i < num_thread; i++)
		{
			std::thread scope_thread(
					[&out_file, &out_format, &in_file, &in_format, i]() -> void
					{
						// ########################### INIT INPUT ######################
						Input input;
						int ret;
						ret = init_input(in_file, in_format, input);
						if (ret < 0)
						{
							return;
						}
						// ############################################################

						// ################## INIT OUTPUT #############################
						Output output;
						int length = snprintf(nullptr, 0, "./out/%d%s", i, out_file);
						char *outputName = new char[length + 1];
						snprintf(outputName, length + 1, "./out/%d%s", i, out_file);

						std::cout << "init output " << outputName << std::endl;
						ret = init_output(outputName, out_format, output, &input);
						if (ret < 0)
						{
							delete[] outputName;
							return;
						}
						// ############################################################

						// ################ INIT Buffer Channel #######################
						concurrent_queue<AVFrame> buffCh;
						// concurrent_queue<AVFrame> buffCh;
						// ############################################################

						auto callback = [&buffCh](AVFrame data) mutable -> void
						{
							buffCh.wait_and_push(data);
						};

						// auto callback = [&buffCh](AVFrame frame) mutable -> void
						// {
						// 	buffCh.wait_and_push(frame);
						// };

						std::thread readThread(read_loop, &input, callback);
						std::thread writeThread(write_loop, &output, &buffCh);

						writeThread.join();
						readThread.join();
						delete[] outputName;
					});

			threads.emplace_back(std::move(scope_thread));
		}

		// ################### WAIT-ALL-THREADS ###################
		std::cout << "waiting for end\n";
		for (auto &th : thread_pool)
			th.join();

		for (auto &th : threads)
			th.join();
		// ######################################

		std::terminate();
		return 1;
	}
	default:
		std::cout << "Usage:" << argv[0] << "<num_stream>"
							<< "<in_format>"
							<< "<in_file>"
							<< "<out_format>"
							<< "<out_file>" << std::endl;
		std::cout << argv[0] << "<num_stream>"
							<< "<in_format>"
							<< "<in_file>"
							<< std::endl;
		return 0;
	}
}
