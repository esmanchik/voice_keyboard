#include <sphinxbase/err.h>
#include <sphinxbase/ad.h>
#include <sphinxbase/cont_ad.h>

#include <pocketsphinx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <execinfo.h>

int open_uinput_device();
int write_key(int fd, int key);
void close_uinput_device(int fd);

void recognition_loop(void *ctx, int (*hypcb)(void *ctx, char const *hyp));

void type_key(int key) {
    int fd = open_uinput_device();
    usleep(100000);
    write_key(fd, key);
    close_uinput_device(fd);
}

int char2key(int ch);

int voice_keyboard(void *ctx, char const *hyp) {
    int i, key;
    char word[64];
    printf("hyp=%s\n", hyp);
    int fd = open_uinput_device();
    usleep(100000);
    for (i = 0, *word = '\0'; i < strlen(hyp); i += strlen(word)+1) {
        if (sscanf(hyp + i, "%s", word) < 1) break;
        printf("word=%s\n", word);
        if (!strcmp(word, "BACKSPACE")) key = KEY_BACKSPACE;
        else if (!strcmp(word, "ENTER")) key = KEY_ENTER;
        else if (!strcmp(word, "SPACE")) key = KEY_SPACE;
        else if (!strcmp(word, "SLASH")) key = KEY_SLASH;
        else if (!strcmp(word, "DOT")) key = KEY_DOT;
        else if (!strcmp(word, "TAB")) key = KEY_TAB;
        else if (!strcmp(word, "CAPSLOCK")) key = KEY_CAPSLOCK;
        else if (!strcmp(word, "ZERO")) key = KEY_0;
        else if (!strcmp(word, "ONE")) key = KEY_1;
        else if (!strcmp(word, "TWO")) key = KEY_2;
        else if (!strcmp(word, "THREE")) key = KEY_3;
        else if (!strcmp(word, "FOUR")) key = KEY_4;
        else if (!strcmp(word, "FIVE")) key = KEY_5;
        else if (!strcmp(word, "SIX")) key = KEY_6;
        else if (!strcmp(word, "SEVEN")) key = KEY_7;
        else if (!strcmp(word, "EIGHT")) key = KEY_8;
        else if (!strcmp(word, "NINE")) key = KEY_9;
        else key = char2key(word[0]); 
        write_key(fd, key);
    }
    close_uinput_device(fd);
    return key == KEY_DOT;
}

int main(void) {
    recognition_loop(NULL, voice_keyboard);
}

int open_uinput_device() {
    int i, fd;
    struct uinput_user_dev uidev;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(fd < 0)
        E_FATAL("error: open");
    if(ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
        E_FATAL("error: ioctl EV_KEY");
    if(ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0)
        E_FATAL("error: ioctl EV_SYN");
    for (i = KEY_ESC; i < KEY_STOP; i++)
        if(ioctl(fd, UI_SET_KEYBIT, i) < 0)
            E_FATAL("error: ioctl KEY_*");

    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "uinput-voice");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1;
    uidev.id.product = 0x1;
    uidev.id.version = 1;
    
    if(write(fd, &uidev, sizeof(uidev)) < 0)
        E_FATAL("error: write");

    if(ioctl(fd, UI_DEV_CREATE) < 0)
        E_FATAL("error: ioctl");

    return fd;
} 

int write_key(int fd, int key) {
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = key;
    ev.value = 1;

    if(write(fd, &ev, sizeof(ev)) < 0) return -1;

    ev.value = 0;

    if(write(fd, &ev, sizeof(ev)) < 0) return -1;

    return 0;
}

void close_uinput_device(int fd) {
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}

void sleep_msec(int ms) {
    usleep(ms * 1000);
}

/*
 * Main utterance processing loop:
 *     for (;;) {
 * 	   wait for start of next utterance;
 * 	   decode utterance until silence of at least 1 sec observed;
 * 	   print utterance result;
 *     }
 */
void recognition_loop(void *ctx, int (*hypcb)(void *ctx, char const *hyp)) {
    ad_rec_t *ad;
    int16 adbuf[4096];
    int32 k, ts, rem;
    char const *hyp;
    char const *uttid;
    cont_ad_t *cont;
    char word[256];
    cmd_ln_t *config;
    ps_decoder_t *ps;

	config = cmd_ln_init(NULL, ps_args(), TRUE,
			     "-hmm", MODELDIR "/hmm/en_US/hub4wsj_sc_8k",
                 "-lm", "alpha.lm", 
                 "-dict", "alpha.dic", 
			     NULL);
	if (config == NULL) E_FATAL("Failed to open init cmdln config\n");
	ps = ps_init(config);
	if (ps == NULL) E_FATAL("Failed to open init pocket sphinx\n");

    ad = ad_open_dev(NULL, 16000);
    if (ad == NULL) E_FATAL("Failed to open audio device\n");

    /* Initialize continuous listening module */
    if ((cont = cont_ad_init(ad, ad_read)) == NULL)
        E_FATAL("Failed to initialize voice activity detection\n");
    if (ad_start_rec(ad) < 0)
        E_FATAL("Failed to start recording\n");
    if (cont_ad_calib(cont) < 0)
        E_FATAL("Failed to calibrate voice activity detection\n");

    for (;;) {
        /* Indicate listening for next utterance */
        printf("READY....\n");
        fflush(stdout);
        fflush(stderr);

        /* Wait data for next utterance */
        while ((k = cont_ad_read(cont, adbuf, 4096)) == 0)
            sleep_msec(100);

        if (k < 0)
            E_FATAL("Failed to read audio\n");

        /*
         * Non-zero amount of data received; start recognition of new utterance.
         * NULL argument to uttproc_begin_utt => automatic generation of utterance-id.
         */
        if (ps_start_utt(ps, NULL) < 0)
            E_FATAL("Failed to start utterance\n");
        ps_process_raw(ps, adbuf, k, FALSE, FALSE);
        printf("Listening...\n");
        fflush(stdout);

        /* Note timestamp for this first block of data */
        ts = cont->read_ts;

        /* Decode utterance until end (marked by a "long" silence, >1sec) */
        for (;;) {
            /* Read non-silence audio data, if any, from continuous listening module */
            if ((k = cont_ad_read(cont, adbuf, 4096)) < 0)
                E_FATAL("Failed to read audio\n");
            if (k == 0) {
                /*
                 * No speech data available; check current timestamp with most recent
                 * speech to see if more than 1 sec elapsed.  If so, end of utterance.
                 */
                if ((cont->read_ts - ts) > DEFAULT_SAMPLES_PER_SEC)
                    break;
            }
            else {
                /* New speech data received; note current timestamp */
                ts = cont->read_ts;
            }

            /*
             * Decode whatever data was read above.
             */
            rem = ps_process_raw(ps, adbuf, k, FALSE, FALSE);

            /* If no work to be done, sleep a bit */
            if ((rem == 0) && (k == 0))
                sleep_msec(20);
        }

        /*
         * Utterance ended; flush any accumulated, unprocessed A/D data and stop
         * listening until current utterance completely decoded
         */
        ad_stop_rec(ad);
        while (ad_read(ad, adbuf, 4096) >= 0);
        cont_ad_reset(cont);

        printf("Stopped listening, please wait...\n");
        fflush(stdout);
        /* Finish decoding, obtain and print result */
        ps_end_utt(ps);
        hyp = ps_get_hyp(ps, NULL, &uttid);
        printf("%s: %s\n", uttid, hyp);
        fflush(stdout);

        /* Exit if the first word spoken was GOODBYE */
        if (hyp) {
            if (hypcb && hypcb(ctx, hyp)) break;
        }

        /* Resume A/D recording for next utterance */
        if (ad_start_rec(ad) < 0)
            E_FATAL("Failed to start recording\n");
    }

    cont_ad_close(cont);
    ad_close(ad);
}

int char2key(int ch) {
/* Use following code in Emacs to generate function body:

(setq keys (number-sequence (string-to-char "A") (string-to-char "Z")))

(mapcar (lambda (code)
          (let ((key (char-to-string code)))
            (insert (concat "case '" key "': return KEY_" key ";\n")))) 
        keys)

*/
    switch (ch) {
    case 'A': return KEY_A;
    case 'B': return KEY_B;
    case 'C': return KEY_C;
    case 'D': return KEY_D;
    case 'E': return KEY_E;
    case 'F': return KEY_F;
    case 'G': return KEY_G;
    case 'H': return KEY_H;
    case 'I': return KEY_I;
    case 'J': return KEY_J;
    case 'K': return KEY_K;
    case 'L': return KEY_L;
    case 'M': return KEY_M;
    case 'N': return KEY_N;
    case 'O': return KEY_O;
    case 'P': return KEY_P;
    case 'Q': return KEY_Q;
    case 'R': return KEY_R;
    case 'S': return KEY_S;
    case 'T': return KEY_T;
    case 'U': return KEY_U;
    case 'V': return KEY_V;
    case 'W': return KEY_W;
    case 'X': return KEY_X;
    case 'Y': return KEY_Y;
    case 'Z': return KEY_Z;
    }
}
