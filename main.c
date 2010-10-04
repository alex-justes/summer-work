#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <sys/select.h>
#include <sys/time.h>

#include <cairo.h>
#include <gtk/gtk.h>

#include "types.h"

//#define DEBUG
#define TIME 1000000 // One second in microseconds
#define ALSA_PCM_NEW_HW_PARAMS_API

#define RUS

//#include "futurama.h"
#include "lock.h"

bool v_ready = FALSE, s_ready = FALSE;
struct queue_t s_queue, v_queue;
struct wav_info_t info;
void *sound_buff, *video_buff;
bool flag = TRUE;
bool r_term = FALSE, s_term = FALSE, v_term = FALSE, buffer_ready = FALSE;
pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t k_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t v_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t v_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t k_cond = PTHREAD_COND_INITIALIZER;
pthread_t r_th, s_th, v_th;

void add_timespec(struct timespec *ts, struct timeval *tv){
    (*ts).tv_sec = (*tv).tv_sec;
    (*ts).tv_nsec += ((*tv).tv_usec * 1000);
    if ((*ts).tv_nsec >= 1000000000){
        (*ts).tv_nsec %= 1000000000;
        ++(*ts).tv_sec;
    }
}

void s_queue_destroy(struct queue_t *queue){
    for(int i = 0; i < BUFF_SIZE; ++i){
       free(((*queue).buffer[i]).data);
    }
    printf("Sound queue destroyed\n");
}

void v_queue_destroy(struct queue_t *queue){
    for(int i = 0; i < BUFF_SIZE; ++i){
        cairo_surface_destroy((cairo_surface_t *)((*queue).buffer[i]).data);
    }
    printf("Video queue destroyed\n");
}

void wait(struct timeval *time){
    struct timeval tmp;
    tmp.tv_sec = (*time).tv_sec;
    tmp.tv_usec = (*time).tv_usec;
    select(0,NULL,NULL,NULL,time);
    (*time).tv_sec = tmp.tv_sec;
    (*time).tv_usec = tmp.tv_usec;
}

void exception(const char *msg, int error_no){
    int ret_code = 0;
    if (error_no > 0){
        if (error_no < 21){
            if (error_no == UNKNOWN_ERROR){
                fprintf(stderr, "ERROR: %sUnknown error.\n",msg);
                ret_code = 1;
            } else if (error_no == MEM_ALLOC_FAIL){
                fprintf(stderr, "ERROR: %sMemory allocation failure.\n",msg);
                ret_code = 1;
            } else if (error_no == UNSUPPORTED_FORMAT){
                fprintf(stderr, "ERROR: %sUnsupported file format.\n",msg);
                ret_code = 1;
            } else if (error_no == BAD_PCM_DEV){
                fprintf(stderr, "ERROR: Unable to open pcm device: %s\n",msg);
                ret_code = 1;
            } else if (error_no == BAD_HW_PARAMS){
                fprintf(stderr, "ERROR: Unable to set hw parameters: %s\n",msg);
                ret_code = 1;
            }


        } else {
#ifdef DEBUG
            if (error_no == QUEUE_FULL){
                fprintf(stderr, "DEBUG: %sQueue is full.\n",msg);
                error_no = 0;
            } else if (error_no == QUEUE_EMPTY){
                fprintf(stderr, "DEBUG: %sQueue is empty.\n",msg);
                error_no = 0;
            }
#endif
        }
    }
    if (ret_code == 1){
        s_queue_destroy(&s_queue);
        v_queue_destroy(&v_queue);
        exit(1);
    }
}

void queue_init(struct queue_t *queue, unsigned int data_size){
    for(int i = 0; i < BUFF_SIZE; ++i){
        (*queue).buffer[i].data = malloc(data_size);
        if ((*queue).buffer[i].data == NULL){
            exception("Queue init: ", MEM_ALLOC_FAIL);
        }
    }
    (*queue).current = 0;
    (*queue).next = 0;
    (*queue).size = 0;
}


bool queue_full(struct queue_t *queue){
    if((*queue).size == BUFF_SIZE){
        return TRUE;
    }
    return FALSE;
}

bool queue_empty(struct queue_t *queue){
    if ((*queue).size == 0){
        return TRUE;
    }
    return FALSE;
}

struct packet_t *queue_push(struct queue_t *queue){
    if (queue_full(queue) == TRUE){
        exception("Queue push: ",QUEUE_FULL);
        return NULL;
    }
    int tmp = (*queue).next;
    ++(*queue).next;
    (*queue).next %= BUFF_SIZE;
    ++(*queue).size;
    return &((*queue).buffer[tmp]);
}

int queue_size(struct queue_t *queue){
    return (*queue).size;
}

struct packet_t *queue_pop(struct queue_t *queue){
    if (queue_empty(queue)){
        exception("Queue pop: ", QUEUE_EMPTY);
        return NULL;
    }
    int tmp = (*queue).current;
    return &((*queue).buffer[tmp]);
}

struct packet_t *queue_pop_next(struct queue_t *queue){
    if (queue_empty(queue)){
        exception("Queue pop: ", QUEUE_EMPTY);
        return NULL;
    }
    int tmp = ((*queue).current + 1) % BUFF_SIZE;
    return &((*queue).buffer[tmp]);
}

void queue_skip(struct queue_t *queue, int n){
    if (queue_empty(queue)){
        exception("Queue pop: ", QUEUE_EMPTY);
    } else {
        if (n > (*queue).size){
            (*queue).current += (*queue).size;
            (*queue).current %= BUFF_SIZE;
            (*queue).size = 0;
        } else {
            (*queue).current += n;
            (*queue).current %= BUFF_SIZE;
            (*queue).size -= n;
        }
    }
}

unsigned int le_convert(char *digits,int size){
    int tmp = 0;
    for(int i = 0; i < size; ++i){
        tmp |= ((digits[i]&0xFF)<<(8*i));
    }
    return tmp;
}

unsigned int str_to_int(char *digits,int size){
    int tmp = 0;
    for(int i = 0; i < size; ++i){
        tmp |= ((digits[i]&0xFF)<<(8*(size - i - 1)));
    }
    return tmp;
}

void wav_get_info(const char *name, struct wav_info_t *info){
    int sound_fd;
    char buf[4];
    sound_fd = open(name, O_RDONLY);
    memset(info, 0, sizeof(struct wav_info_t));
    read(sound_fd, (*info).riff.id, 4);
    if ( str_to_int((*info).riff.id, 4) != 0x52494646){ //RIFF
        close(sound_fd);
        exception("WAV: ",UNSUPPORTED_FORMAT);
    }
    read(sound_fd, buf, 4);
    (*info).riff.data_size = le_convert(buf,4);
    read(sound_fd, (*info).riff.type, 4);
    if ( str_to_int((*info).riff.type, 4) != 0x57415645){ //WAVE
        close(sound_fd);
        exception("WAV: ",UNSUPPORTED_FORMAT);
    }
    read(sound_fd, (*info).fmt.id, 4);
    if ( str_to_int((*info).fmt.id,4) != 0x666D7420 ){ //fmt
        close(sound_fd);
        exception("WAV: ",UNSUPPORTED_FORMAT);
    }
    read(sound_fd, buf, 4);
    (*info).fmt.data_size = le_convert(buf, 4);
    read(sound_fd, buf, 2);
    (*info).fmt.compression = le_convert(buf, 2);
    read(sound_fd, buf, 2);
    (*info).fmt.channels = le_convert(buf, 2);
    read(sound_fd, buf, 4);
    (*info).fmt.sample_rate = le_convert(buf, 4);
    read(sound_fd, buf, 4);
    (*info).fmt.av_bps = le_convert(buf, 4);
    read(sound_fd, buf, 2);
    (*info).fmt.block_align = le_convert(buf, 2);
    read(sound_fd, buf, 2);
    (*info).fmt.sign_bps = le_convert(buf, 2);
    read(sound_fd, (*info).data.id, 4);
    read(sound_fd, buf, 4);
    (*info).data.size = le_convert(buf, 4);
    (*info).data.offset = OFFSET;
    close(sound_fd);
}

void sound_dev_prepare(const char *device, struct sound_device_t *dev, unsigned int sample_rate, unsigned int channels){
    int rc;
    rc = snd_pcm_open(&(*dev).handle, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0){
        exception(snd_strerror(rc),BAD_PCM_DEV);
    }
    snd_pcm_hw_params_alloca(&(*dev).params);
    snd_pcm_hw_params_any((*dev).handle, (*dev).params);
    snd_pcm_hw_params_set_access((*dev).handle, (*dev).params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format((*dev).handle, (*dev).params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels((*dev).handle, (*dev).params, channels);
    snd_pcm_hw_params_set_rate_near((*dev).handle, (*dev).params, &sample_rate, &(*dev).dir);
    rc = snd_pcm_hw_params((*dev).handle, (*dev).params);
    if (rc < 0){
        exception(snd_strerror(rc), BAD_HW_PARAMS);
    }
    snd_pcm_hw_params_get_period_size((*dev).params, &(*dev).frames, &(*dev).dir);
    (*dev).size = (*dev).frames * 4;
    snd_pcm_hw_params_get_period_time((*dev).params, &(*dev).val, &(*dev).dir);
    (*dev).loops = TIME / (*dev).val;
    (*dev).buff_size_bytes = (*dev).size * (*dev).loops;
    (*dev).buff_size_frames = (*dev).frames * (*dev).loops;
}

void sound_dev_close(struct sound_device_t *dev){
    snd_pcm_drain((*dev).handle);
    snd_pcm_close((*dev).handle);
}

void *sound_thread(void *args){
    struct sound_device_t *dev;
    dev = (struct sound_device_t *)args;
    s_ready = TRUE;
    while(s_term == FALSE){
        pthread_mutex_lock(&s_mutex);
        pthread_cond_wait(&s_cond,&s_mutex);
        pthread_mutex_unlock(&s_mutex);
        int rc = snd_pcm_writei((*dev).handle, sound_buff, (*dev).buff_size_frames);
        pthread_cond_signal(&k_cond);
        if (rc == -EPIPE){
            fprintf(stderr, "underrun occurred\n");
            snd_pcm_prepare((*dev).handle);
        } else if (rc < 0){
            fprintf(stderr, "error from writei: %s\n", snd_strerror(rc));
        }
    }

}

static gboolean v_quit(GtkWidget *widget, GdkEventExpose *event, gpointer data){
    flag = FALSE;
}

void *video_thread(){
    GtkWidget *window;
    struct packet_t *tmp;
    int argc = 0;
    char *argv[1];

    struct timeval t1;
    t1.tv_sec = 2;
    t1.tv_usec = 0;

    gtk_init(&argc,(char ***)argv);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL); 
    g_signal_connect(window, "destroy", G_CALLBACK(v_quit), NULL);
    cairo_t *cr;
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(window), W, H);
    gtk_widget_set_app_paintable(window, TRUE);
    gtk_widget_show_all(window);

        cr = gdk_cairo_create(window->window);
        cairo_set_source_surface(cr, (cairo_surface_t *)(video_buff), 10, 10);
        cairo_paint(cr);
        cairo_destroy(cr);

    wait(&t1);

    v_ready = TRUE;

    while(v_term == FALSE){
        pthread_mutex_lock(&v_mutex);
        pthread_cond_wait(&v_cond,&v_mutex);
        cr = gdk_cairo_create(window->window);
        cairo_set_source_surface(cr, (cairo_surface_t *)(video_buff), 10, 10);
        cairo_paint(cr);
        cairo_destroy(cr);
        pthread_mutex_unlock(&v_mutex);
    }
}

void *reader(void *t){
    struct reader_t *info = (struct reader_t *)t;
    int s_fd, i, s_size, v_size, frame_num = 1 + (*info).start_frame;
    printf("DEBUG: start_frame: %d\n",frame_num);
    int s_timestamp = 0, v_timestamp = 0;
    char str[25];
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    struct packet_t *tmp;
    s_fd = open(SOUND, O_RDONLY);
    lseek(s_fd, (*info).sound_offset + ((*info).start_sound * (*info).sound_buff_size),SEEK_SET);
    while(r_term == FALSE){
        s_size = queue_size(&s_queue);
        i = s_size;
        while(i < BUFF_SIZE){
            tmp = queue_push(&s_queue);
            if (tmp != NULL){
                read(s_fd, (*tmp).data, (*info).sound_buff_size);
                (*tmp).timestamp = s_timestamp;
                s_timestamp += FPS;
            } else {
                i = BUFF_SIZE;
            }
        }
        v_size = queue_size(&v_queue);
        i = v_size;
        while(i < BUFF_SIZE){
            tmp = queue_push(&v_queue);
            if (tmp != NULL){
                sprintf(str,VIDEO,frame_num);
                cairo_surface_destroy((cairo_surface_t *)(*tmp).data);
                (*tmp).data = (void *)cairo_image_surface_create_from_png(str);
                (*tmp).timestamp = v_timestamp;
                v_timestamp += 1000;
                ++frame_num;
            } else {
                i = BUFF_SIZE;
            }
        }
        buffer_ready = TRUE;
        wait(&timeout);
    }
}

void termination(){
    r_term = TRUE;
    v_term = TRUE;
    s_term = TRUE;
    pthread_join(r_th,0);
    pthread_join(s_th,0);
    v_queue_destroy(&v_queue);
    s_queue_destroy(&s_queue);
    sound_dev_close(&(info.dev)); 
    exit(0);
}

int main(int argc, char *argv[]){

printf("Preparing...\n");
    
//Sound preparing
    wav_get_info(SOUND,&info);
    sound_dev_prepare(DEVICE, &(info.dev), info.fmt.sample_rate, info.fmt.channels);
    queue_init(&s_queue, info.dev.buff_size_bytes);
//Done

//Some usefull info about wav-file

#ifdef WAV_INFO
    printf("File Name: %s\n Chunk ID: %s\n Data Size: %u\n RIFF type: %s\n  Chunk ID: %s\n  Fmt Data Size: %d\n  Comression code: 0x%04x\n  Channels: %u\n  Sample Rate: %u\n  Average Bytes Per Second: %u\n  Block Align: %u\n  Significant Bytes Per Second: %u\n   Chunk ID: %s\n   Data Size: %u\n",SOUND,info.riff.id, info.riff.data_size,info.riff.type, info.fmt.id, info.fmt.data_size, info.fmt.compression, info.fmt.channels, info.fmt.sample_rate, info.fmt.av_bps, info.fmt.block_align, info.fmt.sign_bps, info.data.id, info.data.size);
#endif

//Let's play some music!

    struct reader_t reader_info;

    int time;

    if (argc == 3){
        sscanf(argv[1],"%u",&reader_info.start_sound);
        sscanf(argv[2],"%u",&time);
    } else {    
        reader_info.start_sound = 0;
        time = 1;
    }

    reader_info.start_frame = (reader_info.start_sound * FPS) / 1000;
    reader_info.sound_offset = info.data.offset;
    reader_info.sound_buff_size = info.dev.buff_size_bytes;

    pthread_create(&r_th, NULL, reader, &(reader_info));

    struct timeval time1;
    struct timezone tz;

    int fps_timeout = 1000000000 / FPS;

    memset(&tz, 0, sizeof(tz));

    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    while(buffer_ready == FALSE)
       wait(&tv);
    
    struct packet_t *s_buff, *v_buff;

    sound_buff = (*(queue_pop(&s_queue))).data;
    s_buff = queue_pop_next(&s_queue);

    video_buff = (*(queue_pop(&v_queue))).data;
    v_buff = queue_pop_next(&s_queue);

    pthread_create(&s_th, NULL, sound_thread, &(info.dev));
    pthread_create(&v_th, NULL, video_thread, NULL);
    printf("Threads started...\n");

    while(v_ready == FALSE)
        wait(&tv);

//----------------------* KICKER
    int frames = 0;
    int rc = 0;

printf("Ready to play...\n");

    int no_skip = 0;
    for(int i = 0; i < ((time * FPS )/ 1000); ++i){
        pthread_cond_signal(&s_cond);
        pthread_cond_signal(&v_cond);
        v_buff = queue_pop_next(&v_queue);
        gettimeofday(&time1, &tz);
        pthread_mutex_lock(&k_mutex);
        timeout.tv_nsec = fps_timeout*1000;
        add_timespec(&timeout, &time1);
        rc = pthread_cond_timedwait(&k_cond, &k_mutex, (const struct timespec *restrict)&timeout);
        pthread_mutex_unlock(&k_mutex);
        if (rc == 0){
            sound_buff = (*s_buff).data;
            pthread_cond_signal(&s_cond);
            queue_skip(&s_queue,1);
            frames = (s_buff->timestamp / 1000) - (v_buff->timestamp / 1000);     
            printf("DEBUG: S: %ld V: %ld\n",s_buff->timestamp / 1000,v_buff->timestamp / 1000);
            if (frames > 0){
                queue_skip(&v_queue,frames);
                printf("DEBUG: Skip frames: %d\n",frames);
            } else if (frames < 0) {
                printf("DEBUG: No skip: %d\n",-frames);
                no_skip = -frames;
            }
            s_buff = queue_pop_next(&s_queue);
        }
        video_buff = (*v_buff).data;
        if (no_skip > 0){
            --no_skip;
        } else {
            queue_skip(&v_queue,1);
        }
    
    }

    pthread_cond_signal(&s_cond);

printf("THE END\n");

//---------------------------* KICKER END
    termination();
    return 0;
}
