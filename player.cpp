#include "common.h"
#include "container.h"

using namespace std;

Queue<AVPicture> picture_queue;
PacketQueue audio_queue;
PacketQueue video_queue;

int decode_audio(AVCodecContext *codec_context_audio, uint8_t *audio_buffer, int buffer_size) {
	static AVPacket packet;
	static uint8_t *audio_packet_data = nullptr;
	static int audio_packet_size = 0;
	static AVFrame frame;

	int len1, data_size = 0;

	for(;;) {
		while(audio_packet_size > 0) {
			int got_frame = 0;
			len1 = avcodec_decode_audio4(codec_context_audio, &frame, &got_frame, &packet);
			if (len1 < 0) {
				cerr << "Skip audio frame" << endl;	
				audio_packet_size = 0;
				break;
			}

			audio_packet_data += len1;
			audio_packet_size -= len1;

			if (got_frame) {
				data_size = frame.linesize[0];

				copy(&frame.data[0][0], &frame.data[0][data_size], audio_buffer);
			}
			if (data_size <= 0) {
				// No data yet, get more frames
				continue;
			}

			// We have data, return it and come back for more later
			return data_size;
		}

		if (packet.data) {
			av_free_packet(&packet);
		}
		//if (quit) {
		//	clog << "Quit" << endl;
		//	throw "Quit";
		//}

		if (!audio_queue.pop(packet))
			break;

		audio_packet_data = packet.data;
		audio_packet_size = packet.size;
	}
	return 0;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
	AVCodecContext *codec_context_audio = (AVCodecContext*) userdata;
	int len1, audio_size;

	static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned audio_buf_size = 0;
	static unsigned audio_buf_index = 0;

	while (len > 0) {
		if (audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = decode_audio(codec_context_audio, audio_buf, sizeof(audio_buf));
			if (audio_size < 0) {
				/* If error, output silence */
				cerr << "Output Silence" << endl;
				audio_buf_size = 1024; // arbitrary?
				memset(audio_buf, 0, audio_buf_size);
			}
		       	else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;
		copy((uint8_t*)audio_buf + audio_buf_index, (uint8_t*)audio_buf + audio_buf_index + len1, stream);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

void decode_video(Container &container) {
	AVPacket packet;
	shared_ptr<AVFrame> frame(avcodec_alloc_frame(), av_free);
	AVPicture pict;

	int finished_frame;

	try {
		if (avpicture_alloc(&pict, container.get_pixel_format(), container.get_width(), container.get_height()) < 0) {
			throw runtime_error("Allocating picture"); 
		}

		for(;;) {
			if (!video_queue.pop(packet))
				break;

			clog << "Decode video" << endl;

			container.decode_frame(frame.get(), finished_frame, packet);

			if (finished_frame) {
				clog << "Frame finished" << endl;
				for(;;) {
					if (picture_queue.full(1)) {
						this_thread::sleep_for(chrono::milliseconds(10));
					}
					else {
						break;
					}
				}

				// Convert YUV to RGB 
				static SwsContext *conversion_context = sws_getContext(
					// Source
					container.get_width(), container.get_height(), container.get_pixel_format(), 
					// Destination
					container.get_width(), container.get_height(), PIX_FMT_YUV420P,
					// Filters
					SWS_BICUBIC, nullptr, nullptr, nullptr);

				if (!conversion_context) {
					throw runtime_error("Conversion context");
				}
				
				clog << "Convert picture" << endl;
				sws_scale(conversion_context,
					// Source
					frame.get()->data, frame.get()->linesize, 0, container.get_height(),
					// Destination
					pict.data, pict.linesize);	
		
				picture_queue.push(pict);
			}
			else {
				clog << "Frame not finished" << endl;
			}
			av_free_packet(&packet);
		}
	}
	catch (exception &e) {
		clog << "Decoding error: " <<  e.what() << endl;
		exit(1);
	}

}
void demux(Container &container) {
	AVPacket packet;

	try {
		for (;;) {
			if (!container.read_frame(packet))
				break;

			if (audio_queue.full(5 * 16 * 1024) || video_queue.full(5 * 256 * 1024)) {
				this_thread::sleep_for(chrono::milliseconds(10));
			}

			if (packet.stream_index == container.get_video_stream()) {
				video_queue.push(packet);
			}
			else if (packet.stream_index == container.get_audio_stream()) {
				audio_queue.push(packet);
			}
			else {
				av_free_packet(&packet);
			}
		}
	}
	catch (exception &e) {
		clog << "Demuxing  error: " << e.what() << endl;
		exit(1);
	}

}
enum class Signal {
     play = 0,
     kill = 1
};
	     
Queue<Signal> event;

void refresh(const size_t ms) {
	auto sleep_event = [ms]() {
		this_thread::sleep_for(chrono::milliseconds(ms));
		event.push(Signal::play);
	};
	thread t(sleep_event);
	t.detach();
}

void display(Container &container, SDL_Overlay *bmp, AVPicture &pict) 
{
	SDL_Rect rect;

	SDL_LockYUVOverlay(bmp);

	for (size_t channel = 0; channel < 3; ++channel) {
		bmp->pitches[channel] = pict.linesize[channel];
	}

	move(&pict.data[0][0], &pict.data[0][bmp->pitches[0] * container.get_height()], bmp->pixels[0]); 
	move(&pict.data[1][0], &pict.data[1][bmp->pitches[1] * container.get_height() / 2], bmp->pixels[2]); 
	move(&pict.data[2][0], &pict.data[2][bmp->pitches[2] * container.get_height() / 2], bmp->pixels[1]); 

	SDL_UnlockYUVOverlay(bmp);

	rect.x = 0;
	rect.y = 0;
	rect.w = container.get_width();
	rect.h = container.get_height();
	SDL_DisplayYUVOverlay(bmp, &rect);

	clog << "Display picture" << endl;
}

void init(Container &container, SDL_Overlay *&bmp, const string &file_name)
{
	SDL_AudioSpec spec_required;
	SDL_AudioSpec spec_attained;
	SDL_Surface *screen;

	// FFmpeg
	static once_flag init_flag;	
	call_once(init_flag, [](){ av_register_all(); });

	// SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		throw runtime_error("SDL init");
	}

	// Parse file for information 
	container.init(file_name.c_str());

	// Give audio spec to SDL
	spec_required = container.get_audio_spec();
	if (spec_required.channels) {
		spec_required.callback = audio_callback;
		if (SDL_OpenAudio(&spec_required, &spec_attained) < 0) {
			throw runtime_error("SDL audio");
		}	
		SDL_PauseAudio(0);
	}

	screen = SDL_SetVideoMode(container.get_width(), container.get_height(), 0, 0);
	if (!screen) {
		throw runtime_error("SDL video");
	}

	bmp = SDL_CreateYUVOverlay(container.get_width(), container.get_height(), SDL_YV12_OVERLAY, screen);

	clog << "Initialize" << endl;
}

int main(int argc, char **argv) {

	Container container;
	AVPicture picture;

	SDL_Overlay *bmp = nullptr;

	try {
		if (argc < 2) {
			throw runtime_error("Arguements");
		}

		string file_name = argv[1];

		init(container, bmp, file_name);
	}

	catch (exception &e) {
		cerr << "Initialization error: " << e.what() << endl;
		exit(1);
	}

	thread t1(demux, ref(container)); 
	thread t2(decode_video, ref(container));

	refresh(20);
	try {
		for(;;) {
			Signal s;
			event.pop(s);
			switch (s)
			{
				case Signal::play: {
					if (!picture_queue.pop(picture)) {
						break;
					}

					refresh(40);
					display(container, bmp, picture);
					break;
				}
				case Signal::kill: {
					// XXX: Signals
					//quit = true;
					break;
				}
				default: {
					this_thread::sleep_for(chrono::milliseconds(1));
					break;
				}
			}
		}
	}

	catch (exception &e) {
		cerr << "Playback error: " << e.what() << endl;
		exit(1);
	}

	return 0;
}
