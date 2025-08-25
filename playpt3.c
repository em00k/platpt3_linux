#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include "ayumi.h"
#include "pt3player.h"
#include "load_text.h"


static int load_file(const char* name, char** buffer, int* size) {
  FILE* f = fopen(name, "rb");
  if (f == NULL) {
    return 0;
  }
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  rewind(f);
  *buffer = (char*) malloc(*size + 1);
  if (*buffer == NULL) {
    fclose(f);
    return 0;
  }
  if ((int) fread(*buffer, 1, *size, f) != *size) {
    free(*buffer);
    fclose(f);
    return 0;
  }
  fclose(f);
  (*buffer)[*size] = 0;
  return 1;
}

static void show_help(void);

unsigned char ayreg[14];
struct ayumi ay[10];
int numofchips=0;
int volume = 10000;
int is_paused = 0;

struct ay_data t;
char files[10][255];
int numfiles=0;

//---------------------------------------------------------------
    //Simple Windows sound streaming code..
//static HWAVEOUT waveOutHand;
//static WAVEHDR waveHdr;
int lastsamp=0;
int isr_step=1;
int lastleng=0;
short *tmpbuf[10];
int frame[10];
int sample[10];
int fast=0;
int sample_count;
int mute[10];

char* music_buf;
int music_size;

int loadfiles();

//---------------------------------------------------------------
// Simple non-blocking keyboard input for Linux
void changemode(int dir) {
    static struct termios oldt, newt;
    if (dir == 1) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }
}


//---------------------------------------------------------------
    //Simple Alsa sound streaming code..
static snd_pcm_t *pcm_handle;

#define LBUFSIZE 10
static short sndbuf[4<<LBUFSIZE];
//static float sndbuf[4<<LBUFSIZE];
short *sptr;

char* music_buf;
int music_size;


static void snd_init(int samprate) {
    snd_pcm_hw_params_t *hw_params;
    int err;

    if ((err = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "cannot open audio device %s (%s)\\n", "default", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
        fprintf(stderr, "cannot allocate hardware parameter structure (%s)\\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_any(pcm_handle, hw_params)) < 0) {
        fprintf(stderr, "cannot initialize hardware parameter structure (%s)\\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "cannot set access type (%s)\\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
        fprintf(stderr, "cannot set sample format (%s)\\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &samprate, 0)) < 0) {
        fprintf(stderr, "cannot set sample rate (%s)\\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_channels(pcm_handle, hw_params, 2)) < 0) {
        fprintf(stderr, "cannot set channel count (%s)\\n", snd_strerror(err));
        exit(1);
    }

    // Set smaller buffer size for lower latency
    snd_pcm_uframes_t buffer_size = 4096/2;
    snd_pcm_uframes_t period_size = 1024/2;
    
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_size)) < 0) {
        fprintf(stderr, "cannot set buffer size (%s)\\n", snd_strerror(err));
    }
    
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, 0)) < 0) {
        fprintf(stderr, "cannot set period size (%s)\\n", snd_strerror(err));
    }

    if ((err = snd_pcm_hw_params(pcm_handle, hw_params)) < 0) {
        fprintf(stderr, "cannot set parameters (%s)\\n", snd_strerror(err));
        exit(1);
    }

    snd_pcm_hw_params_free(hw_params);

    if ((err = snd_pcm_prepare(pcm_handle)) < 0) {
        fprintf(stderr, "cannot prepare audio interface for use (%s)\\n", snd_strerror(err));
        exit(1);
    }
}

static void snd_end() {
    snd_pcm_close(pcm_handle);
}

static int get_snd_pos() {
    snd_pcm_sframes_t delay;
    if (snd_pcm_delay(pcm_handle, &delay) == 0) {
        return delay;
    }
    return 0;
}
//---------------------------------------------------------------


void update_ayumi_state(struct ayumi* ay, uint8_t* r, int ch) {
    func_getregs(r, ch);
    ayumi_set_tone(ay, 0, (r[1] << 8) | r[0]);
    ayumi_set_tone(ay, 1, (r[3] << 8) | r[2]);
    ayumi_set_tone(ay, 2, (r[5] << 8) | r[4]);
    ayumi_set_noise(ay, r[6]);
    ayumi_set_mixer(ay, 0, r[7] & 1, (r[7] >> 3) & 1, r[8] >> 4);
    ayumi_set_mixer(ay, 1, (r[7] >> 1) & 1, (r[7] >> 4) & 1, r[9] >> 4);
    ayumi_set_mixer(ay, 2, (r[7] >> 2) & 1, (r[7] >> 5) & 1, r[10] >> 4);
    ayumi_set_volume(ay, 0, r[8] & 0xf);
    ayumi_set_volume(ay, 1, r[9] & 0xf);
    ayumi_set_volume(ay, 2, r[10] & 0xf);
    ayumi_set_envelope(ay, (r[12] << 8) | r[11]);
    if (r[13] != 255) {
        ayumi_set_envelope_shape(ay, r[13]);
    }

    for (int i=0; i<9; i++) {
        if (i % 3 == 0 && mute[i] && ch == i / 3) {
            ayumi_set_volume(ay, 0, 0);
            ayumi_set_mixer(ay, 0, r[7] & 1, (r[7] >> 3) & 1, 0);
        }
        if (i % 3 == 1 && mute[i] && ch == i / 3) {
            ayumi_set_volume(ay, 1, 0);
            ayumi_set_mixer(ay, 1, (r[7] >> 1) & 1, (r[7] >> 4) & 1, 0);
        }
        if (i % 3 == 2 && mute[i] && ch == i / 3) {
            ayumi_set_volume(ay, 2, 0);
            ayumi_set_mixer(ay, 2, (r[7] >> 2) & 1, (r[7] >> 5) & 1, 0);
        }
    }
}

static int renday(void *snd, int leng, struct ayumi* ay, struct ay_data* t, int ch)
{
    isr_step = t->sample_rate / t->frame_rate;
    int isr_counter = 0;
    int16_t *buf = snd;
    int i = 0;
    if (fast) isr_step /= 4;
    lastleng=leng;
    while (leng>0)
    {
        if (!is_paused) {
            if (sample[ch] >= isr_step) {
                func_play_tick(ch);
                update_ayumi_state(ay,ayreg,ch);
                sample[ch] = 0;
                frame[ch]++;
            }
            ayumi_process(ay);
            if (t->dc_filter_on) {
                ayumi_remove_dc(ay);
            }
            buf[i] = (short) (ay->left * volume);
            buf[i+1] = (short) (ay->right * volume);
        }
        else {
            buf[i] = (short) (0);
            buf[i+1] = (short) (0);
        }

        sample[ch]++;
        leng-=4;
        i+=2;
    }
    return 1;	
}


void rewindto(struct ay_data* t, int skip) {
    snd_pcm_drain(pcm_handle);
    snd_end();
    if (skip<0) skip=0;
//	printf("\nrewind to %i", skip);

    for (int ch=0; ch<numofchips; ch++) {
        func_restart_music(ch);
        frame[ch] = 0;
        sample[ch] = 0;
    }
    for (int i=0;i<skip;i++) {
        for (int ch=0; ch<numofchips; ch++) {
            func_play_tick(ch);
            frame[ch] += 1;
//			for (int in=0; in<isr_step; in++)
//				ayumi_skip(&ay[ch]);
        }
    }
    snd_init(t->sample_rate);
//	printf("\n->%i\n", frame[0]);
}


void ayumi_play(struct ayumi ay[6], struct ay_data* t) {
    snd_init(t->sample_rate);
    int i, j, skip, curpos;
    while (1) {
        for (int ch = 0; ch < numofchips; ch++) {
            renday(tmpbuf[ch], (2 << LBUFSIZE), &ay[ch], t, ch);
        }

        for (int j = 0; j < (2 << LBUFSIZE) / 2; j++) {
            int tv = 0;
            for (int ch = 0; ch < numofchips; ch++) {
                tv += *(int16_t*)(tmpbuf[ch] + j);
            }
            sndbuf[j] = tv / numofchips;
        }

        snd_pcm_writei(pcm_handle, sndbuf, (2 << LBUFSIZE) / 4);

        skip = frame[0];
        lastsamp = get_snd_pos();

        usleep(10000);

        if (isr_step > 1) {
            int skip1 = get_snd_pos();
//			if (lastsamp > skip1) lastsamp = skip;
            int skip3 = (skip1 - lastsamp) / isr_step; //number of frames
            curpos = skip + skip3;
            if (curpos>=0) printf("\rpos=%i   ",curpos);
        }
        else curpos = skip;


        // Check for input using select() with no timeout (non-blocking)
        fd_set fds;
        struct timeval timeout;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) > 0) {
            char buf[8];
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            
            if (n > 0) {
                // Debug output key pressed 
                // printf("DEBUG: Read %d bytes:", n);
                // for (int j = 0; j < n; j++) {
                // 	printf(" %d", (unsigned char)buf[j]);
                // }
                // printf("\n");
                
                // Parse the input
                if (n == 1) {
                    // Single character
                    char c = buf[0];
                    if (c == 27) {
                        // ESC alone
                        // printf("DEBUG: ESC alone - exiting\n");
                        break;
                    } else if (c == 'q') {
                        break;
                    } else if (c == 'h') {
                        printf("\n");
                        show_help();
                    } else if (c == 'f') {
                        fast = !fast;
                        printf("Fast mode: %s\n", fast ? "ON" : "OFF");
                    } else if (c == ' ') {
                        is_paused = !is_paused;
                        printf("Paused: %s\n", is_paused ? "YES" : "NO");
                    } else if (c == 'r') {
                        printf("\nReloading...\n");
                        skip = frame[0];
                        loadfiles(0);
                        curpos -= 25;
                        if (curpos < 0) curpos = 0;
                        rewindto(t, curpos);
                        skip = frame[0];
                    } else if (c >= '1' && c <= '9') {
                        mute[c-'1'] = !mute[c-'1'];
                        printf("    (1=%s 2=%s 3=%s) (4=%s 5=%s 6=%s) (7=%s 8=%s 9=%s) \r",
                            !mute[0]?"on ":"off",
                            !mute[1]?"on ":"off",
                            !mute[2]?"on ":"off",
                            !mute[3]?"on ":"off",
                            !mute[4]?"on ":"off",
                            !mute[5]?"on ":"off",
                            !mute[6]?"on ":"off",
                            !mute[7]?"on ":"off",
                            !mute[8]?"on ":"off");
                    }
                } else if (n >= 3 && buf[0] == 27 && buf[1] == '[') {
                    // ESC[ sequences
                    switch (buf[2]) {
                        case 'A': // Up arrow
                            // printf("DEBUG: Up arrow\n");
                            volume *= 1.1;
                            if (volume > 15000) volume = 15000;
                            printf("   vol=%.3i%%  \r", volume / 150);
                            break;
                        case 'B': // Down arrow
                            // printf("DEBUG: Down arrow\n");
                            volume /= 1.1;
                            if (volume < 10) volume = 10;
                            printf("   vol=%.3i%%  \r", volume / 150);
                            break;
                        case 'C': // Right arrow
                            // printf("DEBUG: Right arrow\n");
                            {
                                int sk = curpos + 100;
                                rewindto(t, sk);
                                skip = frame[0];
                            }
                            break;
                        case 'D': // Left arrow
                            // printf("DEBUG: Left arrow\n");
                            {
                                int sk = curpos - 100;
                                if (sk < 0) sk = 0;
                                rewindto(t, sk);
                                skip = frame[0];
                            }
                            break;
                        case 'H': // Home
                            // printf("DEBUG: Home key\n");
                            rewindto(t, 0);
                            skip = frame[0];
                            break;
                        case '1': // Home ESC[1~
                            if (n >= 4 && buf[3] == '~') {
                                // printf("DEBUG: Home key (1~)\n");
                                rewindto(t, 0);
                                skip = frame[0];
                            }
                            break;
                        case '5': // Page Up ESC[5~
                            if (n >= 4 && buf[3] == '~') {
                                // printf("DEBUG: Page Up\n");
                                volume *= 1.1;
                                if (volume > 15000) volume = 15000;
                                printf("   vol=%.3i%%  \r", volume / 150);
                            }
                            break;
                        case '6': // Page Down ESC[6~
                            if (n >= 4 && buf[3] == '~') {
                                // printf("DEBUG: Page Down\n");
                                volume /= 1.1;
                                if (volume < 10) volume = 10;
                                printf("   vol=%.3i%%  \r", volume / 150);
                            }
                            break;
                        default:
                            // printf("DEBUG: Unknown escape sequence ESC[%c\n", buf[2]);
                            break;
                    }
                }
            }
        }

    }
    printf(".\n");
    snd_end();

}


void set_default_data(struct ay_data* t) {
    memset(t, 0, sizeof(struct ay_data));
    t->sample_rate = 44100;
    t->eqp_stereo_on = 1;
    t->dc_filter_on = 1;
    t->is_ym = 1;
    t->pan[0]=0.1;
    t->pan[1]=0.5;
    t->pan[2]=0.9;
    t->clock_rate = 1750000;
    t->frame_rate = 50;
    t->note_table = -1;
}

int loadfiles(int first)
{
    load_text_file("playpt3.txt", &t);
    forced_notetable=t.note_table;

    numofchips=0;
    for (int fn=0; fn<numfiles;fn++) {
        if(!load_file(files[fn], &music_buf, &music_size)) {
            printf("Load error\n");
            return 1;
        }
        printf("*** Loaded \"%s\" %i bytes\n",files[fn],music_size);

        int num = func_setup_music(music_buf, music_size, numofchips, first);

        numofchips+=num;
        if (first) printf("Number of chips: %i\n",num);
    }


    if (first) printf("Total number of chips: %i\n",numofchips);

    for (int ch=0; ch<numofchips; ch++) {
        if (!ayumi_configure(&ay[ch], t.is_ym, t.clock_rate, t.sample_rate)) {
            printf("ayumi_configure error (wrong sample rate?)\n");
            return 1;
        }
        ayumi_set_pan(&ay[ch], 0, t.pan[0], t.eqp_stereo_on);
        ayumi_set_pan(&ay[ch], 1, t.pan[1], t.eqp_stereo_on);
        ayumi_set_pan(&ay[ch], 2, t.pan[2], t.eqp_stereo_on);
        if (tmpbuf[ch]==0) tmpbuf[ch] = malloc(sizeof(sndbuf));
        else tmpbuf[ch] = realloc(tmpbuf[ch],sizeof(sndbuf));
        frame[ch]=0;
        sample[ch]=0;
        if (first) printf("Ayumi #%i configured\n",ch);
    }
    is_paused = 0;
}

static void show_help(void) {

    printf(
"ESC/q      - exit\n"
"PgUp/PgDn  - change volume\n"
"F          - toggle fast forward (x4)\n"
"SPACE      - pause/unpause\n"
"R          - reload song files\n"
"HOME       - rewind to beginning\n"
"←/→        - seek 2s backward/forward\n"
"1-9        - toggle channel mute\n\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("playpt3.exe <file.pt3> ...\n");
        return 1;
    }

    show_help();

    set_default_data(&t);
    changemode(1);
    for (int ch=0; ch<10; ch++) {
        tmpbuf[ch]=0;
    }

    for (int fn=1; fn<argc; fn++) {
        strcpy(files[fn-1],argv[fn]);
        numfiles++;
    }

//	printf("numfiles=%i\n",numfiles);
//	printf("files[0]=%s\n",files[0]);
    for (int i=0;i<10;i++)
        mute[i]=0;

    if (loadfiles(1)) {
    //	printf("Skipping...\n");
        //  for (int i=0;i<5376;i++)  //5376
        //	  func_play_tick(0);
        printf("Playing...\n");

        ayumi_play(ay, &t);
        printf("Finished\n");
        changemode(0);
        return 0;
    }
    else {
        printf("Failed.\n");
        changemode(0);
        return 1;
    }
}