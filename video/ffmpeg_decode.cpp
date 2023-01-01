/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define __STDC_LIMIT_MACROS 1
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
}

#include "ffmpeg_decode.hpp"
#include "logging.hpp"
#include "math.hpp"
#include "muglm/muglm_impl.hpp"
#include "muglm/matrix_helper.hpp"
#include "transforms.hpp"
#include "thread_group.hpp"
#include "global_managers.hpp"
#include "thread_priority.hpp"
#include "timer.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#ifdef HAVE_GRANITE_AUDIO
#include "audio_mixer.hpp"
#include "dsp/dsp.hpp"
#endif

#ifndef AV_CHANNEL_LAYOUT_STEREO
// Legacy API.
#define AVChannelLayout uint64_t
#define ch_layout channel_layout
#define AV_CHANNEL_LAYOUT_STEREO AV_CH_LAYOUT_STEREO
#define AV_CHANNEL_LAYOUT_MONO AV_CH_LAYOUT_MONO
#define av_channel_layout_compare(pa, pb) ((*pa) != (*pb))
#endif

namespace Granite
{
struct CodecStream
{
	AVStream *av_stream = nullptr;
	AVCodecContext *av_ctx = nullptr;
};

static void free_av_objects(CodecStream &stream)
{
	if (stream.av_ctx)
		avcodec_free_context(&stream.av_ctx);
}

#ifdef HAVE_GRANITE_AUDIO
struct AVFrameRingStream final : Audio::MixerStream, Util::ThreadSafeIntrusivePtrEnabled<AVFrameRingStream>
{
	AVFrameRingStream(float sample_rate, unsigned num_channels, double timebase);
	~AVFrameRingStream() override;

	float sample_rate;
	unsigned num_channels;
	double timebase;
	double inv_sample_rate_ns;

	bool setup(float mixer_output_rate, unsigned mixer_channels, size_t max_num_frames) override;
	size_t accumulate_samples(float * const *channels, const float *gain, size_t num_frames) noexcept override;
	unsigned get_num_channels() const override;
	float get_sample_rate() const override;
	void dispose() override;

	// Buffering in terms of AVFrame is a little questionable since packet sizes can vary a fair bit,
	// might have to revisit later.
	// In practice, any codec will have a reasonably short packet window (10ms - 20ms),
	// but not too long either.
	enum { Frames = 64, FramesHighWatermark = 48 };
	AVFrame *frames[Frames] = {};
	std::atomic_uint32_t write_count;
	std::atomic_uint32_t read_count;
	std::atomic_uint32_t read_frames_count;
	uint32_t write_frames_count = 0;
	std::atomic_bool complete;
	int packet_frames = 0;
	bool running_state = false;
	unsigned get_num_buffered_audio_frames();
	unsigned get_num_buffered_av_frames();

	struct
	{
		double pts = -1.0;
		int64_t sampled_ns = 0;
	} progress[Frames];
	std::atomic_uint32_t pts_index;

	AVFrame *acquire_write_frame();
	void mark_uncorked_audio_pts();
	void submit_write_frame();
	void mark_complete();
};

AVFrameRingStream::AVFrameRingStream(float sample_rate_, unsigned num_channels_, double timebase_)
		: sample_rate(sample_rate_), num_channels(num_channels_), timebase(timebase_), inv_sample_rate_ns(1e9 / sample_rate)
{
	for (auto &f : frames)
		f = av_frame_alloc();
	write_count = 0;
	read_count = 0;
	read_frames_count = 0;
	pts_index = 0;
	complete = false;
}

void AVFrameRingStream::mark_uncorked_audio_pts()
{
	uint32_t index = (pts_index.load(std::memory_order_acquire) - 1) % Frames;

	// This is not a hazard, we know the mixer thread is done writing here.
	if (progress[index].pts >= 0.0)
		progress[index].sampled_ns = Util::get_current_time_nsecs();
}

bool AVFrameRingStream::setup(float, unsigned mixer_channels, size_t)
{
	// TODO: Could promote mono to stereo.
	return mixer_channels == num_channels;
}

void AVFrameRingStream::dispose()
{
	release_reference();
}

float AVFrameRingStream::get_sample_rate() const
{
	return sample_rate;
}

unsigned AVFrameRingStream::get_num_channels() const
{
	return num_channels;
}

size_t AVFrameRingStream::accumulate_samples(float *const *channels, const float *gain, size_t num_frames) noexcept
{
	// Hold back playback until we have buffered enough to avoid instant underrun.
	uint32_t written_count = write_count.load(std::memory_order_acquire);
	if (!running_state)
	{
		int buffered_audio_frames = 0;
		for (uint32_t i = 0; i < written_count; i++)
			buffered_audio_frames += frames[i]->nb_samples;

		// Wait until we have 50ms worth of audio buffered to avoid a potential instant underrun.
		if (float(buffered_audio_frames) <= sample_rate * 0.05f)
			return complete.load(std::memory_order_relaxed) ? 0 : num_frames;
		running_state = true;
	}

	size_t write_offset = 0;
	uint32_t buffer_index = read_count.load(std::memory_order_relaxed);

	while (write_offset < num_frames && buffer_index != written_count)
	{
		size_t to_write = num_frames - write_offset;
		auto *frame = frames[buffer_index % Frames];
		if (packet_frames < frame->nb_samples)
		{
			to_write = std::min<size_t>(frame->nb_samples - packet_frames, to_write);

			// Update latest audio PTS.
			// TODO: Might have to also mark when this PTS was written,
			// along with some way to compensate for latency.
			// However, the audio backend latency is fairly low and is comparable with
			// video latency, so we might be able to get away with simply ignoring it.
			if (packet_frames == 0)
			{
				uint32_t pts_buffer_index = pts_index.load(std::memory_order_relaxed);
				auto new_pts = double(frame->pts) * timebase;
				auto &p = progress[pts_buffer_index % Frames];
				p.pts = new_pts;
				p.sampled_ns = Util::get_current_time_nsecs();
				// If we're deep into mixing, we need to compensate for the fact that this PTS will be delayed
				// a little when played back.
				p.sampled_ns += int64_t(double(write_offset) * inv_sample_rate_ns);
				pts_index.store(pts_buffer_index + 1, std::memory_order_release);
			}

			if (frame->format == AV_SAMPLE_FMT_FLTP || (frame->format == AV_SAMPLE_FMT_FLT && num_channels == 1))
			{
				for (unsigned i = 0; i < num_channels; i++)
				{
					Audio::DSP::accumulate_channel(
							channels[i] + write_offset,
							reinterpret_cast<const float *>(frame->data[i]) + packet_frames,
							gain[i], to_write);
				}
			}
			else if (frame->format == AV_SAMPLE_FMT_FLT)
			{
				// We only care about supporting STEREO here.
				Audio::DSP::accumulate_channel_deinterleave_stereo(
						channels[0] + write_offset, channels[1] + write_offset,
						reinterpret_cast<const float *>(frame->data[0]) + 2 * packet_frames,
						gain, to_write);
			}
			else if (frame->format == AV_SAMPLE_FMT_S32P || (frame->format == AV_SAMPLE_FMT_S32 && num_channels == 1))
			{
				for (unsigned i = 0; i < num_channels; i++)
				{
					Audio::DSP::accumulate_channel_s32(
							channels[i] + write_offset,
							reinterpret_cast<const int32_t *>(frame->data[i]) + packet_frames,
							gain[i], to_write);
				}
			}
			else if (frame->format == AV_SAMPLE_FMT_S32)
			{
				Audio::DSP::accumulate_channel_deinterleave_stereo_s32(
						channels[0] + write_offset, channels[1] + write_offset,
						reinterpret_cast<const int32_t *>(frame->data[0]) + 2 * packet_frames,
						gain, to_write);
			}
			else if (frame->format == AV_SAMPLE_FMT_S16P || (frame->format == AV_SAMPLE_FMT_S16 && num_channels == 1))
			{
				for (unsigned i = 0; i < num_channels; i++)
				{
					Audio::DSP::accumulate_channel_s16(
							channels[i] + write_offset,
							reinterpret_cast<const int16_t *>(frame->data[i]) + packet_frames,
							gain[i], to_write);
				}
			}
			else if (frame->format == AV_SAMPLE_FMT_S16)
			{
				Audio::DSP::accumulate_channel_deinterleave_stereo_s16(
						channels[0] + write_offset, channels[1] + write_offset,
						reinterpret_cast<const int16_t *>(frame->data[0]) + 2 * packet_frames,
						gain, to_write);
			}

			packet_frames += int(to_write);
			write_offset += to_write;
		}
		else
		{
			// We've consumed this packet, retire it.
			packet_frames = 0;
			buffer_index++;
		}
	}

	read_count.store(buffer_index, std::memory_order_release);
	read_frames_count.store(read_frames_count.load(std::memory_order_relaxed) + uint32_t(write_offset),
	                        std::memory_order_release);

	return complete.load(std::memory_order_relaxed) ? write_offset : num_frames;
}

AVFrame *AVFrameRingStream::acquire_write_frame()
{
	auto index = write_count.load(std::memory_order_relaxed) % Frames;
	auto *frame = frames[index];
	return frame;
}

void AVFrameRingStream::submit_write_frame()
{
	uint32_t index = write_count.load(std::memory_order_relaxed);
	write_count.store(index + 1, std::memory_order_release);
	write_frames_count += uint32_t(frames[index % Frames]->nb_samples);
}

void AVFrameRingStream::mark_complete()
{
	complete.store(true, std::memory_order_relaxed);
}

unsigned AVFrameRingStream::get_num_buffered_av_frames()
{
	uint32_t read_index = read_count.load(std::memory_order_acquire);
	uint32_t count = write_count - read_index;
	return count;
}

unsigned AVFrameRingStream::get_num_buffered_audio_frames()
{
	uint32_t result = write_frames_count - read_frames_count.load(std::memory_order_acquire);
	VK_ASSERT(result < 0x80000000u);
	return result;
}

AVFrameRingStream::~AVFrameRingStream()
{
	for (auto *f : frames)
		av_frame_free(&f);
}
#endif

struct VideoDecoder::Impl
{
	Vulkan::Device *device = nullptr;
	Audio::Mixer *mixer = nullptr;
	DecodeOptions opts;
	AVFormatContext *av_format_ctx = nullptr;
	AVPacket *av_pkt = nullptr;
	CodecStream video, audio;

	enum class ImageState
	{
		Idle, // Was released by application.
		Locked, // Decode thread locked this image.
		Ready, // Can be acquired.
		Acquired // Acquired, can be released.
	};

	struct DecodedImage
	{
		Vulkan::ImageHandle rgb_image;
		Vulkan::ImageViewHandle rgb_storage_view;
		Vulkan::ImageHandle planes[3];

		Vulkan::Semaphore sem_to_client;
		Vulkan::Semaphore sem_from_client;
		uint64_t idle_order = 0;
		uint64_t lock_order = 0;

		double pts = 0.0;
		ImageState state = ImageState::Idle;
	};
	std::vector<DecodedImage> video_queue;
	uint64_t idle_timestamps = 0;
	bool is_video_eof = false;
	bool is_audio_eof = false;
	bool is_flushing = false;
	bool acquire_is_eof = false;

	VkFormat plane_formats[3] = {};
	unsigned plane_subsample_log2[3] = {};
	unsigned num_planes = 0;
	float unorm_rescale = 1.0f;

	bool init(Audio::Mixer *mixer, const char *path, const DecodeOptions &opts);
	unsigned get_width() const;
	unsigned get_height() const;
	bool init_video_decoder();
	bool init_audio_decoder();

	bool begin_device_context(Vulkan::Device *device);
	void end_device_context();
	bool play();
	bool get_stream_id(Audio::StreamID &id) const;
	bool stop();
	bool seek(double ts);
	void set_paused(bool enable);
	bool get_paused() const;

	double get_estimated_audio_playback_timestamp(double elapsed_time);
	double get_estimated_audio_playback_timestamp_raw();

	bool acquire_video_frame(VideoFrame &frame);
	int try_acquire_video_frame(VideoFrame &frame);
	void release_video_frame(unsigned index, Vulkan::Semaphore sem);

	bool decode_video_packet(AVPacket *pkt);
	bool decode_audio_packet(AVPacket *pkt);

	int find_idle_decode_video_frame_locked() const;
	int find_acquire_video_frame_locked() const;

	unsigned acquire_decode_video_frame();
	void process_video_frame(AVFrame *frame);
	void process_video_frame_in_task(unsigned frame, AVFrame *av_frame);

	void flush_codecs();

	struct Push
	{
		uvec2 resolution;
		vec2 inv_resolution;
		vec2 chroma_siting;
		float unorm_rescale;
	};
	Push push = {};

	struct UBO
	{
		mat4 yuv_to_rgb;
		mat4 primary_conversion;
	};
	UBO ubo = {};

	// The decoding thread.
	std::thread decode_thread;
	std::condition_variable cond;
	std::mutex lock;
	std::mutex iteration_lock;
	bool teardown = false;
	bool acquire_blocking = false;
	TaskSignal video_upload_signal;
	uint64_t video_upload_count = 0;
	ThreadGroup *thread_group = nullptr;
	TaskGroupHandle upload_dependency;
	void thread_main();
	bool iterate();
	bool should_iterate_locked();

	void init_yuv_to_rgb();
	void setup_yuv_format_planes();
	void begin_audio_stream();
	AVPixelFormat active_upload_pix_fmt = AV_PIX_FMT_NONE;

	~Impl();

#ifdef HAVE_GRANITE_AUDIO
	Audio::StreamID stream_id;
	AVFrameRingStream *stream = nullptr;
#endif

	struct
	{
		const AVCodecHWConfig *config = nullptr;
		AVBufferRef *device = nullptr;
	} hw;

	bool is_paused = false;

	double smooth_elapsed = 0.0;
	double smooth_pts = 0.0;
};

int VideoDecoder::Impl::find_idle_decode_video_frame_locked() const
{
	int best_index = -1;
	for (size_t i = 0, n = video_queue.size(); i < n; i++)
	{
		auto &img = video_queue[i];
		if (img.state == ImageState::Idle &&
		    (best_index < 0 || img.idle_order < video_queue[best_index].idle_order))
		{
			best_index = int(i);
		}
	}

	return best_index;
}

unsigned VideoDecoder::Impl::acquire_decode_video_frame()
{
	int best_index;

	do
	{
		std::unique_lock<std::mutex> holder{lock};
		best_index = find_idle_decode_video_frame_locked();

		// We have no choice but to trample on a frame we already decoded.
		// This can happen if audio is running ahead for whatever reason,
		// and we need to start catching up due to massive stutters or similar.
		// For this reason, we should consume the produced image with lowest PTS.
		if (best_index < 0)
		{
			for (size_t i = 0, n = video_queue.size(); i < n; i++)
			{
				if (video_queue[i].state == ImageState::Ready &&
				    (best_index < 0 || video_queue[i].pts < video_queue[best_index].pts))
				{
					best_index = int(i);
					LOGW("FFmpeg decode: Trampling on decoded frame.\n");
				}
			}
		}

		// We have completely stalled.
		if (best_index < 0)
		{
			uint64_t wait_count = UINT64_MAX;
			for (size_t i = 0, n = video_queue.size(); i < n; i++)
				if (video_queue[i].state == ImageState::Locked)
					wait_count = std::min<uint64_t>(wait_count, video_queue[i].lock_order);

			// Completing the task needs to take lock.
			holder.unlock();

			// Could happen if application is acquiring images beyond all reason.
			VK_ASSERT(wait_count != UINT64_MAX);
			if (wait_count != UINT64_MAX)
				video_upload_signal.wait_until_at_least(wait_count);
		}
	} while (best_index < 0);

	auto &img = video_queue[best_index];

	// Defer allocating the planar images until we know for sure what kind of
	// format we're dealing with.
	if (!img.rgb_image)
	{
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(video.av_ctx->width,
		                                                        video.av_ctx->height,
		                                                        VK_FORMAT_R8G8B8A8_SRGB);
		info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.flags = VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		info.misc = Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_GRAPHICS_BIT |
		            Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_COMPUTE_BIT |
		            Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_GRAPHICS_BIT |
		            Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_TRANSFER_BIT |
		            Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
		if (opts.mipgen)
			info.levels = 0;
		img.rgb_image = device->create_image(info);

		Vulkan::ImageViewCreateInfo view = {};
		view.image = img.rgb_image.get();
		view.format = VK_FORMAT_R8G8B8A8_UNORM;
		view.layers = 1;
		view.levels = 1;
		view.view_type = VK_IMAGE_VIEW_TYPE_2D;
		img.rgb_storage_view = device->create_image_view(view);
	}

	return best_index;
}

void VideoDecoder::Impl::init_yuv_to_rgb()
{
	push.resolution = uvec2(video.av_ctx->width, video.av_ctx->height);
	push.inv_resolution = vec2(1.0f / float(video.av_ctx->width),
	                           1.0f / float(video.av_ctx->height));

	switch (video.av_ctx->chroma_sample_location)
	{
	case AVCHROMA_LOC_TOPLEFT:
		push.chroma_siting = vec2(1.0f) * push.inv_resolution;
		break;

	case AVCHROMA_LOC_TOP:
		push.chroma_siting = vec2(0.5f, 1.0f) * push.inv_resolution;
		break;

	case AVCHROMA_LOC_LEFT:
		push.chroma_siting = vec2(1.0f, 0.5f) * push.inv_resolution;
		break;

	case AVCHROMA_LOC_CENTER:
	default:
		push.chroma_siting = vec2(0.5f) * push.inv_resolution;
		break;

	case AVCHROMA_LOC_BOTTOMLEFT:
		push.chroma_siting = vec2(1.0f, 0.0f) * push.inv_resolution;
		break;

	case AVCHROMA_LOC_BOTTOM:
		push.chroma_siting = vec2(0.5f, 0.0f) * push.inv_resolution;
		break;
	}

	bool full_range = video.av_ctx->color_range == AVCOL_RANGE_JPEG;

	// 16.3.9 from Vulkan spec.
	// YCbCr samplers is not universally supported,
	// so we need to do this translation ourselves.
	// This is ok, since we have to do EOTF and primary conversion manually either way,
	// and those are not supported.

	int luma_offset = full_range ? 0 : 16;
	int chroma_narrow_range = 224;
	int luma_narrow_range = 219;
	int bit_depth = av_pix_fmt_desc_get(video.av_ctx->pix_fmt)->comp[0].depth;
	if (bit_depth > 8)
	{
		luma_offset <<= (bit_depth - 8);
		luma_narrow_range <<= (bit_depth - 8);
		chroma_narrow_range <<= (bit_depth - 8);
	}

	// 10-bit and 12-bit YUV need special consideration for how to do scale and bias.
	float midpoint = float(1 << (bit_depth - 1));
	float unorm_range = float((1 << bit_depth) - 1);
	float unorm_divider = 1.0f / unorm_range;
	float chroma_shift = -midpoint * unorm_divider;

	const float luma_scale = float(unorm_range) / float(luma_narrow_range);
	const float chroma_scale = float(unorm_range) / float(chroma_narrow_range);

	const vec3 yuv_bias = vec3(float(-luma_offset) * unorm_divider, chroma_shift, chroma_shift);
	const vec3 yuv_scale = full_range ? vec3(1.0f) :
	                       vec3(luma_scale, chroma_scale, chroma_scale);

	AVColorSpace col_space = video.av_ctx->colorspace;
	if (col_space == AVCOL_SPC_UNSPECIFIED)
	{
		// The common case is when we have an unspecified color space.
		// We have to deduce the color space based on resolution since NTSC, PAL, HD and UHD all
		// have different conversions.
		if (video.av_ctx->height < 625)
			col_space = AVCOL_SPC_SMPTE170M; // 525 line NTSC
		else if (video.av_ctx->height < 720)
			col_space = AVCOL_SPC_BT470BG; // 625 line PAL
		else if (video.av_ctx->height < 2160)
			col_space = AVCOL_SPC_BT709; // BT709 HD
		else
			col_space = AVCOL_SPC_BT2020_CL; // UHD
	}

	// Khronos Data Format Specification 15.1.1:

	// EOTF is based on BT.2087 which recommends that an approximation to BT.1886 is used
	// for purposes of color conversion.
	// E = pow(E', 2.4).
	// We apply this to everything for now, but might not be correct for SD content, especially PAL.
	// Can be adjusted as needed with spec constants.
	// AVCodecContext::color_rtc can signal a specific EOTF,
	// but I've only seen UNSPECIFIED here.

	const Primaries bt709 = {
			{ 0.640f, 0.330f },
			{ 0.300f, 0.600f },
			{ 0.150f, 0.060f },
			{ 0.3127f, 0.3290f },
	};

	const Primaries bt601_625 = {
			{ 0.640f, 0.330f },
			{ 0.290f, 0.600f },
			{ 0.150f, 0.060f },
			{ 0.3127f, 0.3290f },
	};

	const Primaries bt601_525 = {
			{ 0.630f, 0.340f },
			{ 0.310f, 0.595f },
			{ 0.155f, 0.070f },
			{ 0.3127f, 0.3290f },
	};

	const Primaries bt2020 = {
			{ 0.708f, 0.292f },
			{ 0.170f, 0.797f },
			{ 0.131f, 0.046f },
			{ 0.3127f, 0.3290f },
	};

	switch (col_space)
	{
	default:
		LOGW("Unknown color space: %u, assuming BT.709.\n", col_space);
		// fallthrough
	case AVCOL_SPC_BT709:
		ubo.yuv_to_rgb = mat4(mat3(vec3(1.0f, 1.0f, 1.0f),
		                           vec3(0.0f, -0.13397432f / 0.7152f, 1.8556f),
		                           vec3(1.5748f, -0.33480248f / 0.7152f, 0.0f)));
		ubo.primary_conversion = mat4(1.0f); // sRGB shares primaries.
		break;

	case AVCOL_SPC_BT2020_CL:
	case AVCOL_SPC_BT2020_NCL:
		ubo.yuv_to_rgb = mat4(mat3(vec3(1.0f, 1.0f, 1.0f),
		                           vec3(0.0f, -0.11156702f / 0.6780f, 1.8814f),
		                           vec3(1.4746f, -0.38737742f / 0.6780f, 0.0f)));
		ubo.primary_conversion = mat4(inverse(compute_xyz_matrix(bt709)) * compute_xyz_matrix(bt2020));
		break;

	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_BT470BG:
		// BT.601. Primaries differ between EBU and SMPTE.
		ubo.yuv_to_rgb = mat4(mat3(vec3(1.0f, 1.0f, 1.0f),
		                           vec3(0.0f, -0.202008f / 0.587f, 1.772f),
		                           vec3(1.402f, -0.419198f / 0.587f, 0.0f)));
		ubo.primary_conversion = mat4(inverse(compute_xyz_matrix(bt709)) *
		                              compute_xyz_matrix(col_space == AVCOL_SPC_BT470BG ? bt601_625 : bt601_525));
		break;

	case AVCOL_SPC_SMPTE240M:
		ubo.yuv_to_rgb = mat4(mat3(vec3(1.0f, 1.0f, 1.0f),
		                           vec3(0.0f, -0.58862f / 0.701f, 1.826f),
		                           vec3(1.576f, -0.334112f / 0.701f, 0.0f)));
		ubo.primary_conversion = mat4(inverse(compute_xyz_matrix(bt709)) *
		                              compute_xyz_matrix(bt601_525));
		break;
	}

	ubo.yuv_to_rgb = ubo.yuv_to_rgb * scale(yuv_scale) * translate(yuv_bias);
}

bool VideoDecoder::Impl::init_audio_decoder()
{
	// This is fine. We can support no-audio files.
	int ret;
	if ((ret = av_find_best_stream(av_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)) < 0)
		return true;

	audio.av_stream = av_format_ctx->streams[ret];
	const AVCodec *codec = avcodec_find_decoder(audio.av_stream->codecpar->codec_id);
	if (!codec)
	{
		LOGE("Failed to find codec.\n");
		return false;
	}

	audio.av_ctx = avcodec_alloc_context3(codec);
	if (!audio.av_ctx)
	{
		LOGE("Failed to allocate codec context.\n");
		return false;
	}

	if (avcodec_parameters_to_context(audio.av_ctx, audio.av_stream->codecpar) < 0)
	{
		LOGE("Failed to copy codec parameters.\n");
		return false;
	}

	if (avcodec_open2(audio.av_ctx, codec, nullptr) < 0)
	{
		LOGE("Failed to open codec.\n");
		return false;
	}

	const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
	const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	if (av_channel_layout_compare(&audio.av_ctx->ch_layout, &mono) != 0 &&
	    av_channel_layout_compare(&audio.av_ctx->ch_layout, &stereo) != 0)
	{
		LOGE("Unrecognized audio channel layout.\n");
		return false;
	}

	switch (audio.av_ctx->sample_fmt)
	{
	case AV_SAMPLE_FMT_S16:
	case AV_SAMPLE_FMT_S16P:
	case AV_SAMPLE_FMT_S32:
	case AV_SAMPLE_FMT_S32P:
	case AV_SAMPLE_FMT_FLT:
	case AV_SAMPLE_FMT_FLTP:
		break;

	default:
		LOGE("Unsupported sample format.\n");
		return false;
	}

	return true;
}

void VideoDecoder::Impl::begin_audio_stream()
{
#ifdef HAVE_GRANITE_AUDIO
	if (!audio.av_ctx)
		return;

	stream = new AVFrameRingStream(
			float(audio.av_ctx->sample_rate),
#ifdef ch_layout
			audio.av_ctx->channels,
#else
			audio.av_ctx->ch_layout.nb_channels,
#endif
			av_q2d(audio.av_stream->time_base));

	stream->add_reference();
	stream_id = mixer->add_mixer_stream(stream, !is_paused);
	if (!stream_id)
	{
		stream->release_reference();
		stream = nullptr;
	}

	// Reset PTS smoothing.
	smooth_elapsed = 0.0;
	smooth_pts = 0.0;
#endif
}

bool VideoDecoder::Impl::init_video_decoder()
{
	int ret;
	if ((ret = av_find_best_stream(av_format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)) < 0)
	{
		LOGE("Failed to find best stream.\n");
		return false;
	}

	video.av_stream = av_format_ctx->streams[ret];

	const AVCodec *codec = avcodec_find_decoder(video.av_stream->codecpar->codec_id);
	if (!codec)
	{
		LOGE("Failed to find codec.\n");
		return false;
	}

	for (int i = 0; ; i++)
	{
		const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
		if (!config)
			break;

		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
		{
			AVBufferRef *hw_dev = nullptr;
			if (av_hwdevice_ctx_create(&hw_dev, config->device_type, nullptr, nullptr, 0) == 0)
			{
				LOGI("Created HW decoder: %s.\n", av_hwdevice_get_type_name(config->device_type));
				hw.config = config;
				hw.device = hw_dev;
				break;
			}
		}
	}

	video.av_ctx = avcodec_alloc_context3(codec);
	if (!video.av_ctx)
	{
		LOGE("Failed to allocate codec context.\n");
		return false;
	}

	if (avcodec_parameters_to_context(video.av_ctx, video.av_stream->codecpar) < 0)
	{
		LOGE("Failed to copy codec parameters.\n");
		return false;
	}

	video.av_ctx->opaque = this;

	if (hw.device)
	{
		video.av_ctx->get_format = [](AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) -> AVPixelFormat
		{
			auto *self = static_cast<Impl *>(ctx->opaque);
			while (*pix_fmts != AV_PIX_FMT_NONE)
			{
				if (*pix_fmts == self->hw.config->pix_fmt)
					return *pix_fmts;
				pix_fmts++;
			}

			return AV_PIX_FMT_NONE;
		};
		video.av_ctx->hw_device_ctx = av_buffer_ref(hw.device);
	}

	init_yuv_to_rgb();

	if (avcodec_open2(video.av_ctx, codec, nullptr) < 0)
	{
		LOGE("Failed to open codec.\n");
		return false;
	}

	double fps = av_q2d(video.av_stream->avg_frame_rate);
	// If FPS is not specified assume 60 as a "worst case scenario".
	if (fps == 0.0)
		fps = 60.0f;

	// We need to buffer up enough frames without running into starvation scenarios.
	// The low watermark for audio buffer is 100ms, which is where we will start forcing video frames to be decoded.
	// If we allocate 200ms of video frames to absorb any jank, we should be fine.
	// In a steady state, we will keep the audio buffer at 200ms saturation.
	// It would be possible to add new video frames dynamically,
	// but we don't want to end up in an unbounded memory usage situation, especially VRAM.

	unsigned num_frames = std::max<unsigned>(unsigned(muglm::ceil(fps * 0.2)), 4);
	video_queue.resize(num_frames);

	return true;
}

unsigned VideoDecoder::Impl::get_width() const
{
	return video.av_ctx->width;
}

unsigned VideoDecoder::Impl::get_height() const
{
	return video.av_ctx->height;
}

bool VideoDecoder::Impl::init(Audio::Mixer *mixer_, const char *path, const DecodeOptions &opts_)
{
	mixer = mixer_;
	opts = opts_;

	if (avformat_open_input(&av_format_ctx, path, nullptr, nullptr) < 0)
	{
		LOGE("Failed to open input %s.\n", path);
		return false;
	}

	if (avformat_find_stream_info(av_format_ctx, nullptr) < 0)
	{
		LOGE("Failed to find stream info.\n");
		return false;
	}

	if (!init_video_decoder())
		return false;
	if (mixer && !init_audio_decoder())
		return false;

	if (!(av_pkt = av_packet_alloc()))
	{
		LOGE("Failed to allocate packet.\n");
		return false;
	}

	return true;
}

int VideoDecoder::Impl::find_acquire_video_frame_locked() const
{
	// Want frame with lowest PTS and in Ready state.
	int best_index = -1;
	for (size_t i = 0, n = video_queue.size(); i < n; i++)
	{
		auto &img = video_queue[i];
		if (img.state == ImageState::Ready &&
		    (best_index < 0 || (img.pts < video_queue[best_index].pts)))
		{
			best_index = int(i);
		}
	}

	return best_index;
}

void VideoDecoder::Impl::setup_yuv_format_planes()
{
	// TODO: Is there a way to make this data driven from the FFmpeg API?
	// In practice, this isn't going to be used as a fully general purpose
	// media player, so we only need to consider the FMVs that an application ships.

	unorm_rescale = 1.0f;

	switch (active_upload_pix_fmt)
	{
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUV420P:
		plane_formats[0] = VK_FORMAT_R8_UNORM;
		plane_formats[1] = VK_FORMAT_R8_UNORM;
		plane_formats[2] = VK_FORMAT_R8_UNORM;
		plane_subsample_log2[0] = 0;
		plane_subsample_log2[1] = active_upload_pix_fmt == AV_PIX_FMT_YUV420P ? 1 : 0;
		plane_subsample_log2[2] = active_upload_pix_fmt == AV_PIX_FMT_YUV420P ? 1 : 0;
		num_planes = 3;
		break;

	case AV_PIX_FMT_NV12:
	case AV_PIX_FMT_NV21:
		// NV21 is done by spec constant swizzle.
		plane_formats[0] = VK_FORMAT_R8_UNORM;
		plane_formats[1] = VK_FORMAT_R8G8_UNORM;
		num_planes = 2;
		plane_subsample_log2[0] = 0;
		plane_subsample_log2[1] = 1;
		break;

	case AV_PIX_FMT_P010:
#ifdef AV_PIX_FMT_P410
	case AV_PIX_FMT_P410:
#endif
		plane_formats[0] = VK_FORMAT_R16_UNORM;
		plane_formats[1] = VK_FORMAT_R16G16_UNORM;
		num_planes = 2;
		plane_subsample_log2[0] = 0;
		plane_subsample_log2[1] = active_upload_pix_fmt == AV_PIX_FMT_P010 ? 1 : 0;
		// The low bits are zero, rescale to 1.0 range.
		unorm_rescale = float(0xffff) / float(1023 << 6);
		break;

	case AV_PIX_FMT_YUV420P10:
	case AV_PIX_FMT_YUV444P10:
		plane_formats[0] = VK_FORMAT_R16_UNORM;
		plane_formats[1] = VK_FORMAT_R16_UNORM;
		plane_formats[2] = VK_FORMAT_R16_UNORM;
		num_planes = 3;
		plane_subsample_log2[0] = 0;
		plane_subsample_log2[1] = active_upload_pix_fmt == AV_PIX_FMT_YUV420P10 ? 1 : 0;
		plane_subsample_log2[2] = active_upload_pix_fmt == AV_PIX_FMT_YUV420P10 ? 1 : 0;
		// The high bits are zero, rescale to 1.0 range.
		unorm_rescale = float(0xffff) / float(1023);
		break;

	case AV_PIX_FMT_P016:
#ifdef AV_PIX_FMT_P416
	case AV_PIX_FMT_P416:
#endif
		plane_formats[0] = VK_FORMAT_R16_UNORM;
		plane_formats[1] = VK_FORMAT_R16G16_UNORM;
		num_planes = 2;
		plane_subsample_log2[0] = 0;
		plane_subsample_log2[1] = active_upload_pix_fmt == AV_PIX_FMT_P016 ? 1 : 0;
		plane_subsample_log2[2] = active_upload_pix_fmt == AV_PIX_FMT_P016 ? 1 : 0;
		break;

	default:
		LOGE("Unrecognized pixel format: %d.\n", active_upload_pix_fmt);
		num_planes = 0;
		break;
	}
}

void VideoDecoder::Impl::process_video_frame_in_task(unsigned frame, AVFrame *av_frame)
{
	if (hw.device && av_frame->format == hw.config->pix_fmt)
	{
		AVFrame *sw_frame = av_frame_alloc();

		// TODO: Is there a way we can somehow export this to an FD instead?
		if (av_hwframe_transfer_data(sw_frame, av_frame, 0) < 0)
		{
			LOGE("Failed to transfer HW frame.\n");
			av_frame_free(&sw_frame);
			av_frame_free(&av_frame);
		}
		else
		{
			sw_frame->pts = av_frame->pts;
			av_frame_free(&av_frame);
			av_frame = sw_frame;
		}
	}

	// Not sure if it's possible to just spuriously change the format like this,
	// but be defensive.
	if (!av_frame || active_upload_pix_fmt != av_frame->format)
	{
		active_upload_pix_fmt = AV_PIX_FMT_NONE;
		num_planes = 0;
		// Reset the planar images.
		for (auto &i : video_queue)
			for (auto &img : i.planes)
				img.reset();
	}

	// We might not know our target decoding format until this point due to HW decode.
	// Select an appropriate decoding setup.
	if (active_upload_pix_fmt == AV_PIX_FMT_NONE && av_frame)
	{
		active_upload_pix_fmt = static_cast<AVPixelFormat>(av_frame->format);
		setup_yuv_format_planes();
	}

	auto &img = video_queue[frame];

	for (unsigned i = 0; i < num_planes; i++)
	{
		auto &plane = img.planes[i];
		if (!plane)
		{
			auto info = Vulkan::ImageCreateInfo::immutable_2d_image(
					video.av_ctx->width >> plane_subsample_log2[i],
					video.av_ctx->height >> plane_subsample_log2[i],
					plane_formats[i]);
			info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			info.misc = Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_COMPUTE_BIT |
			            Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_TRANSFER_BIT;
			plane = device->create_image(info);
		}
	}

	img.pts = av_q2d(video.av_stream->time_base) * double(av_frame->pts);
	assert(img.state == ImageState::Locked);

	img.sem_to_client.reset();
	Vulkan::Semaphore transfer_to_compute;
	Vulkan::Semaphore compute_to_user;

	if (img.sem_from_client)
	{
		device->add_wait_semaphore(Vulkan::CommandBuffer::Type::AsyncTransfer,
		                           std::move(img.sem_from_client),
		                           VK_PIPELINE_STAGE_2_COPY_BIT,
		                           true);
		img.sem_from_client = {};
	}

	auto cmd = device->request_command_buffer(Vulkan::CommandBuffer::Type::AsyncTransfer);

	for (unsigned i = 0; i < num_planes; i++)
	{
		cmd->image_barrier(*img.planes[i],
		                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, 0,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	}

	for (unsigned i = 0; i < num_planes; i++)
	{
		auto *buf = static_cast<uint8_t *>(cmd->update_image(*img.planes[i]));
		int byte_width = int(img.planes[i]->get_width());
		byte_width *= int(Vulkan::TextureFormatLayout::format_block_size(plane_formats[i], VK_IMAGE_ASPECT_COLOR_BIT));

		av_image_copy_plane(buf, byte_width,
		                    av_frame->data[i], av_frame->linesize[i],
		                    byte_width, int(img.planes[i]->get_height()));
	}

	for (unsigned i = 0; i < num_planes; i++)
	{
		cmd->image_barrier(*img.planes[i],
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		                   VK_PIPELINE_STAGE_NONE, 0);
	}

	device->submit(cmd, nullptr, 1, &transfer_to_compute);

	auto conversion_queue = opts.mipgen ?
			Vulkan::CommandBuffer::Type::AsyncGraphics : Vulkan::CommandBuffer::Type::AsyncCompute;
	device->add_wait_semaphore(conversion_queue,
	                           std::move(transfer_to_compute),
	                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                           true);

	cmd = device->request_command_buffer(conversion_queue);

	if (num_planes)
	{
		cmd->set_storage_texture(0, 0, *img.rgb_storage_view);
		for (unsigned i = 0; i < num_planes; i++)
		{
			cmd->set_texture(0, 1 + i, img.planes[i]->get_view(),
			                 i == 0 ? Vulkan::StockSampler::NearestClamp : Vulkan::StockSampler::LinearClamp);
		}
		for (unsigned i = num_planes; i < 3; i++)
			cmd->set_texture(0, 1 + i, img.planes[0]->get_view(), Vulkan::StockSampler::NearestClamp);
		cmd->set_program("builtin://shaders/util/yuv_to_rgb.comp");

		cmd->set_specialization_constant_mask(3u << 1);
		cmd->set_specialization_constant(1, num_planes);
		cmd->set_specialization_constant(2, uint32_t(active_upload_pix_fmt == AV_PIX_FMT_NV21));

		memcpy(cmd->allocate_typed_constant_data<UBO>(1, 0, 1), &ubo, sizeof(ubo));

		push.unorm_rescale = unorm_rescale;
		cmd->push_constants(&push, 0, sizeof(push));

		cmd->image_barrier(*img.rgb_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		cmd->dispatch((push.resolution.x + 7) / 8, (push.resolution.y + 7) / 8, 1);

		if (opts.mipgen)
		{
			cmd->barrier_prepare_generate_mipmap(*img.rgb_image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			                                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true);
			cmd->generate_mipmap(*img.rgb_image);
			cmd->image_barrier(*img.rgb_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                   VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
			                   VK_PIPELINE_STAGE_NONE, 0);
		}
		else
		{
			cmd->image_barrier(*img.rgb_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			                   VK_PIPELINE_STAGE_NONE, 0);
		}
	}
	else
	{
		// Fallback, just clear to magenta to make it obvious what went wrong.
		cmd->image_barrier(*img.rgb_image,
		                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

		VkClearValue color = {};
		color.color.float32[0] = 1.0f;
		color.color.float32[2] = 1.0f;
		color.color.float32[3] = 1.0f;
		cmd->clear_image(*img.rgb_image, color);
		cmd->image_barrier(*img.rgb_image,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		                   VK_PIPELINE_STAGE_NONE, 0);
	}

	device->submit(cmd, nullptr, 1, &compute_to_user);

	av_frame_free(&av_frame);

	// Can now acquire.
	std::lock_guard<std::mutex> holder{lock};
	img.sem_to_client = std::move(compute_to_user);
	img.state = ImageState::Ready;
	cond.notify_all();
};

void VideoDecoder::Impl::process_video_frame(AVFrame *av_frame)
{
	unsigned frame = acquire_decode_video_frame();

	video_upload_count++;
	video_queue[frame].state = ImageState::Locked;
	video_queue[frame].lock_order = video_upload_count;

	// This decode thread does not have a TLS thread index allocated in the device,
	// only main threads registered as such as well as task group threads satisfy this.
	// Also, we can parallelize video decode and upload + conversion submission,
	// so it's a good idea either way.
	auto task = thread_group->create_task([this, frame, av_frame]() {
		process_video_frame_in_task(frame, av_frame);
	});
	task->set_desc("ffmpeg-decode-upload");
	task->set_task_class(TaskClass::Background);
	task->set_fence_counter_signal(&video_upload_signal);

	// Need to make sure upload tasks are ordered to ensure that frames
	// are acquired in order.
	if (upload_dependency)
		thread_group->add_dependency(*task, *upload_dependency);
	upload_dependency = thread_group->create_task();
	thread_group->add_dependency(*upload_dependency, *task);
}

bool VideoDecoder::Impl::decode_audio_packet(AVPacket *pkt)
{
#ifdef HAVE_GRANITE_AUDIO
	if (!stream)
		return false;

	int ret;
	if (pkt)
	{
		ret = avcodec_send_packet(audio.av_ctx, pkt);
		if (ret < 0)
		{
			LOGE("Failed to send packet.\n");
			return false;
		}
	}

	// It's okay to acquire the same frame many times.
	auto *av_frame = stream->acquire_write_frame();

	if ((ret = avcodec_receive_frame(audio.av_ctx, av_frame)) >= 0)
	{
		stream->submit_write_frame();
		return true;
	}

	// This marks the end of the stream. Let it die.
	if (!pkt && ret < 0)
		stream->mark_complete();

	return ret >= 0 || ret == AVERROR(EAGAIN);
#else
	(void)pkt;
	return false;
#endif
}

bool VideoDecoder::Impl::decode_video_packet(AVPacket *pkt)
{
	int ret;
	if (pkt)
	{
		ret = avcodec_send_packet(video.av_ctx, pkt);
		if (ret < 0)
		{
			LOGE("Failed to send packet.\n");
			return false;
		}
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return false;

	if ((ret = avcodec_receive_frame(video.av_ctx, frame)) >= 0)
	{
		process_video_frame(frame);
		return true;
	}

	return ret >= 0 || ret == AVERROR(EAGAIN);
}

bool VideoDecoder::Impl::iterate()
{
	std::lock_guard<std::mutex> holder{iteration_lock};

	if (is_video_eof && (is_audio_eof || !audio.av_ctx))
		return false;

	if (!is_flushing)
	{
		int ret;
		if ((ret = av_read_frame(av_format_ctx, av_pkt)) >= 0)
		{
			if (av_pkt->stream_index == video.av_stream->index)
			{
				if (!decode_video_packet(av_pkt))
					is_video_eof = true;
			}
			else if (audio.av_stream && av_pkt->stream_index == audio.av_stream->index)
			{
				if (!decode_audio_packet(av_pkt))
					is_audio_eof = true;
			}

			av_packet_unref(av_pkt);
		}

		if (ret == AVERROR_EOF)
		{
			// Send a flush packet, so we can drain the codecs.
			// There will be no more packets from the file.
			avcodec_send_packet(video.av_ctx, nullptr);
			if (audio.av_ctx)
				avcodec_send_packet(audio.av_ctx, nullptr);
			is_flushing = true;
		}
		else if (ret < 0)
			return false;
	}

	if (!is_video_eof && is_flushing && !decode_video_packet(nullptr))
		is_video_eof = true;

	if (!is_audio_eof && is_flushing && audio.av_ctx && !decode_audio_packet(nullptr))
		is_audio_eof = true;

	return true;
}

bool VideoDecoder::Impl::should_iterate_locked()
{
	// If there are idle images and the audio ring isn't completely saturated, go ahead.
	// The audio ring should be very large to soak variability. Audio does not consume a lot
	// of memory either way.
	// TODO: It is possible to use dynamic rate control techniques to ensure that
	// audio ring does not underflow or overflow.

#ifdef HAVE_GRANITE_AUDIO
	if (stream)
	{
		// If audio buffer saturation reached a high watermark, there is risk of overflowing it.
		// We should be far, far ahead at this point. We should easily be able to just sleep
		// until the audio buffer has drained down to a reasonable level.
		if (stream->get_num_buffered_av_frames() > AVFrameRingStream::FramesHighWatermark)
			return false;

		// If audio buffer saturation is at risk of draining, causing audio glitches,
		// we need to catch up.
		// This really shouldn't happen unless application is not actually acquiring images
		// for a good while.
		// When application is in a steady state, it will acquire images based on the audio timestamp.
		// Thus, there is a natural self-regulating mechanism in place.
		// Ensure that we have at least 100 ms of audio buffered up.
		if (mixer->get_stream_state(stream_id) == Audio::Mixer::StreamState::Playing &&
		    stream->get_num_buffered_audio_frames() <= unsigned(audio.av_ctx->sample_rate / 10))
		{
			return true;
		}
	}
#endif

	// If audio is in a stable situation, we can shift our attention to video.
	// Video is more lenient w.r.t. drops and such.

	// If acquire is blocking despite us having no idle images,
	// it means it's not happy with whatever frames we have decoded,
	// so we should go ahead, even if it means trampling on existing frames.
	if (acquire_blocking)
		return true;

	// We're in a happy state where we only desire progress if there is anything
	// meaningful to do.
	return find_idle_decode_video_frame_locked() >= 0;
}

void VideoDecoder::Impl::thread_main()
{
	Util::set_current_thread_priority(Util::ThreadPriority::High);

	for (;;)
	{
		{
			std::unique_lock<std::mutex> holder{lock};

			while (!should_iterate_locked() && !teardown)
			{
#ifdef HAVE_GRANITE_AUDIO
				// If we're going to sleep, we need to make sure we don't sleep for so long that we drain the audio queue.
				if (stream && mixer->get_stream_state(stream_id) == Audio::Mixer::StreamState::Playing)
				{
					// We want to sleep until there is ~100ms audio left.
					// Need a decent amount of headroom since we might have to decode video before
					// we can pump more audio frames.
					// This could be improved with dedicated decoding threads audio and video,
					// but that is a bit overkill.
					// Reformulate the expression to avoid potential u32 overflow if multiplying.
					// Shouldn't need floats here.
					int sleep_ms = int(stream->get_num_buffered_audio_frames() / ((audio.av_ctx->sample_rate + 999) / 1000));
					sleep_ms = std::max<int>(sleep_ms - 100 + 5, 0);
					cond.wait_for(holder, std::chrono::milliseconds(sleep_ms));
				}
				else
#endif
				{
					cond.wait(holder);
				}
			}
		}

		if (teardown)
			break;

		if (!iterate())
		{
			// Ensure acquire thread can observe last frame if it observes
			// the acquire_is_eof flag.
			video_upload_signal.wait_until_at_least(video_upload_count);

			std::lock_guard<std::mutex> holder{lock};
			teardown = true;
			acquire_is_eof = true;
			cond.notify_one();
			break;
		}
	}
}

int VideoDecoder::Impl::try_acquire_video_frame(VideoFrame &frame)
{
	if (!decode_thread.joinable())
		return false;

	std::unique_lock<std::mutex> holder{lock};

	int index = find_acquire_video_frame_locked();

	if (index >= 0)
	{
		// Now we can return a frame.
		frame.sem.reset();
		std::swap(frame.sem, video_queue[index].sem_to_client);
		video_queue[index].state = ImageState::Acquired;
		frame.view = &video_queue[index].rgb_image->get_view();
		frame.index = index;
		frame.pts = video_queue[index].pts;

		// Progress.
		cond.notify_one();

		return 1;
	}
	else
	{
		return acquire_is_eof || teardown ? -1 : 0;
	}
}

bool VideoDecoder::Impl::acquire_video_frame(VideoFrame &frame)
{
	if (!decode_thread.joinable())
		return false;

	std::unique_lock<std::mutex> holder{lock};

	// Wake up decode thread to make sure it knows acquire thread
	// is blocking and awaits forward progress.
	acquire_blocking = true;
	cond.notify_one();

	int index = -1;
	// Poll the video queue for new frames.
	cond.wait(holder, [this, &index]() {
		return (index = find_acquire_video_frame_locked()) >= 0 || acquire_is_eof || teardown;
	});

	if (index < 0)
		return false;

	// Now we can return a frame.
	frame.sem.reset();
	std::swap(frame.sem, video_queue[index].sem_to_client);
	video_queue[index].state = ImageState::Acquired;
	frame.view = &video_queue[index].rgb_image->get_view();
	frame.index = index;
	frame.pts = video_queue[index].pts;

	// Progress.
	acquire_blocking = false;
	cond.notify_one();
	return true;
}

void VideoDecoder::Impl::release_video_frame(unsigned index, Vulkan::Semaphore sem)
{
	// Need to hold lock here.
	assert(video_queue[index].state == ImageState::Acquired);
	video_queue[index].state = ImageState::Idle;
	video_queue[index].sem_from_client = std::move(sem);
	video_queue[index].idle_order = ++idle_timestamps;
}

bool VideoDecoder::Impl::begin_device_context(Vulkan::Device *device_)
{
	device = device_;
	thread_group = device->get_system_handles().thread_group;
	return true;
}

double VideoDecoder::Impl::get_estimated_audio_playback_timestamp_raw()
{
#ifdef HAVE_GRANITE_AUDIO
	if (stream)
	{
		uint32_t pts_buffer_index = (stream->pts_index.load(std::memory_order_acquire) - 1) %
		                            AVFrameRingStream::Frames;

		double pts = stream->progress[pts_buffer_index].pts;
		if (pts < 0.0)
			pts = 0.0;

		return pts;
	}
	else
#endif
	{
		return -1.0;
	}
}

double VideoDecoder::Impl::get_estimated_audio_playback_timestamp(double elapsed_time)
{
#ifdef HAVE_GRANITE_AUDIO
	if (stream)
	{
		uint32_t pts_buffer_index = (stream->pts_index.load(std::memory_order_acquire) - 1) %
		                            AVFrameRingStream::Frames;

		double pts = stream->progress[pts_buffer_index].pts;
		if (pts < 0.0)
		{
			pts = 0.0;
			smooth_elapsed = 0.0;
			smooth_pts = 0.0;
		}
		else if (!is_paused)
		{
			// Crude estimate based on last reported PTS, offset by time since reported.
			int64_t sampled_ns = stream->progress[pts_buffer_index].sampled_ns;
			int64_t d = std::max<int64_t>(Util::get_current_time_nsecs(), sampled_ns) - sampled_ns;
			pts += 1e-9 * double(d);
		}

		// Smooth out the reported PTS.
		// The reported PTS should be tied to the host timer,
		// but we need to gradually adjust the timer based on the reported audio PTS to be accurate.

		if (smooth_elapsed == 0.0)
		{
			// Latch the PTS.
			smooth_elapsed = elapsed_time;
			smooth_pts = pts;
		}
		else
		{
			// This is the value we should get in principle if everything is steady.
			smooth_pts += elapsed_time - smooth_elapsed;
			smooth_elapsed = elapsed_time;

			if (muglm::abs(smooth_pts - pts) > 0.25)
			{
				// Massive spike somewhere, cannot smooth.
				// Reset the PTS.
				smooth_elapsed = elapsed_time;
				smooth_pts = pts;
			}
			else
			{
				// Bias slightly towards the true estimated PTS.
				smooth_pts += 0.005 * (pts - smooth_pts);
			}
		}

		return smooth_pts;
	}
	else
#endif
	{
		return -1.0;
	}
}

void VideoDecoder::Impl::flush_codecs()
{
	for (auto &img : video_queue)
	{
		img.rgb_image.reset();
		img.rgb_storage_view.reset();
		for (auto &plane : img.planes)
			plane.reset();
		img.sem_to_client.reset();
		img.sem_from_client.reset();
		img.idle_order = 0;
		img.lock_order = 0;
		img.state = ImageState::Idle;
		img.pts = 0.0;
	}

	if (video.av_ctx)
		avcodec_flush_buffers(video.av_ctx);
	if (audio.av_ctx)
		avcodec_flush_buffers(audio.av_ctx);

#ifdef HAVE_GRANITE_AUDIO
	if (stream)
	{
		mixer->kill_stream(stream_id);
		stream->release_reference();
		stream = nullptr;
	}
#endif
}

void VideoDecoder::Impl::end_device_context()
{
	stop();
	device = nullptr;
	thread_group = nullptr;
}

bool VideoDecoder::Impl::play()
{
	if (!device)
		return false;
	if (decode_thread.joinable())
		return false;

	teardown = false;
	flush_codecs();
	begin_audio_stream();

	decode_thread = std::thread(&Impl::thread_main, this);
	return true;
}

bool VideoDecoder::Impl::get_stream_id(Audio::StreamID &id) const
{
#ifdef HAVE_GRANITE_AUDIO
	id = stream_id;
	return bool(id);
#else
	(void)id;
	return false;
#endif
}

bool VideoDecoder::Impl::stop()
{
	if (!decode_thread.joinable())
		return false;

	{
		std::lock_guard<std::mutex> holder{lock};
		teardown = true;
		cond.notify_one();
	}
	decode_thread.join();
	video_upload_signal.wait_until_at_least(video_upload_count);
	upload_dependency.reset();
	flush_codecs();
	return true;
}

bool VideoDecoder::Impl::get_paused() const
{
	return is_paused;
}

void VideoDecoder::Impl::set_paused(bool enable)
{
	is_paused = enable;
#ifdef HAVE_GRANITE_AUDIO
	if (stream)
	{
		// Reset PTS smoothing.
		smooth_elapsed = 0.0;
		smooth_pts = 0.0;

		bool result;
		if (enable)
			result = mixer->pause_stream(stream_id);
		else
		{
			// When we uncork, we need to ensure that estimated PTS
			// picks off where we expect.
			stream->mark_uncorked_audio_pts();

			// If the thread went to deep sleep, we need to make sure it knows
			// about the stream state being playing.
			std::lock_guard<std::mutex> holder{lock};
			result = mixer->play_stream(stream_id);
			cond.notify_one();
		}

		if (!result)
			LOGE("Failed to set stream state.\n");
	}
#endif
}

bool VideoDecoder::Impl::seek(double ts)
{
	std::lock_guard<std::mutex> holder{iteration_lock};

	// Drain this before we take the global lock, since a video task needs to take the global lock to update state.
	video_upload_signal.wait_until_at_least(video_upload_count);

	std::lock_guard<std::mutex> holder2{lock};
	cond.notify_one();

	if (ts < 0.0)
		ts = 0.0;
	auto target_ts = int64_t(AV_TIME_BASE * ts);

	if (avformat_seek_file(av_format_ctx, -1, INT64_MIN, target_ts, INT64_MAX, 0) < 0)
	{
		LOGE("Failed to seek file.\n");
		return false;
	}

	if (decode_thread.joinable())
	{
		flush_codecs();
		begin_audio_stream();
		return true;
	}
	else
		return play();
}

VideoDecoder::Impl::~Impl()
{
	stop();

	free_av_objects(video);
	free_av_objects(audio);

	if (av_format_ctx)
		avformat_close_input(&av_format_ctx);
	if (av_pkt)
		av_packet_free(&av_pkt);
	if (hw.device)
		av_buffer_unref(&hw.device);
}

VideoDecoder::VideoDecoder()
{
	impl.reset(new Impl);
}

VideoDecoder::~VideoDecoder()
{
}

bool VideoDecoder::init(Audio::Mixer *mixer, const char *path, const DecodeOptions &opts)
{
	return impl->init(mixer, path, opts);
}

unsigned VideoDecoder::get_width() const
{
	return impl->get_width();
}

unsigned VideoDecoder::get_height() const
{
	return impl->get_height();
}

bool VideoDecoder::begin_device_context(Vulkan::Device *device)
{
	return impl->begin_device_context(device);
}

void VideoDecoder::end_device_context()
{
	impl->end_device_context();
}

bool VideoDecoder::play()
{
	return impl->play();
}

bool VideoDecoder::get_stream_id(Audio::StreamID &id) const
{
	return impl->get_stream_id(id);
}

bool VideoDecoder::stop()
{
	return impl->stop();
}

bool VideoDecoder::seek(double ts)
{
	return impl->seek(ts);
}

void VideoDecoder::set_paused(bool state)
{
	return impl->set_paused(state);
}

bool VideoDecoder::get_paused() const
{
	return impl->get_paused();
}

double VideoDecoder::get_estimated_audio_playback_timestamp(double elapsed_time)
{
	return impl->get_estimated_audio_playback_timestamp(elapsed_time);
}

double VideoDecoder::get_estimated_audio_playback_timestamp_raw()
{
	return impl->get_estimated_audio_playback_timestamp_raw();
}

bool VideoDecoder::acquire_video_frame(VideoFrame &frame)
{
	return impl->acquire_video_frame(frame);
}

int VideoDecoder::try_acquire_video_frame(VideoFrame &frame)
{
	return impl->try_acquire_video_frame(frame);
}

void VideoDecoder::release_video_frame(unsigned index, Vulkan::Semaphore sem)
{
	impl->release_video_frame(index, std::move(sem));
}
}