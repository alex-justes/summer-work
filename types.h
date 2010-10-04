#include <alsa/asoundlib.h>

#define DEVICE "default"
#define BUFF_SIZE 256
#define OFFSET 44 //Plain uncomressed wav file(offset from the begining of the header)

/*
Critical errors: 1 - 20
Warnings: 21 - 40
*/

#define NO_ERROR 0

/*----CRITICAL SECTION----*/
#define UNKNOWN_ERROR 1
#define MEM_ALLOC_FAIL 2
#define UNSUPPORTED_FORMAT 3
#define BAD_PCM_DEV 4
#define BAD_HW_PARAMS 5

/*----WARNING SECTION----*/
#define QUEUE_FULL 21
#define QUEUE_EMPTY 22

typedef char bool;

struct packet_t{
    unsigned long int timestamp;
    void *data;
};

struct queue_t{
    struct packet_t buffer[BUFF_SIZE];
    unsigned int current,next,size;
};

struct riff_header_t{
    char id[5];
    char type[5];
    unsigned int data_size;
};

struct fmt_header_t{
    char id[5];
    unsigned int data_size;
    unsigned short int compression;
    unsigned short int channels;
    unsigned int sample_rate;
    unsigned int av_bps;
    unsigned short int block_align;
    unsigned short int sign_bps;
};

struct data_header_t{
    char id[5];
    unsigned int size;
    unsigned int offset;
};

struct sound_device_t{
    unsigned long int loops;
    unsigned int size;
    unsigned int dir;
    unsigned int buff_size_bytes;
    unsigned int buff_size_frames;
    unsigned int val;
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    snd_pcm_uframes_t frames;
};

struct wav_info_t{
    struct riff_header_t riff;
    struct fmt_header_t fmt;
    struct data_header_t data;
    struct sound_device_t dev;
};

struct reader_t{
    unsigned int start_sound; //In seconds
    unsigned int start_frame; //In frames
    unsigned int sound_offset;
    unsigned int sound_buff_size;
    unsigned int frame_buff_size;
};
