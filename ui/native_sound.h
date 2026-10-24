#ifndef NATIVE_SOUND_H
#define NATIVE_SOUND_H

namespace native_sound {
	struct sound {
		virtual ~sound() {}
	};
	
	extern int frequency;
	extern int channels;

	void init();
	void play(int channel, sound* s, int volume, int pan);
	bool is_playing(int channel);
	void stop(int channel);
	void set_volume(int channel, int volume);
	std::unique_ptr<sound> load_wav(const void* data, size_t size);

	// Streaming music (separate from the SFX channel pool). play_music copies
	// the buffer internally and streams it; volume is 0-128. Returns false if
	// the data could not be decoded (caller should drop the track).
	bool play_music(const void* data, size_t size, int volume);
	bool music_playing();
	void stop_music();
	void set_music_volume(int volume);

}

#endif
